"""Shared configured/loaded training contract, without importing torch."""
import ast
import copy
from pathlib import Path
from types import SimpleNamespace
import unittest

from native.worker.training_summary import (
    configured_training_summary, emit_loaded_training_summary,
    loaded_training_summary, normalize_training_summary,
)


def options(backend="3dgs"):
    result = {"iterations": 30000, "resolution": 1,
              "densify_until_iter": 22000, "densification_interval": 80,
              "densify_grad_threshold": .00012}
    if backend == "3dgs":
        result.update(optimizer_type="default", antialiasing=True, exposure_compensation=True)
    else:
        result.update(depth_ratio=0.0)
    return result


def configured(backend="3dgs"):
    return configured_training_summary(backend, "original_quality", options(backend))


class TrainingSummaryTests(unittest.TestCase):
    def test_configured_has_effective_backend_specific_options(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                value = configured(backend)
                self.assertEqual(value["phase"], "configured")
                self.assertEqual(value["optimizer"], "default" if backend == "3dgs" else "adam")
                self.assertEqual(value["resolution"], 1)
                self.assertEqual(value["iterations"], 30000)
                self.assertEqual(normalize_training_summary(value), value)
                self.assertEqual("antialiasing" in value, backend == "3dgs")
                self.assertEqual("depthRatio" in value, backend == "2dgs")
                self.assertNotIn("trainImageCount", value)

    def test_loaded_counts_actual_training_cameras_and_mixed_dimensions(self):
        cameras = [SimpleNamespace(image_width=1928, image_height=1084),
                   SimpleNamespace(image_width=961, image_height=541),
                   SimpleNamespace(image_width=1928, image_height=1084)]
        value = loaded_training_summary(configured(), cameras)
        self.assertEqual(value["phase"], "loaded")
        self.assertEqual(value["trainImageCount"], 3)
        self.assertEqual(value["trainDimensions"], [[961, 541, 1], [1928, 1084, 2]])
        self.assertEqual(value["trainDimensionKinds"], 2)
        self.assertEqual(value["trainPixels"], 961 * 541 + 2 * 1928 * 1084)
        self.assertEqual(normalize_training_summary(normalize_training_summary(value)), value)

    def test_dimension_listing_bounded_without_losing_total_kinds_or_pixels(self):
        cameras = [SimpleNamespace(image_width=index, image_height=3) for index in range(1, 21)]
        value = loaded_training_summary(configured("2dgs"), cameras)
        self.assertEqual(len(value["trainDimensions"]), 8)
        self.assertEqual(value["trainDimensionKinds"], 20)
        self.assertEqual(value["trainImageCount"], 20)
        self.assertEqual(value["trainPixels"], sum(range(1, 21)) * 3)
        self.assertEqual(normalize_training_summary(value), value)

    def test_loaded_pixels_are_int64_not_int32(self):
        value = loaded_training_summary(configured(), [SimpleNamespace(image_width=100000, image_height=100000)])
        self.assertEqual(value["trainPixels"], 10000000000)
        self.assertEqual(normalize_training_summary(value), value)

    def test_invalid_options_fail_closed_without_coercion(self):
        for key, bad in (("version", True), ("version", 2), ("phase", "running"),
                         ("backend", "sugar"), ("quality", "arbitrary"), ("quality", []),
                         ("iterations", True), ("iterations", 2.5), ("resolution", -1),
                         ("optimizer", "unavailable"), ("densifyUntil", 30001),
                         ("densificationInterval", 0), ("densifyGradient", float("nan")),
                         ("densifyGradient", float("inf")), ("densifyGradient", 10 ** 400), ("antialiasing", 1)):
            with self.subTest(key=key, bad=bad):
                value = configured()
                value[key] = bad
                self.assertIsNone(normalize_training_summary(value))
        for value in (None, [], "{}", {}):
            self.assertIsNone(normalize_training_summary(value))
        for required in configured():
            value = configured()
            del value[required]
            self.assertIsNone(normalize_training_summary(value), required)

    def test_zero_densification_preserves_valid_historical_settings(self):
        value = configured()
        value["densifyUntil"] = 0
        self.assertEqual(normalize_training_summary(value), value)

    def test_invalid_loaded_totals_dimensions_duplicates_or_overflow_are_rejected(self):
        loaded = loaded_training_summary(configured(), [SimpleNamespace(image_width=20, image_height=10)])
        for changes in ({"trainImageCount": 2}, {"trainPixels": 199}, {"trainPixels": 2 ** 63},
                        {"trainDimensionKinds": 2}, {"trainDimensions": [[20, 10, True]]},
                        {"trainDimensions": [[20, 10, 1], [20, 10, 1]], "trainImageCount": 2,
                         "trainDimensionKinds": 2, "trainPixels": 400},
                        {"trainDimensions": [[0, 10, 1]]}):
            value = copy.deepcopy(loaded)
            value.update(changes)
            with self.subTest(changes=changes):
                self.assertIsNone(normalize_training_summary(value))

    def test_no_loaded_event_for_legacy_or_invalid_control_and_no_camera_mutation(self):
        emitted = []
        cameras = [SimpleNamespace(image_width=22, image_height=11)]
        emitter = lambda *values: emitted.append(values)
        self.assertIsNone(emit_loaded_training_summary(emitter, {}, cameras))
        self.assertEqual(emitted, [])
        result = emit_loaded_training_summary(emitter, {"trainingSummary": configured("2dgs")}, cameras)
        self.assertEqual(emitted, [("[gsw-training-input]", result)])
        self.assertEqual(result["backend"], "2dgs")
        self.assertEqual(cameras[0].image_width, 22)
        self.assertIsNone(loaded_training_summary(configured(), []))
        self.assertIsNone(loaded_training_summary(configured(), [SimpleNamespace(image_width=0, image_height=1)]))

    def test_both_trainers_use_shared_loaded_event_before_iteration_loop(self):
        root = Path(__file__).resolve().parents[2]
        for filename in (root / "gaussian-splatting" / "train.py", Path(__file__).with_name("two_dgs_train.py")):
            tree = ast.parse(filename.read_text(encoding="utf-8"))
            training = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "training")
            calls = [node for node in ast.walk(training) if isinstance(node, ast.Call)
                     and isinstance(node.func, ast.Name) and node.func.id == "emit_loaded_training_summary"]
            self.assertEqual(len(calls), 1, str(filename))
            loop = next(node for node in training.body if isinstance(node, ast.For))
            self.assertLess(calls[0].lineno, loop.lineno)


if __name__ == "__main__":
    unittest.main()
