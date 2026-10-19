import contextlib
import io
import json
import tempfile
import threading
import sys
import unittest
from pathlib import Path
from unittest import mock

from native.worker import mesh_generation

try:
    import numpy
    from plyfile import PlyElement
    NUMERIC_RUNTIME = hasattr(numpy, "__version__")
except ImportError:
    NUMERIC_RUNTIME = False


MESH = ("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
        "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n")


def texture_fixture(directory):
    from PIL import Image
    files = {name: str(directory / ("texture." + name)) for name in ("obj", "mtl", "png", "zip")}
    Path(files["obj"]).write_text("mtllib texture.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                  "vt 0 0\nvt 1 0\nvt 0 1\nusemtl baked\nf 1/1 2/2 3/3\n")
    Path(files["mtl"]).write_text("newmtl baked\nKd 1 1 1\nmap_Kd texture.png\n")
    Image.new("RGB", (2, 2), (160, 70, 20)).save(files["png"])
    Path(files["zip"]).write_bytes(b"owned texture package")
    return files


class Backend:
    def __init__(self, backend, texture_failure=False, corrupt=False, own_texture=False):
        self.backend = backend
        self.texture_failure = texture_failure
        self.corrupt = corrupt
        self.own_texture = own_texture
        self.MESH_LOCK = threading.Lock()
        self.MESH_JOBS = {}
        self.OUTPUT_DIR = None
        self.calls = []

    def mesh_export_options(self, options):
        return options

    def scene_backend(self, *_):
        return self.backend

    def ensure_mesh_environment(self):
        self.calls.append("2dgs")

    def ensure_sugar_environment(self):
        self.calls.append("sugar")

    def ensure_gs2mesh_environment(self):
        self.calls.append("gs2mesh")

    def ensure_openmvs_environment(self):
        self.calls.append("openmvs")

    def start_mesh_export(self, scene, iteration, options):
        mesh = self.OUTPUT_DIR / scene / "mesh.ply"
        mesh.write_text(MESH.replace("3 0 1 2", "3 0 1 99") if self.corrupt else MESH)
        self.MESH_JOBS["mesh"] = {"status": "done", "output_mesh": str(mesh), "log": ["mesh output"]}
        if self.own_texture:
            self.MESH_JOBS["mesh"]["texture"] = texture_fixture(mesh.parent)
        return {"id": "mesh"}

    def start_texture_bake(self, scene, iteration, options):
        self.calls.append(options["backend"])
        self.MESH_JOBS["texture"] = {"status": "failed" if self.texture_failure else "done", "error": "texture failed"}
        if not self.texture_failure:
            self.MESH_JOBS["texture"]["texture"] = texture_fixture(self.OUTPUT_DIR / scene)
        return {"id": "texture"}

    def cancel_mesh_job(self, job_id):
        self.MESH_JOBS[job_id]["status"] = "cancelled"


class GenerationCanonicalCapabilitiesTests(unittest.TestCase):
    def test_camera_policy_and_downstream_limits_are_declared_per_backend(self):
        from native.worker.generation_capabilities import PIPELINES
        for name in ("3dgs", "2dgs"):
            with self.subTest(backend=name):
                capability = PIPELINES[name]
                self.assertTrue(capability["canonical_camera_input"])
                self.assertEqual(capability["reconstruction_gate"], "explicit_pose_policy")
                self.assertEqual(capability["reconstruction_gates"]["locked"], "canonical_locked_v1")
                self.assertEqual(capability["reconstruction_gates"]["solve"], "shared_colmap_viability")
        for name in ("bounded", "unbounded", "sugar", "gs2mesh", "openmvs"):
            with self.subTest(backend=name):
                capability = PIPELINES[name]
                self.assertFalse(capability["canonical_camera_input"])
                self.assertTrue(capability["canonical_camera_reason"])
                self.assertTrue(capability["canonical_camera_acceptance"])
                self.assertEqual(capability["reconstruction_gate"], "shared_colmap_viability")


@unittest.skipUnless(NUMERIC_RUNTIME, "Run with a training Python containing NumPy and plyfile")
class MeshJobTests(unittest.TestCase):
    def test_canonical_outputs_are_rejected_before_all_external_readers_and_environments(self):
        class TrackedExternalBackend(Backend):
            def scene_backend(self, *args):
                self.calls.append("external_scene_reader")
                return super().scene_backend(*args)

            def start_mesh_export(self, *args):
                self.calls.append("external_mesh_command")
                return super().start_mesh_export(*args)

        original = (self.source / "point_cloud/iteration_7/point_cloud.ply").read_bytes()
        original_configuration = (self.source / "cfg_args").read_bytes()
        previous = self.root / "output" / "previous-result.json"
        previous.parent.mkdir()
        previous.write_bytes(b"previous valid artifact remains")
        markers = [
            (self.source / "training-camera-contract.json", {"schema": "gsw.training_camera_contract/1", "pose_policy": "locked"}),
            (self.source / "point_cloud/iteration_7/point_cloud.camera-scale.json", {"schema": "gsw.training_camera_contract/1"}),
            (self.source / "point_cloud/iteration_7/point_cloud.camera-scale.json", {"schema": "gsw.coordinate_export/1",
                "output_frame": "canonical", "training_contract": {"schema": "gsw.training_camera_contract/1"}}),
        ]
        for marker_index, (marker, metadata) in enumerate(markers):
            marker.write_text(json.dumps(metadata), encoding="utf-8")
            for mode in mesh_generation.MODES:
                with self.subTest(marker=marker.name, mode=mode):
                    self.config.update(mode=mode, runName="canonical-{}-{}".format(marker_index, mode))
                    backend = TrackedExternalBackend(mesh_generation.MODES[mode], own_texture=mode == "sugar")
                    self.assertEqual(self.run_job(backend), 1)
                    record = json.loads((self.root / "output" / self.config["runName"] / "result.json").read_text())
                    self.assertEqual(record.get("errorCode"), "camera_dataset_mesh_backend_unavailable")
                    self.assertEqual(record["state"], "failed")
                    self.assertEqual(record["completedStages"], [])
                    self.assertFalse((self.root / "output" / self.config["runName"] / self.config["runName"]).exists())
                    self.assertEqual(backend.calls, [])
                    self.assertEqual(backend.MESH_JOBS, {})
                    self.assertEqual(self.events[-1][3]["generation_issue"], "camera_dataset_mesh_backend_unavailable")
            marker.unlink()
        self.assertEqual((self.source / "point_cloud/iteration_7/point_cloud.ply").read_bytes(), original)
        self.assertEqual((self.source / "cfg_args").read_bytes(), original_configuration)
        self.assertEqual(previous.read_bytes(), b"previous valid artifact remains")

    def test_corrupt_canonical_marker_does_not_fall_back_to_a_legacy_reader(self):
        for index, relative in enumerate(("training-camera-contract.json", "point_cloud/iteration_7/point_cloud.camera-scale.json")):
            marker = self.source / relative
            marker.write_bytes(b"{truncated canonical metadata")
            self.config["runName"] = "corrupt-contract-" + str(index)
            backend = Backend("2dgs")
            self.assertEqual(self.run_job(backend), 1)
            record = json.loads((self.root / "output" / self.config["runName"] / "result.json").read_text())
            self.assertEqual(record.get("errorCode"), "camera_dataset_mesh_backend_unavailable")
            self.assertEqual(backend.calls, [])
            marker.unlink()

    def test_incidental_sparse_does_not_override_canonical_downstream_rejection(self):
        dataset = self.root / "managed-dataset"
        (dataset / "sparse/0").mkdir(parents=True)
        (dataset / "sparse/0/cameras.bin").write_bytes(b"not a camera source for this canonical model")
        (self.source / "cfg_args").write_text("Namespace(source_path={!r})".format(str(dataset)))
        (self.source / "training-camera-contract.json").write_text('{"schema":"gsw.training_camera_contract/1"}')
        for mode in mesh_generation.MODES:
            self.config.update(mode=mode, runName="incidental-sparse-" + mode)
            backend = Backend(mesh_generation.MODES[mode])
            self.assertEqual(self.run_job(backend), 1)
            record = json.loads((self.root / "output" / self.config["runName"] / "result.json").read_text())
            self.assertEqual(record["errorCode"], "camera_dataset_mesh_backend_unavailable")
            self.assertNotIn("reconstructionQuality", record)
            self.assertEqual(backend.calls, [])

    def test_unselected_iteration_and_ordinary_projection_metadata_do_not_ban_legacy_mesh(self):
        other = self.source / "point_cloud/iteration_6"
        other.mkdir()
        (other / "point_cloud.camera-scale.json").write_text('{"schema":"gsw.training_camera_contract/1"}')
        (self.source / "point_cloud/iteration_7/point_cloud.prj").write_text('LOCAL_CS["legacy",UNIT["metre",1]]')
        self.assertEqual(self.run_job(Backend("2dgs")), 0)
        self.assertEqual(self.record()["completedStages"], ["mesh"])

    def test_all_mesh_methods_reject_degenerate_colmap_before_external_work(self):
        from native.worker.test_reconstruction_quality import write_model
        with tempfile.TemporaryDirectory() as tmp:
            dataset = Path(tmp) / "dataset"
            (dataset / "images").mkdir(parents=True)
            for i in range(9):
                (dataset / "images" / ("frame%02d.jpg" % i)).write_bytes(b"raw")
            write_model(dataset / "sparse" / "0", 2, 31)
            (self.source / "cfg_args").write_text("Namespace(source_path={})".format(repr(str(dataset))))
            for mode in ("bounded", "unbounded", "sugar", "gs2mesh"):
                with self.subTest(mode=mode):
                    backend = Backend("2dgs" if mode in ("bounded", "unbounded") else "3dgs")
                    events = []
                    config = dict(self.config, mode=mode, runName="quality-" + mode)
                    with mock.patch.object(sys, "stdin", io.StringIO()):
                        result = mesh_generation.run(config, backend, lambda *args: events.append(args))
                    self.assertEqual(result, 1)
                    self.assertEqual(backend.calls, [])
                    self.assertEqual(events[-1][3]["generation_issue"], "reconstruction_quality")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gsw-mesh-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        cloud = self.source / "point_cloud/iteration_7"
        cloud.mkdir(parents=True)
        (cloud / "point_cloud.ply").write_bytes(b"training input is immutable")
        (self.source / "cfg_args").write_text("Namespace(source_path='owned-dataset')")
        self.config = {"modelDirectory": str(self.source), "outputRoot": str(self.root / "output"),
                       "runName": "mesh-owned-test", "iteration": 7, "mode": "bounded"}
        self.events = []

    def run_job(self, backend, stdin=""):
        with mock.patch("sys.stdin", io.StringIO(stdin)), contextlib.redirect_stdout(io.StringIO()):
            return mesh_generation.run(self.config, backend, lambda *args: self.events.append(args))

    def record(self):
        return json.loads((self.root / "output/mesh-owned-test/result.json").read_text())

    def test_all_mesh_methods_have_isolated_typed_jobs(self):
        # Loop goes through the production orchestration for every source/backend pair.
        for index, (mode, source_backend) in enumerate(mesh_generation.MODES.items()):
            with self.subTest(mode=mode):
                self.config.update(mode=mode, runName="mesh-test-" + str(index))
                self.assertEqual(self.run_job(Backend(source_backend, own_texture=mode == "sugar")), 0)
                record = json.loads((self.root / "output" / self.config["runName"] / "result.json").read_text())
                self.assertEqual(record["completedStages"], ["mesh", "texture", "texture_preview"] if mode == "sugar" else ["mesh"])
                self.assertEqual(record["geometry"], {"vertices": 3, "faces": 1})
                self.assertEqual((self.source / "point_cloud/iteration_7/point_cloud.ply").read_bytes(), b"training input is immutable")

    def test_texture_failure_keeps_validated_mesh_but_is_not_success(self):
        self.config["bakeTexture"] = True
        self.assertEqual(self.run_job(Backend("2dgs", texture_failure=True)), 1)
        record = self.record()
        self.assertEqual(record["state"], "failed")
        self.assertEqual(record["completedStages"], ["mesh"])
        self.assertTrue(Path(record["meshPath"]).is_file())
        self.assertFalse(any(event[0] == "done" for event in self.events))
        self.assertTrue(any(len(event) > 3 and event[3] and event[3].get("previewKind") == "mesh" for event in self.events))

    def test_texture_finishes_after_mesh_without_training_checkpoint_claim(self):
        self.config.update(mode="gs2mesh", bakeTexture=True)
        backend = Backend("3dgs")
        self.assertEqual(self.run_job(backend), 0)
        self.assertEqual(self.record()["completedStages"], ["mesh", "texture", "texture_preview"])
        material = self.record()["materialPreview"]
        self.assertEqual(mesh_generation.validate_mesh(material["ply"], textured=True), {"vertices": 3, "faces": 1})
        self.assertTrue(Path(material["atlas"]).is_file())
        self.assertIn("openmvs", backend.calls)
        self.assertFalse((self.root / "output/mesh-owned-test/.gsw-resume").exists())

    def test_shared_material_preview_covers_every_mesh_backend(self):
        for index, (mode, backend_name) in enumerate(mesh_generation.MODES.items()):
            with self.subTest(mode=mode):
                self.config.update(mode=mode, runName="material-test-" + str(index), bakeTexture=mode != "sugar")
                backend = Backend(backend_name, own_texture=mode == "sugar")
                self.assertEqual(self.run_job(backend), 0)
                record = json.loads((self.root / "output" / self.config["runName"] / "result.json").read_text())
                self.assertEqual(record["completedStages"], ["mesh", "texture", "texture_preview"])
                self.assertEqual(self.events[-1][3]["previewPath"], record["materialPreview"]["ply"])
                self.assertEqual((self.source / "point_cloud/iteration_7/point_cloud.ply").read_bytes(), b"training input is immutable")

    def test_material_adapter_failure_retains_completed_mesh_and_texture(self):
        self.config["bakeTexture"] = True
        backend = Backend("2dgs")
        original = backend.start_texture_bake
        def invalid(*args):
            result = original(*args)
            Path(backend.MESH_JOBS["texture"]["texture"]["obj"]).write_text("v 0 0 0\nf 1 2 3\n")
            return result
        backend.start_texture_bake = invalid
        self.assertEqual(self.run_job(backend), 1)
        record = self.record()
        self.assertEqual(record["completedStages"], ["mesh", "texture"])
        self.assertEqual(record["state"], "failed")
        self.assertTrue(Path(record["meshPath"]).is_file())
        self.assertNotIn("materialPreview", record)
        self.assertFalse(any(event[0] == "done" for event in self.events))

    def test_material_conversion_cancellation_retains_prior_stages(self):
        self.config["bakeTexture"] = True
        with mock.patch.object(mesh_generation, "prepare_material_preview", side_effect=InterruptedError("cancelled")):
            self.assertEqual(self.run_job(Backend("2dgs")), 130)
        self.assertEqual(self.record()["completedStages"], ["mesh", "texture"])
        self.assertEqual(self.record()["state"], "cancelled")
        self.assertNotIn("materialPreview", self.record())

    def test_empty_successful_texture_bundle_is_not_success(self):
        self.config["bakeTexture"] = True
        backend = Backend("2dgs")
        def empty(*args):
            backend.MESH_JOBS["texture"] = {"status": "done", "texture": {}}
            return {"id": "texture"}
        backend.start_texture_bake = empty
        self.assertEqual(self.run_job(backend), 1)
        self.assertEqual(self.record()["completedStages"], ["mesh"])
        self.assertEqual(self.record()["state"], "failed")

    def test_sugar_missing_owned_materials_keeps_only_validated_geometry(self):
        self.config["mode"] = "sugar"
        self.assertEqual(self.run_job(Backend("3dgs")), 1)
        self.assertEqual(self.record()["completedStages"], ["mesh"])
        self.assertTrue(Path(self.record()["meshPath"]).is_file())
        self.assertEqual(self.record()["state"], "failed")

    def test_invalid_face_indices_fail_before_preview(self):
        self.assertEqual(self.run_job(Backend("2dgs", corrupt=True)), 1)
        self.assertEqual(self.record()["completedStages"], [])
        self.assertNotIn("meshPath", self.record())

    def test_texture_cancel_preserves_completed_geometry(self):
        self.config["bakeTexture"] = True
        backend = Backend("2dgs")
        def cancelled(*args):
            backend.MESH_JOBS["texture"] = {"status": "cancelled", "log": []}
            return {"id": "texture"}
        backend.start_texture_bake = cancelled
        self.assertEqual(self.run_job(backend), 130)
        self.assertEqual(self.record()["state"], "cancelled")
        self.assertEqual(self.record()["completedStages"], ["mesh"])
        self.assertTrue(Path(self.record()["meshPath"]).is_file())

    def test_missing_texture_asset_is_not_published_as_success(self):
        self.config["bakeTexture"] = True
        backend = Backend("2dgs")
        original = backend.start_texture_bake
        def incomplete(*args):
            result = original(*args)
            Path(backend.MESH_JOBS["texture"]["texture"]["png"]).unlink()
            return result
        backend.start_texture_bake = incomplete
        self.assertEqual(self.run_job(backend), 1)
        self.assertEqual(self.record()["completedStages"], ["mesh"])
        self.assertEqual(self.record()["state"], "failed")

    def test_backend_mismatch_and_duplicate_run_never_overwrite(self):
        self.assertEqual(self.run_job(Backend("3dgs")), 1)
        previous = (self.root / "output/mesh-owned-test/result.json").read_bytes()
        with self.assertRaises(FileExistsError):
            self.run_job(Backend("2dgs"))
        self.assertEqual((self.root / "output/mesh-owned-test/result.json").read_bytes(), previous)

    def test_cancel_before_generation_stops_at_boundary(self):
        self.assertEqual(self.run_job(Backend("2dgs"), "cancel\n"), 130)
        self.assertEqual(self.record()["state"], "cancelled")
        self.assertNotIn("meshPath", self.record())

    def test_reject_path_escape_before_creating_run(self):
        self.config["runName"] = "../escape"
        with self.assertRaises(ValueError):
            self.run_job(Backend("2dgs"))
        self.assertFalse((self.root / "escape").exists())

    def test_configuration_cannot_execute_python_from_imported_directory(self):
        marker = self.root / "must-not-exist"
        (self.source / "cfg_args").write_text("Namespace(source_path=__import__('pathlib').Path({!r}).touch())".format(str(marker)))
        backend = Backend("2dgs")
        self.assertEqual(self.run_job(backend), 1)
        self.assertFalse(marker.exists())
        self.assertFalse(backend.MESH_JOBS)
        self.assertEqual(self.record()["completedStages"], [])


if __name__ == "__main__":
    unittest.main()
