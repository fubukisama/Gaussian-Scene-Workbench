"""Public solve preparation at Windows legacy path and transaction boundaries."""

import hashlib
import io
import json
import os
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import tempfile
import threading
import unittest
from unittest import mock

from PIL import Image

from native.backend.camera_admission import prepare_solve_dataset_version, validate_solve_dataset_version
from native.backend.camera_dataset import CameraDatasetError


class CameraAdmissionPathTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_PATH_TEST_ROOT",
                    "D:/Apps/GSW/validation/acceptance-followup-oct22/paths/unittest"))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        views = []
        for index, split in enumerate(("train", "train", "val", "test")):
            image = self.source / ("image-%d.png" % index)
            Image.new("RGB", (11, 9), (20 + index, 40, 60)).save(image)
            views.append({"view_id": "view/%d 日本語" % index,
                "image_path": image.name, "K": [[14., 0., 5.5], [0., 15., 4.5], [0., 0., 1.]],
                "pixel_coordinates": "pixel_border", "split": split,
                "c2w_opencv": [[1., 0., 0., index * .2], [0., 1., 0., .1],
                               [0., 0., 1., -.6], [0., 0., 0., 1.]]})
        self.manifest = self.source / "camera_dataset.json"
        self.manifest.write_text(json.dumps({"schema": "gsw.camera_dataset/1",
            "pose_policy": "locked", "source": {"kind": "synthetic"},
            "scale": {"status": "arbitrary"}, "views": views}), encoding="utf-8")
        self.originals = {item.name: hashlib.sha256(item.read_bytes()).hexdigest()
                          for item in self.source.iterdir()}

    def assert_originals_unchanged(self):
        self.assertEqual({item.name: hashlib.sha256(item.read_bytes()).hexdigest()
                          for item in self.source.iterdir()}, self.originals)

    def test_supported_final_paths_do_not_fail_because_internal_staging_grows(self):
        parent = self.root / "日本語 space"
        parent.mkdir()
        # Exercise a normal final path whose historical preparation suffix
        # pushed image copies beyond the legacy Windows API boundary.
        if len(str(parent)) < 124:
            parent = parent / ("p" * (123 - len(str(parent))))
            parent.mkdir()
        destination = parent / ("result" + "r" * 26)
        final_image_upper_bound = destination / "input" / ("view-" + "a" * 64 + ".png")
        self.assertLess(len(str(final_image_upper_bound)), 260)
        result = prepare_solve_dataset_version(self.manifest, destination)
        self.assertEqual(Path(result["datasetPath"]), destination)
        self.assertEqual(len(result["files"]), 2)
        self.assertEqual({entry["sourceSplit"] for entry in result["files"]}, {"train"})
        for entry in result["files"]:
            self.assertEqual((destination / entry["inputImagePath"]).read_bytes(),
                             (self.source / entry["sourceImagePath"]).read_bytes())
        self.assert_originals_unchanged()

    def test_solve_names_leave_legacy_path_room_for_downstream_undistortion(self):
        parent = self.root / ("p" * max(1, 123 - len(str(self.root))))
        parent.mkdir()
        destination = parent / ("output" + "r" * 26)
        result = prepare_solve_dataset_version(self.manifest, destination)
        for record in result["files"]:
            # The actual COLMAP undistorter publishes into this UUID sibling
            # layout; checking input alone misses its longer output path.
            downstream = (destination / (".colmap-undistorted-" + "0" * 32)
                          / "images" / Path(record["inputImagePath"]).name)
            self.assertLess(len(str(downstream).encode("utf-16-le")) // 2, 260)
        other = prepare_solve_dataset_version(self.manifest, self.root / "other")
        self.assertEqual(result["files"], other["files"])
        self.assertEqual(result["solveDataIdentity"], other["solveDataIdentity"])
        self.assert_originals_unchanged()

    @unittest.skipUnless(os.name == "nt", "legacy Windows API path budget")
    def test_excessive_generated_path_is_rejected_before_copy_or_parent_creation(self):
        parent = self.root / ("p" * (231 - len(str(self.root))))
        destination = parent / "result"
        self.assertFalse(parent.exists())
        with self.assertRaises(CameraDatasetError) as raised:
            prepare_solve_dataset_version(self.manifest, destination)
        self.assertEqual(raised.exception.code, "camera_solve_path_budget_exceeded")
        detail = json.loads(raised.exception.detail)
        self.assertEqual(detail["unit"], "utf16_code_units")
        self.assertIn(detail["kind"], ("file", "directory"))
        self.assertGreater(detail["actual"], detail["limit"])
        self.assertFalse(parent.exists())
        self.assert_originals_unchanged()

    @unittest.skipUnless(os.name == "nt", "UTF-16 Windows API path budget")
    def test_windows_budget_counts_surrogate_pairs_not_python_characters(self):
        count = 1
        while True:
            parent = self.root / ("\U0001f4f7" * count)
            destination = parent / "result"
            metadata = destination / "camera-solve-provenance.json"
            if len(str(metadata).encode("utf-16-le")) // 2 > 259:
                break
            count += 1
        self.assertLess(len(str(metadata)), 260)
        with self.assertRaises(CameraDatasetError) as raised:
            prepare_solve_dataset_version(self.manifest, destination)
        self.assertEqual(raised.exception.code, "camera_solve_path_budget_exceeded")
        self.assertFalse(parent.exists())
        self.assert_originals_unchanged()

    def test_parallel_publish_has_one_complete_winner_and_preserves_foreign_data(self):
        parent = self.root / "concurrent"
        parent.mkdir()
        foreign = parent / "unrelated.txt"
        foreign.write_bytes(b"not owned by either solve")
        destination = parent / "result"
        start = threading.Barrier(2)

        def attempt():
            start.wait(timeout=5)
            try:
                return prepare_solve_dataset_version(self.manifest, destination)
            except (CameraDatasetError, FileExistsError):
                return None

        with ThreadPoolExecutor(max_workers=2) as executor:
            results = list(executor.map(lambda unused: attempt(), range(2)))
        winners = [result for result in results if result is not None]
        self.assertEqual(len(winners), 1)
        metadata = json.loads(Path(winners[0]["provenancePath"]).read_text(encoding="utf-8"))
        self.assertEqual(metadata["solveDataIdentity"], winners[0]["solveDataIdentity"])
        for record in metadata["files"]:
            self.assertEqual(hashlib.sha256((destination / record["inputImagePath"]).read_bytes()).hexdigest(),
                             record["sha256"])
        self.assertEqual(set(parent.iterdir()), {destination, foreign})
        self.assertEqual(foreign.read_bytes(), b"not owned by either solve")
        self.assert_originals_unchanged()

    def test_cancel_during_real_output_write_cleans_own_stage_and_retry_succeeds(self):
        parent = self.root / "cancel"
        parent.mkdir()
        foreign = parent / "preserve.txt"
        foreign.write_bytes(b"existing unrelated file")
        destination = parent / "result"
        cancel = threading.Event()
        real_open = io.open

        def interrupt_output(file, mode="r", *args, **kwargs):
            stream = real_open(file, mode, *args, **kwargs)
            if mode == "xb" and isinstance(file, (str, bytes, os.PathLike)):
                cancel.set()
            return stream

        # Inject cancellation at the filesystem boundary after a real output
        # file opens; no internal camera module or fake file is substituted.
        with mock.patch.object(io, "open", side_effect=interrupt_output):
            with self.assertRaises(CameraDatasetError) as raised:
                prepare_solve_dataset_version(self.manifest, destination, cancelled=cancel.is_set)
        self.assertEqual(raised.exception.code, "camera_import_cancelled")
        self.assertFalse(destination.exists())
        self.assertEqual(set(parent.iterdir()), {foreign})
        result = prepare_solve_dataset_version(self.manifest, destination)
        self.assertEqual(len(result["files"]), 2)
        self.assertEqual(foreign.read_bytes(), b"existing unrelated file")
        with self.assertRaises(CameraDatasetError) as existing:
            prepare_solve_dataset_version(self.manifest, destination)
        self.assertEqual(existing.exception.code, "camera_solve_destination_exists")
        self.assert_originals_unchanged()

    def test_legacy_long_image_names_remain_admissible_for_existing_resume(self):
        output = self.root / "legacy-output"
        destination = output / ".gsw" / "solve-dataset"
        result = prepare_solve_dataset_version(self.manifest, destination)
        provenance_path = Path(result["provenancePath"])
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        # A valid pre-upgrade version is a saved artifact, not an instruction
        # to rename/reprepare images during full-state resume.
        for record in provenance["files"]:
            old = destination / record["inputImagePath"]
            legacy = "input/view-" + hashlib.sha256(record["viewId"].encode("utf-8")).hexdigest() + old.suffix
            old.rename(destination / legacy)
            record["inputImagePath"] = legacy
        semantic = {key: value for key, value in provenance.items()
                    if key not in ("version", "solveDataIdentity", "sourceManifestPath")}
        provenance["solveDataIdentity"] = hashlib.sha256(json.dumps(
            semantic, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()
        provenance_path.write_text(json.dumps(provenance), encoding="utf-8")
        before = {path.relative_to(destination).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in destination.rglob("*") if path.is_file()}
        accepted = validate_solve_dataset_version(destination, output, result["sourceDataIdentity"])
        self.assertEqual(accepted["files"], provenance["files"])
        self.assertEqual(before, {path.relative_to(destination).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in destination.rglob("*") if path.is_file()})
        self.assert_originals_unchanged()


if __name__ == "__main__":
    unittest.main()
