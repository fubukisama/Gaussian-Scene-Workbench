"""Compile and execute the real host/device pinhole bounds helper on CPU.

This numeric kernel-boundary oracle is not CUDA runtime acceptance; the latter
must prove the candidate extension marker and actual rasterized gradients.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class RasterizerPinholeCpuTests(unittest.TestCase):
    def test_compiled_shared_bounds_preserve_full_k_covariance_and_gradient(self):
        root = Path(__file__).resolve().parents[2]
        source = root / "native/tests/RasterizerPinholeOracle.cpp"
        if os.name == "nt":
            setup = Path("D:/Apps/PortableMSVC-2022/msvc/setup_x64.bat")
            compiler = shutil.which("cl.exe")
            if compiler is None and not setup.is_file():
                self.skipTest("MSVC is required for the CPU numerical oracle")
            base = (Path("D:/Codex/qa-camera-p0/rasterizer-cpu") if Path("D:/").is_dir()
                    else Path(tempfile.gettempdir()) / "gsw-rasterizer-cpu")
        else:
            compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
            if compiler is None:
                self.skipTest("C++ compiler is required for the CPU numerical oracle")
            base = Path(os.environ.get("TMPDIR", tempfile.gettempdir())) / "gsw-rasterizer-cpu"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            work = Path(temporary)
            executable = work / ("oracle.exe" if os.name == "nt" else "oracle")
            environment = dict(os.environ, TEMP=str(work), TMP=str(work))
            if os.name == "nt":
                compile_args = 'cl.exe /nologo /EHsc /std:c++17 /O2 /Fe:"{}" /Fo:"{}" "{}"'.format(
                    executable, work / "oracle.obj", source)
                command = 'cmd.exe /d /s /c "{}{}"'.format(
                    'call "{}" && '.format(setup) if compiler is None else "", compile_args)
            else:
                command = [compiler, "-std=c++17", "-O2", str(source), "-o", str(executable)]
            options = dict(cwd=work, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           encoding="utf-8", errors="replace", timeout=60)
            if os.name == "nt":
                options["creationflags"] = subprocess.CREATE_NO_WINDOW
            compiled = subprocess.run(command, **options)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            result = subprocess.run([str(executable)], **options)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("offset pixel=579.500000000", result.stdout)
            self.assertIn("center unchanged", result.stdout)
            self.assertIn("transformed camera bounds and gradients", result.stdout)


if __name__ == "__main__":
    unittest.main()
