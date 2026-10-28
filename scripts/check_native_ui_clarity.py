# SPDX-License-Identifier: MIT
"""Independent local-ROI navigation pixel oracle for remote CI captures.

The reference is a deterministic32x32 subpixel area sample, not QPainter,
Blender drawing code or the analytic fixture generator. It evaluates raw RGB8
PNG bytes against declared logical geometry and RGBA. It never changes a PNG,
fits a background color, weakens thresholds or supersamples the whole frame.

Reference geometry: centered stroke, visible outer radius R+S/2. Edge pixels
are within1physical px of either boundary. Halo pixels are more than1.25 and
at most4physical px beyond the visible outer boundary. Other discs/labels and
all shafts are excluded conservatively. Empty edge or halo evidence fails.

Opaque declared backdrop and SourceOver painting are the currently supported
color contract. Other composition/background states fail explicitly instead of
manufacturing pixel acceptance. This script does not attest a real window,
hardware DPR, glyph quality, overall model rendering or full UI acceptance.
"""
from __future__ import annotations

import argparse
from functools import lru_cache
import json
import math
from pathlib import Path
import sys

from PIL import Image


SAMPLES = 32
LIMITS = {"normalized_rgb_rmse": .06, "p95_rgb_error": .16, "outer_halo_max": .04}
REFERENCE_MEMORY_LIMIT = 16 * 1024 * 1024
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_FRAME_PIXELS = 64 * 1024 * 1024
MAX_RING_ROI_PIXELS = 65536
MAX_RING_ROI_SIDE = 512
EDGE_HALF_BAND = 1.0
HALO_START = 1.25
HALO_END = 4.0
OCCLUDER_PAD = 1.75


class CaptureError(ValueError):
    def __init__(self, code, detail=""):
        super().__init__(code)
        self.code, self.detail = code, str(detail)


def _number(value, field, *, positive=False, nonnegative=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CaptureError("invalid_numeric_field", field)
    try:
        value = float(value)
    except (OverflowError, ValueError):
        raise CaptureError("invalid_numeric_field", field)
    if not math.isfinite(value) or positive and value <= 0 or nonnegative and value < 0:
        raise CaptureError("invalid_numeric_field", field)
    return value


def _point(value, field):
    if not isinstance(value, (list, tuple)) or len(value) != 2:
        raise CaptureError("invalid_point", field)
    return tuple(_number(item, field) for item in value)


def _rgba(value, field):
    if not isinstance(value, (list, tuple)) or len(value) != 4:
        raise CaptureError("invalid_rgba", field)
    result = tuple(_number(item, field) for item in value)
    if any(item < 0 or item > 255 or not item.is_integer() for item in result):
        raise CaptureError("invalid_rgba", field)
    return result


@lru_cache(maxsize=4)
def _offsets(samples):
    return tuple((index + .5) / samples for index in range(samples))


def pixel_disc_coverage(center, radius, pixelOrigin, samples=32):
    """Fraction of a unit physical pixel covered by a disc, via area samples."""
    cx, cy = _point(center, "center")
    x, y = _point(pixelOrigin, "pixelOrigin")
    radius = _number(radius, "radius", nonnegative=True)
    if isinstance(samples, bool) or not isinstance(samples, int) or not 1 <= samples <= 128:
        raise CaptureError("invalid_sample_count", "samples must be an integer in1..128")
    if radius == 0:
        return 0.0
    # Work relative to the pixel, preserving translated-coordinate agreement.
    cx, cy = cx - x, cy - y
    if max(math.hypot(px - cx, py - cy) for px in (0., 1.) for py in (0., 1.)) <= radius:
        return 1.0
    nearest_x, nearest_y = min(1., max(0., cx)), min(1., max(0., cy))
    if math.hypot(nearest_x - cx, nearest_y - cy) >= radius:
        return 0.0
    covered = 0
    offsets = _offsets(samples)
    for py in offsets:
        for px in offsets:
            covered += math.hypot(px - cx, py - cy) <= radius
    return covered / float(samples * samples)


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise CaptureError("duplicate_json_key", key)
        result[key] = value
    return result


def _constant(value):
    raise CaptureError("nonfinite_json_constant", value)


def _read_manifest(path):
    if not path.is_file():
        raise CaptureError("capture_manifest_missing", path)
    if path.stat().st_size > MAX_MANIFEST_BYTES:
        raise CaptureError("capture_manifest_too_large", path)
    with path.open(encoding="utf-8-sig") as source:
        result = json.load(source, object_pairs_hook=_pairs, parse_constant=_constant)
    if not isinstance(result, dict):
        raise CaptureError("capture_manifest_not_object")
    return result


def _one_record(rows, name, kind):
    if not isinstance(rows, list):
        raise CaptureError(kind + "_missing", name)
    matches = [row for row in rows if isinstance(row, dict) and row.get("name") == name]
    if len(matches) != 1:
        raise CaptureError(kind + "_not_unique", name)
    return matches[0]


def _handles(value, dpr, opacity):
    if not isinstance(value, list) or not value or len(value) > 32:
        raise CaptureError("invalid_navigation_handles")
    result = []
    for index, source in enumerate(value):
        if not isinstance(source, dict):
            raise CaptureError("invalid_navigation_handle", index)
        for key in ("positive", "aligned", "highlighted"):
            if type(source.get(key)) is not bool:
                raise CaptureError("invalid_handle_flag", str(index) + ":" + key)
        axis = source.get("axisIndex")
        if type(axis) is not int or not 0 <= axis <= 2:
            raise CaptureError("invalid_axis_index", index)
        center = _point(source.get("center"), "handle.center")
        radius = _number(source.get("radius"), "handle.radius", positive=True) * dpr
        stroke = _number(source.get("strokeWidth"), "handle.strokeWidth", positive=True) * dpr
        outline, fill = _rgba(source.get("outline"), "handle.outline"), _rgba(source.get("fill"), "handle.fill")
        shaft = source.get("shaft")
        if shaft is not None:
            if not isinstance(shaft, dict):
                raise CaptureError("invalid_shaft", index)
            first, last = _point(shaft.get("from"), "shaft.from"), _point(shaft.get("to"), "shaft.to")
            shaft = {"from": tuple(v * dpr for v in first), "to": tuple(v * dpr for v in last),
                     "width": _number(shaft.get("width"), "shaft.width", nonnegative=True) * dpr}
        result.append({"index": index, "axisIndex": axis,
                       "positive": source["positive"], "aligned": source["aligned"],
                       "highlighted": source["highlighted"],
                       "center": tuple(v * dpr for v in center), "radius": radius,
                       "stroke": stroke, "outer": radius + stroke * .5,
                       "inner": max(0., radius - stroke * .5),
                       "outline": (*outline[:3], outline[3] * opacity),
                       "fill": (*fill[:3], fill[3] * opacity), "shaft": shaft})
    return result


def _segment_distance(point, first, last):
    dx, dy = last[0] - first[0], last[1] - first[1]
    length_squared = dx * dx + dy * dy
    if length_squared <= 0:
        return math.hypot(point[0] - first[0], point[1] - first[1])
    along = min(1., max(0., ((point[0] - first[0]) * dx + (point[1] - first[1]) * dy) / length_squared))
    return math.hypot(point[0] - first[0] - along * dx, point[1] - first[1] - along * dy)


def _excluded(point, handle, all_handles):
    for other in all_handles:
        if other["index"] != handle["index"] and math.hypot(
                point[0] - other["center"][0], point[1] - other["center"][1]) <= other["outer"] + OCCLUDER_PAD:
            # Includes the other disc's complete label rectangle/AA margin.
            return True
        shaft = other["shaft"]
        if shaft and _segment_distance(point, shaft["from"], shaft["to"]) <= shaft["width"] * .5 + OCCLUDER_PAD:
            return True
    return False


def _percentile95(values):
    ordered = sorted(values)
    position = .95 * (len(ordered) - 1)
    left = int(math.floor(position))
    right = min(len(ordered) - 1, left + 1)
    return ordered[left] + (ordered[right] - ordered[left]) * (position - left)


def _reference_color(handle, backdrop, x, y):
    outer = pixel_disc_coverage(handle["center"], handle["outer"], (x, y), SAMPLES)
    inner = pixel_disc_coverage(handle["center"], handle["inner"], (x, y), SAMPLES)
    border = max(0., outer - inner)
    outline, fill = handle["outline"], handle["fill"]
    outline_alpha, fill_alpha = outline[3] / 255., fill[3] / 255.
    result = []
    for channel in range(3):
        outline_rgb = outline[channel] * outline_alpha + backdrop[channel] * (1 - outline_alpha)
        fill_rgb = fill[channel] * fill_alpha + backdrop[channel] * (1 - fill_alpha)
        result.append(backdrop[channel] * (1 - outer) + outline_rgb * border + fill_rgb * inner)
    return result


def _measure_ring(image, handle, all_handles, backdrop):
    cx, cy = handle["center"]
    outer, inner = handle["outer"], handle["inner"]
    if not all(math.isfinite(v) for v in (cx, cy, outer, inner)):
        raise CaptureError("physical_geometry_nonfinite", handle["index"])
    outline_alpha = handle["outline"][3] / 255.
    if outline_alpha <= 0 or max(abs(handle["outline"][c] - backdrop[c]) * outline_alpha for c in range(3)) < 1.:
        raise CaptureError("ring_reference_has_no_visible_outline", handle["index"])
    # Reject oversized/reference-unbounded evidence before allocating a tile.
    if 2 * (outer + HALO_END + 1) > MAX_RING_ROI_SIDE:
        raise CaptureError("reference_roi_too_large", handle["index"])
    left, top = max(0, math.floor(cx - outer - HALO_END - 1)), max(0, math.floor(cy - outer - HALO_END - 1))
    right, bottom = min(image.width, math.ceil(cx + outer + HALO_END + 1)), min(image.height, math.ceil(cy + outer + HALO_END + 1))
    width, height = right - left, bottom - top
    if width <= 0 or height <= 0:
        raise CaptureError("ring_outside_frame", handle["index"])
    if width > MAX_RING_ROI_SIDE or height > MAX_RING_ROI_SIDE:
        raise CaptureError("reference_roi_too_large", handle["index"])
    area = width * height
    # Conservative design bound for RGB/RGBA tile, error floats/sorted copy,
    # temporary scalars and offset cache; decoded input PNG is separately noted.
    reference_bound = area * 112 + 65536
    if area > MAX_RING_ROI_PIXELS or reference_bound > REFERENCE_MEMORY_LIMIT:
        raise CaptureError("reference_memory_budget_exceeded", handle["index"])
    tile = image.crop((left, top, right, bottom)).convert("RGB")
    pixels = tile.load()
    squared_error, errors, excluded, halo_count, halo_max = 0., [], 0, 0, 0.
    halo_rgb_sum, backdrop_max_delta = [0., 0., 0.], 0.
    try:
        for y in range(top, bottom):
            for x in range(left, right):
                point = (x + .5, y + .5)
                distance = math.hypot(point[0] - cx, point[1] - cy)
                edge = abs(distance - outer) <= EDGE_HALF_BAND or inner > 0 and abs(distance - inner) <= EDGE_HALF_BAND
                halo = outer + HALO_START < distance <= outer + HALO_END
                if not edge and not halo:
                    continue
                if _excluded(point, handle, all_handles):
                    excluded += 1
                    continue
                actual = pixels[x - left, y - top]
                if edge:
                    expected = _reference_color(handle, backdrop, x, y)
                    pixel_squared = sum(((actual[c] - expected[c]) / 255.) ** 2 for c in range(3))
                    squared_error += pixel_squared
                    errors.append(math.sqrt(pixel_squared / 3.))
                if halo:
                    delta = max(abs(actual[c] - backdrop[c]) / 255. for c in range(3))
                    halo_max = max(halo_max, delta)
                    backdrop_max_delta = max(backdrop_max_delta, delta)
                    halo_count += 1
                    for channel in range(3):
                        halo_rgb_sum[channel] += actual[channel]
    finally:
        tile.close()
    record = {"handle_index": handle["index"], "axisIndex": handle["axisIndex"],
              "physical_center": list(handle["center"]), "physical_path_radius": handle["radius"],
              "physical_stroke_width": handle["stroke"], "physical_visible_outer_radius": outer,
              "physical_roi": [left, top, width, height], "edge_pixels": len(errors),
              "halo_pixels": halo_count, "excluded_pixels": excluded,
              "reference_peak_estimated_bytes": reference_bound, "measured": bool(errors and halo_count)}
    if not errors or not halo_count:
        record["error"] = "ring_empty_edge_roi" if not errors else "ring_empty_halo_roi"
        return record
    record.update(normalized_rgb_rmse=math.sqrt(squared_error / (3 * len(errors))),
                  p95_rgb_error=_percentile95(errors), outer_halo_max=halo_max,
                  declared_backdrop_rgb=list(backdrop[:3]),
                  observed_halo_mean_rgb=[v / halo_count for v in halo_rgb_sum],
                  observed_halo_backdrop_max_delta=backdrop_max_delta)
    record["passed"] = all(record[metric] <= limit for metric, limit in LIMITS.items())
    return record


def _error_record(error):
    if isinstance(error, CaptureError):
        return {"code": error.code, "detail": error.detail}
    return {"code": "capture_read_or_decode_failed", "detail": str(error)[:1000]}


def check_capture(manifest_path):
    """Return a fail-closed JSON-compatible report; do not modify evidence."""
    path = Path(manifest_path)
    report = {"schema": "gsw.navigation_pixel_capture_check/1", "manifest": str(path),
              "passed": False, "errors": [], "rings": [], "thresholds": dict(LIMITS),
              "attests_real_window_or_hardware_dpi": False,
              "sample_grid": [SAMPLES, SAMPLES], "reference_memory_limit_bytes": REFERENCE_MEMORY_LIMIT,
              "metric_definitions": {
                  "normalized_rgb_rmse": "sqrt(mean squared normalized RGB8 channel error) over local unoccluded boundary bands",
                  "p95_rgb_error": "linearly interpolated95th percentile of per-pixel normalized RGB RMS over the same edge ROI",
                  "outer_halo_max": "maximum normalized RGB channel difference from declared backdrop beyond(R+S/2)+1.25 up to+4 physical px; no fitted colors"}}
    try:
        manifest = _read_manifest(path)
        if "status" in manifest and manifest["status"] != "complete":
            raise CaptureError("capture_not_complete", manifest["status"])
        if "stageSerialMatch" in manifest and manifest["stageSerialMatch"] is not True:
            raise CaptureError("capture_frame_serial_mismatch")
        dpr = _number(manifest.get("widgetDpr"), "widgetDpr", positive=True)
        logical = _point(manifest.get("logicalWidgetSize"), "logicalWidgetSize")
        if any(v <= 0 or not v.is_integer() for v in logical):
            raise CaptureError("invalid_logical_widget_size")
        stage = _one_record(manifest.get("stages"), "after-painter", "capture_stage")
        if "colorValid" in stage and stage["colorValid"] is not True:
            raise CaptureError("capture_stage_color_invalid")
        note = _one_record(manifest.get("notes"), "navigation-presentation", "navigation_note")
        meta = note.get("meta")
        if not isinstance(meta, dict):
            raise CaptureError("navigation_metadata_missing")
        paint = meta.get("paint", {})
        if not isinstance(paint, dict):
            raise CaptureError("paint_metadata_invalid")
        for key in ("dpr", "devicePixelRatio"):
            if key in paint and not math.isclose(_number(paint[key], "paint." + key, positive=True), dpr, rel_tol=1e-9, abs_tol=1e-9):
                raise CaptureError("paint_widget_dpr_mismatch", key)
        opacity = _number(paint.get("opacity", 1.), "paint.opacity", positive=True)
        if opacity > 1:
            raise CaptureError("paint_opacity_out_of_range")
        if "compositionMode" in paint and paint["compositionMode"] != 0:
            raise CaptureError("paint_composition_unsupported", "Only SourceOver is modeled")
        if meta.get("rotationBackdrop", False):
            raise CaptureError("rotation_backdrop_unsupported", "Hover backdrop needs a separately declared coverage/color contract")
        backdrop = _rgba(meta.get("backdrop"), "backdrop")
        if backdrop[3] != 255:
            raise CaptureError("backdrop_alpha_unsupported", "Opaque raw-frame RGB oracle; transparent compositor acceptance remains separate")
        handles = _handles(meta.get("handles"), dpr, opacity)
        eligible = [h for h in handles if not h["positive"] and not h["aligned"] and not h["highlighted"]]
        if not eligible:
            raise CaptureError("navigation_no_measurable_negative_ring")
        image_value = stage.get("colorPath")
        if not isinstance(image_value, str) or not image_value:
            raise CaptureError("capture_png_missing")
        image_path = Path(image_value)
        if not image_path.is_absolute():
            image_path = path.parent / image_path
        try:
            image_path.resolve().relative_to(path.parent.resolve())
        except ValueError:
            raise CaptureError("capture_png_outside_manifest_directory")
        if not image_path.is_file():
            raise CaptureError("capture_png_missing", image_path)
        observed_paint = {key: paint[key] for key in ("engine", "dpr", "devicePixelRatio", "deviceTransform", "opacity", "compositionMode") if key in paint}
        report.update(widget_dpr=dpr, logical_widget_size=list(logical), image=str(image_path),
                      declared_backdrop=list(backdrop), paint_metadata=observed_paint,
                      color_model="encoded_RGB8_SourceOver_with_declared_opaque_backdrop_no_color_fit")
        with Image.open(image_path) as image:
            if image.format != "PNG" or image.mode not in ("RGB", "RGBA"):
                raise CaptureError("capture_not_rgb8_png", image.mode)
            if image.width <= 0 or image.height <= 0 or image.width * image.height > MAX_FRAME_PIXELS:
                raise CaptureError("capture_frame_size_unsupported")
            expected = tuple(int(math.floor(value * dpr + .5)) for value in logical)
            if any(abs(actual - wanted) > 1 for actual, wanted in zip(image.size, expected)):
                raise CaptureError("capture_dpr_size_mismatch", "actual=%s expected=%s" % (image.size, expected))
            report.update(physical_frame_size=list(image.size), expected_physical_size=list(expected),
                          decoded_input_pixel_bytes_estimate=image.width * image.height * len(image.getbands()))
            for handle in eligible:
                ring = _measure_ring(image, handle, handles, backdrop)
                report["rings"].append(ring)
                if not ring["measured"]:
                    report["errors"].append({"code": ring["error"], "handle_index": handle["index"]})
                else:
                    for metric, limit in LIMITS.items():
                        if ring[metric] > limit:
                            report["errors"].append({"code": "ring_" + metric + "_limit_exceeded",
                                                     "handle_index": handle["index"], "value": ring[metric], "limit": limit})
        report["reference_peak_estimated_bytes"] = max(r["reference_peak_estimated_bytes"] for r in report["rings"])
        report["reference_memory_note"] = "Conservative tile/scalar design bound; not an OS RSS measurement. Decoded input PNG bytes are separate, with no frame supersampling."
        report["passed"] = bool(report["rings"]) and not report["errors"]
    except (CaptureError, OSError, ValueError, TypeError, KeyError, OverflowError, Image.DecompressionBombError) as error:
        report["errors"].append(_error_record(error))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path, help="Owned diagnostic capture tree, or a single manifest.json")
    parser.add_argument("--output", required=True, type=Path, help="Fresh JSON report; existing evidence is never overwritten")
    args = parser.parse_args(argv)
    report = {"schema": "gsw.navigation_pixel_checks/1", "passed": False, "errors": [], "captures": [],
              "root": str(args.root), "thresholds": dict(LIMITS), "sample_grid": [SAMPLES, SAMPLES]}
    try:
        if args.output.exists():
            raise CaptureError("output_already_exists", args.output)
        if args.root.is_file():
            manifests = [args.root]
        elif args.root.is_dir():
            manifests = sorted(args.root.rglob("manifest.json"))
        else:
            raise CaptureError("capture_root_missing", args.root)
        if not manifests:
            raise CaptureError("capture_manifests_missing", args.root)
        for manifest in manifests:
            checked = check_capture(manifest)
            report["captures"].append(checked)
            if not checked["passed"]:
                report["errors"].append({"code": "capture_failed", "manifest": str(manifest)})
        report["passed"] = bool(report["captures"]) and not report["errors"]
    except (CaptureError, OSError, ValueError) as error:
        report["errors"].append(_error_record(error))
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as destination:
            json.dump(report, destination, ensure_ascii=False, indent=2, allow_nan=False)
            destination.write("\n")
    except (OSError, ValueError) as error:
        print(json.dumps({"passed": False, "errors": [_error_record(error)]}), file=sys.stderr)
        return 2
    print(json.dumps({"passed": report["passed"], "captures": len(report["captures"]), "output": str(args.output)}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
