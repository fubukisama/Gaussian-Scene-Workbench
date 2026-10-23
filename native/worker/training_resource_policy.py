# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Read-only, pre-densification memory admission for both native trainers.

The estimator reads tensor metadata only. The optional adapter reads existing
gradient/scale statistics without modifying them or sampling RNG. The caller
executes its existing density routine only when the decision permits it.
"""
from dataclasses import asdict, dataclass, replace
import math
import numbers


@dataclass(frozen=True)
class MemoryObservation:
    """Measured device headroom and explicitly reusable allocator storage.

    reusable_cache_bytes is NOT total reserved CUDA memory. The observer must
    supply only storage it has classified as reusable; the policy credits half
    to remain conservative about fragmentation and competing allocations.
    """
    free_bytes: int
    total_bytes: int
    reusable_cache_bytes: int = 0
    allocated_bytes: object = None
    reserved_bytes: object = None
    active_bytes: object = None
    inactive_split_bytes: object = None
    allocator_status: str = 'unreported'


@dataclass(frozen=True)
class GrowthDecision:
    allowed: bool
    reason: str
    point_count: int = 0
    transient_point_count: int = 0
    final_point_count: int = 0
    parameter_bytes: int = 0
    optimizer_state_bytes: int = 0
    additional_peak_bytes: int = 0
    free_bytes: int = 0
    total_bytes: int = 0
    spare_bytes: int = 0
    available_bytes: int = 0
    observation_valid: bool = False
    cache_credit_bytes: int = 0
    cache_reclaim_attempted: bool = False
    device_free_before_reclaim_bytes: int = 0
    cache_reclaim_status: str = 'not_attempted'
    torch_allocated_bytes: object = None
    torch_reserved_bytes: object = None
    torch_active_bytes: object = None
    torch_inactive_split_bytes: object = None
    allocator_observation_status: str = 'unreported'


_GROUP_NAMES = ("xyz", "f_dc", "f_rest", "opacity", "scaling", "rotation")
GROWTH_INTEGER_FIELDS = (
    "point_count", "transient_point_count", "final_point_count", "parameter_bytes",
    "optimizer_state_bytes", "additional_peak_bytes", "free_bytes", "total_bytes",
    "spare_bytes", "available_bytes", "cache_credit_bytes", "clone_count", "split_count")
GROWTH_BOOLEAN_FIELDS = ("allowed", "observation_valid", "candidate_counts_known", "guard_enabled", "cache_reclaim_attempted")
GROWTH_REASONS = ("admitted", "vram_pressure", "probe_unavailable",
                  "invalid_observation", "estimate_unavailable", "guard_disabled")


def normalize_growth(payload):
    """Return the version-one JSON observer contract, or None when invalid.

    Integers fit Qt JSON's exact range; booleans never count as integers. Unknown
    non-contract keys are dropped, while future versions/backends are rejected.
    """
    if (not isinstance(payload, dict) or type(payload.get("version")) is not int or
            payload["version"] != 1 or payload.get("backend") not in ("3dgs", "2dgs") or
            type(payload.get("iteration")) is not int or
            not 0 <= payload["iteration"] <= 2 ** 53 - 1 or
            payload.get("state") not in ("admitted", "deferred", "bypassed") or
            payload.get("reason") not in GROWTH_REASONS):
        return None
    if ((payload["state"] == "admitted") != (payload["reason"] == "admitted") or
            (payload['state'] == 'bypassed') != (payload['reason'] == 'guard_disabled')):
        return None
    result = {key: payload[key] for key in ("version", "backend", "iteration", "state", "reason")}
    for key in GROWTH_INTEGER_FIELDS:
        value = payload.get(key)
        if type(value) is not int or not 0 <= value <= 2 ** 53 - 1:
            return None
        result[key] = value
    for key in GROWTH_BOOLEAN_FIELDS:
        if key in payload:
            if type(payload[key]) is not bool:
                return None
            result[key] = payload[key]
    if 'device_free_before_reclaim_bytes' in payload:
        value = payload['device_free_before_reclaim_bytes']
        if type(value) is not int or not 0 <= value <= 2 ** 53 - 1:
            return None
        result['device_free_before_reclaim_bytes'] = value
    if 'cache_reclaim_status' in payload:
        if payload['cache_reclaim_status'] not in ('not_attempted', 'reclaimed_remeasured'):
            return None
        result['cache_reclaim_status'] = payload['cache_reclaim_status']
    for key in ('torch_allocated_bytes', 'torch_reserved_bytes', 'torch_active_bytes',
                'torch_inactive_split_bytes'):
        if key in payload:
            value = payload[key]
            if value is not None and (type(value) is not int or not 0 <= value <= 2 ** 53 - 1):
                return None
            result[key] = value
    if 'allocator_observation_status' in payload:
        if payload['allocator_observation_status'] not in ('unreported', 'known', 'partial', 'invalid', 'unavailable'):
            return None
        result['allocator_observation_status'] = payload['allocator_observation_status']
    if ("allowed" in result and result["allowed"] != (result["state"] in ("admitted", "bypassed"))):
        return None
    if result["state"] == "admitted" and result.get("observation_valid") is False:
        return None
    if result['state'] == 'bypassed' and (result.get('guard_enabled') is not False or
            result.get('observation_valid') is not False or any(result[key] for key in GROWTH_INTEGER_FIELDS)):
        return None
    if result.get("observation_valid") and (
            result["total_bytes"] <= 0 or result["free_bytes"] > result["total_bytes"] or
            result["cache_credit_bytes"] > result["total_bytes"] - result["free_bytes"]):
        return None
    return result


def is_cuda_out_of_memory(error):
    """Recognize the external Torch error without importing or starting CUDA."""
    return (any(kind.__name__ == 'OutOfMemoryError' and kind.__module__.startswith('torch')
                for kind in type(error).__mro__) or
            isinstance(error, RuntimeError) and str(error).lstrip().startswith((
                'CUDA out of memory', 'CUDA error: out of memory',
                'CUDA runtime error (2) : out of memory')))


def _tensor_metadata(tensor):
    """Torch tensors and NumPy snapshots share shape/byte metadata only."""
    shape = tuple(int(value) for value in tensor.shape)
    count = tensor.numel() if callable(getattr(tensor, "numel", None)) else tensor.size
    itemsize = (tensor.element_size() if callable(getattr(tensor, "element_size", None))
                else tensor.dtype.itemsize)
    return shape, int(count) * int(itemsize), int(itemsize)


def _estimate(model, backend, clone_count, split_count):
    if backend not in ("3dgs", "2dgs"):
        raise ValueError("Unsupported growth backend")
    optimizer = model.optimizer
    groups = {group["name"]: group for group in optimizer.param_groups}
    if (len(optimizer.param_groups) != len(_GROUP_NAMES) or
            set(groups) != set(_GROUP_NAMES) or
            any(len(groups[name]["params"]) != 1 for name in _GROUP_NAMES)):
        raise ValueError("Unknown Gaussian optimizer layout")
    xyz_shape, _, _ = _tensor_metadata(groups["xyz"]["params"][0])
    if len(xyz_shape) != 2 or xyz_shape[1] != 3:
        raise ValueError("Invalid Gaussian positions")
    points = xyz_shape[0]
    if clone_count is None and split_count is None:
        clone_count, split_count = 0, points
    if (type(clone_count) is not int or type(split_count) is not int or
            clone_count < 0 or split_count < 0 or clone_count + split_count > points):
        raise ValueError("Invalid growth candidate counts")
    parameter_bytes = moment_bytes = gradient_bytes = 0
    largest_element = 4
    for name in _GROUP_NAMES:
        parameter = groups[name]["params"][0]
        shape, size, itemsize = _tensor_metadata(parameter)
        if not shape or shape[0] != points:
            raise ValueError("Gaussian parameter row counts disagree")
        if name == "scaling" and (len(shape) != 2 or
                shape[1] != (3 if backend == "3dgs" else 2)):
            raise ValueError("Gaussian scale dimensions disagree with backend")
        parameter_bytes += size
        largest_element = max(largest_element, itemsize)
        gradient = getattr(parameter, "grad", None)
        if gradient is not None:
            gradient_shape, gradient_size, gradient_itemsize = _tensor_metadata(gradient)
            if gradient_shape != shape:
                raise ValueError("Gaussian gradient shape disagrees")
            gradient_bytes += gradient_size
            largest_element = max(largest_element, gradient_itemsize)
        try:
            state = optimizer.state.get(parameter, {})
        except TypeError:  # Portable, name-indexed NumPy snapshots.
            state = optimizer.state.get(name, {})
        if "exp_avg" in state and "exp_avg_sq" in state:
            for value in state.values():
                if hasattr(value, "shape"):
                    state_shape, state_size, state_itemsize = _tensor_metadata(value)
                    if state_shape:  # Exclude scalar CPU step counters.
                        moment_bytes += state_size
                        largest_element = max(largest_element, state_itemsize)
        else:
            # Lazy Adam state must be admitted before its first allocation.
            moment_bytes += 2 * size
    statistic_bytes = 0
    for name in ("xyz_gradient_accum", "denom", "max_radii2D", "tmp_radii"):
        tensor = getattr(model, name, None)
        if tensor is not None:
            shape, size, itemsize = _tensor_metadata(tensor)
            if not shape or shape[0] != points:
                raise ValueError("Density statistic row counts disagree")
            statistic_bytes += size
            largest_element = max(largest_element, itemsize)
    # Opt-in quality adapters expose their real per-row buffers. Old models
    # have no such attribute and retain the existing estimate exactly.
    for tensor in getattr(model, '_gsw_quality_buffers', ()):
        shape, size, itemsize = _tensor_metadata(tensor)
        if not shape or shape[0] != points:
            raise ValueError('Quality statistic row counts disagree')
        statistic_bytes += size
        largest_element = max(largest_element, itemsize)
    divisor = max(points, 1)
    parameter_per_point = parameter_bytes // divisor
    optimizer_per_point = (moment_bytes + divisor - 1) // divisor
    gradient_per_point = max(parameter_per_point,
                             (gradient_bytes + divisor - 1) // divisor)
    statistic_per_point = max(16, (statistic_bytes + divisor - 1) // divisor)
    new_rows = clone_count + 2 * split_count
    transient = points + new_rows
    final = points + clone_count + split_count
    # Old tensors coexist with complete concatenation replacements. Splitting
    # then prunes parents, creating another full parameter/moment replacement.
    # Do not estimate only net new rows, or discount storage not yet released.
    replacement_rows = max(transient, new_rows + final)
    replacement_bytes = (parameter_per_point + optimizer_per_point) * replacement_rows
    candidate_bytes = parameter_per_point * new_rows
    statistic_peak = statistic_per_point * (transient + final)
    scratch = ((8 * points + 16 * new_rows + 9 * split_count) * largest_element +
               16 * (points + transient))
    growth_peak = replacement_bytes + candidate_bytes + statistic_peak + scratch
    # Account for a future full gradient even if zero_grad(set_to_none=True)
    # has removed the current one. Rendering/driver headroom is separate.
    future_peak = ((clone_count + split_count) *
                   (parameter_per_point + optimizer_per_point + statistic_per_point) +
                   max(0, final * gradient_per_point - gradient_bytes))
    storage = getattr(model, '_gsw_capacity_storage', None)
    capacity_peak = 0
    if storage is not None:
        capacity_peak = storage.estimate_growth(clone_count, split_count)['capacity_additional_peak_bytes']
        if type(capacity_peak) is not int or capacity_peak < 0:
            raise ValueError('Invalid capacity storage estimate')
    return dict(point_count=points, transient_point_count=transient,
                final_point_count=final, parameter_bytes=parameter_bytes,
                optimizer_state_bytes=moment_bytes,
                additional_peak_bytes=max(growth_peak, future_peak) + capacity_peak)


class GrowthAdmission:
    """Estimate additional live storage before permitting native density work.

    Supply a callable returning (free, total) as CUDA mem_get_info does, or a
    MemoryObservation with independently classified reusable cache. Default
    headroom retains 10% of device memory, at least 512 MiB. No point-count cap,
    pruning, allocator clearing, optimizer update or RNG operation occurs here.

    Exact clone/split counts use observed parameter/Adam-state dtypes. Without
    counts, all original rows are conservatively treated as two-child splits.
    The estimate includes complete cat/prune replacements, generated rows,
    statistics and bounded scratch, not merely final added parameters. It is
    a conservative model, not a proof against rasterizer geometry-dependent
    scratch, future external allocations or allocator fragmentation. Scalar
    optimizer step counters are excluded; lazy Adam moments are anticipated.
    """
    VERSION = 1
    CACHE_CREDIT_FRACTION = .5

    def __init__(self, memory_probe, usage_fraction=.90,
                 minimum_spare_bytes=512 * 1024 ** 2, cache_reclaimer=None):
        if (not callable(memory_probe) or isinstance(usage_fraction, bool) or
                not isinstance(usage_fraction, numbers.Real) or
                not math.isfinite(usage_fraction) or not 0 < usage_fraction <= 1 or
                isinstance(minimum_spare_bytes, bool) or
                not isinstance(minimum_spare_bytes, numbers.Integral) or
                not 0 <= minimum_spare_bytes <= 2 ** 63 - 1 or
                cache_reclaimer is not None and not callable(cache_reclaimer)):
            raise ValueError("Invalid growth-admission configuration")
        self.memory_probe = memory_probe
        self.usage_fraction = float(usage_fraction)
        self.minimum_spare_bytes = int(minimum_spare_bytes)
        self.cache_reclaimer = cache_reclaimer

    def evaluate(self, model, backend, clone_count=None, split_count=None):
        decision = self._evaluate_once(model, backend, clone_count, split_count)
        if (decision.reason == 'vram_pressure' and decision.observation_valid
                and decision.cache_credit_bytes > 0 and self.cache_reclaimer is not None):
            # Only release independently classified inactive cache, never live
            # model/Adam tensors. A subsequent measurement, not the credit,
            # must satisfy the actual-free reserve. Real CUDA failures escape.
            self.cache_reclaimer()
            measured = self._evaluate_once(model, backend, clone_count, split_count)
            return replace(measured, cache_reclaim_attempted=True,
                           device_free_before_reclaim_bytes=decision.free_bytes,
                           cache_reclaim_status='reclaimed_remeasured')
        return decision

    def _evaluate_once(self, model, backend, clone_count, split_count):
        try:
            observation = self.memory_probe()
        except Exception as error:
            if is_cuda_out_of_memory(error):
                raise
            return GrowthDecision(False, "probe_unavailable")
        metadata = {}
        if isinstance(observation, MemoryObservation):
            free, total, reusable = (observation.free_bytes,
                                     observation.total_bytes,
                                     observation.reusable_cache_bytes)
            metadata = {target: getattr(observation, source) for source, target in (
                ('allocated_bytes', 'torch_allocated_bytes'), ('reserved_bytes', 'torch_reserved_bytes'),
                ('active_bytes', 'torch_active_bytes'), ('inactive_split_bytes', 'torch_inactive_split_bytes'))}
            metadata['allocator_observation_status'] = observation.allocator_status
        elif isinstance(observation, (tuple, list)) and len(observation) == 2:
            free, total = observation
            reusable = 0
        else:
            return GrowthDecision(False, "invalid_observation")
        if (any(isinstance(value, bool) or not isinstance(value, numbers.Integral)
                for value in (free, total, reusable)) or
                not 0 <= free <= total or not 0 < total <= 2 ** 63 - 1 or
                not 0 <= reusable <= total - free):
            return GrowthDecision(False, "invalid_observation")
        free, total = int(free), int(total)
        cache_credit = int(reusable * self.CACHE_CREDIT_FRACTION)
        spare = max(self.minimum_spare_bytes,
                    total - math.floor(total * self.usage_fraction))
        available = max(0, free + cache_credit - spare)
        try:
            estimate = _estimate(model, backend, clone_count, split_count)
        except Exception as error:
            if is_cuda_out_of_memory(error):
                raise
            return GrowthDecision(False, "estimate_unavailable",
                                  free_bytes=free, total_bytes=total,
                                  spare_bytes=spare, available_bytes=available,
                                  observation_valid=True,
                                  cache_credit_bytes=cache_credit, **metadata)
        allowed = (free >= self.minimum_spare_bytes and
                   estimate["additional_peak_bytes"] <= available)
        return GrowthDecision(allowed,
                              "admitted" if allowed else "vram_pressure",
                              free_bytes=free, total_bytes=total,
                              spare_bytes=spare, available_bytes=available,
                              observation_valid=True,
                              cache_credit_bytes=cache_credit,
                               **dict(estimate, **metadata))


def admit_densification(torch, model, backend, threshold, extent, policy,
                        emit=None, iteration=0, clone_grads=None, split_grads=None,
                        split_threshold=None):
    """Return permission only; the caller retains its existing density routine.

    None explicitly preserves the legacy full-state-resume execution route.
    The preflight uses metadata only, before candidate Tensor operations.
    """
    if policy is None:
        if emit is not None:
            event = asdict(GrowthDecision(True, 'guard_disabled'))
            event.update(version=GrowthAdmission.VERSION, backend=backend, iteration=iteration,
                         state='bypassed', clone_count=0, split_count=0,
                         candidate_counts_known=False, guard_enabled=False)
            try:
                emit('[gsw-training-growth]', event)
            except Exception:
                pass
        return True
    decision = policy.evaluate(model, backend, clone_count=0, split_count=0)
    first_decision = decision
    clone_count = split_count = 0
    candidates_known = False
    if decision.allowed:
        try:
            if (isinstance(threshold, bool) or not isinstance(threshold, numbers.Real) or
                    not math.isfinite(threshold) or threshold < 0 or
                    isinstance(extent, bool) or not isinstance(extent, numbers.Real) or
                    not math.isfinite(extent) or extent <= 0 or
                    not math.isfinite(model.percent_dense) or model.percent_dense < 0):
                raise ValueError("Invalid density candidate inputs")
            with torch.no_grad():
                # Operate only on a newly allocated ratio, never the saved
                # accumulators/denominators. No random sampling is performed.
                grads = (model.xyz_gradient_accum / model.denom).detach()
                if len(grads.shape) != 2 or grads.shape[1] != 1:
                    raise ValueError("Unsupported density gradient layout")
                grads[~torch.isfinite(grads)] = 0.
                clone = grads if clone_grads is None else clone_grads.detach()
                split = grads if split_grads is None else split_grads.detach()
                split_limit = threshold if split_threshold is None else split_threshold
                if (clone.shape != grads.shape or split.shape != grads.shape
                        or not bool(torch.isfinite(clone).all() and torch.isfinite(split).all())
                        or isinstance(split_limit, bool) or not isinstance(split_limit, numbers.Real)
                        or not math.isfinite(split_limit) or split_limit < 0):
                    raise ValueError('Invalid quality density candidates')
                scales = model.get_scaling
                max_scale = scales.max(dim=1).values
                cutoff = model.percent_dense * extent
                # The upstream clone rule uses the norm; its split rule uses
                # the signed padded gradient. Do not silently replace either.
                clone_count = int(((torch.norm(clone, dim=-1) >= threshold) &
                                   (max_scale <= cutoff)).sum().item())
                split_count = int(((split.squeeze(-1) >= split_limit) &
                                   (max_scale > cutoff)).sum().item())
                del grads, scales, max_scale
            candidates_known = True
            decision = policy.evaluate(model, backend, clone_count=clone_count,
                                       split_count=split_count)
            if first_decision.cache_reclaim_attempted and not decision.cache_reclaim_attempted:
                decision = replace(decision, cache_reclaim_attempted=True,
                                   device_free_before_reclaim_bytes=first_decision.device_free_before_reclaim_bytes,
                                   cache_reclaim_status=first_decision.cache_reclaim_status)
        except Exception as error:
            if is_cuda_out_of_memory(error):
                raise
            decision = replace(decision, allowed=False, reason="estimate_unavailable")
    if emit is not None:
        event = asdict(decision)
        event.update(version=GrowthAdmission.VERSION, backend=backend,
                     iteration=iteration,
                     state="admitted" if decision.allowed else "deferred",
                     clone_count=clone_count, split_count=split_count,
                     candidate_counts_known=candidates_known, guard_enabled=True)
        try:
            emit("[gsw-training-growth]", event)
        except Exception:
            pass  # A monitoring failure must not abort optimization.
    return decision.allowed
