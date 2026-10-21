"""Generic batches at the public pinned-plan seam, never training by preview."""
import copy
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from PIL import Image
from native.backend import camera_dataset
from native.backend import training_batch_plan


class TrainingBatchPlanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="gsw-batch-plan-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "dataset"
        self.source.mkdir()
        views = []
        for index, split in enumerate(("train", "train", "test")):
            path = self.source / ("view-{}.png".format(index))
            Image.new("RGB", (16, 12), (index * 30 + 10, 20, 30)).save(str(path))
            views.append({"view_id": "camera-{}".format(index), "image_path": path.name,
                "width": 16, "height": 12, "split": split, "pixel_coordinates": "pixel_border",
                "K": [[12., 0., 8.], [0., 13., 6.], [0., 0., 1.]],
                "c2w_opencv": [[1., 0., 0., index - 1.], [0., 1., 0., 0.],
                    [0., 0., 1., -4.], [0., 0., 0., 1.]]})
        raw = {"schema": "gsw.camera_dataset/1", "pose_policy": "locked", "views": views,
            "source": {"kind": "manual"}, "scale": {"status": "arbitrary"},
            "initialization": {"kind": "random", "frame": "source", "seed": 7,
                "count": 16, "bounds": [[-1., -1., -1.], [1., 1., 1.]]}}
        camera_dataset.write_camera_dataset(raw, self.source / "camera_dataset.json")
        self.data = camera_dataset.load_camera_dataset(self.source)
        camera_dataset.write_camera_dataset(self.data, self.source / "camera_dataset.json")
        self.request = {"schema": "gsw.training_batch_plan/1",
            "datasets": [{"id": "example", "path": str(self.source),
                "data_identity": self.data["data_identity"], "display_name": "任意条件"}],
            "base": {"quality": "quick"},
            "matrix": {"backend": ["3dgs", "2dgs"], "trainingSeed": [11, 29],
                "trainOptions.iterations": [1000], "trainOptions.resolution": [1]},
            "initialization_policy": "dataset_declared", "evaluation_policy": "declared_splits",
            "run_policy": {"on_failure": "stop"}}

    def test_two_backends_and_two_seeds_expand_without_mutating_calibration_or_holdouts(self):
        before = (self.source / "camera_dataset.json").read_bytes()
        request = copy.deepcopy(self.request)
        result = training_batch_plan.expand_training_batch(request)
        self.assertEqual(result["schema"], "gsw.training_batch/1")
        self.assertEqual(result["summary"]["runCount"], 4)
        self.assertEqual(len({run["id"] for run in result["runs"]}), 4)
        self.assertEqual({(run["configuration"]["backend"], run["configuration"]["trainingSeed"])
                          for run in result["runs"]},
                         {("3dgs", 11), ("3dgs", 29), ("2dgs", 11), ("2dgs", 29)})
        for run in result["runs"]:
            config = run["configuration"]
            self.assertEqual(config["cameraDatasetIdentity"], self.data["data_identity"])
            self.assertEqual(config["posePolicy"], "locked")
            self.assertFalse(config["runColmap"])
            self.assertEqual(config["resolutionPolicy"], "preserve")
            self.assertEqual(run["initialization"], self.data["initialization"])
            self.assertEqual(run["splits"], self.data["splits"])
        self.assertEqual(request, self.request)
        self.assertEqual((self.source / "camera_dataset.json").read_bytes(), before)
        self.assertFalse(any(self.root.glob("**/state.json")))

    def test_batch_resource_limit_refuses_every_run_instead_of_silently_sampling(self):
        request = copy.deepcopy(self.request)
        request["matrix"]["trainingSeed"] = list(range(129))
        with self.assertRaisesRegex(training_batch_plan.TrainingBatchError, "batch_resource_limit"):
            training_batch_plan.expand_training_batch(request)

    def test_repeated_same_seed_trials_have_distinct_outputs_without_changing_seed(self):
        request = copy.deepcopy(self.request)
        request["matrix"].update(backend=["3dgs"], trainingSeed=[11], repeat=[1, 2])
        result = training_batch_plan.expand_training_batch(request)
        self.assertEqual(result["summary"]["runCount"], 2)
        self.assertEqual(len({run["id"] for run in result["runs"]}), 2)
        self.assertEqual([run["configuration"]["trainingSeed"] for run in result["runs"]], [11, 11])
        self.assertEqual([run["parameters"]["repeat"] for run in result["runs"]], [1, 2])

    def test_duplicate_dataset_or_parameter_values_cannot_alias_a_run_directory(self):
        request = copy.deepcopy(self.request)
        request["datasets"].append(copy.deepcopy(request["datasets"][0]))
        with self.assertRaisesRegex(training_batch_plan.TrainingBatchError, "batch_duplicate_dataset_id"):
            training_batch_plan.expand_training_batch(request)

    def test_backend_specific_parameters_are_explicit_not_silently_ignored(self):
        request = copy.deepcopy(self.request)
        request["matrix"].update(backend=["2dgs"], trainingSeed=[11])
        request["matrix"]["trainOptions.depth_ratio"] = [.37]
        result = training_batch_plan.expand_training_batch(request)
        self.assertEqual(result["runs"][0]["configuration"]["trainOptions"]["depth_ratio"], .37)
        request["matrix"]["backend"] = ["3dgs", "2dgs"]
        with self.assertRaisesRegex(training_batch_plan.TrainingBatchError, "batch_parameter_backend_mismatch"):
            training_batch_plan.expand_training_batch(request)
        request = copy.deepcopy(self.request)
        request["matrix"]["trainingSeed"] = [11, 11]
        with self.assertRaisesRegex(training_batch_plan.TrainingBatchError, "batch_duplicate_axis_value"):
            training_batch_plan.expand_training_batch(request)

    def test_unknown_policies_and_malformed_base_are_typed_rejections(self):
        cases = [({"run_policy": {"on_failure": "pretend_success"}}, "batch_invalid_run_policy"),
                 ({"extra": True}, "batch_unsupported_parameter"),
                 ({"base": {"trainOptions": []}}, "batch_unsupported_parameter"),
                 ({"base": {"performanceOptions": {"silently_ignore_me": True}}},
                  "batch_invalid_performance_policy")]
        for change, code in cases:
            with self.subTest(change=change):
                request = copy.deepcopy(self.request)
                request.update(change)
                with self.assertRaisesRegex(training_batch_plan.TrainingBatchError, code):
                    training_batch_plan.expand_training_batch(request)

    def test_performance_policy_is_effective_and_part_of_run_identity(self):
        request = copy.deepcopy(self.request)
        before = training_batch_plan.expand_training_batch(request)
        request["base"]["performanceOptions"] = {"enabled": False, "growth_mode": "pause"}
        after = training_batch_plan.expand_training_batch(request)
        self.assertNotEqual(before["runs"][0]["id"], after["runs"][0]["id"])
        self.assertFalse(after["runs"][0]["configuration"]["performanceOptions"]["enabled"])
        self.assertNotIn("runtimeOptions", after["runs"][0]["configuration"])
        effective = after["runtime_configuration"]["effective"]
        self.assertFalse(effective["resourceGuard"]["enabled"])
        self.assertFalse(effective["execution"]["image_staging"])
        self.assertFalse(effective["execution"]["capacity_storage"])
        self.assertEqual(effective["execution"]["loss_mode"], "stock")
        self.assertEqual(after["runtime_configuration"]["migration"]["source"], "performanceOptions")
        self.assertEqual(after["run_policy"], {"on_failure": "stop"})

    def test_independent_guard_off_runtime_keeps_execution_and_training_inputs_for_both_backends(self):
        request = copy.deepcopy(self.request)
        policy = {"version": 2,
            "resourceGuard": {"enabled": False, "mode": "pause", "usage_fraction": .9,
                "minimum_spare_mib": 512, "max_consecutive_denials": 3},
            "execution": {"image_staging": True, "camera_prefetch": True,
                "capacity_storage": True, "surfel_auxiliary": True, "loss_mode": "auto",
                "engine": "stock", "gpu_slots": 2, "pinned_mib": 256, "gpu_mib": 512},
            "monitoring": {"profile_interval": 30}}
        request["base"]["runtimeOptions"] = policy
        before = copy.deepcopy(request)
        result = training_batch_plan.expand_training_batch(request)
        self.assertEqual(result["summary"]["runCount"], 4)
        for run in result["runs"]:
            config = run["configuration"]
            self.assertEqual(config["runtimeOptions"], policy)
            self.assertNotIn("performanceOptions", config)
            self.assertEqual(config["trainOptions"], {"iterations": 1000, "resolution": 1})
            self.assertEqual(config["posePolicy"], "locked")
            self.assertEqual(config["cameraDatasetIdentity"], self.data["data_identity"])
            self.assertEqual(run["initialization"], self.data["initialization"])
            self.assertEqual(run["splits"], self.data["splits"])
        self.assertEqual(request, before)

    def test_runtime_plan_records_requested_dependency_effective_value_without_rewriting_child(self):
        request = copy.deepcopy(self.request)
        request["base"]["runtimeOptions"] = {"version": 2,
            "execution": {"image_staging": False, "camera_prefetch": True}}
        report = training_batch_plan.expand_training_batch(request)
        runtime = report["runtime_configuration"]
        self.assertTrue(runtime["requested"]["execution"]["camera_prefetch"])
        self.assertFalse(runtime["effective"]["execution"]["camera_prefetch"])
        self.assertEqual(runtime["reasons"], [
            {"feature": "execution.camera_prefetch", "code": "requires_image_staging"}])
        self.assertEqual(runtime["migration"]["source"], "runtimeOptions")
        for run in report["runs"]:
            self.assertTrue(run["configuration"]["runtimeOptions"]["execution"]["camera_prefetch"])

    def test_relocation_preserves_plan_identity_but_changed_test_pixels_are_refused(self):
        before = training_batch_plan.expand_training_batch(self.request)
        relocated = self.root / "moved"
        shutil.copytree(str(self.source), str(relocated))
        request = copy.deepcopy(self.request)
        request["datasets"][0]["path"] = str(relocated)
        after = training_batch_plan.expand_training_batch(request)
        self.assertEqual(before["plan_sha256"], after["plan_sha256"])
        Image.new("RGB", (16, 12), (250, 250, 250)).save(str(relocated / "view-2.png"))
        with self.assertRaisesRegex(training_batch_plan.TrainingBatchError, "batch_dataset_invalid"):
            training_batch_plan.expand_training_batch(request)

    def test_cancellation_and_source_fingerprints_are_public_and_durable(self):
        with self.assertRaisesRegex(training_batch_plan.TrainingBatchError, "batch_cancelled"):
            training_batch_plan.expand_training_batch(self.request, cancelled=lambda: True)
        report = training_batch_plan.expand_training_batch(self.request)
        self.assertIn("native/worker/gsw_worker.py", report["source_fingerprints"])
        self.assertTrue(all(len(value) == 64 for value in report["source_fingerprints"].values()))
        self.assertEqual(report["run_policy"]["on_failure"], "stop")

    def test_resolution_axis_is_an_explicit_ratio_never_an_accidental_pixel_width(self):
        for value in (3, 5, 16):
            request = copy.deepcopy(self.request)
            request["matrix"]["trainOptions.resolution"] = [value]
            with self.subTest(value=value), self.assertRaisesRegex(
                    training_batch_plan.TrainingBatchError, "batch_explicit_training_limits_required"):
                training_batch_plan.expand_training_batch(request)

    def test_corresponding_source_newlines_are_portable_but_source_edits_are_not(self):
        repository = Path(__file__).resolve().parents[3]
        copied = self.root / "corresponding-source"
        for relative in training_batch_plan._SOURCE_FILES:
            path = copied / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((repository / relative).read_bytes().replace(b"\r\n", b"\n"))
        before = training_batch_plan.expand_training_batch(self.request, repository)
        portable = training_batch_plan.expand_training_batch(self.request, copied)
        self.assertEqual(before["plan_sha256"], portable["plan_sha256"])
        self.assertEqual(portable["source_hash_mode"], "text-lf-v1")
        path = copied / "native/worker/gsw_worker.py"
        path.write_bytes(path.read_bytes() + b"\n# source changed\n")
        changed = training_batch_plan.expand_training_batch(self.request, copied)
        self.assertNotEqual(portable["plan_sha256"], changed["plan_sha256"])


if __name__ == "__main__":
    unittest.main()
