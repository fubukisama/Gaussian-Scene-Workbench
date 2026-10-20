"""Persisted camera evidence keeps its identity across Python float-sum policies."""

import copy
import os
import struct
import shutil
from pathlib import Path
import tempfile
import unittest

from PIL import Image

from native.backend import camera_dataset


# A valid near-quarter-turn rotation with finite binary64 entries. The inverse
# x translation has two legitimate historical sum results, separated by 1 ULP.
TRANSFORM = {"s": 2.999999999999999, "R": [
    [-5.551115123125783e-16, -1., -1.0547118733938987e-15],
    [1., -1.6653345369377348e-16, 6.106226635438361e-16],
    [-3.885780586188048e-16, -9.43689570931383e-16, 1.]],
    "t": [.999999999999999, 2.0000000000000004, 3.]}
IDENTITY = [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]]


class CameraIdentityPortabilityTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_CAMERA_P1_TEST_ROOT", tempfile.gettempdir())) / "gsw-camera-identity"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        Image.new("RGB", (16, 12), (20, 30, 40)).save(self.root / "image.png")

    def document(self, transform=None):
        raw = {"schema": camera_dataset.SCHEMA, "pose_policy": "locked", "source": {"kind": "manual"},
               "scale": {"status": "arbitrary"}, "transforms": {
                   "source_to_canonical": copy.deepcopy(transform or TRANSFORM)},
               "views": [{"image_path": "image.png", "pixel_coordinates": "pixel_border", "split": "train",
                          "K": [[20., 0., 8.], [0., 21., 6.], [0., 0., 1.]],
                          "c2w_opencv": copy.deepcopy(IDENTITY)}]}
        path = self.root / "input.json"
        camera_dataset.write_camera_dataset(raw, path)
        return camera_dataset.load_camera_dataset(path)

    def test_checked_inverse_from_either_historical_sum_policy_keeps_exact_serialized_identity(self):
        normalized = self.document()
        for index, x in enumerate((-.6666666666666665, -.6666666666666664)):
            with self.subTest(inverse_x=x):
                record = copy.deepcopy(normalized)
                record["transforms"]["source_to_canonical"]["inverse"]["t"][0] = x
                record = camera_dataset.refresh_dataset_identity(record)
                path = self.root / ("serialized-" + str(index) + ".json")
                camera_dataset.write_camera_dataset(record, path)
                self.assertEqual(camera_dataset.load_camera_dataset(path), record)

    def test_checked_stored_canonical_pose_keeps_exact_binary64_values_and_identity(self):
        record = self.document()
        value = record["views"][0]["c2w_opencv"][0][3]
        bits, = struct.unpack("<Q", struct.pack("<d", value))
        next_value, = struct.unpack("<d", struct.pack("<Q", bits + 1))
        record["views"][0]["c2w_opencv"][0][3] = next_value
        record = camera_dataset.refresh_dataset_identity(record)
        path = self.root / "canonical-pose.json"
        camera_dataset.write_camera_dataset(record, path)
        self.assertEqual(camera_dataset.load_camera_dataset(path), record)

    def test_one_ulp_tamper_with_old_semantic_sha_is_refused_before_normalization(self):
        original = self.document()
        for field in ("K", "inverse", "pose"):
            with self.subTest(field=field):
                record = copy.deepcopy(original)
                sequence, index = ((record["views"][0]["K"][0], 0) if field == "K" else
                    (record["transforms"]["source_to_canonical"]["inverse"]["t"], 0) if field == "inverse" else
                    (record["views"][0]["c2w_opencv"][0], 3))
                bits, = struct.unpack("<Q", struct.pack("<d", sequence[index]))
                sequence[index], = struct.unpack("<d", struct.pack("<Q", bits + 1))
                with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "camera_data_identity_mismatch"):
                    camera_dataset.validate_camera_dataset(record, self.root)

    def test_supplied_inverse_must_remain_positive_even_at_tiny_reciprocal_scale(self):
        transform = {"s": 1e14, "inverse": {"s": -1e-14,
            "R": [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]], "t": [0., 0., 0.]}}
        with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "camera_conflicting_inverse"):
            self.document(transform)

    def test_inverse_and_canonical_pose_outside_original_tolerance_refuse_even_with_new_sha(self):
        normalized = self.document()
        for field in ("inverse", "pose"):
            with self.subTest(field=field):
                record = copy.deepcopy(normalized)
                if field == "inverse":
                    record["transforms"]["source_to_canonical"]["inverse"]["t"][0] += 1e-5
                else:
                    record["views"][0]["c2w_opencv"][0][3] += 1e-5
                record = camera_dataset.refresh_dataset_identity(record)
                with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "camera_conflicting_(inverse|pose)"):
                    camera_dataset.validate_camera_dataset(record, self.root)

    def derived_version(self):
        from native.worker.scale_constraints import create_scaled_dataset_version
        original = self.document()
        source = self.root / "source"
        source.mkdir()
        shutil.copyfile(str(self.root / "image.png"), str(source / "image.png"))
        camera_dataset.write_camera_dataset(original, source / camera_dataset.MANIFEST_NAME)
        source_points = [[0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
        reference_points = [[1., 2., 3.], [1., 5., 3.], [-2., 2., 3.], [1., 2., 6.]]
        request = {"schema": "gsw.scale_constraints/1", "input_data_identity": original["data_identity"],
                   "input_frame": "source", "mode": "sim3", "fit_tolerance_m": 1e-8,
                   "constraints": [{"id": "fit-" + str(index), "kind": "control", "role": "fit",
                       "reference_id": "control-" + str(index), "reference_unit": "m",
                       "source_xyz": point, "reference_xyz": reference_points[index]}
                       for index, point in enumerate(source_points)]}
        destination = self.root / "derived"
        create_scaled_dataset_version(source, request, destination, original["data_identity"])
        return destination, camera_dataset.load_camera_dataset(destination)

    def test_derived_replay_preserves_checked_redundant_inverse_and_canonical_pose_only(self):
        destination, normalized = self.derived_version()
        for field in ("inverse", "pose"):
            with self.subTest(field=field):
                record = copy.deepcopy(normalized)
                sequence = (record["transforms"]["source_to_canonical"]["inverse"]["t"] if field == "inverse"
                            else record["views"][0]["c2w_opencv"][0])
                index = 0 if field == "inverse" else 3
                bits, = struct.unpack("<Q", struct.pack("<d", sequence[index]))
                sequence[index], = struct.unpack("<d", struct.pack("<Q", bits + 1))
                record = camera_dataset.refresh_dataset_identity(record)
                camera_dataset.write_camera_dataset(record, destination / camera_dataset.MANIFEST_NAME)
                self.assertEqual(camera_dataset.load_camera_dataset(destination), record)

    def test_derived_replay_still_rejects_changed_source_forward_transform_request_and_lineage(self):
        destination, normalized = self.derived_version()
        for field in ("source_K", "source_pose", "forward", "forward_one_ulp", "request", "lineage"):
            with self.subTest(field=field):
                record = copy.deepcopy(normalized)
                view = record["views"][0]
                transform = record["transforms"]["source_to_canonical"]
                if field == "source_K":
                    view["source_K"][0][0] += .25
                    view["K"][0][0] += .25
                elif field == "source_pose":
                    view["source_c2w_opencv"][0][3] += 1e-3
                elif field in ("forward", "forward_one_ulp"):
                    if field == "forward":
                        transform["t"][0] += 1e-3
                    else:
                        bits, = struct.unpack("<Q", struct.pack("<d", transform["s"]))
                        transform["s"], = struct.unpack("<d", struct.pack("<Q", bits + 1))
                    transform.pop("inverse")
                    inverse = camera_dataset.invert_sim3(transform)
                    transform["inverse"] = {key: inverse[key] for key in ("s", "R", "t")}
                elif field == "request":
                    record["scale"]["derivation"]["request"]["constraints"][0]["role"] = "check"
                else:
                    record["scale"]["derivation"]["lineage"][0]["scale_status"] = "source_declared"
                if field in ("source_pose", "forward", "forward_one_ulp"):
                    center = camera_dataset.apply_sim3(transform,
                        [[view["source_c2w_opencv"][i][3] for i in range(3)]])[0]
                    for index in range(3):
                        view["c2w_opencv"][index][3] = center[index]
                record = camera_dataset.refresh_dataset_identity(record)
                camera_dataset.write_camera_dataset(record, destination / camera_dataset.MANIFEST_NAME)
                with self.assertRaisesRegex(camera_dataset.CameraDatasetError,
                        "camera_scale_(derived_dataset_mismatch|derivation_replay_failed|invalid_lineage)"):
                    camera_dataset.load_camera_dataset(destination)

    def test_nonfinite_direct_input_with_claimed_identity_remains_a_typed_error(self):
        record = self.document()
        record["views"][0]["K"][0][0] = float("nan")
        with self.assertRaises(camera_dataset.CameraDatasetError):
            camera_dataset.validate_camera_dataset(record, self.root)

    def test_retained_fields_cannot_consume_large_origin_relative_tolerance_as_coordinate_edits(self):
        for field in ("inverse_translation", "canonical_translation", "inverse_rotation", "inverse_scale"):
            with self.subTest(field=field):
                record = self.document({"s": 1e14} if field == "inverse_scale"
                                       else {"s": 1., "t": [1e6, 0., 0.]})
                inverse = record["transforms"]["source_to_canonical"]["inverse"]
                if field == "inverse_translation":
                    inverse["t"][0] += 5e-5
                elif field == "canonical_translation":
                    record["views"][0]["c2w_opencv"][0][3] += 5e-7
                elif field == "inverse_rotation":
                    inverse["R"][0][0] += 1e-13
                else:
                    inverse["s"] = 2e-14
                record = camera_dataset.refresh_dataset_identity(record)
                with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "camera_conflicting_(inverse|pose)"):
                    camera_dataset.validate_camera_dataset(record, self.root)

    def test_nonfinite_arithmetic_magnitude_is_refused_not_an_unbounded_tolerance(self):
        record = self.document({"s": 1., "t": [1e308, 0., 0.]})
        record["views"][0]["source_c2w_opencv"][0][3] = -1e308
        record["views"][0]["c2w_opencv"][0][3] = 0.
        record = camera_dataset.refresh_dataset_identity(record)
        with self.assertRaisesRegex(camera_dataset.CameraDatasetError, "camera_numeric_range_exceeded"):
            camera_dataset.validate_camera_dataset(record, self.root)

    def canonical_initialization_versions(self, large_origin=False):
        from native.worker.scale_constraints import create_scaled_dataset_version
        source = self.root / "canonical-source"
        source.mkdir()
        shutil.copyfile(str(self.root / "image.png"), str(source / "image.png"))
        (source / "initialization.ply").write_text(
            "ply\nformat ascii 1.0\nelement vertex 4\nproperty double x\nproperty double y\n"
            "property double z\nend_header\n0.1 0.2 0.3\n0.3 0.2 0.7\n0.8 0.5 0.4\n0.2 0.9 0.1\n",
            encoding="ascii")
        source_rotation = [[.36, -.48, .8], [.8, .6, 0.], [-.48, .64, .6]]
        raw = {"schema": camera_dataset.SCHEMA, "source": {"kind": "manual"}, "pose_policy": "locked",
            "scale": {"status": "arbitrary"}, "initialization": {"kind": "external_point_cloud",
                "frame": "canonical", "path": "initialization.ply", "priors": ["external_geometry"]},
            "transforms": {"source_to_canonical": {"s": .733, "R": source_rotation,
                "t": [1e6, -2e6, 3e6] if large_origin else [.13, -.29, .61]}},
            "views": [{"image_path": "image.png", "K": [[20., 0., 8.], [0., 21., 6.], [0., 0., 1.]],
                "pixel_coordinates": "pixel_border", "split": "train", "c2w_opencv": copy.deepcopy(IDENTITY)}]}
        camera_dataset.write_camera_dataset(raw, source / camera_dataset.MANIFEST_NAME)
        original_point_bytes = (source / "initialization.ply").read_bytes()
        points = [[.123, 2.345, -1.789], [3.12, .17, -2.67], [-1.67, 4.27, 3.21],
                  [4.82, -5.91, 6.34], [2.37, -3.69, -7.13]]
        transforms = [(3.14159, source_rotation, [2.13, -4.31, 5.79]),
                      (4.123, [[.6, 0., .8], [0., 1., 0.], [-.8, 0., .6]], [-7.89, 1.23, -3.45])]
        versions = []
        for index, (scale, rotation, translation) in enumerate(transforms):
            admitted = camera_dataset.load_camera_dataset(source)
            request = {"schema": "gsw.scale_constraints/1", "input_data_identity": admitted["data_identity"],
                "input_frame": "source", "mode": "sim3", "fit_tolerance_m": 1e-10,
                "constraints": [{"id": "p" + str(i), "reference_id": "p" + str(i), "role": "fit",
                    "kind": "control", "reference_unit": "m", "source_xyz": point,
                    "reference_xyz": [scale * sum(rotation[j][k] * point[k] for k in range(3)) + translation[j]
                                      for j in range(3)]} for i, point in enumerate(points)]}
            destination = self.root / ("canonical-derived-" + str(index))
            create_scaled_dataset_version(source, request, destination, admitted["data_identity"])
            versions.append((destination, camera_dataset.load_camera_dataset(destination)))
            self.assertEqual((destination / "initialization.ply").read_bytes(), original_point_bytes)
            source = destination
        return versions

    def test_two_version_canonical_initializer_retains_checked_composition_roundoff(self):
        versions = self.canonical_initialization_versions()
        for destination, normalized in versions:
            for field in ("R", "t"):
                with self.subTest(version=destination.name, field=field):
                    record = copy.deepcopy(normalized)
                    transform = record["initialization"]["input_to_canonical"]
                    if field == "R":
                        transform["R"][0][2] += 2e-16
                    else:
                        transform["t"][1] += 2e-16
                    transform.pop("inverse")
                    inverse = camera_dataset.invert_sim3(transform)
                    transform["inverse"] = {key: inverse[key] for key in ("s", "R", "t")}
                    record = camera_dataset.refresh_dataset_identity(record)
                    camera_dataset.write_camera_dataset(record, destination / camera_dataset.MANIFEST_NAME)
                    self.assertEqual(camera_dataset.load_camera_dataset(destination), record)

    def test_large_origin_derived_initializer_rejects_scale_rotation_and_translation_edits(self):
        destination, normalized = self.canonical_initialization_versions(large_origin=True)[-1]
        for field in ("s", "R", "t"):
            with self.subTest(field=field):
                record = copy.deepcopy(normalized)
                transform = record["initialization"]["input_to_canonical"]
                if field == "s":
                    transform["s"] *= 1.000001
                elif field == "R":
                    transform["R"][0][2] += 5e-7
                else:
                    transform["t"][1] += 5e-5
                transform.pop("inverse")
                inverse = camera_dataset.invert_sim3(transform)
                transform["inverse"] = {key: inverse[key] for key in ("s", "R", "t")}
                record = camera_dataset.refresh_dataset_identity(record)
                camera_dataset.write_camera_dataset(record, destination / camera_dataset.MANIFEST_NAME)
                with self.assertRaisesRegex(camera_dataset.CameraDatasetError,
                        "camera_scale_derived_dataset_mismatch: initialization"):
                    camera_dataset.load_camera_dataset(destination)


if __name__ == "__main__":
    unittest.main()
