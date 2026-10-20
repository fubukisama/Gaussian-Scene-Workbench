"""Explicit, pinned, serial native Gaussian training batches.

Preview/open/create never execute a job. Only explicit run/resume may start
owned gsw_worker children. Completed artifacts and full-state checkpoint
metadata are reverified before skipping/restoring any individual run.
"""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import socket
import stat
import subprocess
import sys
import threading
import time
import uuid

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from native.backend.training_batch_plan import TrainingBatchError, expand_training_batch
from native.worker.camera_import import _InputCancellation
from native.worker.training_checkpoint import atomic_json, file_digest, read_manifest


MAXIMUM_BYTES = 16 * 1024 * 1024
BETWEEN_RUNS_PAUSE_CODE = 76
_RUN_ID = re.compile(r"^run-[0-9a-f]{24}$")


def _fail(code, detail=""):
    raise TrainingBatchError(code, detail)


def _poll(cancelled):
    if cancelled and cancelled():
        _fail("batch_cancelled")


def _no_links(path):
    for node in (Path(path), *Path(path).parents):
        try:
            metadata = os.lstat(node)
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
            _fail("batch_unsafe_path", str(path))


def _directory(path):
    path = Path(path)
    if not path.is_absolute():
        _fail("batch_absolute_path_required")
    _no_links(path)
    if not path.is_dir():
        _fail("batch_directory_missing", str(path))
    return path.resolve()


def _json_bytes(value):
    data = json.dumps(value, ensure_ascii=False, sort_keys=True, allow_nan=False, indent=2).encode("utf-8")
    if len(data) > MAXIMUM_BYTES:
        _fail("batch_metadata_too_large")
    return data


def _json_hash(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def _read_json(path):
    path = Path(path)
    _no_links(path)
    if not path.is_file() or path.stat().st_size > MAXIMUM_BYTES:
        _fail("batch_metadata_missing_or_too_large", str(path))
    from native.backend.camera_dataset import _read_json as read_object
    return read_object(path)


def _fresh_file(path):
    path = Path(path)
    if not path.is_absolute():
        _fail("batch_absolute_path_required")
    _no_links(path)
    if path.exists():
        _fail("batch_output_exists", str(path))
    if not path.parent.is_dir():
        _fail("batch_directory_missing", str(path.parent))
    return path.resolve()


def _write_new(value, path, cancelled=None):
    path = _fresh_file(path)
    temporary = path.with_name(".batch-" + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(_json_bytes(value))
            stream.flush()
            os.fsync(stream.fileno())
        _poll(cancelled)
        os.link(str(temporary), str(path))
    finally:
        if temporary.exists():
            temporary.unlink()


def _runtime_configuration(run, workspace, repository, root):
    config = copy.deepcopy(run["configuration"])
    config.update(repositoryRoot=str(repository), projectRoot=str(workspace),
        outputRoot=str(root / "outputs"), outputScene=run["id"], outputDisplayName=run["id"],
        jobStore=str(root / "jobs"), nativeCheckpoint=True, runColmap=False, overwrite=False)
    return config


def prepare_training_batch(request, repository, workspace, require_pins=False, cancelled=None):
    """Inspect an explicit request without writing state or executing anything."""
    repository, workspace = _directory(repository), _directory(workspace)
    _json_bytes(request)
    for item in request.get("datasets", []):
        if not isinstance(item, dict) or not Path(str(item.get("path", ""))).is_absolute():
            _fail("batch_absolute_dataset_path_required")
        if require_pins and (not isinstance(item.get("data_identity"), str) or not item["data_identity"]):
            _fail("batch_dataset_pin_required", item.get("id", ""))
    _poll(cancelled)
    report = expand_training_batch(request, repository, cancelled=cancelled)
    _json_bytes(report)
    return report


def create_training_batch_plan(request, repository, workspace, cancelled=None):
    """Publish an immutable plan/config set plus atomic mutable ready state."""
    repository, workspace = _directory(repository), _directory(workspace)
    plan = prepare_training_batch(request, repository, workspace, True, cancelled)
    parent = workspace / ".gsw/training-batches"
    _no_links(parent)
    parent.mkdir(parents=True, exist_ok=True)
    identity = uuid.uuid4().hex
    final = parent / identity
    temporary = parent / (".preparing-" + identity)
    temporary.mkdir()
    try:
        references = {}
        for item in request["datasets"]:
            source = Path(item["path"]).resolve()
            try:
                relative = source.relative_to(workspace).as_posix()
            except ValueError:
                references[item["id"]] = {"kind": "external", "absolute_path": str(source)}
            else:
                references[item["id"]] = {"kind": "managed", "relative_path": relative}
        plan["storage"] = {"schema": "gsw.training_batch_storage/1", "workspace_anchor": str(workspace),
                           "repository_anchor": str(repository), "datasets": references}
        (temporary / "configs").mkdir()
        for run in plan["runs"]:
            if not _RUN_ID.fullmatch(run["id"]):
                _fail("batch_invalid_run_id")
            _write_new(_runtime_configuration(run, workspace, repository, final),
                       temporary / "configs" / (run["id"] + ".json"), cancelled)
        _write_new(plan, temporary / "plan.json", cancelled)
        state = {"schema": "gsw.training_batch_state/1", "plan_sha256": plan["plan_sha256"],
                 "plan_file_sha256": file_digest(temporary / "plan.json"), "status": "ready", "active_run_id": None,
                 "runs": {run["id"]: {"status": "pending", "attempts": 0,
                    "parameters_sha256": _json_hash(run["parameters"]), "artifacts": []} for run in plan["runs"]}}
        _write_new(state, temporary / "state.json", cancelled)
        _poll(cancelled)
        if final.exists():
            _fail("batch_output_exists")
        os.rename(str(temporary), str(final))
        return {"planPath": str(final / "plan.json"), "report": plan, "summary": plan["summary"]}
    finally:
        if temporary.exists() and temporary.parent == parent:
            _no_links(temporary)
            shutil.rmtree(str(temporary))


def _child_path(root, relative):
    if (not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative or
            "\x00" in relative or Path(relative).is_absolute() or ".." in Path(relative).parts):
        _fail("batch_unsafe_path", str(relative))
    path = root / relative
    _no_links(path)
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        _fail("batch_unsafe_path", relative)
    return path


def load_training_batch_plan(plan_path, repository=None, cancelled=None):
    """Rebase only declared managed references, then replay every plan pin."""
    plan_path = Path(plan_path)
    if not plan_path.is_absolute() or plan_path.name != "plan.json":
        _fail("batch_invalid_plan_path")
    _no_links(plan_path)
    root = plan_path.parent
    if (not re.fullmatch(r"[0-9a-f]{32}", root.name) or root.parent.name != "training-batches"
            or root.parent.parent.name != ".gsw"):
        _fail("batch_invalid_plan_path")
    workspace = _directory(root.parent.parent.parent)
    repository = _directory(repository or Path(__file__).resolve().parents[2])
    saved = _read_json(plan_path)
    state = _read_json(root / "state.json")
    if (saved.get("schema") != "gsw.training_batch/1" or state.get("schema") != "gsw.training_batch_state/1"
            or state.get("plan_sha256") != saved.get("plan_sha256")
            or state.get("plan_file_sha256") != file_digest(plan_path)):
        _fail("batch_plan_state_identity_mismatch")
    storage = saved.get("storage", {})
    if storage.get("schema") != "gsw.training_batch_storage/1":
        _fail("batch_invalid_storage")
    request = copy.deepcopy(saved["request"])
    old_workspace = Path(storage["workspace_anchor"])
    old_repository = Path(storage["repository_anchor"])
    old_root = old_workspace / ".gsw/training-batches" / root.name
    references = storage.get("datasets", {})
    for item in request["datasets"]:
        reference = references.get(item["id"], {})
        old_source = Path(item["path"])
        if reference.get("kind") == "managed":
            relative = reference.get("relative_path")
            new_source = _child_path(workspace, relative)
            if os.path.normcase(str(old_workspace / relative)) != os.path.normcase(str(old_source)):
                _fail("batch_invalid_storage")
            item["path"] = str(new_source)
        elif reference.get("kind") == "external" and reference.get("absolute_path") == str(old_source):
            item["path"] = str(old_source)
        else:
            _fail("batch_invalid_storage")
    fresh = prepare_training_batch(request, repository, workspace, True, cancelled)
    if fresh["plan_sha256"] != saved["plan_sha256"]:
        if fresh["source_fingerprints"] != saved.get("source_fingerprints"):
            _fail("batch_source_fingerprint_changed")
        _fail("batch_plan_inputs_changed")
    # A valid digest field does not authorize arbitrary stored configurations.
    # Compare to freshly reconstructed configurations before executing them.
    comparison = copy.deepcopy(saved)
    comparison.pop("storage", None)
    comparison["request"] = request
    for old, new in zip(comparison["datasets"], fresh["datasets"]):
        old["path"], old["manifest_path"] = new["path"], new["manifest_path"]
    for old, new in zip(comparison["runs"], fresh["runs"]):
        for key in ("repositoryRoot", "datasetPath", "cameraDatasetPath"):
            old["configuration"][key] = new["configuration"][key]
    if comparison != fresh:
        _fail("batch_plan_configuration_mismatch")
    if set(state.get("runs", {})) != {run["id"] for run in fresh["runs"]}:
        _fail("batch_state_run_mismatch")
    for old_run, run in zip(saved["runs"], fresh["runs"]):
        expected = _runtime_configuration(old_run, old_workspace, old_repository, old_root)
        stored = _read_json(root / "configs" / (run["id"] + ".json"))
        if stored != expected or state["runs"][run["id"]].get("parameters_sha256") != _json_hash(run["parameters"]):
            _fail("batch_plan_configuration_mismatch", run["id"])
    return {"plan": fresh, "saved": saved, "state": state, "root": root,
            "workspace": workspace, "repository": repository, "planPath": plan_path}


def _artifact(path, root, kind):
    _no_links(path)
    if not path.is_file():
        _fail("batch_result_missing", str(path))
    return {"path": path.relative_to(root).as_posix(), "sha256": file_digest(path),
            "byte_size": path.stat().st_size, "kind": kind}


def _verify_artifacts(records, root):
    if not isinstance(records, list) or not records:
        _fail("batch_result_missing")
    for record in records:
        path = _child_path(root, record["path"])
        if (not path.is_file() or path.stat().st_size != record.get("byte_size")
                or file_digest(path) != record.get("sha256")):
            _fail("batch_result_checksum_mismatch", record["path"])


def _result_paths(root, run):
    output = _child_path(root, "outputs/" + run["id"])
    total = run["configuration"]["trainOptions"]["iterations"]
    ply = output / "point_cloud" / ("iteration_" + str(total)) / "point_cloud.ply"
    return [("gaussian_ply", ply), ("camera_scale_sidecar", ply.with_suffix(".camera-scale.json")),
            ("training_camera_contract", output / "training-camera-contract.json"),
            ("observed_training_seed", output / "batch-observed-training-seed.json")]


def _validate_result(root, run, seed_event):
    paths = dict(_result_paths(root, run))
    ply, sidecar, contract_path = (paths[key] for key in
        ("gaussian_ply", "camera_scale_sidecar", "training_camera_contract"))
    records = [_artifact(path, root, kind) for kind, path in _result_paths(root, run)[:3]]
    contract, metadata = _read_json(contract_path), _read_json(sidecar)
    if (contract.get("schema") != "gsw.training_camera_contract/1"
            or contract.get("input_data_identity") != run["configuration"]["cameraDatasetIdentity"]
            or contract.get("backend") != run["configuration"]["backend"]
            or metadata.get("ply_sha256") != records[0]["sha256"]
            or {key: value for key, value in metadata.items() if key != "ply_sha256"} != contract):
        _fail("batch_result_camera_contract_mismatch")
    # The mutable state and two mutually agreeing output JSON files are not
    # authority for camera calibration, held-out views, scale or initialization.
    # Consume the saved normalization; never solve cameras or normalize again.
    from native.worker.canonical_training import load_prepared_training_dataset, training_contract, _training_size
    prepared_path = contract_path.parent / ".gsw/camera-input/training-camera-dataset.json"
    try:
        prepared_record = _read_json(prepared_path)
        initial_path = prepared_path.parent / prepared_record["initialization_file"]
        _no_links(initial_path)
        prepared = load_prepared_training_dataset(prepared_path, run["configuration"]["datasetPath"])
        expected = training_contract(prepared)
        expected["training_initialization_sha256"] = prepared["training_initialization_sha256"]
        if any(contract.get(key) != value for key, value in expected.items()):
            _fail("batch_result_camera_contract_mismatch")
        resolution = run["configuration"]["trainOptions"]["resolution"]
        if (contract["resolution_policy"] != ("preserve" if resolution == 1 else "manual_scale")
                or any(view["training_size"] != _training_size(view["width"], view["height"], resolution)
                       for view in contract["views"])):
            _fail("batch_result_camera_contract_mismatch")
    except (OSError, ValueError, KeyError, TypeError) as error:
        _fail("batch_result_camera_contract_mismatch", str(error))
    if (not isinstance(seed_event, dict) or seed_event.get("schema") != "gsw.training_seed/1"
            or type(seed_event.get("training_seed")) is not int
            or seed_event["training_seed"] != run["configuration"]["trainingSeed"]):
        _fail("batch_training_seed_not_observed")
    return records


def _complete_result(root, run, seed_event):
    records = _validate_result(root, run, seed_event)
    seed_record = dict(_result_paths(root, run))["observed_training_seed"]
    _write_new(seed_event, seed_record)
    records.append(_artifact(seed_record, root, "observed_training_seed"))
    return records


def _verify_completed_result(root, run, records):
    expected = {(kind, path.relative_to(root).as_posix()) for kind, path in _result_paths(root, run)}
    if (not isinstance(records, list) or len(records) != len(expected)
            or any(not isinstance(record, dict) or not isinstance(record.get("kind"), str)
                   or not isinstance(record.get("path"), str) for record in records)
            or {(record["kind"], record["path"]) for record in records} != expected):
        _fail("batch_result_artifact_set_mismatch", run["id"])
    _verify_artifacts(records, root)
    seed_record = dict(_result_paths(root, run))["observed_training_seed"]
    _validate_result(root, run, _read_json(seed_record))


def _checkpoint(root, run):
    output = _child_path(root, "outputs/" + run["id"])
    _no_links(output / ".gsw-resume")
    _no_links(output / ".gsw-resume/ready.json")
    try:
        manifest, payload = read_manifest(output)
    except (OSError, ValueError, KeyError) as error:
        _fail("batch_resume_checkpoint_unavailable", str(error))
    contract = manifest.get("camera_contract", {})
    contract_path = output / "training-camera-contract.json"
    if (not payload.stat().st_size or not re.fullmatch(r"[0-9a-f]{64}", str(manifest.get("identity", "")))
            or type(manifest.get("training_seed")) is not int
            or manifest.get("backend") != run["configuration"]["backend"]
            or manifest.get("total") != run["configuration"]["trainOptions"]["iterations"]
            or manifest.get("training_seed", 0) != run["configuration"]["trainingSeed"]
            or contract.get("schema") != "gsw.training_camera_contract/1"
            or contract.get("pose_policy") != "locked"
            or contract.get("backend") != run["configuration"]["backend"]
            or contract.get("input_data_identity") != run["configuration"]["cameraDatasetIdentity"]
            or _read_json(contract_path) != contract):
        _fail("batch_resume_checkpoint_mismatch")
    return {"manifest": copy.deepcopy(manifest), "artifacts": [
        _artifact(output / ".gsw-resume/ready.json", root, "optimizer_checkpoint_manifest"),
        _artifact(payload, root, "optimizer_checkpoint_payload")],
        "validation": "manifest_and_payload_bytes_verified_not_deserialized"}


class _Commands:
    """Nonblocking pause/cancel input; no stdin thread at interpreter exit."""
    def __init__(self):
        self.descriptor, self.pending = None, b""
        try:
            descriptor = sys.stdin.fileno()
            if os.name == "nt":
                import ctypes
                import msvcrt
                from ctypes import wintypes
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.GetFileType.argtypes, kernel.GetFileType.restype = [wintypes.HANDLE], wintypes.DWORD
                kernel.PeekNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                    wintypes.LPDWORD, wintypes.LPDWORD, wintypes.LPDWORD]
                kernel.PeekNamedPipe.restype = wintypes.BOOL
                handle = wintypes.HANDLE(msvcrt.get_osfhandle(descriptor))
                if kernel.GetFileType(handle) != 3:
                    return
                def available():
                    count = wintypes.DWORD()
                    return count.value if not kernel.PeekNamedPipe(handle, None, 0, None, ctypes.byref(count), None) else count.value
            else:
                import select
                def available():
                    return 4096 if select.select([descriptor], [], [], 0)[0] else 0
            self.descriptor, self.available = descriptor, available
        except (OSError, ValueError, AttributeError):
            pass

    def __call__(self):
        commands = []
        if self.descriptor is None:
            return commands
        try:
            for _ in range(16):
                available = self.available()
                if not available:
                    break
                data = os.read(self.descriptor, min(available, 4096))
                if not data:
                    self.descriptor = None
                    break
                self.pending += data
                while b"\n" in self.pending:
                    line, self.pending = self.pending.split(b"\n", 1)
                    value = line.strip().lower()
                    if value in (b"pause", b"cancel"):
                        commands.append(value.decode("ascii"))
                if len(self.pending) > 4096:
                    self.descriptor = None
                    break
        except (OSError, ValueError):
            self.descriptor = None
        return commands


def _event(state, stage, progress=0., **values):
    payload = dict(version=1, type="status", state=state, stage=stage,
                   progressPercent=max(0, min(100, int(round(progress)))))
    payload.update(values)
    print("[worker-event] " + json.dumps(payload, ensure_ascii=False, separators=(",", ":")), flush=True)


def _run_child(command, configuration, control_reader, process_factory, on_line):
    process = process_factory(command, cwd=str(configuration.parent), stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    messages = queue.Queue(maxsize=512)
    def drain(name, stream):
        try:
            for line in iter(stream.readline, ""):
                messages.put((name, line))
        finally:
            messages.put((name, None))
    readers = [threading.Thread(target=drain, args=("stdout", process.stdout), daemon=True),
               threading.Thread(target=drain, args=("stderr", process.stderr), daemon=True)]
    for reader in readers:
        reader.start()
    closed, requested = set(), set()
    try:
        while process.poll() is None or len(closed) < 2:
            for operation in control_reader() or []:
                if operation not in ("pause", "cancel") or operation in requested:
                    continue
                requested.add(operation)
                try:
                    process.stdin.write(operation + "\n")
                    process.stdin.flush()
                except (OSError, ValueError, AttributeError):
                    pass
            try:
                name, line = messages.get(timeout=.05)
            except queue.Empty:
                continue
            if line is None:
                closed.add(name)
                continue
            terminal = on_line(name, line)
            if terminal and process.stdin and not process.stdin.closed:
                process.stdin.close()
        return process.wait(), requested
    finally:
        if process.stdin and not process.stdin.closed:
            process.stdin.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        for reader in readers:
            reader.join(timeout=2)
        for stream in (process.stdout, process.stderr):
            if stream:
                stream.close()


def _acquire_lock(root):
    path = root / ".run-lock.json"
    _no_links(path)
    if path.exists():
        previous = _read_json(path)
        if (previous.get("host") != socket.gethostname() or type(previous.get("pid")) is not int
                or previous["pid"] <= 0):
            _fail("batch_already_running_or_external_lock")
        alive = _pid_alive(previous["pid"])
        if alive:
            _fail("batch_already_running")
        path.unlink()
    value = {"pid": os.getpid(), "host": socket.gethostname(), "nonce": uuid.uuid4().hex}
    _write_new(value, path)
    return path, value


def _pid_alive(pid):
    # Windows os.kill(pid, 0) is not a harmless POSIX existence probe. Query a
    # limited process handle instead; never terminate a lock owner's process.
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetExitCodeProcess.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return ctypes.get_last_error() == 5  # Access denied is not dead.
        try:
            result = wintypes.DWORD()
            return not kernel.GetExitCodeProcess(handle, ctypes.byref(result)) or result.value == 259
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def run_training_batch(plan_path, resume=False, process_factory=None, control_reader=None):
    """Execute serial owned workers; no PLY-only or automatic state resume."""
    process_factory = process_factory or subprocess.Popen
    control_reader = control_reader or _Commands()
    loaded = load_training_batch_plan(plan_path)
    root, plan, state = loaded["root"], loaded["plan"], loaded["state"]
    lock_path, lock_value = _acquire_lock(root)
    state_path = root / "state.json"
    try:
        # Re-read under the exclusive lock: a previous owner may have completed
        # after the optimistic read but before this worker acquired ownership.
        loaded = load_training_batch_plan(plan_path)
        plan, state = loaded["plan"], loaded["state"]
        if not resume and state["status"] != "ready":
            _fail("batch_explicit_resume_required")
        if resume and state["status"] not in ("paused", "interrupted", "running", "completed"):
            _fail("batch_resume_unavailable", state["status"])
        for run in plan["runs"]:
            record = state["runs"][run["id"]]
            if record["status"] == "completed":
                _verify_completed_result(root, run, record["artifacts"])
            elif record["status"] in ("running", "paused", "interrupted"):
                if not resume:
                    _fail("batch_explicit_resume_required")
                checkpoint = _checkpoint(root, run)
                if record.get("checkpoint"):
                    _verify_artifacts(record["checkpoint"]["artifacts"], root)
                record["checkpoint"] = checkpoint
            elif record["status"] == "failed" and plan["run_policy"]["on_failure"] == "continue":
                # Continuation is a scheduler policy, not optimizer resume for
                # the failed experiment. Keep its failure/attempts unchanged.
                continue
            elif record["status"] != "pending":
                _fail("batch_resume_unavailable", run["id"])
        count = len(plan["runs"])
        completed = sum(record["status"] == "completed" for record in state["runs"].values())
        finished = sum(record["status"] in ("completed", "failed") for record in state["runs"].values())
        for run in plan["runs"]:
            record = state["runs"][run["id"]]
            if (record["status"] == "completed" or
                    record["status"] == "failed" and plan["run_policy"]["on_failure"] == "continue"):
                continue
            commands = control_reader() or []
            if "cancel" in commands or "pause" in commands:
                cancelled = "cancel" in commands
                state.update(status="cancelled" if cancelled else "interrupted", active_run_id=None,
                    reason="cancel_between_runs" if cancelled else "pause_between_runs_no_optimizer_checkpoint")
                atomic_json(state_path, state)
                _event("cancelled" if cancelled else "paused", "batch_between_runs", finished * 100. / count)
                return 130 if cancelled else BETWEEN_RUNS_PAUSE_CODE
            # Verify current bytes/source again before each subsequent child.
            load_training_batch_plan(plan_path)
            commands = control_reader() or []
            if "cancel" in commands or "pause" in commands:
                cancelled = "cancel" in commands
                state.update(status="cancelled" if cancelled else "interrupted", active_run_id=None,
                    reason="cancel_between_runs" if cancelled else "pause_between_runs_no_optimizer_checkpoint")
                atomic_json(state_path, state)
                _event("cancelled" if cancelled else "paused", "batch_between_runs", finished * 100. / count)
                return 130 if cancelled else BETWEEN_RUNS_PAUSE_CODE
            continuing = record["status"] in ("running", "paused", "interrupted")
            if continuing:
                record.setdefault("resume_receipts", []).append(copy.deepcopy(record["checkpoint"]))
            record.update(status="running", attempts=record["attempts"] + 1, artifacts=[])
            state.update(status="running", active_run_id=run["id"])
            atomic_json(state_path, state)
            executions = root / "executions"
            executions.mkdir(exist_ok=True)
            configuration = _runtime_configuration(run, loaded["workspace"], loaded["repository"], root)
            configuration_path = executions / (run["id"] + "-" + str(record["attempts"]) + ".json")
            _write_new(configuration, configuration_path)
            record["active_configuration"] = _artifact(configuration_path, root, "training_configuration")
            atomic_json(state_path, state)
            command = [sys.executable, "-B", str(loaded["repository"] / "native/worker/gsw_worker.py"),
                       "--task", "training", "--config", str(configuration_path)]
            if continuing:
                command.append("--resume-training")
            observed_seed = [None]
            logs = root / "logs"
            logs.mkdir(exist_ok=True)
            with (logs / (run["id"] + "-" + str(record["attempts"]) + ".log")).open("x", encoding="utf-8") as log:
                def on_line(channel, line):
                    log.write(channel + ": " + line)
                    if "[gsw-training-seed] " in line:
                        try:
                            seed = json.JSONDecoder().raw_decode(line.split("[gsw-training-seed] ", 1)[1])[0]
                            if seed.get("schema") == "gsw.training_seed/1":
                                observed_seed[0] = seed
                        except (ValueError, TypeError, AttributeError):
                            pass
                    if line.startswith("[worker-event] "):
                        try:
                            event = json.loads(line[len("[worker-event] "):])
                        except ValueError:
                            event = {}
                        from native.worker.gsw_worker import status_telemetry
                        telemetry = status_telemetry(event)
                        telemetry.pop("previewPath", None)
                        telemetry.pop("previewKind", None)
                        fraction = float(event.get("progressPercent", 0)) / 100.
                        _event("running", "batch_run", (finished + max(0., min(1., fraction))) * 100. / count,
                               batchRunId=run["id"], batchCompletedRuns=completed, batchTotalRuns=count, **telemetry)
                        return event.get("state") in ("done", "completed", "paused", "failed", "cancelled")
                    print("[batch-child " + run["id"] + "] " + line.rstrip(), flush=True)
                    return False
                code, requested = _run_child(command, configuration_path, control_reader, process_factory, on_line)
            record["returncode"] = code
            if code == 0:
                record.update(status="completed", artifacts=_complete_result(root, run, observed_seed[0]))
                record.pop("checkpoint", None)
                completed += 1
            elif code == 75:
                record.update(status="paused", checkpoint=_checkpoint(root, run))
                state.update(status="paused", active_run_id=run["id"])
                atomic_json(state_path, state)
                _event("paused", "batch_optimizer_checkpoint", finished * 100. / count)
                return 75
            else:
                record["status"] = "cancelled" if code == 130 or "cancel" in requested else "failed"
                record.pop("checkpoint", None)
                state.update(status=record["status"], active_run_id=None)
                atomic_json(state_path, state)
                if record["status"] == "cancelled" or plan["run_policy"]["on_failure"] == "stop":
                    _event(record["status"], "batch_child_stopped", finished * 100. / count)
                    return 130 if record["status"] == "cancelled" else 1
            finished += 1
            state["active_run_id"] = None
            atomic_json(state_path, state)
            if "cancel" in requested or "pause" in requested:
                if completed == count:
                    break
                state.update(status="cancelled" if "cancel" in requested else "interrupted",
                    reason="cancel_between_runs" if "cancel" in requested else "pause_between_runs_no_optimizer_checkpoint")
                atomic_json(state_path, state)
                return 130 if "cancel" in requested else BETWEEN_RUNS_PAUSE_CODE
        failed = sum(record["status"] == "failed" for record in state["runs"].values())
        state.update(status="failed" if failed else "completed", active_run_id=None)
        atomic_json(state_path, state)
        _event("failed" if failed else "done", "batch_completed", 100.)
        print(json.dumps({"success": not failed, "status": state["status"], "statePath": str(state_path),
            "planPath": str(plan_path), "completedRuns": completed, "failedRuns": failed}), flush=True)
        return 1 if failed else 0
    except BaseException as error:
        if state.get("status") == "running":
            active = state.get("active_run_id")
            if active:
                state["runs"][active]["status"] = "failed" if isinstance(error, TrainingBatchError) else "interrupted"
            state.update(status="failed" if isinstance(error, TrainingBatchError) else "interrupted",
                         reason=getattr(error, "code", type(error).__name__))
            atomic_json(state_path, state)
        raise
    finally:
        if lock_path.exists() and _read_json(lock_path) == lock_value:
            lock_path.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", required=True, choices=("preview", "create-plan", "run", "resume"))
    parser.add_argument("--request")
    parser.add_argument("--repository")
    parser.add_argument("--workspace")
    parser.add_argument("--report")
    parser.add_argument("--plan")
    arguments = parser.parse_args(argv)
    try:
        if arguments.operation in ("run", "resume"):
            if not arguments.plan:
                _fail("batch_plan_required")
            return run_training_batch(arguments.plan, resume=arguments.operation == "resume")
        cancel = _InputCancellation()
        if not all((arguments.request, arguments.repository, arguments.workspace, arguments.report)):
            _fail("batch_plan_arguments_required")
        report_path = _fresh_file(arguments.report)
        request = _read_json(arguments.request)
        if arguments.operation == "create-plan":
            created = create_training_batch_plan(request, arguments.repository, arguments.workspace, cancel)
            report, plan_path = created["report"], created["planPath"]
        else:
            report = prepare_training_batch(request, arguments.repository, arguments.workspace, cancelled=cancel)
            plan_path = ""
        _write_new(report, report_path, None if plan_path else cancel)
        print(json.dumps({"success": True, "reportPath": str(report_path), "planPath": plan_path,
                          "summary": report["summary"]}, ensure_ascii=False, allow_nan=False), flush=True)
        return 0
    except Exception as error:
        code = getattr(error, "code", "batch_operation_failed")
        print(json.dumps({"success": False, "code": code, "detail": str(getattr(error, "detail", error))},
                         ensure_ascii=False, allow_nan=False), flush=True)
        return 130 if "cancel" in code else 2


if __name__ == "__main__":
    sys.exit(main())
