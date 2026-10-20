"""Actual CPU subprocess imports at the public geometry provenance seam."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class GeometryRuntimeProvenanceTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_GEOMETRY_TEST_ROOT", tempfile.gettempdir()))
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="geometry-runtime-", dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.runtime = self.root / "selected-backend"
        for package in ("scene", "utils"):
            (self.runtime / package).mkdir(parents=True)
            (self.runtime / package / "__init__.py").write_text("# owned CPU import fixture\n", encoding="utf-8")
        (self.runtime / "utils/general_utils.py").write_text("identity = 'owned helper one'\n", encoding="utf-8")
        (self.runtime / "scene/gaussian_model.py").write_text(
            "import numpy as np\nfrom utils.general_utils import identity\n"
            "class GaussianModel:\n"
            " def __init__(self, dimensions=3):\n"
            "  self._xyz=np.zeros((2,3),dtype=np.float32)\n"
            "  self._features_dc=np.zeros((2,1,3),dtype=np.float32)\n"
            "  self._features_rest=np.zeros((2,0,3),dtype=np.float32)\n"
            "  self._opacity=np.zeros((2,1),dtype=np.float32)\n"
            "  self._scaling=np.zeros((2,dimensions),dtype=np.float32)\n"
            "  self._rotation=np.zeros((2,4),dtype=np.float32)\n"
            "  self.max_sh_degree=self.active_sh_degree=0\n"
            "  self.optimizer=None\n", encoding="utf-8")

    def probe(self, tail):
        repository = Path(__file__).resolve().parents[2]
        script = "\n".join([
            "import json,sys", "from pathlib import Path",
            "sys.path.insert(0,sys.argv[1])", "sys.path.insert(0,sys.argv[2])",
            "from native.worker.gs_geometry_runtime import capture_loaded_backend_model,verify_loaded_backend_model_sources",
            "from native.backend.gs_geometry import GsGeometryError",
            "try:", *[" " + line for line in tail.splitlines()],
            "except GsGeometryError as error:", " print(json.dumps({'ok':False,'code':error.code,'detail':error.detail}))"])
        result = subprocess.run([sys.executable, "-B", "-c", script, str(repository), str(self.runtime)],
            capture_output=True, text=True, encoding="utf-8", timeout=20,
            env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"),
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout.strip().splitlines()[-1])

    def test_actual_loaded_model_and_helper_source_are_recorded_without_creating_an_optimizer(self):
        result = self.probe("from scene.gaussian_model import GaussianModel\n"
            "record=capture_loaded_backend_model(GaussianModel(),Path(sys.argv[2]),'3dgs',2,0)\n"
            "verify_loaded_backend_model_sources(record)\nprint(json.dumps({'ok':True,'record':record}))")
        self.assertTrue(result["ok"], result)
        record = result["record"]
        self.assertEqual(record["class_module"], "scene.gaussian_model")
        self.assertEqual(record["class_name"], "GaussianModel")
        self.assertEqual(record["point_count"], 2)
        self.assertEqual(record["scale_dimensions"], 3)
        self.assertEqual(record["sh_degree"], 0)
        self.assertFalse(record["optimizer_created"])
        self.assertEqual(record["source_hash_mode"], "raw-file-v1")
        self.assertEqual(record["source_files"]["scene.gaussian_model"]["path"], "scene/gaussian_model.py")
        self.assertEqual(record["source_files"]["utils.general_utils"]["path"], "utils/general_utils.py")
        self.assertTrue(all(len(value["sha256"]) == 64 for value in record["source_files"].values()))
        self.assertEqual(record["tensor_dtypes"]["_xyz"], "float32")

    def test_preloaded_foreign_model_or_helper_cannot_hide_behind_expected_backend_files(self):
        foreign = self.root / "foreign-backend"
        for package in ("scene", "utils"):
            (foreign / package).mkdir(parents=True)
            (foreign / package / "__init__.py").write_text("# foreign module\n", encoding="utf-8")
        (foreign / "utils/general_utils.py").write_text("identity='foreign helper'\n", encoding="utf-8")
        (foreign / "scene/gaussian_model.py").write_text(
            (self.runtime / "scene/gaussian_model.py").read_text(encoding="utf-8"), encoding="utf-8")
        for imported in ("scene.gaussian_model", "utils.general_utils"):
            script = ("sys.path.insert(0,str(Path(sys.argv[2]).parent/'foreign-backend'))\n"
                "__import__(" + repr(imported) + ")\nsys.path.pop(0)\n"
                "from scene.gaussian_model import GaussianModel\n"
                "capture_loaded_backend_model(GaussianModel(),Path(sys.argv[2]),'3dgs',2,0)\n"
                "print(json.dumps({'ok':True}))")
            with self.subTest(imported=imported):
                result = self.probe(script)
                self.assertFalse(result["ok"], result)
                self.assertEqual(result["code"], "geometry_renderer_source_mismatch")

    def test_loaded_model_layout_and_no_optimizer_claim_are_checked_for_both_backends(self):
        for backend, dimensions in (("3dgs", 3), ("2dgs", 2)):
            result = self.probe("from scene.gaussian_model import GaussianModel\n"
                "record=capture_loaded_backend_model(GaussianModel(" + str(dimensions) + "),Path(sys.argv[2])," + repr(backend) + ",2,0)\n"
                "print(json.dumps({'ok':True,'record':record}))")
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["record"]["scale_dimensions"], dimensions)
        for change, expected_count, expected_degree in (("model.optimizer=object()", 2, 0),
                ("model.active_sh_degree=1", 2, 0), ("model.max_sh_degree=1", 2, 0),
                ("model._scaling=model._scaling[:,:2]", 2, 0),
                ("model._features_rest=model._features_dc", 2, 0),
                ("model._opacity=model._opacity.astype('float64')", 2, 0),
                ("del model._rotation", 2, 0),
                ("pass", 3, 0), ("pass", 2, 1)):
            with self.subTest(change=change, count=expected_count, degree=expected_degree):
                result = self.probe("from scene.gaussian_model import GaussianModel\nmodel=GaussianModel()\n" + change + "\n"
                    "capture_loaded_backend_model(model,Path(sys.argv[2]),'3dgs'," + str(expected_count) + "," + str(expected_degree) + ")\n"
                    "print(json.dumps({'ok':True}))")
                self.assertFalse(result["ok"], result)
                self.assertEqual(result["code"], "geometry_invalid_gaussian_model")

    def test_changed_helper_bytes_or_late_imports_are_rejected_before_completion(self):
        for change in ("(Path(sys.argv[2])/'utils/general_utils.py').write_text('identity=2\\n',encoding='utf-8')",
                "(Path(sys.argv[2])/'utils/late.py').write_text('identity=3\\n',encoding='utf-8');__import__('utils.late')"):
            with self.subTest(change=change):
                result = self.probe("from scene.gaussian_model import GaussianModel\n"
                    "record=capture_loaded_backend_model(GaussianModel(),Path(sys.argv[2]),'3dgs',2,0)\n" + change + "\n"
                    "verify_loaded_backend_model_sources(record)\nprint(json.dumps({'ok':True}))")
                self.assertFalse(result["ok"], result)
                self.assertEqual(result["code"], "geometry_source_changed_during_extraction")

    def test_actual_namespace_package_is_accepted_but_rebinding_its_search_path_is_not(self):
        (self.runtime / "utils/__init__.py").unlink()
        foreign = self.root / "foreign-utils"
        foreign.mkdir()
        result = self.probe("from scene.gaussian_model import GaussianModel\n"
            "record=capture_loaded_backend_model(GaussianModel(),Path(sys.argv[2]),'3dgs',2,0)\n"
            "verify_loaded_backend_model_sources(record)\nprint(json.dumps({'ok':True}))")
        self.assertTrue(result["ok"], result)
        result = self.probe("from scene.gaussian_model import GaussianModel\nimport utils\n"
            "record=capture_loaded_backend_model(GaussianModel(),Path(sys.argv[2]),'3dgs',2,0)\n"
            "utils.__path__=[str(Path(sys.argv[2]).parent/'foreign-utils')]\n"
            "verify_loaded_backend_model_sources(record)\nprint(json.dumps({'ok':True}))")
        self.assertFalse(result["ok"], result)
        self.assertEqual(result["code"], "geometry_renderer_source_mismatch")

    def test_public_extraction_refuses_completion_if_its_loaded_sources_change(self):
        result = self.probe("from scene.gaussian_model import GaussianModel\nimport numpy as np\n"
            "from native.backend.tests.test_gs_geometry import GsGeometrySourceTests\n"
            "from native.worker.gs_geometry import extract_geometry\n"
            "fixture=GsGeometrySourceTests();fixture.setUp()\n"
            "try:\n"
            " class CheckedRenderer:\n"
            "  def __init__(self):\n"
            "   self.record=capture_loaded_backend_model(GaussianModel(),Path(sys.argv[2]),'3dgs',2,0)\n"
            "   self.identity={'kind':'owned_CPU_fixture_not_GPU_proof','loaded_backend_model':self.record}\n"
            "  def render(self,view):\n"
            "   (Path(sys.argv[2])/'utils/general_utils.py').write_text('identity=99\\n',encoding='utf-8')\n"
            "   width,height=view['training_size']\n"
            "   return {'alpha':np.full((height,width),.75,dtype=np.float32),'camera_z_moment':np.full((height,width),2.,dtype=np.float32)}\n"
            "  def verify_sources(self): verify_loaded_backend_model_sources(self.record)\n"
            "  def close(self): pass\n"
            " config={'schema':'gsw.gs_geometry_request/1','model_directory':str(fixture.model),'dataset_path':str(fixture.dataset),'iteration':7,"
            "'output_directory':str(fixture.root/'output'),'alpha_threshold':.5,'depth_range':{'min':.2,'max':10.,'frame':'training'},'memory_budget_mb':8}\n"
            " outcome=extract_geometry(config,fixture.root/'receipt.json',renderer_factory=lambda source:CheckedRenderer())\n"
            " manifest=json.loads(Path(outcome['manifestPath']).read_text(encoding='utf-8'))\n"
            " print(json.dumps({'ok':True,'success':outcome['success'],'code':outcome.get('error',{}).get('code'),"
            "'status':manifest['status'],'geometry':manifest['geometry'],'cloud_exists':(Path(outcome['manifestPath']).parent/'point_cloud.ply').exists()}))\n"
            "finally: fixture.doCleanups()")
        self.assertTrue(result["ok"], result)
        self.assertFalse(result["success"], result)
        self.assertEqual(result["code"], "geometry_source_changed_during_extraction")
        self.assertEqual(result["status"], "failed")
        self.assertIsNone(result["geometry"])
        self.assertFalse(result["cloud_exists"])


if __name__ == "__main__":
    unittest.main()
