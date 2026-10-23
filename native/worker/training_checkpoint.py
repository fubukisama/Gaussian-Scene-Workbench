"""Native, iteration-boundary training recovery. No torch import on UI workers.

The tensor payload uses PyTorch's pickle-based checkpoint format: load only after
an explicit user trust confirmation. SHA-256 detects corruption, not provenance.
The manifest is published last; an interrupted save preserves the previous one.
"""

import hashlib
import json
import os
from pathlib import Path
import random
import copy
import re
import uuid

from native.backend.atomic_file import atomic_sibling, atomic_sibling_directory

PAUSED_EXIT_CODE = 75
CONTROL_ENV = "GSW_NATIVE_TRAINING_CONTROL"
CHECKPOINT_NAME = re.compile(r"^state-[0-9a-f]{32}\.pth$")


def _quality_resolution(value, backend, require_resolution=False):
    from native.worker.training_quality_plan import resolve_quality_plan
    if isinstance(value, dict) and value.get("schema") == "gsw.quality_plan_resolution/1":
        expected = resolve_quality_plan(value.get("requested"), backend=backend)
        if value != expected:
            raise ValueError("quality_plan_resolution_invalid")
        return expected
    if require_resolution:
        raise ValueError("quality_plan_resolution_invalid")
    return resolve_quality_plan(value, backend=backend)


def quality_plan_for_control(requested=None, *, backend="3dgs", checkpoint_plan=None, resume=False):
    """Resolve explicit opt-in or inherit an exact, complete checkpoint plan.

    Legacy absence stays absent. Selecting even Standard explicitly changes the
    RNG contract, so it cannot silently resume a legacy checkpoint without one.
    """
    saved = (_quality_resolution(checkpoint_plan, backend, require_resolution=True)
             if checkpoint_plan is not None else None)
    selected = _quality_resolution(requested, backend) if requested is not None else None
    if resume:
        if selected is not None and selected != saved:
            raise ValueError("quality_resume_plan_mismatch")
        return copy.deepcopy(saved)
    return copy.deepcopy(selected)


class QualityTrainingLifecycle:
    """Plan/LR/RNG lifecycle; algorithm execution belongs to a real controller."""
    VERSION = 1

    def __init__(self, plan, seed, overrides):
        from native.worker.quality_random import QualityRandomStreams
        self.plan = copy.deepcopy(plan)
        self.random_streams = QualityRandomStreams(seed)
        self.geometry_overrides = dict(overrides)
        self.controller = None

    def bind_model(self, model):
        model._gsw_quality_training = self

    def bind_controller(self, controller):
        if controller is not None and (not callable(getattr(controller, "capture", None))
                                       or not callable(getattr(controller, "restore", None))):
            raise ValueError("quality_controller_state_interface_invalid")
        if self.controller is not None and self.controller is not controller:
            raise ValueError("quality_controller_already_bound")
        self.controller = controller

    def assert_ready(self):
        effective = self.plan["effective"]
        baseline = "legacy_ewa" if self.plan["backend"] == "3dgs" else "surfel_default"
        supervision = effective.get("supervision", {})
        needs_controller = (effective["densification"]["strategy"] != "standard"
            or effective["filter"]["mode"] != baseline
            or bool(supervision))
        if needs_controller and self.controller is None:
            raise ValueError("quality_controller_required")

    def choose_index(self, count):
        return self.random_streams.choose_index(count)

    def report(self):
        return {"quality_plan": copy.deepcopy(self.plan), "geometry_rate_overrides": dict(self.geometry_overrides),
                "rng_policy": "independent_image_and_strategy_streams_v1", "quality_accepted": False}

    def capture_extras(self):
        self.assert_ready()
        state = {"version": self.VERSION, "rng": self.random_streams.state_dict()}
        if self.controller is not None:
            state["controller"] = self.controller.capture()
        return state

    def validate_restore(self, plan, state):
        if _quality_resolution(plan, self.plan["backend"], require_resolution=True) != self.plan:
            raise ValueError("quality_resume_plan_mismatch")
        if (type(state) is not dict or state.get("version") != self.VERSION
                or set(state) - {"version", "rng", "controller"} or "rng" not in state):
            raise ValueError("quality_checkpoint_state_invalid")
        if ("controller" in state) != (self.controller is not None):
            raise ValueError("quality_controller_checkpoint_mismatch")
        from native.worker.quality_random import QualityRandomStreams
        candidate = QualityRandomStreams(self.random_streams.seed)
        candidate.load_state_dict(state["rng"])
        self.assert_ready()

    def restore_extras(self, state):
        # Restore the same streams object: bound controllers may retain it.
        self.random_streams.load_state_dict(state["rng"])
        if self.controller is not None:
            self.controller.restore(state["controller"])


def prepare_quality_training(control, dataset, optimization, backend):
    """Prepare exactly once before optimizer setup; absence is a strict no-op."""
    if not control:
        return None
    manifest = None
    if control.get("resume"):
        manifest, unused = read_manifest(control["output"])
    plan = quality_plan_for_control(control.get("qualityPlan"), backend=backend,
        checkpoint_plan=(manifest or {}).get("quality_plan"), resume=bool(control.get("resume")))
    if plan is None:
        return None
    from native.worker.training_quality_plan import geometry_rate_overrides
    overrides = geometry_rate_overrides(plan, vars(optimization))
    for name, value in overrides.items():
        setattr(optimization, name, value)
    control["qualityPlan"] = copy.deepcopy(plan)
    dataset._gsw_quality_plan = copy.deepcopy(plan)
    return QualityTrainingLifecycle(plan, getattr(dataset, "training_seed", 0), overrides)


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with atomic_sibling(path) as (temporary, stream):
        json.dump(value, stream, ensure_ascii=False, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
        stream.close()
        os.replace(str(temporary), str(path))


def file_digest(path, text_source=False):
    if text_source:
        return hashlib.sha256(Path(path).read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_manifest(output, verify=True):
    root = Path(output).resolve() / ".gsw-resume"
    if root.is_symlink() or root.resolve().parent != Path(output).resolve():
        raise ValueError("Training checkpoint directory leaves the output directory")
    with (root / "ready.json").open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    if (manifest.get("version") != 1 or
            not CHECKPOINT_NAME.fullmatch(manifest.get("file", "")) or
            type(manifest.get("iteration")) is not int or
            type(manifest.get("total")) is not int or
            not 0 < manifest["iteration"] < manifest["total"]):
        raise ValueError("Invalid native training checkpoint manifest")
    checkpoint = root / manifest["file"]
    if checkpoint.resolve().parent != root.resolve() or not checkpoint.is_file():
        raise ValueError("Native training checkpoint is missing or outside its directory")
    if verify and file_digest(checkpoint) != manifest.get("sha256"):
        raise ValueError("Native training checkpoint checksum does not match")
    return manifest, checkpoint


def _scheduled_directory(output, iteration):
    if type(iteration) is not int or iteration <= 0:
        raise ValueError("quality_scheduled_iteration_invalid")
    output = Path(output).resolve()
    root = output / ".gsw-resume"
    parent = root / "scheduled"
    target = parent / ("iteration-" + str(iteration))
    for path in (root, parent, target):
        if path.exists() and (path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400):
            raise ValueError("quality_scheduled_checkpoint_unsafe_path")
    if target.resolve().parent != parent.resolve() or parent.resolve().parent != root.resolve() or root.resolve().parent != output:
        raise ValueError("quality_scheduled_checkpoint_unsafe_path")
    return target


def read_scheduled_manifest(output, iteration, verify=True):
    root = _scheduled_directory(output, iteration)
    with (root / "ready.json").open(encoding="utf-8") as stream:
        manifest = json.load(stream)
    if (manifest.get("schema") != "gsw.training_scheduled_checkpoint/1" or manifest.get("version") != 1
            or manifest.get("state_kind") != "scheduled" or not CHECKPOINT_NAME.fullmatch(manifest.get("file", ""))
            or type(manifest.get("iteration")) is not int or manifest["iteration"] != iteration
            or type(manifest.get("total")) is not int or not 0 < iteration <= manifest["total"]
            or type(manifest.get("completed")) is not bool or manifest["completed"] != (iteration == manifest["total"])):
        raise ValueError("quality_scheduled_checkpoint_invalid")
    checkpoint = root / manifest["file"]
    if checkpoint.is_symlink() or checkpoint.resolve().parent != root.resolve() or not checkpoint.is_file():
        raise ValueError("quality_scheduled_checkpoint_unsafe_path")
    if verify and file_digest(checkpoint) != manifest.get("sha256"):
        raise ValueError("quality_scheduled_checkpoint_checksum_mismatch")
    return manifest, checkpoint


def training_identity(dataset, opt, pipe):
    """Detect changed settings, camera reconstruction and image inputs before load."""
    source = Path(dataset.source_path).resolve()
    files = []
    for path in sorted(source.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(source)
        # Exclude generated previews, logs, cache and unrelated user files.
        if relative.parts[0] in (".gsw", ".alignment_cache"):
            continue
        if (relative.parts[0] not in ("sparse", "images", "images_2", "images_4", "images_8", "depths")
                and path.suffix.lower() not in (".png", ".jpg", ".jpeg", ".tif", ".tiff", ".exr")
                and path.name not in ("transforms_train.json", "transforms_val.json", "transforms_test.json",
                                      "points3d.ply", "camera_dataset.json")):
            continue
        info = path.stat()
        files.append((relative.as_posix(), info.st_size, file_digest(path)))
    settings = {"dataset": {k: v for k, v in vars(dataset).items()
                             if k not in ("source_path", "model_path")},
                "optimization": vars(opt), "pipeline": vars(pipe), "files": files,
                # Density policy changes invalidate old optimizer-state resumes.
                "native_density_source": file_digest(Path(__file__).with_name("training_density_control.py"))}
    if getattr(dataset, "_gsw_camera_contract", None) is not None:
        settings["native_density_source"] = file_digest(Path(__file__).with_name("training_density_control.py"), text_source=True)
        settings["native_camera_training_source"] = file_digest(Path(__file__).with_name("canonical_training.py"), text_source=True)
        settings["native_camera_dataset_source"] = file_digest(Path(__file__).parents[1] / "backend" / "camera_dataset.py", text_source=True)
        settings["native_initialization_source"] = file_digest(Path(__file__).parents[1] / "backend" / "initialization.py", text_source=True)
    if hasattr(dataset, "training_seed"):
        settings["native_training_seed_source"] = file_digest(Path(__file__).with_name("training_seed.py"), text_source=True)
    if getattr(dataset, "_gsw_quality_plan", None) is not None:
        settings["quality_sources"] = {name: file_digest(Path(__file__).with_name(name), text_source=True)
            for name in ("training_quality_plan.py", "quality_random.py", "training_checkpoint.py")}
        entry = (Path(__file__).parents[2] / "gaussian-splatting/train.py" if
            dataset._gsw_quality_plan["backend"] == "3dgs" else Path(__file__).with_name("two_dgs_train.py"))
        settings["quality_trainer_source"] = file_digest(entry, text_source=True)
    return hashlib.sha256(json.dumps(settings, sort_keys=True).encode("utf-8")).hexdigest()


def capture_state(torch, numpy, gaussians, iteration, total, identity,
                  camera_names, pending_indices, elapsed, ema_loss, ema_depth, backend="3dgs"):
    state = {
        "version": 1, "backend": backend, "iteration": iteration, "total": total, "identity": identity,
        # Reuse the vendored Graphdeco implementation, including Adam moments
        # and densification accumulators; exposure is not included upstream.
        "model": gaussians.capture(),
        "random": random.getstate(), "numpy": numpy.random.get_state(),
        "torch": torch.get_rng_state(),
        "cuda": torch.cuda.get_rng_state_all() if torch.cuda.is_available() else [],
        "camera_names": camera_names, "pending_indices": list(pending_indices),
        "elapsed": elapsed, "ema_loss": ema_loss, "ema_depth": ema_depth,
        "training_seed": getattr(gaussians, "_gsw_training_seed", 0),
    }
    if getattr(gaussians, "_gsw_camera_contract", None) is not None:
        state["camera_contract"] = copy.deepcopy(gaussians._gsw_camera_contract)
    quality_training = getattr(gaussians, "_gsw_quality_training", None)
    if quality_training is not None:
        state["quality_plan"] = copy.deepcopy(quality_training.plan)
        state["quality_state"] = quality_training.capture_extras()
    if backend == "3dgs":
        state.update(exposure=gaussians._exposure,
                     exposure_optimizer=gaussians.exposure_optimizer.state_dict(),
                     exposure_mapping=gaussians.exposure_mapping,
                     pretrained_exposures=gaussians.pretrained_exposures)
    elif backend != "2dgs":
        raise ValueError("Unsupported checkpoint backend: " + backend)
    return state


def restore_state(torch, numpy, gaussians, opt, state, identity, camera_names, backend="3dgs"):
    if state.get("training_seed", 0) != getattr(gaussians, "_gsw_training_seed", 0):
        raise ValueError("Training seed changed; cannot resume this checkpoint")
    if state.get("camera_contract") != getattr(gaussians, "_gsw_camera_contract", None):
        raise ValueError("Camera/scale contract changed; cannot resume this checkpoint")
    if (state.get("version") != 1 or state.get("identity") != identity or
            state.get("total") != opt.iterations or state.get("camera_names") != camera_names or
            state.get("backend", "3dgs") != backend or backend not in ("3dgs", "2dgs")):
        raise ValueError("Training inputs/settings changed; cannot resume this checkpoint")
    pending = state["pending_indices"]
    if (len(set(pending)) != len(pending) or
            any(type(i) is not int or not 0 <= i < len(camera_names) for i in pending)):
        raise ValueError("Invalid pending camera sampler state")
    quality_training = getattr(gaussians, "_gsw_quality_training", None)
    if quality_training is None:
        if "quality_plan" in state or "quality_state" in state:
            raise ValueError("quality_checkpoint_requires_explicit_lifecycle")
    else:
        quality_training.validate_restore(state.get("quality_plan"), state.get("quality_state"))
    if backend == "3dgs":
        gaussians._exposure = state["exposure"]
        gaussians.exposure_mapping = state["exposure_mapping"]
        gaussians.pretrained_exposures = state["pretrained_exposures"]
    gaussians.restore(state["model"], opt)
    if backend == "3dgs":
        gaussians.exposure_optimizer.load_state_dict(state["exposure_optimizer"])
    if quality_training is not None:
        quality_training.restore_extras(state["quality_state"])
    random.setstate(state["random"])
    numpy.random.set_state(state["numpy"])
    torch.set_rng_state(state["torch"].cpu())
    if state["cuda"]:
        torch.cuda.set_rng_state_all([value.cpu() for value in state["cuda"]])


class TrainingCheckpoint:
    def __init__(self, output, control):
        self.output = Path(output).resolve()
        self.control = control
        if Path(control["output"]).resolve() != self.output:
            raise ValueError("Native training control/output mismatch")
        self.root = self.output / ".gsw-resume"

    def pause_requested(self):
        try:
            with open(self.control["request"], "r", encoding="utf-8") as stream:
                return json.load(stream).get("session") == self.control["session"]
        except (OSError, ValueError):
            return False

    def save(self, torch, state):
        self.root.mkdir(parents=True, exist_ok=True)
        if self.root.resolve().parent != self.output:
            raise ValueError("Checkpoint directory leaves output root")
        previous = None
        try:
            _, previous = read_manifest(self.output, verify=False)
        except (OSError, ValueError):
            pass
        path = self.root / ("state-" + uuid.uuid4().hex + ".pth")
        temporary = path.with_suffix(".tmp")
        try:
            with temporary.open("wb") as stream:
                torch.save(state, stream)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(str(temporary), str(path))
            manifest = {"version": 1, "file": path.name, "iteration": state["iteration"],
                        "backend": state.get("backend", "3dgs"),
                        "total": state["total"], "identity": state["identity"],
                        "sha256": file_digest(path), "session": self.control["session"],
                        "training_seed": state.get("training_seed", 0)}
            if "camera_contract" in state:
                manifest["camera_contract"] = copy.deepcopy(state["camera_contract"])
            if "quality_plan" in state:
                manifest["quality_plan"] = copy.deepcopy(state["quality_plan"])
            for key in ("performance", "renderer_engine"):
                if key in state:
                    manifest[key] = copy.deepcopy(state[key])
            for key in ("runtime_options", "runtime_configuration", "runtime_policy_history"):
                if key in state:
                    manifest[key] = copy.deepcopy(state[key])
            if state.get("pause_reason") in ("vram_pressure", "resource_probe_unavailable", "resource_estimate_unavailable"):
                manifest["pause_reason"] = state["pause_reason"]
            atomic_json(self.root / "ready.json", manifest)
        finally:
            if temporary.exists():
                temporary.unlink()
        # Never delete the previous state until its replacement is durable.
        if previous and previous != path:
            try:
                previous.unlink()
            except OSError:
                pass
        return manifest

    def load(self, torch, identity):
        manifest, path = read_manifest(self.output)
        return self._load_verified(torch, identity, manifest, path)

    def save_scheduled(self, torch, state):
        """Publish a new full-state schedule directory; never replace history.

        Final-iteration state is an explicitly completed audit snapshot, not a
        resumable main ready.json. Legacy tuple checkpoints remain untouched.
        """
        required = {"version", "backend", "iteration", "total", "identity", "model", "random", "numpy",
                    "torch", "cuda", "camera_names", "pending_indices", "training_seed", "quality_plan", "quality_state"}
        if (type(state) is not dict or required - set(state) or state.get("version") != 1
                or type(state.get("iteration")) is not int or type(state.get("total")) is not int
                or not 0 < state["iteration"] <= state["total"]):
            raise ValueError("quality_scheduled_state_invalid")
        plan = quality_plan_for_control(self.control.get("qualityPlan"), backend=state["backend"])
        if plan is None or plan != state["quality_plan"]:
            raise ValueError("quality_resume_plan_mismatch")
        destination = _scheduled_directory(self.output, state["iteration"])
        if destination.exists():
            raise ValueError("quality_scheduled_checkpoint_exists")
        destination.parent.mkdir(parents=True, exist_ok=True)
        with atomic_sibling_directory(destination) as temporary:
            checkpoint = temporary / ("state-" + uuid.uuid4().hex + ".pth")
            with checkpoint.open("wb") as stream:
                torch.save(state, stream)
                stream.flush()
                os.fsync(stream.fileno())
            manifest = {"schema": "gsw.training_scheduled_checkpoint/1", "version": 1,
                "state_kind": "scheduled", "completed": state["iteration"] == state["total"],
                "file": checkpoint.name, "sha256": file_digest(checkpoint), "backend": state["backend"],
                "iteration": state["iteration"], "total": state["total"], "identity": state["identity"],
                "training_seed": state["training_seed"], "session": self.control["session"]}
            for key in ("camera_contract", "quality_plan", "performance", "renderer_engine", "runtime_options",
                        "runtime_configuration", "runtime_policy_history"):
                if key in state:
                    manifest[key] = copy.deepcopy(state[key])
            atomic_json(temporary / "ready.json", manifest)
            if destination.exists():
                raise ValueError("quality_scheduled_checkpoint_exists")
            os.rename(str(temporary), str(destination))
        return manifest

    def load_scheduled(self, torch, identity, iteration):
        manifest, path = read_scheduled_manifest(self.output, iteration)
        if manifest["completed"]:
            raise ValueError("quality_checkpoint_completed_not_resumable")
        return self._load_verified(torch, identity, manifest, path)

    def _load_verified(self, torch, identity, manifest, path):
        if (manifest.get("identity") != identity or
                manifest.get("backend", "3dgs") != self.control.get("backend", "3dgs")):
            raise ValueError("Training inputs/settings changed; checkpoint was not loaded")
        if manifest.get("training_seed", 0) != self.control.get("training_seed", 0):
            raise ValueError("Training seed changed; checkpoint was not loaded")
        quality_plan_for_control(self.control.get("qualityPlan"), backend=self.control.get("backend", "3dgs"),
            checkpoint_plan=manifest.get("quality_plan"), resume=True)
        # Only called by the explicit, trust-confirmed native Resume action.
        state = torch.load(str(path))
        if (state.get("backend", "3dgs") != manifest.get("backend", "3dgs") or
                state.get("iteration") != manifest["iteration"] or
                state.get("total") != manifest["total"] or
                state.get("training_seed", 0) != manifest.get("training_seed", 0)):
            raise ValueError("Checkpoint payload and manifest disagree")
        if state.get("camera_contract") != manifest.get("camera_contract"):
            raise ValueError("Checkpoint camera/scale contract and manifest disagree")
        if state.get("quality_plan") != manifest.get("quality_plan"):
            raise ValueError("quality_checkpoint_manifest_mismatch")
        for key in ("performance", "renderer_engine"):
            if (key in manifest) != (key in state) or manifest.get(key) != state.get(key):
                raise ValueError("Checkpoint performance/renderer metadata and manifest disagree: " + key)
        for key in ("runtime_options", "runtime_configuration", "runtime_policy_history"):
            if (key in manifest) != (key in state) or manifest.get(key) != state.get(key):
                raise ValueError("Checkpoint runtime metadata and manifest disagree: " + key)
        return state
