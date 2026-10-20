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

from native.backend.atomic_file import atomic_sibling

PAUSED_EXIT_CODE = 75
CONTROL_ENV = "GSW_NATIVE_TRAINING_CONTROL"
CHECKPOINT_NAME = re.compile(r"^state-[0-9a-f]{32}\.pth$")


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
    if hasattr(dataset, "training_seed"):
        settings["native_training_seed_source"] = file_digest(Path(__file__).with_name("training_seed.py"), text_source=True)
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
    if backend == "3dgs":
        gaussians._exposure = state["exposure"]
        gaussians.exposure_mapping = state["exposure_mapping"]
        gaussians.pretrained_exposures = state["pretrained_exposures"]
    gaussians.restore(state["model"], opt)
    if backend == "3dgs":
        gaussians.exposure_optimizer.load_state_dict(state["exposure_optimizer"])
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
            if "performance" in state:
                manifest["performance"] = state["performance"]
                manifest["renderer_engine"] = state.get("renderer_engine", "stock")
            if state.get("pause_reason") == "vram_pressure":
                manifest["pause_reason"] = "vram_pressure"
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
        if (manifest.get("identity") != identity or
                manifest.get("backend", "3dgs") != self.control.get("backend", "3dgs")):
            raise ValueError("Training inputs/settings changed; checkpoint was not loaded")
        if manifest.get("training_seed", 0) != self.control.get("training_seed", 0):
            raise ValueError("Training seed changed; checkpoint was not loaded")
        # Only called by the explicit, trust-confirmed native Resume action.
        state = torch.load(str(path))
        if (state.get("backend", "3dgs") != manifest.get("backend", "3dgs") or
                state.get("iteration") != manifest["iteration"] or
                state.get("total") != manifest["total"] or
                state.get("training_seed", 0) != manifest.get("training_seed", 0)):
            raise ValueError("Checkpoint payload and manifest disagree")
        if state.get("camera_contract") != manifest.get("camera_contract"):
            raise ValueError("Checkpoint camera/scale contract and manifest disagree")
        if "performance" in manifest and (manifest["performance"] != state.get("performance") or
                manifest.get("renderer_engine", "stock") != state.get("renderer_engine", "stock")):
            raise ValueError("Checkpoint performance policy and manifest disagree")
        return state
