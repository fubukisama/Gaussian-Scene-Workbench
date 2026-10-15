# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Bounded FP32 backing storage at the native Gaussian optimizer-surgery seam.

This changes storage, not clone/split/prune decisions or optimizer arithmetic.
Only exact standard Adam or validated canonical SparseGaussianAdam layouts are
accepted. Every surgery still returns
fresh leaf Parameters without gradients, as the upstream methods do. Reserved
tails are zero-initialized and bounded across all six parameter groups.

Checkpoints deliberately retain upstream capture semantics. PyTorch serializes
the bounded backing storage too; no full-model CUDA clone is attempted during
an already memory-constrained safe pause.
"""
import math
import inspect
import sys
from types import MethodType


_NAMES = ("xyz", "f_dc", "f_rest", "opacity", "scaling", "rotation")


def _storage_bytes(tensor):
    return int(tensor.untyped_storage().nbytes())


def _tensor_bytes(tensor):
    return int(tensor.numel()) * int(tensor.element_size())


def _whole_backing(tensor):
    row_bytes = math.prod(tensor.shape[1:]) * tensor.element_size()
    if not row_bytes:
        return tensor.detach()
    capacity = _storage_bytes(tensor) // row_bytes
    return tensor.detach().as_strided((capacity,) + tuple(tensor.shape[1:]), tensor.stride())


def _dense_f_xyz(tensor):
    """The exact dense column-major layout produced by upstream PLY loading."""
    return (tensor.ndim == 2 and tensor.shape[1] == 3 and
            tuple(tensor.stride()) == (1, int(tensor.shape[0])) and
            tensor.storage_offset() == 0 and _storage_bytes(tensor) == _tensor_bytes(tensor))


class CapacityStorage:
    """Installed adapter; snapshots report live logical and physical storage.

    Callers must not read stale parameters or backward again through the old
    graph after surgery, and must install after full-state restore. Allocation failure is
    not swallowed: partially completed *density algorithms* cannot be retried
    as though they had not consumed RNG or committed a preceding clone stage.
    """

    def __init__(self, torch, model, backend, memory_probe, max_reserve_bytes,
                 growth_fraction, chunk_bytes, optimizer_kind="adam", layout_normalization=None):
        self.torch, self.model, self.backend = torch, model, backend
        self.memory_probe = memory_probe
        self.max_reserve_bytes = max_reserve_bytes
        self.growth_fraction = growth_fraction
        self.chunk_bytes = chunk_bytes
        self.optimizer_kind = optimizer_kind
        self.layout_normalization = layout_normalization or {
            "kind": "none", "events": 0, "copies": 0, "copied_bytes": 0}
        self._buffers = {}
        self._originals = {name: getattr(model, name) for name in
            ("cat_tensors_to_optimizer", "_prune_optimizer", "replace_tensor_to_optimizer")}

    def _groups(self):
        return self.model.optimizer.param_groups

    def _rows(self):
        return int(self._groups()[0]["params"][0].shape[0])

    def _row_bytes(self):
        # Include two lazy Adam moments even before their first optimizer step.
        return sum(int(group["params"][0][0:1].numel()) * 4 * 3
                   if self._rows() else math.prod(group["params"][0].shape[1:]) * 4 * 3
                   for group in self._groups())

    def _capacity(self, required):
        extra = min(int(math.ceil(required * self.growth_fraction)),
                    self.max_reserve_bytes // max(1, self._row_bytes()))
        if self.memory_probe is not None and extra:
            try:
                observation = self.memory_probe()
                if hasattr(observation, "free_bytes"):
                    free, total = observation.free_bytes, observation.total_bytes
                else:
                    free, total = observation
                if (type(free) is not int or type(total) is not int or
                        not 0 <= free <= total or total <= 0):
                    extra = 0
                else:
                    spare = max(512 * 1024 ** 2, int(math.ceil(total * .1)))
                    # Optional capacity must never consume the safety reserve.
                    optional = max(0, free - spare - required * self._row_bytes())
                    extra = min(extra, optional // max(1, self._row_bytes()))
            except Exception:
                extra = 0
        return required + extra

    def _owned_buffer(self, name, key, tensor):
        buffer = self._buffers.get(name, {}).get(key)
        if (buffer is not None and buffer.data_ptr() == tensor.data_ptr()
                and tuple(buffer.shape[1:]) == tuple(tensor.shape[1:])):
            return buffer
        return None

    def _commit(self, staged):
        result = {}
        optimizer = self.model.optimizer
        for group, old, parameter, state, buffers in staged:
            optimizer.state.pop(old, None)
            group["params"][0] = parameter
            if state is not None:
                optimizer.state[parameter] = state
            self._buffers[group["name"]] = buffers
            result[group["name"]] = parameter
        return result

    def append(self, tensors):
        torch, old_rows = self.torch, self._rows()
        if set(tensors) != set(_NAMES):
            raise ValueError("Gaussian extension groups disagree")
        additions = None
        for group in self._groups():
            parameter, extension = group["params"][0], tensors[group["name"]]
            if (extension.dtype != torch.float32 or extension.device != parameter.device or
                    tuple(extension.shape[1:]) != tuple(parameter.shape[1:])):
                raise ValueError("Gaussian extension storage disagrees")
            if additions is None:
                additions = int(extension.shape[0])
            if extension.shape[0] != additions:
                raise ValueError("Gaussian extension row counts disagree")
        required = old_rows + additions
        capacity = self._capacity(required)
        staged, fills = [], []
        with torch.no_grad():
            # Complete every allocation before touching any retained backing.
            for group in self._groups():
                name, old = group["name"], group["params"][0]
                state = self.model.optimizer.state.get(old)
                state = dict(state) if state is not None else None
                owned = self._owned_buffer(name, "parameter", old)
                rows = max(capacity, int(owned.shape[0]) if owned is not None else 0)
                buffers = {}
                values = [("parameter", old)]
                if state:
                    values += [(key, state[key]) for key in ("exp_avg", "exp_avg_sq")]
                for key, value in values:
                    buffer = self._owned_buffer(name, key, value)
                    reuse = buffer is not None and int(buffer.shape[0]) >= required
                    if not reuse:
                        buffer = torch.zeros((rows,) + tuple(value.shape[1:]),
                                             dtype=value.dtype, device=value.device)
                    buffers[key] = buffer
                    fills.append((buffer, value, not reuse,
                                  tensors[name] if key == "parameter" else None))
                    if key != "parameter":
                        state[key] = buffer[:required]
                parameter = torch.nn.Parameter(buffers["parameter"][:required],
                                               requires_grad=True)
                staged.append((group, old, parameter, state, buffers))
            for buffer, value, copy_old, extension in fills:
                if copy_old:
                    buffer[:old_rows].copy_(value)
                if extension is None:
                    buffer[old_rows:required].zero_()
                else:
                    buffer[old_rows:required].copy_(extension)
        return self._commit(staged)

    def prune(self, mask):
        torch, old_rows = self.torch, self._rows()
        parameter = self._groups()[0]["params"][0]
        if (mask.dtype != torch.bool or mask.device != parameter.device or
                tuple(mask.shape) != (old_rows,)):
            raise ValueError("Gaussian pruning mask disagrees")
        with torch.no_grad():
            indices = mask.nonzero(as_tuple=False).reshape(-1)
            required = int(indices.numel())
            maximum_capacity = required + self.max_reserve_bytes // max(1, self._row_bytes())
            largest_row = max(math.prod(group["params"][0].shape[1:]) for group in self._groups())
            scratch = torch.empty(max(largest_row, self.chunk_bytes // 4),
                                  dtype=torch.float32, device=parameter.device)
            staged, moves = [], []
            # Allocate every needed destination/scratch before any live prefix
            # is changed. A large prune drops excessive spare capacity instead
            # of turning formerly live rows into an unbounded retained cache.
            for group in self._groups():
                name, old = group["name"], group["params"][0]
                old_state = self.model.optimizer.state.get(old)
                state = dict(old_state) if old_state is not None else None
                buffers = {}
                values = [("parameter", old)]
                if state:
                    values += [(key, state[key]) for key in ("exp_avg", "exp_avg_sq")]
                for key, value in values:
                    buffer = self._owned_buffer(name, key, value)
                    if buffer is None:
                        buffer = _whole_backing(value)
                    if buffer.shape[0] > maximum_capacity:
                        buffer = torch.zeros((self._capacity(required),) + tuple(value.shape[1:]),
                                             dtype=value.dtype, device=value.device)
                    buffers[key] = buffer
                    moves.append((value, buffer))
                    if key != "parameter":
                        state[key] = buffer[:required]
                new = torch.nn.Parameter(buffers["parameter"][:required], requires_grad=True)
                staged.append((group, old, new, state, buffers))
            for source, destination in moves:
                row_elements = math.prod(source.shape[1:])
                chunk_rows = max(1, int(scratch.numel()) // max(1, row_elements))
                # nonzero() returns stable ascending source indices. Each
                # source index >= its destination row, so earlier prefix writes
                # cannot overwrite source rows needed by later chunks.
                for start in range(0, required, chunk_rows):
                    stop = min(required, start + chunk_rows)
                    view = scratch[:(stop - start) * row_elements].reshape(
                        (stop - start,) + tuple(source.shape[1:]))
                    torch.index_select(source, 0, indices[start:stop], out=view)
                    destination[start:stop].copy_(view)
                destination[required:].zero_()
        return self._commit(staged)

    def replace(self, tensor, name):
        torch = self.torch
        for group in self._groups():
            if group["name"] != name:
                continue
            old = group["params"][0]
            if (tuple(tensor.shape) != tuple(old.shape) or
                    tensor.dtype != old.dtype or tensor.device != old.device):
                # Preserve the old interface for unusual replacements, while
                # dropping stale ownership before the next supported surgery.
                result = self._originals["replace_tensor_to_optimizer"](tensor, name)
                self._buffers.pop(name, None)
                return result
            stored_state = self.model.optimizer.state.get(old)
            if stored_state is None:
                return self._originals["replace_tensor_to_optimizer"](tensor, name)
            state, buffers = dict(stored_state), {}
            for key, value in (("parameter", old), ("exp_avg", state["exp_avg"]),
                               ("exp_avg_sq", state["exp_avg_sq"])):
                buffer = self._owned_buffer(name, key, value)
                buffers[key] = buffer if buffer is not None else _whole_backing(value)
            new = torch.nn.Parameter(buffers["parameter"][:old.shape[0]], requires_grad=True)
            state["exp_avg"] = buffers["exp_avg"][:old.shape[0]]
            state["exp_avg_sq"] = buffers["exp_avg_sq"][:old.shape[0]]
            with torch.no_grad():
                buffers["parameter"][:old.shape[0]].copy_(tensor)
                buffers["parameter"][old.shape[0]:].zero_()
                buffers["exp_avg"].zero_()
                buffers["exp_avg_sq"].zero_()
            return self._commit([(group, old, new, state, buffers)])
        return {}

    def snapshot(self):
        logical, reserved, storages = 0, 0, set()
        capacities = []
        for group in self._groups():
            parameter = group["params"][0]
            values = [parameter]
            values += [value for key, value in self.model.optimizer.state.get(parameter, {}).items()
                       if key in ("exp_avg", "exp_avg_sq")]
            for value in values:
                logical += _tensor_bytes(value)
                identity = (str(value.device), value.untyped_storage().data_ptr())
                if identity not in storages:
                    storages.add(identity)
                    reserved += _storage_bytes(value)
            row_size = math.prod(parameter.shape[1:]) * parameter.element_size()
            if row_size:
                capacities.append(_storage_bytes(parameter) // row_size)
        return {"version": 1, "backend": self.backend,
                "optimizer_kind": self.optimizer_kind,
                "layout_normalization": dict(self.layout_normalization),
                "logical_rows": self._rows(), "capacity_rows": min(capacities),
                "logical_bytes": logical, "reserved_bytes": reserved,
                "spare_bytes": max(0, reserved - logical),
                "max_reserve_bytes": self.max_reserve_bytes}

    def estimate_growth(self, clone_count=0, split_count=0):
        """Extra peak to ADD to the conservative stock density estimate.

        Two successive append expansions can briefly overlap two new bounded
        tails; current retained capacity is already charged by the free-memory
        observation. Chunk pruning scratch is additional. This intentionally
        does not credit anticipated allocator releases or claim a tighter peak.
        """
        rows = self._rows()
        if (type(clone_count) is not int or type(split_count) is not int or
                min(clone_count, split_count) < 0 or clone_count + split_count > rows):
            raise ValueError("Invalid Gaussian capacity growth counts")
        result = self.snapshot()
        result.update(transient_point_count=rows + clone_count + 2 * split_count,
                      final_point_count=rows + clone_count + split_count,
                      capacity_additional_peak_bytes=2 * self.max_reserve_bytes + self.chunk_bytes)
        return result


def install_capacity_storage(torch, model, backend, memory_probe=None,
                             max_reserve_bytes=64 * 1024 ** 2,
                             growth_fraction=.1, chunk_bytes=4 * 1024 ** 2):
    """Install bounded surgery for a supported model, else leave it unchanged.

    memory_probe optionally returns CUDA (free, total) bytes or a memory
    observation. An unavailable/invalid probe disables speculative spare rows.
    The already-loaded canonical Graphdeco SparseGaussianAdam class is accepted
    only for CUDA 3DGS with the validated Python step/adamUpdate ABI. Importing
    this module never loads a rasterizer. Custom optimizers/subclasses are not
    implicitly treated as equivalent to the real validated CUDA optimizer.
    """
    if (backend not in ("3dgs", "2dgs") or type(max_reserve_bytes) is not int or
            max_reserve_bytes < 0 or isinstance(growth_fraction, bool) or
            not isinstance(growth_fraction, (int, float)) or
            not math.isfinite(growth_fraction) or not 0 <= growth_fraction <= .5 or
            type(chunk_bytes) is not int or chunk_bytes < 4 or
            (memory_probe is not None and not callable(memory_probe))):
        raise ValueError("Invalid Gaussian capacity configuration")
    def decline(reason):
        model._gsw_capacity_storage_status = {"enabled": False, "reason": reason}
        return None

    existing = getattr(model, "_gsw_capacity_storage", None)
    if existing is not None:
        return existing
    optimizer = getattr(model, "optimizer", None)
    optimizer_kind = "adam"
    if type(optimizer) is not torch.optim.Adam:
        module = sys.modules.get("diff_gaussian_rasterization")
        canonical = getattr(module, "SparseGaussianAdam", None)
        canonical_abi = False
        if (backend == "3dgs" and canonical is not None and type(optimizer) is canonical
                and canonical.__module__ == "diff_gaussian_rasterization"
                and issubclass(canonical, torch.optim.Adam)):
            try:
                canonical_abi = (tuple(inspect.signature(canonical.step).parameters) ==
                                 ("self", "visibility", "N") and
                                 callable(getattr(getattr(module, "_C", None), "adamUpdate", None)))
            except (TypeError, ValueError):
                pass
        if canonical_abi:
            optimizer_kind = "canonical_sparse_adam"
        else:
            return decline("sparse_adam_validation_required" if
                           type(optimizer).__name__ == "SparseGaussianAdam" else "unsupported_optimizer")
    groups = optimizer.param_groups
    if (len(groups) != len(_NAMES) or {group.get("name") for group in groups} != set(_NAMES)
            or any(len(group.get("params", [])) != 1 for group in groups)):
        return decline("unsupported_optimizer_layout")
    rows, device, storage_identities = None, None, set()
    normalize_group = None
    for group in groups:
        parameter = group["params"][0]
        known_dense_f = group["name"] == "xyz" and _dense_f_xyz(parameter)
        if not parameter.is_contiguous() and known_dense_f and optimizer_kind == "canonical_sparse_adam":
            return decline("sparse_adam_layout_not_verified")
        if (type(parameter) is not torch.nn.Parameter or parameter.dtype != torch.float32
                or parameter.ndim < 2 or (not parameter.is_contiguous() and not known_dense_f)
                or parameter.storage_offset() != 0 or not parameter.is_leaf or not parameter.requires_grad or
                any(group.get(key, False) for key in ("amsgrad", "capturable", "differentiable", "fused"))):
            return decline("unsupported_parameter_layout")
        if not parameter.is_contiguous():
            normalize_group = group
        if rows is None:
            rows, device = int(parameter.shape[0]), parameter.device
        if parameter.shape[0] != rows or parameter.device != device:
            return decline("unsupported_parameter_layout")
        if group["name"] == "xyz" and tuple(parameter.shape[1:]) != (3,):
            return decline("unsupported_parameter_layout")
        if group["name"] == "scaling" and tuple(parameter.shape[1:]) != ((3,) if backend == "3dgs" else (2,)):
            return decline("unsupported_parameter_layout")
        tail = tuple(parameter.shape[1:])
        fixed = {"f_dc": (1, 3), "opacity": (1,), "rotation": (4,)}
        if ((group["name"] in fixed and tail != fixed[group["name"]]) or
                (group["name"] == "f_rest" and
                 (len(tail) != 2 or tail[1] != 3 or tail[0] not in (0, 3, 8, 15)))):
            return decline("unsupported_parameter_layout")
        state = optimizer.state.get(parameter, {})
        if state and set(state) != {"step", "exp_avg", "exp_avg_sq"}:
            return decline("unsupported_optimizer_state")
        for key in ("exp_avg", "exp_avg_sq"):
            value = state.get(key)
            if value is not None and (value.dtype != torch.float32 or value.device != device
                    or value.shape != parameter.shape or
                    (not value.is_contiguous() and not (known_dense_f and _dense_f_xyz(value)))
                    or value.storage_offset() != 0 or value.requires_grad):
                return decline("unsupported_optimizer_state")
        for value in [parameter] + [state[key] for key in ("exp_avg", "exp_avg_sq") if key in state]:
            if not value.numel():
                continue
            row_bytes = math.prod(value.shape[1:]) * value.element_size()
            identity = (str(value.device), value.untyped_storage().data_ptr())
            if identity in storage_identities or _storage_bytes(value) % row_bytes:
                return decline("unsupported_aliased_storage")
            storage_identities.add(identity)
    if memory_probe is None and device.type == "cuda":
        memory_probe = lambda: torch.cuda.mem_get_info(device)
    if optimizer_kind == "canonical_sparse_adam" and device.type != "cuda":
        return decline("unsupported_sparse_adam_device")
    normalization = {"kind": "none", "events": 0, "copies": 0, "copied_bytes": 0}
    if normalize_group is not None:
        old = normalize_group["params"][0]
        if (getattr(model, "_xyz", None) is not old or
                any(group["params"][0].grad is not None for group in groups)):
            return decline("layout_normalization_pending_gradient")
        old_state = optimizer.state.get(old)
        values = [("parameter", old)]
        if old_state:
            values += [(key, old_state[key]) for key in ("exp_avg", "exp_avg_sq")
                       if not old_state[key].is_contiguous()]
        copied_bytes = sum(_tensor_bytes(value) for _, value in values)
        if memory_probe is not None:
            try:
                observation = memory_probe()
                if hasattr(observation, "free_bytes"):
                    free, total = observation.free_bytes, observation.total_bytes
                else:
                    free, total = observation
                if (type(free) is not int or type(total) is not int or
                        not 0 <= free <= total or total <= 0 or
                        free - max(512 * 1024 ** 2, int(math.ceil(total * .1))) < copied_bytes):
                    return decline("layout_normalization_headroom_unavailable")
            except Exception:
                return decline("layout_normalization_headroom_unavailable")
        copies = {}
        with torch.no_grad():
            # Finish every allocation and exact FP32 copy before replacing any
            # model/optimizer reference. This is a once-only initialization or
            # full-state-restore adaptation, never a per-iteration conversion.
            for key, value in values:
                copies[key] = torch.empty_like(value, memory_format=torch.contiguous_format)
                copies[key].copy_(value)
        parameter = torch.nn.Parameter(copies["parameter"], requires_grad=old.requires_grad)
        state = dict(old_state) if old_state is not None else None
        if state is not None:
            for key in ("exp_avg", "exp_avg_sq"):
                if key in copies:
                    state[key] = copies[key]
        optimizer.state.pop(old, None)
        normalize_group["params"][0] = parameter
        model._xyz = parameter
        if state is not None:
            optimizer.state[parameter] = state
        normalization = {"kind": "dense_f_xyz", "events": 1,
                         "copies": len(copies), "copied_bytes": copied_bytes}
    manager = CapacityStorage(torch, model, backend, memory_probe,
                              max_reserve_bytes, float(growth_fraction), chunk_bytes, optimizer_kind,
                              normalization)
    model.cat_tensors_to_optimizer = MethodType(lambda unused, tensors: manager.append(tensors), model)
    model._prune_optimizer = MethodType(lambda unused, mask: manager.prune(mask), model)
    model.replace_tensor_to_optimizer = MethodType(lambda unused, tensor, name: manager.replace(tensor, name), model)
    model._gsw_capacity_storage = manager
    model._gsw_capacity_storage_status = {"enabled": True, "reason": "bounded_fp32_storage",
                                          "optimizer_kind": optimizer_kind,
                                          "layout_normalization": dict(normalization)}
    return manager
