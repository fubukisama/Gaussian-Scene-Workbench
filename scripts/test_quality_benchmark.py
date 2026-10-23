"""CPU-only tests at the public quality benchmark prepare/inspect boundary."""
import copy
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts import run_quality_benchmark as benchmark


class QualityBenchmarkProtocolTests(unittest.TestCase):
    def test_initial_protocol_is_bounded_engineering_only(self):
        protocol = benchmark.load_protocol(ROOT / "docs/quality_upgrade/benchmark_protocol.json")
        self.assertEqual([17, 29], protocol["seeds"])
        self.assertEqual(64, protocol["profile"]["scheduled_steps"])
        self.assertEqual(600, protocol["budget"]["total_gpu_seconds"])
        self.assertEqual(1200, protocol["budget"]["total_wall_seconds"])
        self.assertFalse(protocol["promotion_allowed"])
        changed = copy.deepcopy(protocol)
        changed["profile"]["scheduled_steps"] = 1000
        with self.assertRaisesRegex(ValueError, "baseline_step_cap"):
            benchmark.validate_protocol(changed)

    def test_protocol_rejects_unexecuted_quality_as_baseline_or_promotion(self):
        protocol = benchmark.load_protocol(ROOT / "docs/quality_upgrade/benchmark_protocol.json")
        protocol["profile"]["mask_supervision"] = True
        with self.assertRaisesRegex(ValueError, "unsupported_baseline_profile"):
            benchmark.validate_protocol(protocol)


class QualityBenchmarkPreparationTests(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ.get("GSW_QUALITY_TEST_TMP", "D:/Codex/validation/gsw-quality-upgrade-20261023/cpu-tests"))
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=str(parent))
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name) / "owned"

    def test_prepare_publishes_known_camera_t0_without_gpu_or_overwrite(self):
        result = benchmark.prepare_benchmark(self.output, case_names=["T0"])
        self.assertEqual("prepared", result["status"])
        self.assertEqual("not_run", result["acceptance"]["gpu"])
        self.assertEqual("not_evaluable", result["acceptance"]["quality"])
        self.assertEqual([17, 29], [item["seed"] for item in result["fixtures"]])
        for item in result["fixtures"]:
            self.assertEqual([3, 1, 1], item["split_counts"])
            self.assertEqual(128, item["initialization"]["point_count"])
            self.assertEqual([128, 96], item["cameras"][0]["size"])
            self.assertEqual({"checkerboard", "plane", "l_structure", "thin_geometry"}, set(item["features"]))
            self.assertTrue(item["initialization"]["points_sha256"])
            self.assertTrue(item["initialization"]["colors_sha256"])
        self.assertNotIn("torch", sys.modules)
        with self.assertRaisesRegex(ValueError, "fresh_output_required"):
            benchmark.prepare_benchmark(self.output, case_names=["T0"])

    def test_t1_distance_focal_and_resolution_changes_are_separable(self):
        names = ["T1-base", "T1-distance-near", "T1-distance-far", "T1-focal", "T1-resize"]
        result = benchmark.prepare_benchmark(self.output, case_names=names)
        rows = {item["fixture_id"]: item for item in result["fixtures"] if item["seed"] == 17}
        base = rows["T1-base"]["cameras"][0]
        near = rows["T1-distance-near"]["cameras"][0]
        far = rows["T1-distance-far"]["cameras"][0]
        focal = rows["T1-focal"]["cameras"][0]
        resize = rows["T1-resize"]["cameras"][0]
        self.assertEqual(base["K"], near["K"])
        self.assertEqual(base["K"], far["K"])
        self.assertAlmostEqual(-1.875, near["c2w_opencv"][2][3])
        self.assertAlmostEqual(-3.75, far["c2w_opencv"][2][3])
        self.assertEqual(base["c2w_opencv"], focal["c2w_opencv"])
        self.assertEqual(base["size"], focal["size"])
        self.assertAlmostEqual(base["K"][0][0] * 1.4, focal["K"][0][0])
        self.assertEqual(base["c2w_opencv"], resize["c2w_opencv"])
        self.assertEqual([64, 48], resize["size"])
        self.assertAlmostEqual(base["K"][0][0] / base["size"][0], resize["K"][0][0] / resize["size"][0])
        self.assertEqual(1, len({item["initialization"]["points_sha256"] for item in rows.values()}))
        self.assertEqual(1, len({item["initialization"]["colors_sha256"] for item in rows.values()}))

    def test_inspect_verifies_repeatability_and_rejects_changed_input_bytes(self):
        first = benchmark.prepare_benchmark(self.output, case_names=["T0"])
        second = benchmark.prepare_benchmark(self.output.with_name("repeat"), case_names=["T0"])
        self.assertEqual(first["files"], second["files"])
        inspected = benchmark.inspect_benchmark(self.output / "baseline_manifest.json")
        self.assertEqual("pass", inspected["input_integrity"])
        self.assertEqual("not_run", inspected["acceptance"]["gpu"])
        image = self.output / first["fixtures"][0]["dataset_root"] / "train/frame-003.png"
        with image.open("ab") as stream:
            stream.write(b"modified")
        with self.assertRaisesRegex(ValueError, "artifact_hash_mismatch"):
            benchmark.inspect_benchmark(self.output / "baseline_manifest.json")

    def test_reference_noise_stream_does_not_change_sampler_but_is_not_gpu_evidence(self):
        first = benchmark.reference_rng_trace(17, ["c", "a", "b"], noise_draws_per_step=0)
        changed = benchmark.reference_rng_trace(17, ["c", "a", "b"], noise_draws_per_step=13)
        self.assertEqual(first["view_sequence"], changed["view_sequence"])
        self.assertEqual(64, len(first["view_sequence"]))
        self.assertNotEqual(first["noise_final_state_sha256"], changed["noise_final_state_sha256"])
        self.assertEqual("not_run", changed["actual_training_trace_status"])
        self.assertEqual("reference_only_not_mcmc_implementation", changed["strategy_noise_status"])

    def test_prepared_commands_are_direct_64_step_plans_not_worker_or_execution(self):
        result = benchmark.prepare_benchmark(self.output, case_names=["T0"])
        plan = result["gpu_execution_plan"]
        self.assertFalse(plan["executed"])
        self.assertEqual(4, len(plan["runs"]))
        for run in plan["runs"]:
            self.assertEqual("64", run["argv"][run["argv"].index("--iterations") + 1])
            self.assertNotIn("gsw_worker.py", " ".join(run["argv"]))
            self.assertFalse(run["full_state_resume_accepted"])
        self.assertEqual("not_run", result["acceptance"]["worker"])

    def test_inspection_does_not_accept_fabricated_gpu_or_quality_pass(self):
        benchmark.prepare_benchmark(self.output, case_names=["T0"])
        path = self.output / "baseline_manifest.json"
        with path.open(encoding="utf-8") as stream:
            changed = json.load(stream)
        changed["acceptance"]["gpu"] = "pass"
        path.write_text(json.dumps(changed), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "prepared_acceptance_changed"):
            benchmark.inspect_benchmark(path)

    def test_public_cli_prepares_inspects_and_has_no_execute_command(self):
        command = [sys.executable, "-B", str(ROOT / "scripts/run_quality_benchmark.py")]
        prepared = subprocess.run(command + ["prepare", "--output", str(self.output), "--case", "T0"],
            capture_output=True, text=True, cwd=str(ROOT), timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(0, prepared.returncode, prepared.stderr)
        record = json.loads(prepared.stdout)
        inspected = subprocess.run(command + ["inspect", "--manifest", record["manifest"],
            "--expected-manifest-sha256", record["manifest_sha256"]], capture_output=True, text=True,
            cwd=str(ROOT), timeout=30, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(0, inspected.returncode, inspected.stderr)
        self.assertFalse(json.loads(inspected.stdout)["gpu_execution_performed"])
        forbidden = subprocess.run(command + ["execute"], capture_output=True, text=True,
            cwd=str(ROOT), timeout=10, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertNotEqual(0, forbidden.returncode)


if __name__ == "__main__":
    unittest.main()
