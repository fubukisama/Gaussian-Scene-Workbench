# Copyright (C) 2023, Inria, GRAPHDECO research group.
# Upstream densification orchestration is adapted under the research/evaluation
# license in native/worker/licenses/2dgs-LICENSE.md and gaussian-splatting/LICENSE.md.
# No commercial-use permission or MIT relicensing is implied.

"""Native-only density safety for both Graphdeco 3DGS and hbb1 2DGS.

Clone/split and optimizer surgery remain the upstream model's implementation.
The orchestration follows their densify_and_prune, with a separate, fixed
geometry-aware size limit and bounded final pruning. See THIRD_PARTY_LICENSES.
No torch import here: UI workers need not load the CUDA training environment.
"""
import math


class NativeDensityControl:
    VERSION = 1
    MAX_PRUNE_FRACTION = .20
    RECOVERY_ITERATIONS = 300

    def __init__(self, torch, model, camera_extent, backend, emit=None):
        if backend not in ("3dgs", "2dgs") or not math.isfinite(camera_extent) or camera_extent <= 0:
            raise ValueError("Invalid native density-control backend or camera extent")
        self.torch, self.backend, self.emit = torch, backend, emit
        self.camera_extent = float(camera_extent)
        # Robust fixed source bounds, not optimized/expanding Gaussian positions.
        # Deterministic bounded sampling consumes no RNG and is translation invariant.
        xyz = model.get_xyz.detach()
        stride = max(1, (len(xyz) + 65535) // 65536)
        sample = xyz[::stride][:65536].float()
        sample = sample[torch.isfinite(sample).all(dim=1)]
        geometry_extent = 0.
        if len(sample) > 1:
            bounds = torch.quantile(sample, sample.new_tensor([.05, .95]), dim=0)
            geometry_extent = float(torch.linalg.vector_norm(bounds[1] - bounds[0]).item()) * .5
        self.world_size_limit = .1 * max(self.camera_extent, geometry_extent)

    def capture(self):
        return dict(version=self.VERSION, backend=self.backend,
                    camera_extent=self.camera_extent, world_size_limit=self.world_size_limit)

    def restore(self, state):
        if (not isinstance(state, dict) or state.get("version") != self.VERSION
                or state.get("backend") != self.backend):
            raise ValueError("Incompatible native density-control checkpoint")
        for key in ("camera_extent", "world_size_limit"):
            value = state.get(key)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                raise ValueError("Invalid density-control checkpoint scale")
        if state["world_size_limit"] < .1 * state["camera_extent"]:
            raise ValueError("Density-control checkpoint scale is inconsistent")
        self.camera_extent = float(state["camera_extent"])
        self.world_size_limit = float(state["world_size_limit"])

    def densify_and_prune(self, model, max_grad, min_opacity, max_screen_size,
                          iteration, reset_interval, radii=None,
                          white_background=False, densify_from_iter=500):
        torch = self.torch
        grads = model.xyz_gradient_accum / model.denom
        grads[~torch.isfinite(grads)] = 0.
        if self.backend == "3dgs":
            if radii is None:
                raise ValueError("3DGS density control requires source radii")
            model.tmp_radii = radii
        try:
            # Never replace the camera extent used for clone/split or learning rates.
            model.densify_and_clone(grads, max_grad, self.camera_extent)
            model.densify_and_split(grads, max_grad, self.camera_extent)
            opacity = model.get_opacity.reshape(-1)
            scales = model.get_scaling
            invalid = (~torch.isfinite(model.get_xyz).all(dim=1)
                       | ~torch.isfinite(scales).all(dim=1) | ~torch.isfinite(opacity))
            # The reset clamps opacity to .01. Give newly reset splats time to
            # recover before 2DGS's .05 cull; 3DGS's .005 cull is unchanged.
            last_reset = ((iteration - 1) // reset_interval) * reset_interval if reset_interval > 0 else 0
            if white_background and iteration > densify_from_iter:
                last_reset = max(last_reset, densify_from_iter)
            threshold = min_opacity
            if last_reset > 0 and 0 < iteration - last_reset <= self.RECOVERY_ITERATIONS:
                threshold = min(threshold, .005)
            candidate = opacity < threshold
            if max_screen_size:
                candidate |= ((model.max_radii2D > max_screen_size)
                              | (scales.max(dim=1).values > self.world_size_limit))
            candidate &= ~invalid
            requested = int(candidate.sum().item())
            budget = int((len(opacity) - int(invalid.sum().item())) * self.MAX_PRUNE_FRACTION)
            deferred = 0
            if requested > budget:
                # Keep high-opacity contributors first, with stable source-order
                # ties. Defer rather than destroy potentially useful geometry.
                indices = candidate.nonzero(as_tuple=False).flatten()
                # torch.sort exposes stable ties in the supported Torch 1.12
                # runtime; torch.argsort(stable=...) does not there.
                order = torch.sort(opacity[indices], stable=True).indices
                candidate.zero_()
                candidate[indices[order[:budget]]] = True
                deferred = requested - budget
            model.prune_points(candidate | invalid)
            if deferred and self.emit:
                self.emit("[gsw-density-control]", dict(
                    version=self.VERSION, iteration=iteration, deferred=deferred,
                    requested=requested, removed=budget, before=len(opacity),
                    world_size_limit=self.world_size_limit))
        finally:
            if self.backend == "3dgs":
                model.tmp_radii = None
        if model.get_xyz.device.type == "cuda":
            torch.cuda.empty_cache()
