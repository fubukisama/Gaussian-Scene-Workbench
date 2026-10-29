"""Package six verified synthetic native-training models for a remote recorder.

CPU/file processing only: no Torch import, training, renderer or application.
The bundle excludes optimizer states, raw logs and local absolute paths. File
write timing is an approximate observation including startup, not a benchmark.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import zipfile

LIMIT = 30 * 1024 ** 2
SHA = re.compile(r"[0-9a-f]{64}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 ** 2), b""):
            result.update(block)
    return result.hexdigest()


def d_path(value, *, exists=True):
    path = Path(value).absolute()
    require(path.drive.lower() == "d:", "All local paths must be on D:")
    for part in (path, *path.parents):
        require(not part.is_symlink() and not (part.exists() and getattr(part.lstat(), "st_file_attributes", 0) & 0x400),
                "Links/reparse paths are not admitted")
    return path.resolve(strict=exists)


def relative(value):
    require(isinstance(value, str) and bool(value) and ":" not in value and "\\" not in value,
            "Invalid public relative path")
    path = PurePosixPath(value)
    require(not path.is_absolute() and all(part not in ("", ".", "..") for part in value.split("/")),
            "ZIP traversal/ambiguous path refused")
    return value


def child(root, value):
    path = d_path(root / relative(value))
    path.relative_to(root)
    require(path.is_file(), "An ordinary input file is required")
    return path


def read(path):
    require(Path(path).stat().st_size <= 16 * 1024 ** 2, "JSON exceeds bounded size")
    value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    require(isinstance(value, dict), "JSON object required")
    return value


def public_json(value):
    """Allowlisted provenance must not accidentally contain local identities."""
    text = json.dumps(value, sort_keys=True, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    require(not re.search(r"[A-Za-z]:[\\/]|\\\\|/(?:Users|home)/|S-1-5-21-|GPU-[0-9a-fA-F-]{20}", text),
            "Private absolute path or machine identity entered public provenance")
    return text.encode("utf-8")


def finite_ply(path, backend, recorded):
    """Validate every scalar, not only a finite bounding box or preview."""
    require(digest(path) == recorded.get("sha256"), "Recorded PLY digest differs")
    require(path.stat().st_size == recorded.get("bytes") and path.stat().st_size <= LIMIT,
            "Recorded PLY byte count differs or exceeds package limit")
    properties = []
    count = None
    current = None
    encoding = None
    scalar = {"float": "f", "float32": "f", "double": "d", "float64": "d",
              "char": "b", "uchar": "B", "short": "h", "ushort": "H",
              "int": "i", "uint": "I", "int8": "b", "uint8": "B",
              "int16": "h", "uint16": "H", "int32": "i", "uint32": "I"}
    with path.open("rb") as stream:
        require(stream.readline(4096).strip() == b"ply", "Missing PLY marker")
        for _ in range(1024):
            raw = stream.readline(4096)
            require(raw and len(raw) < 4096, "Invalid/oversized PLY header")
            line = raw.decode("ascii").strip()
            require(not re.search(r"[A-Za-z]:[\\/]|\\\\|/(?:Users|home)/", line),
                    "Private source path in PLY header")
            parts = line.split()
            if parts[:1] == ["format"]:
                require(len(parts) == 3 and parts[2] == "1.0", "Unsupported PLY version")
                encoding = parts[1]
            elif parts[:1] == ["element"]:
                current = parts[1]
                if current == "vertex":
                    count = int(parts[2])
                else:
                    require(int(parts[2]) == 0, "Model snapshot may not contain extra non-vertex payload")
            elif parts[:1] == ["property"] and current == "vertex":
                require(len(parts) == 3 and parts[1] in scalar, "Only scalar native Gaussian properties are admitted")
                properties.append((parts[2], scalar[parts[1]]))
            elif line == "end_header":
                break
        else:
            raise ValueError("PLY header line limit exceeded")
        require(isinstance(count, int) and 0 < count <= 1000000 and count == recorded.get("gaussian_count"),
                "PLY point count differs from the actual training receipt")
        names = [name for name, _ in properties]
        required = {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                    "scale_0", "scale_1", "rot_0", "rot_1", "rot_2", "rot_3"}
        require(len(names) == len(set(names)) and required.issubset(names), "Incomplete Gaussian attributes")
        require(("scale_2" in names) == (backend == "3dgs"), "3DGS/2DGS scale layout mismatch")
        if encoding == "binary_little_endian":
            layout = struct.Struct("<" + "".join(kind for _, kind in properties))
            payload = stream.read(LIMIT + 1)
            require(len(payload) == count * layout.size, "PLY payload length does not match declared vertices")
            require(all(math.isfinite(value) for row in layout.iter_unpack(payload) for value in row),
                    "Non-finite scalar in an actual model snapshot")
        elif encoding == "ascii":
            for _ in range(count):
                values = stream.readline(16384).split()
                require(len(values) == len(properties) and all(math.isfinite(float(value)) for value in values),
                        "Invalid/non-finite ASCII Gaussian vertex")
            require(not stream.read().strip(), "Unexpected ASCII payload after vertices")
        else:
            raise ValueError("Unsupported PLY encoding")
    return {"gaussian_count": count, "bytes": path.stat().st_size,
            "sha256": recorded["sha256"], "all_numeric_scalars_finite": True}


def camera_audit(root, backend, case, identity, dataset):
    audit = case.get("camera_audit", {})
    require(audit.get("passed") is True and audit.get("validation_test_excluded_from_optimization") is True
            and audit.get("full_K_locked") is True, "Actual camera/split gate did not pass")
    contract_path = child(root, backend + "/model/training-camera-contract.json")
    samples_path = child(root, backend + "/model/training-camera-samples.jsonl")
    require(digest(contract_path) == audit.get("camera_contract_sha256")
            and digest(samples_path) == audit.get("sample_log_sha256"), "Actual camera evidence digest differs")
    contract = read(contract_path)
    require(contract["input_data_identity"] == identity and contract["splits"] == dataset["splits"],
            "Camera/data split identity differs")
    by_id = {view["view_id"]: view for view in contract["views"]}
    loaded = contract["loaded_cameras"]
    require(len(loaded) == len(by_id), "Not all calibrated views were accounted for")
    for camera in loaded:
        expected = by_id[camera["view_id"]]
        require(camera["K"] == expected["training_K"] and camera["image_size"] == expected["training_size"]
                and camera["calibration_requires_grad"] is False, "Full-K immutable calibration audit failed")
    groups = [set(contract["splits"][name]) for name in ("train", "val", "test")]
    require(all(not groups[i] & groups[j] for i in range(3) for j in range(i + 1, 3)), "Dataset split overlap")
    samples = [json.loads(line) for line in samples_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    require(samples and len(samples) == audit["sample_count"], "Actual sampled-view count differs")
    require(all(row["split"] == "train" and row["view_id"] in groups[0]
                and row["view_id"] not in groups[1] | groups[2] for row in samples),
            "Validation/test view entered optimization")
    require(max(row["iteration"] for row in samples) == audit["last_observed_iteration"],
            "Last observed optimizer iteration differs")
    return {"full_K_locked": True, "train_only_optimization": True,
        "sample_count": len(samples), "last_observed_iteration": audit["last_observed_iteration"],
        "sample_log_sha256": audit["sample_log_sha256"],
        "camera_contract_sha256": audit["camera_contract_sha256"],
        "split_counts": {name: len(contract["splits"][name]) for name in ("train", "val", "test")}}


def observed_timing(log, paths, run_elapsed):
    info = log.stat()
    birth = getattr(info, "st_birthtime", None)
    method = "training_log_birthtime_to_snapshot_mtime"
    if birth is None and os.name == "nt":
        birth = info.st_ctime
        method = "windows_training_log_creation_ctime_to_snapshot_mtime"
    explanation = "Approximate retained-file write wall observation; includes process startup and I/O; not iteration-scaled, GPU-only time or a benchmark."
    if birth is None or not math.isfinite(birth):
        return [None] * len(paths), {"status": "unknown", "reason": "log_birth_time_unavailable", "description": explanation}
    times = [path.stat().st_mtime for path in paths]
    require(all(math.isfinite(value) for value in times) and times == sorted(times),
            "Observed snapshot file times are not monotone")
    elapsed = [value - birth for value in times]
    require(all(0 <= value <= run_elapsed + 5 for value in elapsed),
            "Snapshot file timing does not belong to this observed training run")
    return elapsed, {"status": "observed_approximate", "method": method, "description": explanation,
        "clock_origin": "owned_training_log_creation", "includes_startup_and_io": True,
        "initialization_time_uses_retained_initial_snapshot_write": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--training-root", type=Path, required=True)
    parser.add_argument("--staging", type=Path, required=True)
    parser.add_argument("--zip", dest="archive", type=Path)
    args = parser.parse_args()
    root = d_path(args.training_root)
    stage = d_path(args.staging, exists=False)
    archive = d_path(args.archive or stage.with_suffix(".zip"), exists=False)
    report_path = archive.with_name(archive.stem + ".package.json")
    require(not stage.exists() and not archive.exists() and not report_path.exists(),
            "Staging, ZIP and package receipt must all be fresh")
    try:
        archive.relative_to(stage)
    except ValueError:
        pass
    else:
        raise ValueError("ZIP must remain outside its staging directory")
    for value in (stage, archive):
        try:
            value.relative_to(root)
        except ValueError:
            pass
        else:
            raise ValueError("Outputs must remain outside original training evidence")
    results_path = child(root, "RESULTS.json")
    results = read(results_path)
    require(results.get("schema") == "gsw.readme_native_training_demo/1" and results.get("status") == "passed"
            and results.get("original_inputs_unchanged") is True, "Actual training evidence is not accepted")
    identity = results.get("input_data_identity")
    require(isinstance(identity, str) and SHA.fullmatch(identity), "Invalid dataset identity")
    dataset = read(child(root, "dataset/camera_dataset.json"))
    declaration = read(child(root, "input-manifest.raw.json"))
    copy_receipt = read(child(root, "input-copy.json"))
    init = dataset.get("initialization", {})
    demo = declaration.get("demo_provenance", {})
    require(dataset.get("data_identity") == identity and copy_receipt.get("input_data_identity") == identity
            and dataset.get("pose_policy") == "locked" and init.get("kind") == "random"
            and init.get("priors") == [] and not init.get("path")
            and init.get("provenance", {}).get("geometry_or_depth_consumed") is False
            and demo.get("initialization_independent_of_render_geometry") is True
            and demo.get("truth_depth_normals_mesh_exported") is False
            and demo.get("third_party_assets") is False
            and declaration.get("source", {}).get("kind") == "original_procedural_readme_demo",
            "Synthetic no-GT/random-initialization declaration failed")
    require(results.get("quality_or_accuracy_accepted") is False
            and results.get("physical_scale_accuracy_accepted") is False
            and results.get("initialization_geometry_prior_used") is False
            and results.get("test_image_feedback_into_optimization") is False,
            "Unexpected quality, physical accuracy or input-feedback claim")
    originals = d_path(copy_receipt["source"])
    require(str(originals) == str(d_path(results["input_source"])), "Original source reference differs")
    for value in (stage, archive):
        try:
            value.relative_to(originals)
        except ValueError:
            pass
        else:
            raise ValueError("Outputs must remain outside original source inputs")
    for name, expected in copy_receipt["files"].items():
        require(digest(child(originals, name)) == expected, "An original demo input changed")
        copied = child(root, "input-manifest.raw.json") if name == "camera_dataset.json" else child(root / "dataset", name)
        require(digest(copied) == expected, "An admitted copy of an original demo input changed")
    cases = results.get("cases", [])
    require(len(cases) == 2 and {case.get("backend") for case in cases} == {"3dgs", "2dgs"},
            "Exactly two actual backend runs required")
    selections = []
    summaries = []
    timing_known = True
    for backend in ("3dgs", "2dgs"):
        case = next(item for item in cases if item["backend"] == backend)
        case_path = child(root, backend + "/RESULTS.json")
        require(read(case_path) == case and case.get("passed") is True and case.get("returncode") == 0
                and case.get("forced_owned_child_termination") is False and not case.get("warnings")
                and case.get("input_data_identity") == identity, "Actual completed backend receipt differs")
        log = child(root, backend + "/training.log")
        require(digest(log) == case["training_log_sha256"], "Raw training log changed")
        audit = camera_audit(root, backend, case, identity, dataset)
        initial = next((item for item in case["initial_snapshots"] if item["iteration"] == 0), None)
        intermediate = next((item for item in case["saved_model_snapshots"] if item["iteration"] == 600), None)
        final = next((item for item in case["saved_model_snapshots"] if item["iteration"] == 1500), None)
        require(all((initial, intermediate, final)), "Required actual 0/600/1500 model snapshots are unavailable")
        rows = [initial, intermediate, final]
        sources = []
        for item in rows:
            path = d_path(item["path"])
            path.relative_to(root / backend)
            sources.append(path)
        elapsed, timing = observed_timing(log, sources, case["elapsed_seconds"])
        timing_known &= timing["status"] == "observed_approximate"
        for kind, item, source, seconds in zip(("initialization", "training", "final"), rows, sources, elapsed):
            verified = finite_ply(source, backend, item)
            name = f"snapshots/{backend}/{item['iteration']:06d}-{kind}.ply"
            selections.append((source, {"backend": backend, "kind": kind, "iteration": item["iteration"],
                "path": name, "elapsed_seconds": seconds, "timing_status": timing["status"], **verified}))
        runtime = case.get("actual_runtime", {})
        summaries.append({"backend": backend, "passed": True, "whole_run_elapsed_seconds": case["elapsed_seconds"],
            "timing": timing, "camera_audit": audit, "densification_requested": case["densification_requested"],
            "runtime": {key: runtime[key] for key in ("python", "torch", "torch_cuda")},
            "evidence_references": {"raw_log_sha256": digest(log), "backend_result_sha256": digest(case_path),
                                    "raw_logs_and_optimizer_states_included": False},
            "allocator_target_mib": case["allocator_target_mib"], "os_gpu_hard_limit": False})
    view = next((view for view in dataset["views"] if view["view_id"] == "orbit-000"), None)
    require(view is not None and view["split"] == "train", "Actual orbit-000 training RGB is unavailable")
    sample = child(root / "dataset", view["image_path"])
    require(digest(sample) == view["sha256"], "Actual RGB sample digest differs")
    with sample.open("rb") as stream:
        png = stream.read(33)
    require(png[:8] == b"\x89PNG\r\n\x1a\n" and png[12:16] == b"IHDR"
            and png[24] == 8 and png[25] == 2, "The original sample must be an actual RGB8 PNG")
    sample_name = "input/orbit-000.png"
    stage.mkdir(parents=True)
    for source, item in selections:
        target = stage / relative(item["path"])
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        require(digest(target) == item["sha256"], "Copied model differs")
    (stage / "input").mkdir()
    shutil.copyfile(sample, stage / sample_name)
    provenance = {"schema": "gsw.readme_training_public_provenance/1", "dataset_identity": identity,
        "training_source_commit": results["source_commit_at_start"],
        "selected_source_files_sha256": results["selected_source_files_sha256"],
        "selected_hashes_are_not_complete_source_closure": True,
        "raw_supervisor_result_sha256": digest(results_path),
        "raw_input_manifest_sha256": digest(child(root, "input-manifest.raw.json")),
        "original_inputs_reverified_unchanged": True,
        "source": {"kind": "original_procedural_readme_demo", "generator": declaration["source"]["generator"],
                   "generator_sha256": declaration["source"]["generator_sha256"], "third_party_assets": False},
        "initialization": {key: init[key] for key in ("kind", "bounds", "frame", "count", "seed", "priors")},
        "initialization_geometry_prior_used": False, "truth_depth_normals_mesh_used": False,
        "scale": {key: dataset["scale"][key] for key in ("status", "source_unit", "meters_per_unit")},
        "test_images_feedback_into_optimization": False, "quality_or_accuracy_accepted": False,
        "physical_accuracy_accepted": False, "full_optimizer_resume_demonstrated": False,
        "description": "Actual bounded native 3DGS and 2DGS optimization on original synthetic RGB; not a quality or speed benchmark.",
        "cases": summaries,
        "recorder_application_binding": "Separate actual native executable/source receipt required; this bundle does not attest the recorder.",
        "sample_rgb": {"path": sample_name, "sha256": digest(sample), "view_id": "orbit-000",
                       "split": "train", "width": view["width"], "height": view["height"]}}
    provenance_name = "training_provenance.json"
    (stage / provenance_name).write_bytes(public_json(provenance))
    manifest = {"schema": "gsw.readme_training_demo/1", "dataset_identity": identity,
        "training_provenance_path": provenance_name, "training_provenance_sha256": digest(stage / provenance_name),
        "snapshots": [item for _, item in selections], "sample_rgb": provenance["sample_rgb"],
        "timing_status": "observed_approximate" if timing_known else "unknown",
        "recorder_numeric_timing_contract_satisfied": timing_known,
        "timing_note": "Snapshot times are observed file-write wall times, not fabricated proportional iteration times; whole-run elapsed is separate."}
    (stage / "demo_manifest.json").write_bytes(public_json(manifest))
    files = sorted(path for path in stage.rglob("*") if path.is_file())
    require(len(files) == 9 and sum(path.stat().st_size for path in files) <= LIMIT,
            "Unexpected payload files or uncompressed package over 30 MiB")
    archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for path in files:
            name = relative(path.relative_to(stage).as_posix())
            require(path.suffix.lower() not in (".pth", ".log", ".pt"), "Private/optimizer payload refused")
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            bundle.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    require(archive.stat().st_size <= LIMIT, "ZIP exceeds 30 MiB")
    with zipfile.ZipFile(archive) as bundle:
        require(bundle.testzip() is None and len(bundle.infolist()) == len(files), "ZIP readback failed")
        require({relative(item.filename) for item in bundle.infolist()} ==
                {path.relative_to(stage).as_posix() for path in files}, "ZIP entry inventory differs")
    report = {"schema": "gsw.readme_training_package_receipt/1", "zip": str(archive),
        "zip_sha256": digest(archive), "zip_bytes": archive.stat().st_size,
        "file_count": len(files), "snapshot_count": 6, "dataset_identity": identity,
        "timing_status": manifest["timing_status"], "recorder_compatible": timing_known,
        "source_data_or_models_modified": False}
    with report_path.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
    print(json.dumps(report, ensure_ascii=False))
    return 0 if timing_known else 3


if __name__ == "__main__":
    raise SystemExit(main())
