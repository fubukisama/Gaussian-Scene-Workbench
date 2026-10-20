"""Batch orchestration at public plan/CLI and owned child-process seams."""

import copy
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from PIL import Image

from native.backend.camera_dataset import load_camera_dataset, write_camera_dataset


class TrainingBatchWorkerTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_CAMERA_P1_TEST_ROOT", tempfile.gettempdir())) / "gsw-batch-worker-p1"
        base.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.repository = Path(__file__).resolve().parents[2]
        self.workspace = self.root / "workspace"
        self.dataset = self.workspace / "datasets/example"
        self.dataset.mkdir(parents=True)
        views = []
        for index, split in enumerate(("train", "train", "test")):
            image = self.dataset / (str(index) + ".png")
            Image.new("RGB", (16, 12), (10 + index, 20, 30)).save(image)
            views.append({"view_id": "v-" + str(index), "image_path": image.name,
                "K": [[12., 0., 8.], [0., 13., 6.], [0., 0., 1.]], "pixel_coordinates": "pixel_border",
                "split": split, "c2w_opencv": [[1., 0., 0., index - 1.], [0., 1., 0., 0.],
                                                [0., 0., 1., -4.], [0., 0., 0., 1.]]})
        write_camera_dataset({"schema": "gsw.camera_dataset/1", "pose_policy": "locked",
            "source": {"kind": "manual"}, "scale": {"status": "arbitrary"}, "views": views,
            "initialization": {"kind": "random", "seed": 7, "count": 16,
                               "frame": "source", "bounds": [[-.5, -.5, -.5], [.5, .5, .5]]}},
            self.dataset / "camera_dataset.json")
        self.data = load_camera_dataset(self.dataset)
        write_camera_dataset(self.data, self.dataset / "camera_dataset.json")
        self.request = {"schema": "gsw.training_batch_plan/1", "datasets": [{"id": "example",
            "path": str(self.dataset), "data_identity": ""}], "matrix": {"trainingSeed": [17, 23]},
            "base": {"backend": "3dgs", "quality": "quick", "trainOptions": {"iterations": 1000, "resolution": 1}},
            "initialization_policy": "dataset_declared", "evaluation_policy": "declared_splits",
            "run_policy": {"on_failure": "stop"}}

    def cli(self, operation, request, report_name):
        request_path = self.root / (report_name + "-request.json")
        report_path = self.root / (report_name + ".json")
        request_path.write_text(json.dumps(request), encoding="utf-8")
        result = subprocess.run([sys.executable, "-B", str(self.repository / "native/worker/training_batch.py"),
            "--operation", operation, "--request", str(request_path), "--report", str(report_path),
            "--repository", str(self.repository), "--workspace", str(self.workspace)],
            cwd=str(self.root), input="", capture_output=True, text=True, encoding="utf-8", timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        lines = result.stdout.splitlines()
        return result.returncode, json.loads(lines[-1]) if lines else {}, result.stdout + result.stderr

    def pinned_request(self):
        value = copy.deepcopy(self.request)
        value["datasets"][0]["data_identity"] = self.data["data_identity"]
        return value

    def fake_process_factory(self, mode="done"):
        """External process adapter only; never a real optimizer/GPU fixture."""
        script = self.root / "owned-child.py"
        script.write_text('''import argparse, hashlib, json, os, sys, uuid
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--task');p.add_argument('--config');p.add_argument('--resume-training',action='store_true');a=p.parse_args()
c=json.loads(Path(a.config).read_text(encoding='utf-8'));o=Path(c['outputRoot'])/c['outputScene'];o.mkdir(parents=True,exist_ok=True)
Path(os.environ['GSW_BATCH_FIXTURE_MARKER']).write_text(str(c['trainingSeed']),encoding='utf-8')
print('out-'+('o'*80000),flush=True);print('err-'+('e'*80000),file=sys.stderr,flush=True)
print('[gsw-training-seed] '+json.dumps({'schema':'gsw.training_seed/1','training_seed':c['trainingSeed']}),flush=True)
print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'running','stage':'train','progressPercent':10,'previewPath':'never-a-batch-result.ply'}),flush=True)
sys.path.insert(0,c['repositoryRoot'])
from native.worker.canonical_training import prepare_training_dataset
prepared=prepare_training_dataset(c['cameraDatasetPath'],o/'.gsw/camera-input',resolution=c['trainOptions']['resolution'])
contract=dict(prepared['contract'],backend=c['backend'])
(o/'training-camera-contract.json').write_text(json.dumps(contract),encoding='utf-8')
mode=os.environ.get('GSW_BATCH_FIXTURE_MODE','done')
if mode=='fail-first' and c['trainingSeed']==17:
 print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'failed','stage':'controlled-failure'}),flush=True);sys.exit(1)
if mode in ('pause-first','cancel-first','false-pause') and c['trainingSeed']==17 and not a.resume_training:
 if mode!='false-pause':
  for line in sys.stdin:
   if line.strip()=='cancel':
    print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'cancelled','stage':'cancelled'}),flush=True);sys.exit(130)
   if line.strip()=='pause':break
 if mode!='false-pause':
  r=o/'.gsw-resume';r.mkdir();name='state-'+uuid.uuid4().hex+'.pth';payload=r/name;payload.write_bytes(b'owned external-process checkpoint boundary, not actual optimizer evidence')
  (r/'ready.json').write_text(json.dumps({'version':1,'file':name,'iteration':10,'total':c['trainOptions']['iterations'],'identity':'a'*64,'backend':c['backend'],'training_seed':c['trainingSeed'],'sha256':hashlib.sha256(payload.read_bytes()).hexdigest(),'session':'owned-fixture','camera_contract':contract}),encoding='utf-8')
 print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'paused','stage':'paused'}),flush=True);sys.exit(75)
d=o/'point_cloud'/('iteration_'+str(c['trainOptions']['iterations']));d.mkdir(parents=True,exist_ok=True);f=d/'point_cloud.ply';f.write_bytes(b'ply\\nformat ascii 1.0\\nelement vertex 1\\nproperty float x\\nproperty float y\\nproperty float z\\nend_header\\n0 0 0\\n')
(o/'training-camera-contract.json').write_text(json.dumps(contract),encoding='utf-8');m=dict(contract,ply_sha256=hashlib.sha256(f.read_bytes()).hexdigest());f.with_suffix('.camera-scale.json').write_text(json.dumps(m),encoding='utf-8')
print('[worker-event] '+json.dumps({'version':1,'type':'status','state':'done','stage':'done','progressPercent':100}),flush=True)
sys.stdin.read()
''', encoding="utf-8")
        calls = []
        marker = self.root / "child-started.txt"
        def factory(command, **options):
            calls.append(list(command))
            command = list(command)
            command[2] = str(script)
            options["env"] = dict(os.environ, GSW_BATCH_FIXTURE_MODE=mode, GSW_BATCH_FIXTURE_MARKER=str(marker))
            return subprocess.Popen(command, **options)
        return factory, calls, marker

    def create_public_plan(self, request=None):
        from native.worker.training_batch import create_training_batch_plan
        return Path(create_training_batch_plan(request or self.pinned_request(), self.repository,
                                              self.workspace)["planPath"])

    def test_camera_metadata_writer_keeps_legal_long_target_with_atomic_publication(self):
        parent = self.root
        while len(str(parent)) < 223:
            parent = parent / ("w" * min(40, max(1, 222 - len(str(parent)))))
        target = parent / "ready.json"
        self.assertLess(len(str(target)), 260)
        write_camera_dataset({"retained": "first"}, target)
        write_camera_dataset({"retained": "second"}, target)
        self.assertEqual(json.loads(target.read_text(encoding="utf-8")), {"retained": "second"})
        self.assertEqual(list(parent.iterdir()), [target])

    def test_preview_does_not_launch_or_persist_training_and_create_plan_requires_pins(self):
        before = (self.dataset / "camera_dataset.json").read_bytes()
        code, result, log = self.cli("preview", self.request, "preview")
        self.assertEqual(code, 0, log)
        report = json.loads(Path(result["reportPath"]).read_text(encoding="utf-8"))
        self.assertEqual(report["summary"]["runCount"], 2)
        self.assertEqual(report["datasets"][0]["data_identity"], self.data["data_identity"])
        self.assertFalse((self.workspace / ".gsw/training-batches").exists())
        code, result, log = self.cli("create-plan", self.request, "unpinned")
        self.assertEqual(code, 2, log)
        self.assertEqual(result["code"], "batch_dataset_pin_required")
        code, result, log = self.cli("create-plan", self.pinned_request(), "created")
        self.assertEqual(code, 0, log)
        plan_path = Path(result["planPath"])
        state = json.loads(plan_path.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["status"], "ready")
        self.assertEqual(state["schema"], "gsw.training_batch_state/1")
        self.assertEqual(len(state["runs"]), 2)
        self.assertFalse((plan_path.parent / "outputs").exists())
        self.assertEqual((self.dataset / "camera_dataset.json").read_bytes(), before)

    def test_serial_processes_drain_both_pipes_finish_with_stdin_open_and_skip_verified_results(self):
        from native.worker.training_batch import run_training_batch
        plan = self.create_public_plan()
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: []), 0)
        self.assertEqual(len(calls), 2)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["status"], "completed")
        self.assertTrue(all(item["status"] == "completed" for item in state["runs"].values()))
        events = [json.loads(line[len("[worker-event] "):]) for line in output.getvalue().splitlines()
                  if line.startswith("[worker-event] ")]
        self.assertTrue(events)
        self.assertTrue(all("previewPath" not in event for event in events))
        calls.clear()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: []), 0)
        self.assertEqual(calls, [])

    def test_pause_uses_owned_checkpoint_and_resume_only_restores_the_active_run(self):
        from native.worker.training_batch import run_training_batch
        plan = self.create_public_plan()
        factory, calls, marker = self.fake_process_factory("pause-first")
        sent = [False]
        def pause_active():
            if marker.exists() and not sent[0]:
                sent[0] = True
                return ["pause"]
            return []
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=pause_active), 75)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["status"], "paused")
        self.assertEqual(len(calls), 1)
        active = state["runs"][state["active_run_id"]]
        self.assertEqual(active["checkpoint"]["manifest"]["iteration"], 10)
        self.assertEqual(active["checkpoint"]["manifest"]["training_seed"], 17)
        calls.clear()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: []), 0)
        self.assertEqual(len(calls), 2)
        self.assertIn("--resume-training", calls[0])
        self.assertNotIn("--resume-training", calls[1])

    def test_pause_exit_without_a_complete_state_is_refused_not_advertised_as_resume(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        plan = self.create_public_plan()
        factory, _, _ = self.fake_process_factory("false-pause")
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(TrainingBatchError, "checkpoint_unavailable"):
            run_training_batch(plan, process_factory=factory, control_reader=lambda: [])
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["status"], "failed")
        self.assertFalse(any(record["status"] == "paused" for record in state["runs"].values()))

    def test_continue_policy_retains_failure_and_later_result_without_false_success(self):
        from native.worker.training_batch import run_training_batch
        request = self.pinned_request()
        request["run_policy"]["on_failure"] = "continue"
        plan = self.create_public_plan(request)
        factory, calls, _ = self.fake_process_factory("fail-first")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: []), 1)
        self.assertEqual(len(calls), 2)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["status"], "failed")
        self.assertEqual(sorted(record["status"] for record in state["runs"].values()), ["completed", "failed"])

    def test_continue_after_failed_run_and_between_jobs_pause_runs_only_pending_fresh(self):
        from native.worker.training_batch import run_training_batch, BETWEEN_RUNS_PAUSE_CODE
        request = self.pinned_request()
        request["run_policy"]["on_failure"] = "continue"
        plan = self.create_public_plan(request)
        factory, calls, _ = self.fake_process_factory("fail-first")
        sent = [False]
        def pause_after_failure():
            state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
            if not sent[0] and any(item["status"] == "failed" for item in state["runs"].values()):
                sent[0] = True
                return ["pause"]
            return []
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=pause_after_failure),
                             BETWEEN_RUNS_PAUSE_CODE)
        paused = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(paused["status"], "interrupted")
        self.assertEqual(sorted(item["status"] for item in paused["runs"].values()), ["failed", "pending"])
        calls.clear()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: []), 1)
        self.assertEqual(len(calls), 1)
        self.assertNotIn("--resume-training", calls[0])
        configuration = json.loads(Path(calls[0][calls[0].index("--config") + 1]).read_text(encoding="utf-8"))
        self.assertEqual(configuration["trainingSeed"], 23)
        final = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(final["status"], "failed")
        self.assertEqual(sorted(item["status"] for item in final["runs"].values()), ["completed", "failed"])
        self.assertEqual(next(item for item in final["runs"].values() if item["status"] == "failed")["attempts"], 1)

    def test_saved_plan_and_state_move_with_workspace_and_only_managed_paths_rebase(self):
        from native.worker.training_batch import run_training_batch
        plan = self.create_public_plan()
        original_plan = plan.read_bytes()
        relocated = self.root / "saved-as-workspace"
        shutil.copytree(self.workspace, relocated)
        moved_plan = relocated / plan.relative_to(self.workspace)
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(moved_plan, process_factory=factory, control_reader=lambda: []), 0)
        for command in calls:
            configuration = json.loads(Path(command[command.index("--config") + 1]).read_text(encoding="utf-8"))
            self.assertTrue(Path(configuration["datasetPath"]).relative_to(relocated).parts)
            self.assertTrue(Path(configuration["outputRoot"]).relative_to(relocated).parts)
        self.assertEqual(moved_plan.read_bytes(), original_plan)
        self.assertEqual(plan.read_bytes(), original_plan)
        self.assertEqual(json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))["status"], "ready")

    def test_completed_result_tampering_stops_before_any_new_child(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        plan = self.create_public_plan()
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: []), 0)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        artifact = next(iter(state["runs"].values()))["artifacts"][0]
        path = plan.parent / artifact["path"]
        path.write_bytes(path.read_bytes() + b"changed")
        calls.clear()
        with self.assertRaisesRegex(TrainingBatchError, "checksum_mismatch"):
            run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: [])
        self.assertEqual(calls, [])

    def test_completed_result_cannot_hide_missing_ply_by_reducing_mutable_artifact_list(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        plan = self.create_public_plan()
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: []), 0)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        record = next(iter(state["runs"].values()))
        ply = next(item for item in record["artifacts"] if item["kind"] == "gaussian_ply")
        (plan.parent / ply["path"]).unlink()
        record["artifacts"] = [item for item in record["artifacts"] if item["kind"] == "observed_training_seed"]
        plan.with_name("state.json").write_text(json.dumps(state), encoding="utf-8")
        calls.clear()
        with self.assertRaisesRegex(TrainingBatchError, "batch_result_artifact_set_mismatch"):
            run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: [])
        self.assertEqual(calls, [])

    def test_completed_result_rejects_valid_hash_artifacts_borrowed_from_another_run(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        plan = self.create_public_plan()
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: []), 0)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        records = list(state["runs"].values())
        records[0]["artifacts"] = copy.deepcopy(records[1]["artifacts"])
        plan.with_name("state.json").write_text(json.dumps(state), encoding="utf-8")
        calls.clear()
        with self.assertRaisesRegex(TrainingBatchError, "batch_result_artifact_set_mismatch"):
            run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: [])
        self.assertEqual(calls, [])

    def test_completed_result_rejects_removed_expected_ply_without_starting_child(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        plan = self.create_public_plan()
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: []), 0)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        artifact = next(item for item in next(iter(state["runs"].values()))["artifacts"]
                        if item["kind"] == "gaussian_ply")
        (plan.parent / artifact["path"]).unlink()
        calls.clear()
        with self.assertRaisesRegex(TrainingBatchError, "batch_result_checksum_mismatch"):
            run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: [])
        self.assertEqual(calls, [])

    def test_completed_result_rechecks_observed_seed_even_with_refreshed_state_hash(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        from native.worker.training_checkpoint import file_digest
        plan = self.create_public_plan()
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: []), 0)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        artifact = next(item for item in next(iter(state["runs"].values()))["artifacts"]
                        if item["kind"] == "observed_training_seed")
        path = plan.parent / artifact["path"]
        seed = json.loads(path.read_text(encoding="utf-8"))
        seed["training_seed"] += 1
        path.write_text(json.dumps(seed), encoding="utf-8")
        artifact.update(sha256=file_digest(path), byte_size=path.stat().st_size)
        plan.with_name("state.json").write_text(json.dumps(state), encoding="utf-8")
        calls.clear()
        with self.assertRaisesRegex(TrainingBatchError, "batch_training_seed_not_observed"):
            run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: [])
        self.assertEqual(calls, [])

    def test_completed_result_rechecks_full_camera_contract_even_with_refreshed_state_hashes(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        from native.worker.training_checkpoint import file_digest
        plan = self.create_public_plan()
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: []), 0)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        record = next(iter(state["runs"].values()))
        # Identity and backend remain intact. Neither a mutable state hash nor
        # a mutually agreeing sidecar turns a changed calibration into evidence.
        for artifact in record["artifacts"]:
            if artifact["kind"] in ("training_camera_contract", "camera_scale_sidecar"):
                path = plan.parent / artifact["path"]
                contract = json.loads(path.read_text(encoding="utf-8"))
                contract["views"][0]["training_K"][0][0] += 1.
                path.write_text(json.dumps(contract), encoding="utf-8")
                artifact.update(sha256=file_digest(path), byte_size=path.stat().st_size)
        plan.with_name("state.json").write_text(json.dumps(state), encoding="utf-8")
        calls.clear()
        with self.assertRaisesRegex(TrainingBatchError, "batch_result_camera_contract_mismatch"):
            run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: [])
        self.assertEqual(calls, [])

    def test_pause_between_runs_is_not_an_optimizer_pause_and_resume_starts_pending_fresh(self):
        from native.worker.training_batch import run_training_batch, BETWEEN_RUNS_PAUSE_CODE
        plan = self.create_public_plan()
        factory, calls, _ = self.fake_process_factory()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=lambda: ["pause"]),
                             BETWEEN_RUNS_PAUSE_CODE)
        self.assertEqual(calls, [])
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["status"], "interrupted")
        self.assertEqual(state["reason"], "pause_between_runs_no_optimizer_checkpoint")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: []), 0)
        self.assertTrue(all("--resume-training" not in command for command in calls))

    def test_cancel_only_the_owned_active_child_and_never_relabel_it_as_full_resume(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        plan = self.create_public_plan()
        factory, calls, marker = self.fake_process_factory("cancel-first")
        sent = [False]
        def cancel_active():
            if marker.exists() and not sent[0]:
                sent[0] = True
                return ["cancel"]
            return []
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_training_batch(plan, process_factory=factory, control_reader=cancel_active), 130)
        self.assertEqual(len(calls), 1)
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["status"], "cancelled")
        self.assertEqual(sorted(record["status"] for record in state["runs"].values()), ["cancelled", "pending"])
        with self.assertRaisesRegex(TrainingBatchError, "batch_resume_unavailable"):
            run_training_batch(plan, resume=True, process_factory=factory, control_reader=lambda: [])
        self.assertEqual(len(calls), 1)

    def test_a_live_lock_is_queried_without_signalling_its_process(self):
        from native.worker.training_batch import run_training_batch, TrainingBatchError
        plan = self.create_public_plan()
        sentinel = subprocess.Popen([sys.executable, "-B", "-c", "import time; time.sleep(30)"],
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        def cleanup():
            if sentinel.poll() is None:
                sentinel.terminate()
            sentinel.wait(timeout=5)
        self.addCleanup(cleanup)
        plan.with_name(".run-lock.json").write_text(json.dumps({"host": socket.gethostname(),
            "pid": sentinel.pid, "nonce": "owned-other-task"}), encoding="utf-8")
        factory, calls, _ = self.fake_process_factory()
        with self.assertRaisesRegex(TrainingBatchError, "batch_already_running"):
            run_training_batch(plan, process_factory=factory, control_reader=lambda: [])
        self.assertIsNone(sentinel.poll())
        self.assertEqual(calls, [])

    def test_public_cli_pause_pipe_reaches_only_its_owned_child_with_parent_stdin_left_open(self):
        plan = self.create_public_plan()
        _, _, marker = self.fake_process_factory("pause-first")
        wrapper = self.root / "cli-process-boundary.py"
        wrapper.write_text("import os,sys\n" +
            "sys.path.insert(0," + repr(str(self.repository)) + ")\n" +
            "from native.worker import training_batch as worker\n" +
            "original=worker.subprocess.Popen\n" +
            "def boundary(command,**options):\n" +
            " command=list(command);command[2]=" + repr(str(self.root / "owned-child.py")) + "\n" +
            " options['env']=dict(os.environ,GSW_BATCH_FIXTURE_MODE='pause-first',GSW_BATCH_FIXTURE_MARKER=" + repr(str(marker)) + ")\n" +
            " return original(command,**options)\n" +
            "worker.subprocess.Popen=boundary\n" +
            "sys.exit(worker.main())\n", encoding="utf-8")
        process = subprocess.Popen([sys.executable, "-B", str(wrapper), "--operation", "run", "--plan", str(plan)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8",
            cwd=str(self.root), creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
        self.addCleanup(cleanup)
        output = []
        readers = [threading.Thread(target=lambda stream=stream: output.append(stream.read()), daemon=True)
                   for stream in (process.stdout, process.stderr)]
        for reader in readers:
            reader.start()
        deadline = time.monotonic() + 10.
        while not marker.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertTrue(marker.exists())
        process.stdin.write("pause\n")
        process.stdin.flush()
        self.assertEqual(process.wait(timeout=15), 75)
        process.stdin.close()
        for reader in readers:
            reader.join(timeout=5)
        process.stdout.close()
        process.stderr.close()
        state = json.loads(plan.with_name("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["status"], "paused", "".join(output)[-2000:])
        self.assertEqual(sum(item["status"] == "pending" for item in state["runs"].values()), 1)


if __name__ == "__main__":
    unittest.main()
