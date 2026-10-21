"""Public UTF-8 machine-pipe behavior under a Japanese Windows encoding."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

from native.backend.tests import test_geometry_evaluation as evaluation_fixtures
from native.backend.tests import test_reconstruction_comparison as comparison_fixtures


class GeometryCliUtf8Tests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_GEOMETRY_TEST_ROOT", tempfile.gettempdir())) / "gsw-cli-utf8"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=str(base))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = Path(__file__).resolve().parents[2]

    def run_cli(self, script, arguments, expected_code):
        environment = dict(os.environ, PYTHONIOENCODING="cp932", PYTHONDONTWRITEBYTECODE="1")
        environment.pop("PYTHONUTF8", None)
        command = [sys.executable, "-B", str(self.repository / "native/worker" / script)] + list(arguments)
        child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=environment, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        captured = {}
        def drain(name, stream):
            captured[name] = stream.read()
        readers = [threading.Thread(target=drain, args=(name, stream), daemon=True)
                   for name, stream in (("stdout", child.stdout), ("stderr", child.stderr))]
        for reader in readers:
            reader.start()
        try:
            # Like the native QProcess owner, consume both output channels while
            # leaving control input open. Waiting before reads deadlocks on a
            # legitimate report larger than the OS pipe's buffer.
            code = child.wait(timeout=20)
            for reader in readers:
                reader.join(timeout=5)
                self.assertFalse(reader.is_alive(), "Worker did not close an output pipe after exit")
            stdout, stderr = captured["stdout"], captured["stderr"]
            self.assertEqual(code, expected_code, stderr.decode("utf-8", "replace"))
            self.assertFalse(child.stdin.closed, "Parent control pipe stays open until the worker has exited")
            return json.loads(stdout.decode("utf-8"))
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
            child.stdin.close()
            for reader in readers:
                reader.join(timeout=5)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()

    def test_comparison_verify_unicode_conditions_emit_utf8_with_control_pipe_open(self):
        from native.backend.reconstruction_comparison import export_comparison
        fixture = comparison_fixtures.ReconstructionComparisonTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        saved = export_comparison(fixture.request, fixture.root / "profile")
        result = self.run_cli("reconstruction_comparison.py", ["--operation", "verify", "--profile", saved["profilePath"]], 0)
        self.assertTrue(result["success"])
        self.assertEqual([row["label"] for row in result["conditions"]], [row["label"] for row in fixture.request["conditions"]])

    def test_comparison_report_larger_than_pipe_buffer_completes_with_input_open(self):
        from native.backend.reconstruction_comparison import export_comparison
        fixture = comparison_fixtures.ReconstructionComparisonTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        label = "尺度比較🙂" * 10000
        for index, condition in enumerate(fixture.request["conditions"]):
            condition["label"] = label + str(index)
        saved = export_comparison(fixture.request, fixture.root / "large-profile")
        result = self.run_cli("reconstruction_comparison.py", ["--operation", "verify", "--profile", saved["profilePath"]], 0)
        self.assertGreater(len(json.dumps(result, ensure_ascii=False).encode("utf-8")), 128 * 1024)
        self.assertTrue(result["success"])
        self.assertEqual([row["label"] for row in result["conditions"]], [label + "0", label + "1"])

    def test_geometry_evaluation_unicode_output_path_emits_utf8_without_locale_dependency(self):
        candidate = self.root / "candidate.ply"
        evaluation_fixtures.write_ply(candidate, [[0., 0., .1]])
        request = {"schema": "gsw.geometry_evaluation_request/1", "evaluation_frame": {"id": "survey", "unit": "m"},
            "candidate": {"path": str(candidate), "frame": "survey", "unit": "m", "to_evaluation": evaluation_fixtures.identity()},
            "reference": {"kind": "plane_patch", "origin": [0., 0., 0.], "axis_u": [1., 0., 0.], "axis_v": [0., 1., 0.],
                "range_u": [-1., 1.], "range_v": [-1., 1.], "frame": "survey", "unit": "m", "to_evaluation": evaluation_fixtures.identity(),
                "provenance": {"role": "independent_reference", "source": "owned analytical fixture"}},
            "sampling": {"count": 4, "seed": 0}, "coverage_threshold": .01}
        configuration = self.root / "request.json"
        configuration.write_text(json.dumps(request), encoding="utf-8")
        output = self.root / "较验收结果"
        result = self.run_cli("geometry_evaluation.py", ["--config", str(configuration), "--output-directory", str(output)], 0)
        self.assertTrue(result["success"])
        self.assertEqual(Path(result["reportPath"]), output / "report.json")

    def test_geometry_admission_unicode_error_is_utf8_json_before_any_renderer(self):
        model, dataset = self.root / "较模型", self.root / "dataset"
        model.mkdir(); dataset.mkdir()
        configuration = self.root / "request.json"
        configuration.write_text(json.dumps({"schema": "gsw.gs_geometry_request/1", "model_directory": str(model),
            "dataset_path": str(dataset), "iteration": 1, "output_directory": str(self.root / "geometry")}), encoding="utf-8")
        result = self.run_cli("gs_geometry.py", ["--config", str(configuration), "--report", str(self.root / "receipt.json")], 1)
        self.assertFalse(result["success"])
        self.assertEqual(result["error"]["code"], "geometry_gaussian_model_missing")
        self.assertIn("较模型", result["error"]["detail"])
        self.assertFalse((self.root / "geometry").exists())


if __name__ == "__main__":
    unittest.main()
