"""Real fresh-D staging and isolated CPU probes; never compile/train/open GUI."""
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
QUALITY_MODULES = ("training_quality_plan", "quality_random", "quality_densification", "quality_mip",
    "quality_supervision", "quality_training_runtime", "quality_render_adapter", "quality_rasterizer_runtime",
    "quality_mcmc", "quality_mcmc_model", "quality_postprocess", "quality_export", "surfel_depth_adapter")


@unittest.skipUnless(os.name == "nt", "actual Windows PowerShell staging seam")
class QualityPackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(os.environ.get("GSW_QUALITY_PACKAGE_TEST_ROOT", "D:/Codex/validation/gsw-quality-upgrade-20261023/p6/package-tests"))
        parent.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="owned-", dir=str(parent))
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.output = Path(cls.temporary.name) / "backend 日本語 空白"
        cls.python = os.environ.get("GSW_QUALITY_PACKAGE_PYTHON", "D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe")
        if not Path(cls.python).is_file():
            cls.python = sys.executable
        cls.environment = dict(os.environ, CUDA_VISIBLE_DEVICES="-1", PYTHONDONTWRITEBYTECODE="1",
                               PYTHONUTF8="1", PYTHONIOENCODING="utf-8", TEMP="D:/CodexData/Temp", TMP="D:/CodexData/Temp")
        cls.environment.pop("PYTHONPATH", None)
        result = subprocess.run(["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-File", str(ROOT / "scripts/stage_native_backend.ps1"), "-SourceRoot", str(ROOT),
            "-DestinationRoot", str(cls.output), "-PythonExecutable", cls.python],
            cwd=str(parent), env=cls.environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW, timeout=120)
        if result.returncode:
            raise AssertionError("real_staging_failed:\n" + result.stdout.decode("utf-8", errors="replace") +
                                 result.stderr.decode("utf-8", errors="replace"))

    def test_staged_quality_modules_import_from_package_and_resolve_without_gpu(self):
        code = """import importlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
sys.path.insert(0,str(root))
names=json.loads(sys.argv[2])
for name in names:
 module=importlib.import_module('native.worker.'+name)
 Path(module.__file__).resolve().relative_to(root)
from native.worker.training_quality_plan import resolve_quality_plan
absplan=resolve_quality_plan({'version':1,'densification':{'strategy':'abs_gs'}})
mcmc=resolve_quality_plan({'version':1,'densification':{'strategy':'mcmc','parameters':{'cap':256}}})
assert not absplan['quality_accepted'] and not mcmc['quality_accepted']
import torch
assert not torch.cuda.is_initialized()
print(json.dumps({'modules':len(names),'GPU_executed':False,'quality_accepted':False,'mcmc_cap':mcmc['effective']['densification']['parameters']['cap']}))
"""
        result = subprocess.run([self.python, "-I", "-B", "-c", code, str(self.output), json.dumps(QUALITY_MODULES)],
            cwd=str(self.output), env=self.environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
        self.assertEqual(0, result.returncode, result.stderr.decode("utf-8", errors="replace"))
        report = json.loads(result.stdout.decode("utf-8"))
        self.assertEqual(13, report["modules"])
        self.assertEqual(256, report["mcmc_cap"])
        self.assertFalse(report["GPU_executed"])
        self.assertFalse(report["quality_accepted"])

    def test_staged_public_clis_cpu_oracles_and_three_language_catalog_are_complete(self):
        scripts = ("cleanup_gaussians.py", "run_quality_benchmark.py", "prepare_quality_real_fixture.py",
            "verify_quality_rasterizer.py", "verify_pinhole_rasterizer_gpu.py", "verify_quality_pipeline_cuda.py",
            "verify_quality_training.py", "verify_supervision_gradient.py", "build_quality_rasterizer.ps1",
            "verify_native_backend.py", "native_i18n.py")
        for name in scripts:
            self.assertTrue((self.output / "scripts" / name).is_file(), name)
        catalog_path = self.output / "native/i18n/catalog.json"
        self.assertEqual(hashlib.sha256((ROOT / "native/i18n/catalog.json").read_bytes()).hexdigest(),
                         hashlib.sha256(catalog_path.read_bytes()).hexdigest())
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        self.assertTrue(catalog)
        for row in catalog.values():
            self.assertEqual({"zh_CN", "en_US", "ja_JP"}, set(row))
            self.assertTrue(all(isinstance(value, str) and value for value in row.values()))
        for name in ("verify_quality_rasterizer.py", "verify_pinhole_rasterizer_gpu.py"):
            evidence = self.output.parent / (name + ".CPU.json")
            result = subprocess.run([self.python, "-I", "-B", str(self.output / "scripts" / name),
                "--cpu-only", "--output", str(evidence)], cwd=str(self.output), env=self.environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
            self.assertEqual(0, result.returncode, result.stderr.decode("utf-8", errors="replace"))
            report = json.loads(evidence.read_text(encoding="utf-8"))
            self.assertFalse(report["gpu_executed"])
            if name == "verify_quality_rasterizer.py":
                self.assertTrue(report["cpu_reference_passed"])
                self.assertFalse(report["accepted"])
            else:
                self.assertTrue(report["oracle_only"])
        probe = subprocess.run([self.python, "-I", "-B", "-c",
            "import sys;sys.path.insert(0,sys.argv[1]);from scripts.run_quality_benchmark import load_protocol;assert load_protocol()['promotion_allowed'] is False",
            str(self.output)], cwd=str(self.output), env=self.environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
        self.assertEqual(0, probe.returncode, probe.stderr.decode("utf-8", errors="replace"))

    def test_staged_manifest_binds_all_copied_source_files_and_rejects_tampering(self):
        manifest = json.loads((self.output / "backend_manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(2, manifest["schemaVersion"])
        records = manifest["files"]
        by_path = {record["path"]: record for record in records}
        self.assertEqual(len(records), len(by_path))
        for module in QUALITY_MODULES:
            name = "native/worker/" + module + ".py"
            self.assertIn(name, manifest["components"])
            self.assertIn(name, by_path)
        for relative, record in by_path.items():
            path = self.output / relative
            self.assertEqual(path.stat().st_size, record["bytes"])
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), record["sha256"])
            self.assertNotIn(path.suffix.lower(), {".html", ".js", ".css", ".pyd", ".npz", ".pth"})
            self.assertNotIn(".planning", path.parts)
            self.assertNotIn("local_asset_bindings", path.name)
            self.assertNotEqual("test_source_closure.py", path.name)
        verifier = self.output / "scripts/verify_native_backend.py"
        command = [self.python, "-I", "-B", str(verifier), "--root", str(self.output), "--package"]
        target = self.output / "native/worker/training_quality_plan.py"
        original = target.read_bytes()
        try:
            target.write_bytes(original + b"\n# controlled staged-byte tamper\n")
            result = subprocess.run(command, cwd=str(self.output), env=self.environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("hash mismatch", result.stderr.decode("utf-8", errors="replace"))
        finally:
            target.write_bytes(original)

    def test_public_verifier_rejects_malformed_or_incomplete_manifest_without_traceback(self):
        path = self.output / "backend_manifest.json"
        original = path.read_bytes()
        good = json.loads(original.decode("utf-8"))
        missing = dict(good, files=[row for row in good["files"] if row["path"] != "native/worker/training_quality_plan.py"])
        unsafe = dict(good, files=[{"path": "../escape.py", "bytes": 0, "sha256": "0" * 64}] + good["files"])
        for malformed in ([], dict(good, schemaVersion=1), missing, unsafe):
            try:
                path.write_text(json.dumps(malformed), encoding="utf-8")
                result = subprocess.run([self.python, "-I", "-B", str(self.output / "scripts/verify_native_backend.py"),
                    "--root", str(self.output), "--package"], cwd=str(self.output), env=self.environment,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
                self.assertNotEqual(0, result.returncode)
                self.assertNotIn("Traceback", result.stderr.decode("utf-8", errors="replace"))
            finally:
                path.write_bytes(original)

    def test_explicit_fresh_kernel_proofs_select_from_staged_sources_without_gpu(self):
        quality = os.environ.get("GSW_QUALITY_PACKAGE_KERNEL_ROOT")
        stock = os.environ.get("GSW_QUALITY_PACKAGE_STOCK_ROOT")
        if not quality or not stock:
            self.skipTest("Actual fresh kernel roots must be explicitly bound; no compile/import/GPU substitute")
        code = """import json,sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
sys.path.insert(0,str(root))
from native.worker.quality_rasterizer_runtime import select_quality_rasterizer
from native.worker.camera_rasterizer_runtime import select_camera_rasterizer
quality=select_quality_rasterizer(root,sys.argv[2])
stock=select_camera_rasterizer(root,sys.argv[3])
assert 'torch' not in sys.modules
for proof in (quality['build'],stock['proof']):
 assert all('/doc/' not in name and '/docs/' not in name and not name.endswith(('.html','.js')) for name in proof['source_files'])
print(json.dumps({'quality_source_files':len(quality['build']['source_files']),'stock_source_files':len(stock['proof']['source_files']),
 'quality_build_sha256':quality['buildSha256'],'stock_proof_sha256':stock['proofSha256'],'GPU_executed':False}))
"""
        result = subprocess.run([self.python, "-I", "-B", "-c", code, str(self.output), quality, stock],
            cwd=str(self.output), env=self.environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
        self.assertEqual(0, result.returncode, result.stderr.decode("utf-8", errors="replace"))
        report = json.loads(result.stdout.decode("utf-8"))
        self.assertGreater(report["quality_source_files"], 100)
        self.assertGreater(report["stock_source_files"], 100)
        self.assertFalse(report["GPU_executed"])


if __name__ == "__main__":
    unittest.main()
