"""Versioned trajectories enter the public fixed-camera admission seam."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

from native.backend import camera_dataset


class TrajectoryDatasetTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_TEST_TEMP", tempfile.gettempdir()))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def manifest(self, trajectory="1.00000000000000000001 1 2 3 0 0 0 1\n", images=None, **changes):
        (self.root / "poses.txt").write_text(trajectory, encoding="utf-8")
        images = images or [{"image_path": "train/same.png", "timestamp_s": "1.00000000000000000001",
                             "view_id": "training", "split": "train"}]
        for index, image in enumerate(images):
            path = self.root / image["image_path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            Image.new("RGB", (12, 8), (index + 10, 40, 80)).save(path)
        value = {"schema": "gsw.trajectory_dataset/1", "trajectory": {
            "path": "poses.txt", "format": "tum", "pose_convention": "c2w", "camera_axes": "opencv",
            "source_unit": "m", "quaternion_norm_tolerance": .0001},
            "images": images, "association": {"mode": "exact", "max_difference_s": "0", "missing": "reject"},
            "calibration": {"K": [[20., 0., 3.], [0., 30., 2.], [0., 0., 1.]],
                            "width": 12, "height": 8, "pixel_coordinates": "opencv_integer",
                            "projection_model": "PINHOLE", "undistorted": True, "distortion": []}}
        value.update(changes)
        path = self.root / "trajectory_dataset.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_exact_decimal_tum_pose_enters_locked_camera_admission(self):
        path = self.manifest()
        before = {item.name: hashlib.sha256(item.read_bytes()).hexdigest() for item in (path, self.root / "poses.txt")}
        result = camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(result["pose_policy"], "locked")
        self.assertEqual(result["views"][0]["source_c2w_opencv"],
                         [[1., 0., 0., 1.], [0., 1., 0., 2.], [0., 0., 1., 3.], [0., 0., 0., 1.]])
        self.assertEqual(result["views"][0]["K"][0][2], 3.5)
        self.assertEqual(result["views"][0]["timestamp_s"], "1.00000000000000000001")
        self.assertEqual(result["source"]["association"]["matches"][0]["difference_s"], "0E-20")
        self.assertEqual(result["scale"]["status"], "source_declared")
        self.assertEqual(before, {item.name: hashlib.sha256(item.read_bytes()).hexdigest() for item in (path, self.root / "poses.txt")})

    def test_unique_nearest_association_uses_decimal_threshold_and_preserves_split(self):
        images = [{"image_path": "train/same.png", "timestamp_s": "1000000000.00000000000000000011", "split": "train"},
                  {"image_path": "test/same.png", "timestamp_s": "1000000000.00000000000000000029", "split": "test"}]
        path = self.manifest("1000000000.00000000000000000010 0 0 0 0 0 0 1\n"
                             "1000000000.00000000000000000030 2 0 0 0 0 0 1\n", images,
                             association={"mode": "nearest_one_to_one", "max_difference_s": "0.00000000000000000001", "missing": "reject"})
        result = camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(len(result["splits"]["train"]), 1)
        self.assertEqual(len(result["splits"]["test"]), 1)
        self.assertEqual(result["views"][1]["source_c2w_opencv"][0][3], 2.)
        self.assertEqual([match["difference_s"] for match in result["source"]["association"]["matches"]], ["1E-20", "1E-20"])

    def test_drop_policy_records_only_unmatched_images_without_merging_holdout(self):
        images = [{"image_path": "train/a.png", "timestamp_s": "1", "view_id": "fit", "split": "train"},
                  {"image_path": "test/b.png", "timestamp_s": "9", "view_id": "missing", "split": "test"}]
        path = self.manifest("1 0 0 0 0 0 0 1\n", images,
                             association={"mode": "nearest_one_to_one", "max_difference_s": ".1", "missing": "drop"})
        result = camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(result["splits"], {"train": ["fit"], "val": [], "test": []})
        self.assertEqual(result["source"]["association"]["dropped"], [{"view_id": "missing", "image_path": "test/b.png",
                         "timestamp_s": "9", "split": "test", "reason": "no_pose_within_declared_association"}])

    def test_explicit_per_view_calibrations_preserve_full_intrinsics_and_nested_names(self):
        images = [{"image_path": "a/same.png", "timestamp_s": "1", "calibration_id": "wide", "split": "train"},
                  {"image_path": "b/same.png", "timestamp_s": "2", "calibration_id": "long", "split": "val"}]
        path = self.manifest("1 0 0 0 0 0 0 1\n2 2 0 0 0 0 0 1\n", images)
        value = json.loads(path.read_text(encoding="utf-8"))
        first = value.pop("calibration")
        first["id"] = "wide"
        second = copy.deepcopy(first)
        second.update(id="long", width=16, height=10, K=[[50., 0., 4.], [0., 70., 3.], [0., 0., 1.]])
        value["calibrations"] = [first, second]
        Image.new("RGB", (16, 10), (40, 80, 120)).save(self.root / "b/same.png")
        path.write_text(json.dumps(value), encoding="utf-8")
        result = camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual([view["K"][0][0] for view in result["views"]], [20., 50.])
        self.assertEqual([view["K"][1][1] for view in result["views"]], [30., 70.])
        self.assertEqual([view["width"] for view in result["views"]], [12, 16])
        self.assertEqual(len({view["view_id"] for view in result["views"]}), 2)

    def test_unmatched_drop_does_not_hide_missing_or_invalid_image_calibration(self):
        images = [{"image_path": "ok.png", "timestamp_s": "1", "split": "train"},
                  {"image_path": "bad.png", "timestamp_s": "9", "split": "test"}]
        path = self.manifest("1 0 0 0 0 0 0 1\n", images,
                             association={"mode": "exact", "max_difference_s": "0", "missing": "drop"})
        Image.new("RGB", (16, 10), (1, 2, 3)).save(self.root / "bad.png")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(raised.exception.code, "trajectory_image_dimensions_mismatch")

    def test_named_csv_world_to_camera_blender_and_explicit_cm_convert_once(self):
        path = self.manifest("timestamp_s,tx,ty,tz,qx,qy,qz,qw\n1,-1,-2,-3,0,0,0,1\n",
                             [{"image_path": "frame.png", "timestamp_s": "1", "split": "train"}])
        value = json.loads(path.read_text(encoding="utf-8"))
        value["trajectory"].update(format="gsw_pose_csv_v1", pose_convention="w2c", camera_axes="blender", source_unit="cm")
        path.write_text(json.dumps(value), encoding="utf-8")
        result = camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(result["views"][0]["source_c2w_opencv"],
                         [[1., 0., 0., 1.], [0., -1., 0., 2.], [0., 0., -1., 3.], [0., 0., 0., 1.]])
        self.assertEqual([result["views"][0]["c2w_opencv"][index][3] for index in range(3)], [.01, .02, .03])

    def test_named_json_trajectory_keeps_unknown_scale_and_rejects_conflicting_pose_keys(self):
        path = self.manifest(images=[{"image_path": "frame.png", "timestamp_s": "1", "split": "train"}])
        value = json.loads(path.read_text(encoding="utf-8"))
        value["trajectory"].update(format="gsw_pose_json_v1", source_unit="unknown")
        path.write_text(json.dumps(value), encoding="utf-8")
        pose = [[1., 0., 0., 4.], [0., 1., 0., 5.], [0., 0., 1., 6.], [0., 0., 0., 1.]]
        trajectory = {"schema": "gsw.pose_trajectory/1", "poses": [{"timestamp_s": "1", "c2w_opencv": pose}]}
        (self.root / "poses.txt").write_text(json.dumps(trajectory), encoding="utf-8")
        result = camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(result["views"][0]["source_c2w_opencv"], pose)
        self.assertEqual(result["scale"]["status"], "arbitrary")
        self.assertIsNone(result["scale"]["meters_per_unit"])
        trajectory["poses"][0]["w2c_opencv"] = pose
        (self.root / "poses.txt").write_text(json.dumps(trajectory), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(raised.exception.code, "trajectory_conflicting_pose_convention")

    def test_publisher_tum_pose_prefix_preserves_optical_center_not_synthetic_accuracy(self):
        fixture = Path(__file__).parent / "fixtures" / "tum-fr1-xyz-first3" / "groundtruth.txt"
        poses = fixture.read_text(encoding="utf-8")
        path = self.manifest(poses, [
            {"image_path": "train/placeholder.png", "timestamp_s": "1305031098.6659", "split": "train"},
            {"image_path": "test/placeholder.png", "timestamp_s": "1305031098.6858", "split": "test"}])
        result = camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual([result["views"][0]["source_c2w_opencv"][index][3] for index in range(3)], [1.3563, .6305, 1.6380])
        self.assertEqual([result["views"][1]["source_c2w_opencv"][index][3] for index in range(3)], [1.3525, .6306, 1.6339])
        self.assertEqual(result["source"]["trajectory"]["pose_count"], 3)
        self.assertEqual(result["scale"]["status"], "source_declared")

    def test_auto_dispatch_requires_exact_schema_and_saved_canonical_reopens_once(self):
        path = self.manifest()
        explicit = camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        automatic = camera_dataset.load_camera_dataset(path)
        self.assertEqual(explicit, automatic)
        self.assertEqual(camera_dataset.load_camera_dataset(self.root)["data_identity"], explicit["data_identity"])
        saved = self.root / "camera_dataset.json"
        camera_dataset.write_camera_dataset(explicit, saved)
        reopened = camera_dataset.load_camera_dataset(saved)
        self.assertEqual(reopened["data_identity"], explicit["data_identity"])
        self.assertEqual(reopened["views"], explicit["views"])

    def test_exact_schema_wins_over_an_incidental_transforms_filename(self):
        path = self.manifest()
        selected = self.root / "transforms_external.json"
        selected.write_bytes(path.read_bytes())
        result = camera_dataset.load_camera_dataset(selected)
        self.assertEqual(result["source"]["adapter"], "trajectory_dataset")
        self.assertEqual(result["views"][0]["source_c2w_opencv"][0][3], 1.)

    def test_duplicate_identity_time_and_multiple_sensors_are_not_silently_accepted(self):
        for change, code in (("path", "trajectory_duplicate_image"),
                             ("timestamp", "trajectory_duplicate_timestamp"),
                             ("id", "trajectory_duplicate_view_id"),
                             ("sensor", "trajectory_multiple_sensors_unsupported"),
                             ("calibration_override", "trajectory_invalid_image_fields")):
            images = [{"image_path": "a.png", "timestamp_s": "1", "view_id": "a", "split": "train"},
                      {"image_path": "b.png", "timestamp_s": "2", "view_id": "b", "split": "test"}]
            if change == "path":
                images[1]["image_path"] = images[0]["image_path"]
            elif change == "timestamp":
                images[1]["timestamp_s"] = images[0]["timestamp_s"]
            elif change == "id":
                images[1]["view_id"] = "a"
            elif change == "sensor":
                images[0]["sensor_id"], images[1]["sensor_id"] = "camera-a", "camera-b"
            else:
                images[0]["K"] = [[100., 0., 3.], [0., 100., 2.], [0., 0., 1.]]
            path = self.manifest("1 0 0 0 0 0 0 1\n2 2 0 0 0 0 0 1\n", images)
            with self.subTest(change=change), self.assertRaises(camera_dataset.CameraDatasetError) as raised:
                camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
            self.assertEqual(raised.exception.code, code)

    def test_ties_or_reused_nearest_poses_never_use_greedy_alternates_or_drop(self):
        cases = [("trajectory_ambiguous_association", [
            {"image_path": "a.png", "timestamp_s": "1.5", "split": "train"}]),
            ("trajectory_pose_reused", [
                {"image_path": "a.png", "timestamp_s": "1.01", "split": "train"},
                {"image_path": "b.png", "timestamp_s": "1.02", "split": "test"}])]
        for code, images in cases:
            path = self.manifest("1 0 0 0 0 0 0 1\n2 2 0 0 0 0 0 1\n", images,
                                 association={"mode": "nearest_one_to_one", "max_difference_s": "1", "missing": "drop"})
            with self.subTest(code=code), self.assertRaises(camera_dataset.CameraDatasetError) as raised:
                camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
            self.assertEqual(raised.exception.code, code)

    def test_unsupported_calibration_units_and_nonunit_poses_fail_closed(self):
        changes = [("unit", "trajectory_unit_declaration_required"),
                   ("distortion", "trajectory_invalid_calibration"),
                   ("quaternion", "trajectory_nonunit_quaternion"),
                   ("convention", "trajectory_conflicting_pose_convention"),
                   ("path", "trajectory_path_outside_dataset")]
        for change, code in changes:
            path = self.manifest()
            value = json.loads(path.read_text(encoding="utf-8"))
            if change == "unit":
                value["trajectory"].pop("source_unit")
            elif change == "distortion":
                value["calibration"]["distortion"] = [.01]
            elif change == "quaternion":
                (self.root / "poses.txt").write_text("1 0 0 0 0 0 0 2\n", encoding="utf-8")
            elif change == "convention":
                value["trajectory"]["pose_convention"] = "w2c"
            else:
                value["trajectory"]["path"] = "../outside.txt"
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.subTest(change=change), self.assertRaises(camera_dataset.CameraDatasetError) as raised:
                camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
            self.assertEqual(raised.exception.code, code)

    def test_cancellation_and_missing_pose_never_solve_cameras_or_change_source(self):
        path = self.manifest()
        original = path.read_bytes()
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset", cancelled=lambda: True)
        self.assertEqual(raised.exception.code, "camera_import_cancelled")
        self.assertEqual(path.read_bytes(), original)
        value = json.loads(original.decode("utf-8"))
        value["images"][0]["timestamp_s"] = "99"
        path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(raised.exception.code, "trajectory_unmatched_image")

    def test_real_worker_import_archives_original_trajectory_and_keeps_holdouts_separate(self):
        images = [{"image_path": "train/same.png", "timestamp_s": "1", "split": "train", "view_id": "train"},
                  {"image_path": "test/same.png", "timestamp_s": "2", "split": "test", "view_id": "test"}]
        path = self.manifest("1 0 0 0 0 0 0 1\n2 2 0 0 0 0 0 1\n", images)
        original = {item: (self.root / item).read_bytes() for item in ("trajectory_dataset.json", "poses.txt", "train/same.png", "test/same.png")}
        repository = Path(__file__).resolve().parents[3]
        project = self.root / "project"
        project.mkdir()
        config = {"task": "import", "repositoryRoot": str(repository), "projectRoot": str(project),
                  "datasetRoot": str(project / "datasets"), "scene": "capture",
                  "cameraInput": {"adapter": "trajectory_dataset", "sourcePath": str(path), "posePolicy": "locked"}}
        job = self.root / "job.json"
        job.write_text(json.dumps(config), encoding="utf-8")
        result = subprocess.run([sys.executable, "-B", str(repository / "native/worker/gsw_worker.py"),
                                 "--task", "import", "--config", str(job)], input="", text=True,
                                encoding="utf-8", errors="replace", capture_output=True, timeout=30,
                                cwd=str(self.root), creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        final = project / "datasets/capture"
        admitted = camera_dataset.load_camera_dataset(final)
        self.assertEqual(admitted["splits"], {"train": ["train"], "val": [], "test": ["test"]})
        self.assertEqual((final / "source/camera_input/poses.txt").read_bytes(), original["poses.txt"])
        self.assertEqual((final / "source/camera_input/trajectory_dataset.json").read_bytes(), original["trajectory_dataset.json"])
        for name in original:
            self.assertEqual((self.root / name).read_bytes(), original[name])
        self.assertFalse(os.path.samefile(self.root / "train/same.png", final / "train/same.png"))

    def test_real_worker_cancel_preserves_existing_dataset_with_control_pipe_open(self):
        path = self.manifest()
        repository = Path(__file__).resolve().parents[3]
        project = self.root / "project"
        final = project / "datasets/capture"
        final.mkdir(parents=True)
        (final / "original.txt").write_bytes(b"preserve existing project")
        config = {"task": "import", "repositoryRoot": str(repository), "projectRoot": str(project),
                  "datasetRoot": str(project / "datasets"), "scene": "capture", "overwrite": True,
                  "cameraInput": {"adapter": "trajectory_dataset", "sourcePath": str(path), "posePolicy": "locked"}}
        job = self.root / "job.json"
        job.write_text(json.dumps(config), encoding="utf-8")
        process = subprocess.Popen([sys.executable, "-B", str(repository / "native/worker/gsw_worker.py"),
                                    "--task", "import", "--config", str(job)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8",
                                   errors="replace", cwd=str(self.root),
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        process.stdin.write("cancel\n")
        process.stdin.flush()
        process.wait(timeout=30)
        process.stdin.close()
        log = process.stdout.read() + process.stderr.read()
        process.stdout.close()
        process.stderr.close()
        self.assertEqual(process.returncode, 130, log)
        self.assertIn('"state":"cancelled"', log)
        self.assertEqual((final / "original.txt").read_bytes(), b"preserve existing project")
        self.assertFalse((final / "camera_dataset.json").exists())

    def test_unimplemented_rig_or_time_offset_fields_are_explicit_refusals(self):
        for group, field in (("trajectory", "rig"), ("association", "time_offset_s")):
            path = self.manifest()
            value = json.loads(path.read_text(encoding="utf-8"))
            value[group][field] = "not implemented"
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.subTest(group=group):
                with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
                    camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
                self.assertEqual(raised.exception.code, "trajectory_unsupported_fields")

    def test_malformed_csv_is_a_typed_pose_refusal(self):
        path = self.manifest('timestamp_s,tx,ty,tz,qx,qy,qz,qw\n"unterminated,0,0,0,0,0,0,1\n')
        value = json.loads(path.read_text(encoding="utf-8"))
        value["trajectory"]["format"] = "gsw_pose_csv_v1"
        path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(raised.exception.code, "trajectory_invalid_pose_row")

    def test_reparse_components_are_rejected_before_trajectory_is_admitted(self):
        path = self.manifest()
        target, alias = self.root / "real", self.root / "alias"
        target.mkdir()
        (target / "poses.txt").write_bytes((self.root / "poses.txt").read_bytes())
        if os.name == "nt":
            command = "New-Item -ItemType Junction -Path '{}' -Target '{}' | Out-Null".format(
                str(alias).replace("'", "''"), str(target).replace("'", "''"))
            created = subprocess.run(["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command],
                                     text=True, capture_output=True, timeout=30,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
        else:
            os.symlink(target, alias, target_is_directory=True)
        value = json.loads(path.read_text(encoding="utf-8"))
        value["trajectory"]["path"] = "alias/poses.txt"
        path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(raised.exception.code, "trajectory_reparse_path_unsupported")

    def test_binary_float_timestamps_are_refused_instead_of_guessing_lost_digits(self):
        path = self.manifest()
        value = json.loads(path.read_text(encoding="utf-8"))
        value["images"][0]["timestamp_s"] = 1.0
        path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(raised.exception.code, "trajectory_timestamp_requires_decimal")

    def test_manifest_cannot_silently_override_unit_or_calibration_protocol(self):
        path = self.manifest()
        value = json.loads(path.read_text(encoding="utf-8"))
        value["scale"] = {"status": "independently_checked", "meters_per_unit": 1000.}
        path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaises(camera_dataset.CameraDatasetError) as raised:
            camera_dataset.load_camera_dataset(path, adapter="trajectory_dataset")
        self.assertEqual(raised.exception.code, "trajectory_unsupported_fields")


if __name__ == "__main__":
    unittest.main()
