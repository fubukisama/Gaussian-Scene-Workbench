"""Read-only fixed-camera admission shared by compute, probes and import.

This seam never discovers, invokes or repairs COLMAP. Known calibration is an
explicit policy, not a heuristic inferred from the presence of sparse points.
"""

import copy
import hashlib
import json
import os
from pathlib import Path
import stat
import time
import uuid

from native.backend.atomic_file import atomic_sibling_directory
from native.backend.path_budget import PathBudgetError, require_legacy_windows_paths
from native.backend.camera_dataset import CameraDatasetError, MANIFEST_NAME, load_camera_dataset
from native.backend.camera_dataset import write_camera_dataset


def camera_dataset_summary(dataset, metadata_path=MANIFEST_NAME):
    """Presentation metadata only, not a model-quality/metric-accuracy proof."""
    scale = copy.deepcopy(dataset["scale"])
    derivation = scale.pop("derivation", None)
    result = {"version": 1, "schema": dataset["schema"], "blocked": False,
            "dataIdentity": dataset["data_identity"], "dataVersion": dataset.get("data_version", dataset["data_identity"]),
            "adapter": dataset["source"]["adapter"], "sourceKind": dataset["source"].get("kind", "manual"),
            "posePolicy": dataset["pose_policy"], "scale": scale,
            "counts": {key: len(value) for key, value in dataset["splits"].items()},
            "splits": copy.deepcopy(dataset["splits"]), "initialization": copy.deepcopy(dataset["initialization"]),
            "transforms": copy.deepcopy(dataset["transforms"]), "metadataPath": str(metadata_path),
            "independentTest": dataset["independent_test"],
            "warnings": [item["code"] for item in dataset["warnings"]], "errors": []}
    if derivation:
        result["scaleDerivation"] = scale_solution_summary(derivation["solution"])
    return result


def scale_solution_summary(solution):
    """Presentation-only distinction between fit/check agreement and accuracy."""
    def compact(values):
        result = {key: copy.deepcopy(value) for key, value in values.items()
                  if key not in ("per_constraint", "derived_pairs", "constraint_ids")}
        result["constraintCount"] = len(values.get("constraint_ids", []))
        return result
    return {"schema": "gsw.scale_derivation/1", "solutionSha256": solution["solution_sha256"],
            "inputDataIdentity": solution["input_data_identity"], "mode": solution["request"]["mode"],
            "method": solution["solver"], "fit": compact(solution["fit"]),
            "independentChecks": compact(solution["independent_checks"]),
            "scope": solution["scope"], "modelGeometryAccuracy": "not_measured",
            "poseSemantics": "fixed_source_poses_uniform_similarity_only"}


def admit_locked_dataset(source, expected_identity=None, run_colmap=False):
    """Validate a canonical managed root/JSON with byte identity checks.

    Returns {dataset, root, manifestPath, cameraDataset}. Paths are absolute
    strings outside the hashed dataset. Raises CameraDatasetError and never
    silently falls back to a solver, raw photos, sparse or cached SfM data.
    """
    if run_colmap:
        raise CameraDatasetError("camera_locked_colmap_conflict")
    path = Path(source)
    if not path.is_absolute():
        raise CameraDatasetError("camera_absolute_manifest_required", str(path))
    path = path.resolve()
    manifest = path / MANIFEST_NAME if path.is_dir() else path
    dataset = load_camera_dataset(manifest, adapter="generic_json")
    if dataset["pose_policy"] != "locked":
        raise CameraDatasetError("camera_pose_policy_mismatch", dataset["pose_policy"])
    if expected_identity is not None and expected_identity != dataset["data_identity"]:
        raise CameraDatasetError("camera_data_identity_mismatch")
    return {"dataset": dataset, "root": str(manifest.parent), "manifestPath": str(manifest),
            "cameraDataset": camera_dataset_summary(dataset, manifest)}


def _is_reparse(path):
    try:
        metadata = os.lstat(path)
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(metadata.st_mode) or bool(getattr(metadata, "st_file_attributes", 0)
                                               & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def prepare_solve_dataset_version(source, destination, expected_identity=None, cancelled=None):
    """Create a new train-only raw-photo input for an explicit fresh solve.

    No known pose/metric claim is carried into freely solved SfM coordinates.
    Holdout image bytes are never copied. Original calibration, split and scale
    remain traceable through the source identity and explicit provenance file.
    The destination must be a fresh, absolute path outside the source dataset.
    """
    source = Path(source)
    destination = Path(destination)
    if not source.is_absolute() or not destination.is_absolute():
        raise CameraDatasetError("camera_absolute_manifest_required")
    if cancelled and cancelled():
        raise CameraDatasetError("camera_import_cancelled")
    manifest = source / MANIFEST_NAME if source.is_dir() else source
    manifest = manifest.resolve()
    root = manifest.parent
    dataset = load_camera_dataset(manifest, adapter="generic_json", cancelled=cancelled)
    if expected_identity is not None and dataset["data_identity"] != expected_identity:
        raise CameraDatasetError("camera_data_identity_mismatch")
    for ancestor in (destination, *destination.parents):
        if _is_reparse(ancestor):
            raise CameraDatasetError("camera_unsafe_solve_destination")
    destination = destination.resolve()
    if destination.exists():
        raise CameraDatasetError("camera_solve_destination_exists", str(destination))
    try:
        destination.relative_to(root)
    except ValueError:
        pass
    else:
        raise CameraDatasetError("camera_unsafe_solve_destination", "must remain outside original dataset")
    train_views = [view for view in dataset["views"] if view["split"] == "train"]
    image_names = ["v%08x%s" % (index, Path(view["image_path"]).suffix.lower())
                   for index, view in enumerate(train_views)]
    # The shared stage basename never exceeds destination.name, and the
    # atomic JSON sibling never exceeds its final metadata filename. Thus the
    # final preparation paths bound all writes performed by this function.
    try:
        require_legacy_windows_paths(
            files=[destination / "input" / name for name in image_names]
                  + [destination / "camera-solve-provenance.json"],
            directories=[destination, destination / "input"])
    except PathBudgetError as error:
        raise CameraDatasetError("camera_solve_path_budget_exceeded", str(error)) from error
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Keep all writes on the destination volume, but do not make the path
    # longer than the final published directory. The shared stage context
    # also binds cleanup to the original directory and parent identities.
    with atomic_sibling_directory(destination) as temporary:
        (temporary / "input").mkdir()
        records = []
        for view, name in zip(train_views, image_names):
            if cancelled and cancelled():
                raise CameraDatasetError("camera_import_cancelled")
            original = root / view["image_path"]
            original.resolve().relative_to(root)
            # A stable train-order ordinal avoids long/unsafe source names and
            # leaves room for downstream COLMAP staging folders. The full
            # stable view ID and SHA256 stay in the provenance mapping; an
            # ordinal is never used as a substitute for source identity.
            relative = "input/" + name
            copied = temporary / relative
            digest = hashlib.sha256()
            with original.open("rb") as incoming, copied.open("xb") as outgoing:
                for block in iter(lambda: incoming.read(1024 * 1024), b""):
                    if cancelled and cancelled():
                        raise CameraDatasetError("camera_import_cancelled")
                    digest.update(block)
                    outgoing.write(block)
                outgoing.flush()
                os.fsync(outgoing.fileno())
            if digest.hexdigest() != view["sha256"] or os.path.samefile(original, copied):
                raise CameraDatasetError("camera_image_hash_mismatch", view["image_path"])
            records.append({"viewId": view["view_id"], "sourceImagePath": view["image_path"],
                            "sourceSplit": "train", "inputImagePath": relative, "sha256": view["sha256"],
                            "copyMode": "independent_bytes"})
        semantic = {"schema": "gsw.camera_solve_source/1", "sourceDataIdentity": dataset["data_identity"],
                    "posePolicy": "solve", "files": records,
                    "excludedSplits": {key: dataset["splits"][key] for key in ("val", "test")},
                    "scale": {"status": "arbitrary", "source_unit": "unknown", "meters_per_unit": None},
                    "sourceScale": copy.deepcopy(dataset["scale"]),
                    "scaleResetReason": "free_sfm_does_not_preserve_source_scale"}
        identity = hashlib.sha256(json.dumps(semantic, sort_keys=True, separators=(",", ":"),
                                            allow_nan=False).encode("utf-8")).hexdigest()
        provenance = dict(semantic, version=1, solveDataIdentity=identity, sourceManifestPath=str(manifest))
        write_camera_dataset(provenance, temporary / "camera-solve-provenance.json")
        if cancelled and cancelled():
            raise CameraDatasetError("camera_import_cancelled")
        if destination.exists() or _is_reparse(destination):
            raise CameraDatasetError("camera_solve_destination_exists", str(destination))
        os.rename(temporary, destination)
        return {"datasetPath": str(destination), "imageRoot": str(destination / "input"),
                "sourceDataIdentity": dataset["data_identity"], "solveDataIdentity": identity,
                "provenancePath": str(destination / "camera-solve-provenance.json"),
                "files": records, "posePolicy": "solve", "scale": semantic["scale"]}


def validate_solve_dataset_version(dataset_path, output_root, expected_identity):
    """Admit a portable, owned solve version before complete-state resume."""
    output_root = Path(output_root).resolve()
    expected_root = output_root / '.gsw' / 'solve-dataset'
    candidate = Path(dataset_path)
    for path in (candidate, *candidate.parents):
        if _is_reparse(path):
            raise CameraDatasetError('camera_unsafe_solve_destination')
    if candidate.resolve() != expected_root.resolve() or not candidate.is_dir():
        raise CameraDatasetError('camera_solve_resume_version_missing')
    return validate_solve_dataset_content(candidate, expected_identity)


def validate_solve_dataset_content(dataset_path, expected_identity):
    """Validate portable solve bytes without relaxing resume's owned-root gate."""
    candidate = Path(dataset_path)
    if not candidate.is_absolute() or not candidate.is_dir():
        raise CameraDatasetError('camera_solve_resume_version_missing')
    for path in (candidate, *candidate.parents):
        if _is_reparse(path):
            raise CameraDatasetError('camera_unsafe_solve_destination')
    with (candidate / 'camera-solve-provenance.json').open(encoding='utf-8') as stream:
        provenance = json.load(stream)
    semantic = {key: value for key, value in provenance.items()
                if key not in ('version', 'solveDataIdentity', 'sourceManifestPath')}
    digest = hashlib.sha256(json.dumps(semantic, sort_keys=True, separators=(',', ':'),
                                      allow_nan=False).encode('utf-8')).hexdigest()
    if (provenance.get('schema') != 'gsw.camera_solve_source/1' or
            provenance.get('posePolicy') != 'solve' or
            provenance.get('sourceDataIdentity') != expected_identity or
            provenance.get('solveDataIdentity') != digest):
        raise CameraDatasetError('camera_solve_resume_source_identity_mismatch')
    files = provenance.get('files')
    if not isinstance(files, list) or not files:
        raise CameraDatasetError('camera_solve_resume_source_identity_mismatch')
    seen = set()
    for record in files:
        relative = Path(record['inputImagePath'])
        if (relative.is_absolute() or '..' in relative.parts or len(relative.parts) != 2 or
                relative.parts[0] != 'input' or record.get('sourceSplit') != 'train' or
                relative.as_posix() in seen):
            raise CameraDatasetError('camera_unsafe_solve_destination')
        seen.add(relative.as_posix())
        path = candidate / relative
        if _is_reparse(path) or not path.is_file():
            raise CameraDatasetError('camera_solve_resume_image_hash_mismatch')
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != record.get('sha256'):
            raise CameraDatasetError('camera_solve_resume_image_hash_mismatch')
    return provenance


def _solve_tree(root):
    root = Path(root)
    if not root.is_absolute() or any(_is_reparse(path) for path in (root, *root.parents)):
        raise CameraDatasetError('camera_unsafe_solve_destination')
    root = root.resolve()
    if not root.is_dir() or any(_is_reparse(path) for path in (root, *root.parents)):
        raise CameraDatasetError('camera_unsafe_solve_destination')
    files, directories = [], []
    for directory, children, names in os.walk(str(root), followlinks=False):
        for name in children + names:
            path = Path(directory) / name
            metadata = path.lstat()
            if _is_reparse(path):
                raise CameraDatasetError('camera_unsafe_solve_destination', str(path))
            relative = path.relative_to(root).as_posix()
            if stat.S_ISDIR(metadata.st_mode):
                directories.append(relative)
            elif stat.S_ISREG(metadata.st_mode):
                files.append({'relativePath': relative, 'bytes': metadata.st_size})
            else:
                raise CameraDatasetError('camera_unsafe_solve_destination', str(path))
    return sorted(files, key=lambda item: item['relativePath']), sorted(directories)


def plan_solve_training_output(source, output, extra_source_files=(), extra_source_directories=(),
                               iterations=None, resume=False):
    """Read-only actual-tree and projected COLMAP-path admission before work."""
    source, output = Path(source), Path(output)
    if not source.is_absolute() or not output.is_absolute():
        raise CameraDatasetError('camera_absolute_manifest_required')
    for initial in (source, output):
        if any(_is_reparse(path) for path in (initial, *initial.parents)):
            raise CameraDatasetError('camera_unsafe_solve_destination')
    source, output = source.resolve(), output.resolve()
    portable = output / '.gsw' / 'solve-dataset'
    permitted_resume = resume and source == portable
    if not permitted_resume and (source == output or source in output.parents or output in source.parents):
        raise CameraDatasetError('camera_unsafe_solve_destination', 'overlapping solve/output roots')
    entries, relative_directories = _solve_tree(source)
    files = [portable / item['relativePath'] for item in entries] + [output / 'training-source.json']
    directories = [output, portable] + [portable / item for item in relative_directories]
    for incoming, target in ((extra_source_files, files), (extra_source_directories, directories)):
        for item in incoming:
            try:
                relative = Path(item).resolve().relative_to(source)
            except ValueError:
                raise CameraDatasetError('camera_unsafe_solve_destination', 'projected path outside solve source')
            target.append(portable / relative)
    # A UUID journal basename is independent of the user's output name. The
    # eventual backup/stage basenames are never longer than output.name.
    files.append(output.parent / '.gsw-transactions' / ('0' * 32 + '.json'))
    # The 32-hex payload component is the actual checkpoint format enforced
    # by training_checkpoint.CHECKPOINT_NAME, not an arbitrary task-ID bound.
    checkpoint_component = 'state-' + uuid.uuid4().hex
    files.extend((output / '.gsw-resume' / (checkpoint_component + '.pth'),
                  output / '.gsw-resume' / (checkpoint_component + '.tmp'),
                  output / '.gsw-resume/ready.json', output / 'training-performance.json',
                  output / 'training_backend.json', output / 'reconstruction_quality.json'))
    if iterations is not None:
        files.append(output / 'point_cloud' / ('iteration_' + str(iterations)) / 'point_cloud.ply')
    if output.is_dir():
        old_files, old_directories = _solve_tree(output)
        files.extend(output / item['relativePath'] for item in old_files)
        directories.extend(output / item for item in old_directories)
    try:
        require_legacy_windows_paths(files=files, directories=directories)
    except PathBudgetError as error:
        raise CameraDatasetError('camera_solve_path_budget_exceeded', str(error)) from error
    return {'schema': 'gsw.solve_output_plan/1', 'sourcePath': str(source), 'outputPath': str(output),
            'portablePath': str(portable), 'sourceFiles': entries, 'sourceDirectories': relative_directories,
            'files': sorted(map(str, files)), 'directories': sorted(map(str, directories))}


def _hash_solve_file(path, cancelled=None):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            if cancelled and cancelled():
                raise CameraDatasetError('camera_import_cancelled')
            digest.update(block)
    return digest.hexdigest()


def _copy_solve_tree(source, destination, cancelled=None, destination_owned=False):
    entries, directories = _solve_tree(source)
    if destination_owned:
        if not destination.is_dir() or any(destination.iterdir()):
            raise CameraDatasetError('camera_solve_destination_exists', str(destination))
    else:
        destination.mkdir(parents=True, exist_ok=False)
    for relative in directories:
        (destination / relative).mkdir(parents=True, exist_ok=False)
    copied = []
    for entry in entries:
        if cancelled and cancelled():
            raise CameraDatasetError('camera_import_cancelled')
        original, target = Path(source) / entry['relativePath'], destination / entry['relativePath']
        before = original.lstat()
        digest = hashlib.sha256()
        with original.open('rb') as incoming, target.open('xb') as outgoing:
            for block in iter(lambda: incoming.read(1024 * 1024), b''):
                if cancelled and cancelled():
                    raise CameraDatasetError('camera_import_cancelled')
                digest.update(block)
                outgoing.write(block)
            outgoing.flush()
            os.fsync(outgoing.fileno())
        after = original.lstat()
        if (_is_reparse(original) or not os.path.samestat(before, after)
                or before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns
                or _hash_solve_file(target, cancelled) != digest.hexdigest()):
            raise CameraDatasetError('camera_solve_resume_image_hash_mismatch', entry['relativePath'])
        copied.append(dict(entry, sha256=digest.hexdigest()))
    # Detect additions/removals and changed source bytes during publication,
    # not just a successful copy API return value.
    if _solve_tree(source) != (entries, directories):
        raise CameraDatasetError('camera_solve_resume_source_identity_mismatch')
    for entry in copied:
        if cancelled and cancelled():
            raise CameraDatasetError('camera_import_cancelled')
        if _hash_solve_file(Path(source) / entry['relativePath'], cancelled) != entry['sha256']:
            raise CameraDatasetError('camera_solve_resume_image_hash_mismatch', entry['relativePath'])
    return copied


def publish_solve_training_output(source, output, expected_identity, training_source,
                                  overwrite=False, allow_existing=False, cancelled=None):
    """Atomically publish canonical-solve input without discarding old models.

    All bytes are prepared and verified in an owned same-parent stage before
    the previous output is moved. A completed previous output is retained as
    a same-volume backup; a durable journal describes crash-recovery paths.
    No automatic orphan GC or cross-volume atomicity is assumed.
    """
    plan = plan_solve_training_output(source, output)
    source, output = Path(plan['sourcePath']), Path(plan['outputPath'])
    validate_solve_dataset_content(source, expected_identity)
    if (training_source.get('posePolicy') != 'solve'
            or training_source.get('datasetPath') != '.gsw/solve-dataset'
            or training_source.get('cameraDatasetIdentity') != expected_identity):
        raise CameraDatasetError('camera_solve_resume_source_identity_mismatch')
    previous = output.exists()
    previous_identity = output.lstat() if previous else None
    if previous and not output.is_dir():
        raise CameraDatasetError('camera_solve_destination_exists', str(output))
    nonempty = previous and any(output.iterdir())
    if nonempty and not (overwrite or allow_existing):
        raise CameraDatasetError('camera_solve_destination_exists', str(output))
    if nonempty and allow_existing and not overwrite and (output / '.gsw/solve-dataset').exists():
        raise CameraDatasetError('camera_solve_destination_exists', str(output / '.gsw/solve-dataset'))
    output.parent.mkdir(parents=True, exist_ok=True)
    nonce = uuid.uuid4().hex
    backup = None
    if previous:
        for unused in range(128):
            candidate = output.with_name(uuid.uuid4().hex[:min(32, len(output.name))])
            if candidate != output and not candidate.exists() and not _is_reparse(candidate):
                backup = candidate
                break
        if backup is None:
            raise CameraDatasetError('camera_solve_destination_exists', 'backup namespace exhausted')
    journal_path = output.parent / '.gsw-transactions' / (nonce + '.json')
    journal_path.parent.mkdir(exist_ok=True)
    if _is_reparse(journal_path.parent) or journal_path.exists():
        raise CameraDatasetError('camera_unsafe_solve_destination', str(journal_path))
    journal = {'schema': 'gsw.solve_output_transaction/1', 'nonce': nonce, 'ownerPid': os.getpid(),
               'sourcePath': str(source), 'outputPath': str(output),
               'backupPath': str(backup) if backup else None, 'sourceDataIdentity': expected_identity}
    with atomic_sibling_directory(output) as stage:
        journal.update(stagePath=str(stage), state='preparing', updatedAt=time.time())
        write_camera_dataset(journal, journal_path)
        moved = published = False
        stage_identity = stage.lstat()
        try:
            if nonempty and allow_existing and not overwrite:
                _copy_solve_tree(output, stage, cancelled, destination_owned=True)
            copied = _copy_solve_tree(source, stage / '.gsw/solve-dataset', cancelled)
            validate_solve_dataset_content(stage / '.gsw/solve-dataset', expected_identity)
            write_camera_dataset(training_source, stage / 'training-source.json')
            journal.update(state='prepared', sourceTreeSha256=hashlib.sha256(json.dumps(
                copied, sort_keys=True, separators=(',', ':')).encode('utf-8')).hexdigest(), updatedAt=time.time())
            write_camera_dataset(journal, journal_path)
            if cancelled and cancelled():
                raise CameraDatasetError('camera_import_cancelled')
            if previous:
                if (not output.exists() or backup.exists() or _is_reparse(output)
                        or not os.path.samestat(previous_identity, output.lstat())):
                    raise CameraDatasetError('camera_solve_destination_exists', str(output))
                os.rename(str(output), str(backup))
                moved = True
                journal.update(state='backup_moved', updatedAt=time.time())
                write_camera_dataset(journal, journal_path)
            elif output.exists():
                raise CameraDatasetError('camera_solve_destination_exists', str(output))
            os.rename(str(stage), str(output))
            published = True
            journal.update(state='published', updatedAt=time.time())
            write_camera_dataset(journal, journal_path)
        except BaseException as error:
            state = 'failed_before_publish'
            if moved or published:
                try:
                    if published:
                        if not output.exists() or not os.path.samestat(stage_identity, output.lstat()):
                            raise OSError('published solve output identity changed')
                        os.rename(str(output), str(stage))
                    if moved and not output.exists() and backup.exists():
                        os.rename(str(backup), str(output))
                        state = 'rolled_back'
                    elif not moved and not output.exists():
                        state = 'rolled_back'
                    else:
                        state = 'rollback_requires_manual_recovery'
                except OSError:
                    state = 'rollback_requires_manual_recovery'
            journal.update(state=state, updatedAt=time.time())
            try:
                write_camera_dataset(journal, journal_path)
            except OSError:
                pass  # The previously fsynced prepared journal still names all recovery paths.
            error.solve_output_transaction = {'journalPath': str(journal_path),
                'backupPath': str(backup) if backup else None, 'stagePath': str(stage), 'state': state}
            raise
    return {'datasetPath': str(output / '.gsw/solve-dataset'), 'outputPath': str(output),
            'backupPath': str(backup) if moved else None, 'journalPath': str(journal_path),
            'sourceTreeSha256': journal['sourceTreeSha256']}
