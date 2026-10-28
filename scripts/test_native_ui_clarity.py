"""Public navigation-image acceptance contract; execute on remote CI only.

This file is the red slice: check_native_ui_clarity.py intentionally does not
exist when this test is first submitted. Do not run this suite on the user's
machine and do not create a permissive stub merely to make the import green.

The independent fixture generator below analytically integrates a circle over
each unit pixel. The oracle under test must use its separate 32x32 area-sample
reference, not call this fixture code or the production navigation renderer.

Geometry is logical px in the capture metadata and physical px after applying
widgetDpr. A QPainter ellipse radius R and centered pen width S have visible
outer radius R+S/2 and inner radius max(0,R-S/2). Halo is measured beyond the
visible outer radius plus 1.25 physical px, not beyond the pen centerline.

Frozen per-ring limits: normalized RGB RMSE <= .06, P95 RGB error <= .16,
outer-halo maximum <= .04. Edge ROIs must remain local and avoid other handle,
shaft and label coverage; a whole-frame average cannot dilute a bad small ring.
Reference working memory is capped at16MiB by the implementation protocol;
these behavior tests do not treat a self-reported allocation flag as proof.

Stable public report shape: {passed: bool, errors: list, rings: list}, where a
measured ring has normalized_rgb_rmse, p95_rgb_error and outer_halo_max. A
missing/invalid capture or no measurable negative ring must fail closed. The CLI
uses the same passed verdict and returns a nonzero code on failure.
"""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_native_ui_clarity import check_capture, pixel_disc_coverage


ROOT = Path(__file__).resolve().parents[1]
LIMITS = {"normalized_rgb_rmse": .06, "p95_rgb_error": .16, "outer_halo_max": .04}
BACKGROUND = (240, 244, 248, 255)


def _analytic_pixel_disc_area(cx, cy, radius, x, y):
    """Exact circle-square intersection by piecewise analytic integration.

    Breakpoints are where a circle crosses a pixel's horizontal edges. Within
    each interval the upper/lower boundary is either constant or a circle arc;
    its antiderivative gives the area. No sampling grid, Pillow ellipse or
    production/oracle function is used to generate the expected image.
    """
    if radius <= 0:
        return 0.0
    left, right = max(x - cx, -radius), min(x + 1 - cx, radius)
    if right <= left:
        return 0.0
    low, high = y - cy, y + 1 - cy
    breaks = [left, right]
    for boundary in (low, high):
        if abs(boundary) < radius:
            crossing = math.sqrt(max(0.0, radius * radius - boundary * boundary))
            for point in (-crossing, crossing):
                if left < point < right:
                    breaks.append(point)
    breaks = sorted(set(breaks))

    def arc_primitive(point):
        point = min(radius, max(-radius, point))
        return .5 * (point * math.sqrt(max(0.0, radius * radius - point * point))
                     + radius * radius * math.asin(point / radius))

    area = 0.0
    for first, last in zip(breaks, breaks[1:]):
        middle = (first + last) * .5
        circle_y = math.sqrt(max(0.0, radius * radius - middle * middle))
        if min(high, circle_y) <= max(low, -circle_y):
            continue
        arc_area = arc_primitive(last) - arc_primitive(first)
        upper_area = high * (last - first) if high <= circle_y else arc_area
        lower_area = low * (last - first) if low >= -circle_y else -arc_area
        area += upper_area - lower_area
    return min(1.0, max(0.0, area))


def _handle(**changes):
    result = {
        "axisIndex": 0, "positive": False, "aligned": False,
        "highlighted": False, "center": [48.3, 39.7],
        "radius": 11.2, "strokeWidth": 1.6,
        "outline": [25, 35, 45, 255], "fill": [245, 120, 40, 96],
        "shaft": None,
    }
    result.update(changes)
    return result


def _paint_analytic_ring(image, handle, dpr, *, hard=False, offset=(0.0, 0.0)):
    """Paint only a local bbox; no whole-viewport supersampled allocation."""
    cx = handle["center"][0] * dpr + offset[0]
    cy = handle["center"][1] * dpr + offset[1]
    radius = handle["radius"] * dpr
    half_stroke = handle["strokeWidth"] * dpr * .5
    outer, inner = radius + half_stroke, max(0.0, radius - half_stroke)
    outline, fill = handle["outline"], handle["fill"]
    for y in range(max(0, math.floor(cy - outer - 1)),
                   min(image.height, math.ceil(cy + outer + 1))):
        for x in range(max(0, math.floor(cx - outer - 1)),
                       min(image.width, math.ceil(cx + outer + 1))):
            if hard:
                distance = math.hypot(x + .5 - cx, y + .5 - cy)
                outer_area = float(distance <= outer)
                inner_area = float(distance <= inner)
            else:
                outer_area = _analytic_pixel_disc_area(cx, cy, outer, x, y)
                inner_area = _analytic_pixel_disc_area(cx, cy, inner, x, y)
            border_area = max(0.0, outer_area - inner_area)
            if outer_area <= 0:
                continue
            base = image.getpixel((x, y))
            rgb = []
            for channel in range(3):
                border_color = outline[channel] * outline[3] / 255 + base[channel] * (1 - outline[3] / 255)
                fill_color = fill[channel] * fill[3] / 255 + base[channel] * (1 - fill[3] / 255)
                value = base[channel] * (1 - outer_area) + border_color * border_area + fill_color * inner_area
                rgb.append(min(255, max(0, round(value))))
            image.putpixel((x, y), (*rgb, 255))


class DiscCoveragePublicContractTests(unittest.TestCase):
    def test_known_literal_and_analytic_areas_without_renderer_reference(self):
        cases = [
            ((.5, .5), 2.0, (0, 0), 1.0),
            ((5.0, 5.0), 1.0, (0, 0), 0.0),
            ((.5, .5), .5, (0, 0), math.pi / 4),
            ((0.0, 0.0), 1.0, (0, 0), math.pi / 4),
            ((0.0, 0.0), 1.0, (-1, -1), math.pi / 4),
            ((.5, .5), .25, (0, 0), math.pi / 16),
        ]
        for center, radius, origin, expected in cases:
            with self.subTest(center=center, radius=radius, origin=origin):
                actual = pixel_disc_coverage(center, radius, pixelOrigin=origin, samples=32)
                self.assertIsInstance(actual, (float, int))
                self.assertTrue(math.isfinite(actual))
                self.assertGreaterEqual(actual, 0)
                self.assertLessEqual(actual, 1)
                self.assertAlmostEqual(actual, expected, delta=.01)

    def test_coverage_is_deterministic_and_translation_invariant(self):
        first = pixel_disc_coverage((.23, .61), .7, (0, 0), samples=32)
        repeated = pixel_disc_coverage((.23, .61), .7, (0, 0), samples=32)
        shifted = pixel_disc_coverage((10.23, -6.39), .7, (10, -7), samples=32)
        self.assertEqual(first, repeated)
        self.assertAlmostEqual(first, shifted, delta=1e-12)


class CaptureAcceptancePublicContractTests(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ.get("GSW_UI_CLARITY_TEST_ROOT", tempfile.gettempdir()))
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="gsw-clarity-", dir=str(parent))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def capture(self, name, *, dpr=1.0, logical=(96, 80), handle=None, hard=False,
                offset=(0.0, 0.0), extra_handles=()):
        folder = self.root / "captures" / name
        folder.mkdir(parents=True)
        image_path = folder / "after-painter.png"
        first = _handle() if handle is None else handle
        handles = [first, *extra_handles]
        image = Image.new("RGBA", (round(logical[0] * dpr), round(logical[1] * dpr)), BACKGROUND)
        _paint_analytic_ring(image, first, dpr, hard=hard, offset=offset)
        for other in extra_handles:
            shaft = other.get("shaft")
            if shaft:
                points = [tuple(coordinate * dpr for coordinate in shaft[key]) for key in ("from", "to")]
                ImageDraw.Draw(image).line(points, fill=tuple(other["outline"]),
                                           width=max(1, round(shaft["width"] * dpr)))
            _paint_analytic_ring(image, other, dpr)
            if other["positive"] or other["aligned"] or other["highlighted"]:
                # This deliberately unrelated label is inside an excluded handle.
                ImageDraw.Draw(image).text(tuple(value * dpr - 2 for value in other["center"]),
                                           "X", fill=(255, 255, 255, 255))
        image.save(image_path)
        image.close()
        manifest = {
            "captureKind": "synthetic_unit_fixture_not_real_viewport",
            "widgetDpr": dpr, "logicalWidgetSize": list(logical),
            "stages": [{"name": "after-painter", "colorPath": str(image_path)}],
            "notes": [{"name": "navigation-presentation", "meta": {
                "handles": handles, "backdrop": list(BACKGROUND),
                "paint": {"devicePixelRatio": dpr, "fixture": "independent analytic area integration"},
            }}],
        }
        path = folder / "manifest.json"
        path.write_text(json.dumps(manifest, ensure_ascii=False, allow_nan=False), encoding="utf-8")
        return path, image_path

    def assert_ring_passes(self, report):
        self.assertIs(report["passed"], True, report)
        self.assertEqual([], report["errors"], report)
        self.assertGreaterEqual(len(report["rings"]), 1, report)
        for ring in report["rings"]:
            for metric, limit in LIMITS.items():
                self.assertIn(metric, ring)
                self.assertTrue(math.isfinite(ring[metric]), ring)
                self.assertGreaterEqual(ring[metric], 0)
                self.assertLessEqual(ring[metric], limit, ring)

    def assert_image_fails(self, report):
        self.assertIs(report["passed"], False, report)
        self.assertTrue(report["rings"], report)
        self.assertTrue(any(ring.get(metric, -1) > limit
                            for ring in report["rings"] for metric, limit in LIMITS.items()), report)

    def test_independent_smooth_ring_passes_all_five_controlled_dpr_mappings(self):
        for dpr in (1.0, 1.25, 1.5, 1.75, 2.0):
            with self.subTest(dpr=dpr):
                manifest, _ = self.capture("smooth-" + str(dpr), dpr=dpr)
                self.assert_ring_passes(check_capture(manifest))

    def test_hard_edge_is_rejected_locally_not_diluted_by_large_background(self):
        manifest, _ = self.capture("hard-ring-large-canvas", logical=(512, 384), hard=True)
        self.assert_image_fails(check_capture(manifest))

    def test_physical_pixel_misalignment_is_rejected(self):
        manifest, _ = self.capture("shifted-ring", dpr=1.5, offset=(1.75, -.75))
        self.assert_image_fails(check_capture(manifest))

    def test_blurred_edges_do_not_pass_as_antialiasing(self):
        manifest, image_path = self.capture("blurred-ring")
        with Image.open(image_path) as source:
            blurred = source.filter(ImageFilter.GaussianBlur(radius=1.8))
        blurred.save(image_path)
        blurred.close()
        self.assert_image_fails(check_capture(manifest))

    def test_color_halo_beyond_visible_outer_edge_is_rejected(self):
        handle = _handle()
        manifest, image_path = self.capture("halo-ring", handle=handle)
        outer = handle["radius"] + handle["strokeWidth"] * .5
        with Image.open(image_path) as source:
            image = source.convert("RGBA")
        # Pixel center is more than1.25px outside R+S/2, within a local halo ROI.
        image.putpixel((math.floor(handle["center"][0] + outer + 2.5),
                        math.floor(handle["center"][1])), (90, 30, 200, 255))
        image.save(image_path)
        image.close()
        report = check_capture(manifest)
        self.assert_image_fails(report)
        self.assertGreater(max(ring["outer_halo_max"] for ring in report["rings"]), .04)

    def test_other_handle_label_and_shaft_coverage_is_excluded(self):
        other = _handle(axisIndex=1, positive=True, center=[60.0, 41.0], radius=8.5,
                        fill=[30, 190, 90, 255], outline=[20, 90, 40, 255],
                        shaft={"from": [25.0, 29.0], "to": [60.0, 41.0], "width": 3.0})
        manifest, _ = self.capture("occluded-ring", extra_handles=(other,))
        report = check_capture(manifest)
        self.assert_ring_passes(report)
        self.assertEqual(1, len(report["rings"]), report)

    def test_frame_dimensions_must_match_logical_size_times_actual_dpr(self):
        manifest, _ = self.capture("mismatched-dpr")
        value = json.loads(manifest.read_text(encoding="utf-8"))
        value["widgetDpr"] = 1.25
        manifest.write_text(json.dumps(value), encoding="utf-8")
        report = check_capture(manifest)
        self.assertIs(report["passed"], False, report)
        self.assertTrue(report["errors"], report)

    def test_missing_png_and_missing_navigation_evidence_fail_closed(self):
        manifest, image_path = self.capture("missing-png")
        image_path.unlink()
        report = check_capture(manifest)
        self.assertIs(report["passed"], False, report)
        self.assertTrue(report["errors"], report)
        manifest, _ = self.capture("missing-navigation-note")
        value = json.loads(manifest.read_text(encoding="utf-8"))
        value["notes"] = []
        manifest.write_text(json.dumps(value), encoding="utf-8")
        report = check_capture(manifest)
        self.assertIs(report["passed"], False, report)
        self.assertTrue(report["errors"], report)

    def test_positive_or_labeled_handles_alone_cannot_manufacture_a_pass(self):
        manifest, _ = self.capture("no-measurable-ring", handle=_handle(positive=True))
        report = check_capture(manifest)
        self.assertIs(report["passed"], False, report)
        self.assertTrue(report["errors"], report)

    def test_cli_exit_code_and_report_agree_for_real_png_files(self):
        self.capture("cli-通過-日本語")
        output = self.root / "passed-report.json"
        command = [sys.executable, "-I", "-B", str(ROOT / "scripts/check_native_ui_clarity.py"),
                   "--root", str(self.root / "captures"), "--output", str(output)]
        options = dict(stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
        if os.name == "nt":
            options["creationflags"] = subprocess.CREATE_NO_WINDOW
        process = subprocess.run(command, **options)
        self.assertEqual(0, process.returncode, process.stderr.decode("utf-8", "replace"))
        self.assertIs(json.loads(output.read_text(encoding="utf-8"))["passed"], True)
        self.capture("cli-hard-failure", hard=True)
        failed_output = self.root / "failed-report.json"
        command[-1] = str(failed_output)
        process = subprocess.run(command, **options)
        self.assertNotEqual(0, process.returncode)
        self.assertTrue(failed_output.is_file(), process.stderr.decode("utf-8", "replace"))
        self.assertIs(json.loads(failed_output.read_text(encoding="utf-8"))["passed"], False)


if __name__ == "__main__":
    unittest.main()
