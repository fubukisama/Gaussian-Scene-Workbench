"""Versioned effective parameters and *loaded* training-camera dimensions.

No torch, CUDA, image decoding or directory enumeration occurs here. Dimensions
come only from the cameras actually loaded by the two trainers, not input-file
counts or a preview. This contract does not estimate peak GPU memory.
"""
import math
from collections import Counter

SUMMARY_VERSION = 1
INPUT_EVENT_PREFIX = "[gsw-training-input]"
DIMENSION_LIMIT = 8
INT32_MAX = 2147483647
INT64_MAX = 9223372036854775807
QUALITY_IDS = frozenset(("quick", "full", "quality", "max_quality", "original_quality"))
PARAMETER_FIELDS = (
    "version", "backend", "quality", "iterations", "resolution", "optimizer",
    "densifyUntil", "densificationInterval", "densifyGradient",
    "antialiasing", "exposureCompensation", "depthRatio",
)


def _integer(value, minimum=0, maximum=INT32_MAX):
    return type(value) is int and minimum <= value <= maximum


def _real(value, minimum=0.0, maximum=1.0):
    return (type(value) in (int, float) and minimum <= value <= maximum
            and math.isfinite(value))


def normalize_training_summary(payload):
    """Return a fresh safe canonical dict, or None for an invalid contract.

    Required fields fail closed rather than being silently coerced. Unknown
    fields are dropped. Calling this on its own result is idempotent.
    """
    if not isinstance(payload, dict):
        return None
    backend = payload.get("backend")
    phase = payload.get("phase")
    iterations = payload.get("iterations")
    if (type(payload.get("version")) is not int or payload["version"] != SUMMARY_VERSION
            or backend not in ("3dgs", "2dgs") or phase not in ("configured", "loaded")
            or not isinstance(payload.get("quality"), str) or payload["quality"] not in QUALITY_IDS
            or not _integer(iterations, 1, 200000)
            or not _integer(payload.get("resolution"), 1, 16)
            or not _integer(payload.get("densifyUntil"), 0, iterations)
            or not _integer(payload.get("densificationInterval"), 1)
            or not _real(payload.get("densifyGradient"), 0.000000000001, 1.0)):
        return None
    common = ("version", "backend", "quality", "iterations", "resolution", "optimizer",
              "densifyUntil", "densificationInterval", "densifyGradient")
    if "optimizer" not in payload:
        return None
    result = {name: payload[name] for name in common}
    result["phase"] = phase
    if backend == "3dgs":
        if (payload.get("optimizer") not in ("default", "sparse_adam")
                or type(payload.get("antialiasing")) is not bool
                or type(payload.get("exposureCompensation")) is not bool):
            return None
        result.update(antialiasing=payload["antialiasing"],
                      exposureCompensation=payload["exposureCompensation"])
    else:
        if payload.get("optimizer") != "adam" or not _real(payload.get("depthRatio")):
            return None
        result["depthRatio"] = payload["depthRatio"]
    if phase == "configured":
        return result

    count = payload.get("trainImageCount")
    kinds = payload.get("trainDimensionKinds")
    pixels = payload.get("trainPixels")
    dimensions = payload.get("trainDimensions")
    if (not _integer(count, 1) or not _integer(kinds, 1, count)
            or not _integer(pixels, 1, INT64_MAX) or not isinstance(dimensions, list)
            or len(dimensions) != min(kinds, DIMENSION_LIMIT)):
        return None
    canonical = []
    seen = set()
    listed_count = 0
    listed_pixels = 0
    for item in dimensions:
        if (not isinstance(item, (list, tuple)) or len(item) != 3
                or not all(_integer(value, 1) for value in item)
                or (item[0], item[1]) in seen):
            return None
        width, height, dimension_count = item
        seen.add((width, height))
        listed_count += dimension_count
        listed_pixels += width * height * dimension_count
        canonical.append([width, height, dimension_count])
    if kinds <= DIMENSION_LIMIT:
        if listed_count != count or listed_pixels != pixels:
            return None
    elif listed_count >= count or listed_pixels >= pixels or count - listed_count < kinds - DIMENSION_LIMIT:
        return None
    result.update(trainImageCount=count, trainDimensionKinds=kinds,
                  trainPixels=pixels, trainDimensions=sorted(canonical))
    return result


def configured_training_summary(backend, quality, options):
    """Build from resolved effective options, never an unadapted preset."""
    if not isinstance(options, dict):
        return None
    result = {"version": SUMMARY_VERSION, "phase": "configured", "backend": backend,
              "quality": quality, "iterations": options.get("iterations"),
              "resolution": options.get("resolution"),
              "optimizer": options.get("optimizer_type") if backend == "3dgs" else "adam",
              "densifyUntil": options.get("densify_until_iter"),
              "densificationInterval": options.get("densification_interval"),
              "densifyGradient": options.get("densify_grad_threshold")}
    if backend == "3dgs":
        result.update(antialiasing=options.get("antialiasing"),
                      exposureCompensation=options.get("exposure_compensation"))
    elif backend == "2dgs":
        result["depthRatio"] = options.get("depth_ratio")
    return normalize_training_summary(result)


def loaded_training_summary(configured, cameras):
    """Summarize camera image_width/image_height without touching their tensors."""
    result = normalize_training_summary(configured)
    if result is None:
        return None
    dimensions = Counter()
    count = 0
    pixels = 0
    for camera in cameras:
        width = getattr(camera, "image_width", None)
        height = getattr(camera, "image_height", None)
        if not _integer(width, 1) or not _integer(height, 1):
            return None
        dimensions[(width, height)] += 1
        count += 1
        pixels += width * height
        if count > INT32_MAX or pixels > INT64_MAX:
            return None
    result.update(phase="loaded", trainImageCount=count, trainPixels=pixels,
                  trainDimensionKinds=len(dimensions),
                  trainDimensions=[[width, height, dimensions[(width, height)]]
                                   for width, height in sorted(dimensions)[:DIMENSION_LIMIT]])
    return normalize_training_summary(result)


def emit_loaded_training_summary(emit, control, cameras):
    """Optional additive telemetry; old native controls keep their behavior."""
    if not isinstance(control, dict):
        return None
    summary = loaded_training_summary(control.get("trainingSummary"), cameras)
    if summary is not None:
        emit(INPUT_EVENT_PREFIX, summary)
    return summary
