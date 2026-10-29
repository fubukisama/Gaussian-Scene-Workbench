"""Remote-only corresponding-source acceptance, never application acceptance.

The executable payload is deliberately non-executable test bytes. Nothing in
the payload is launched. No full package, private key or Cargo cache is copied
to the small evidence directory.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import traceback


SPEC = importlib.util.spec_from_file_location("gsw_source_bundle_package", Path(__file__).with_name("package_native_update.py"))
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)
OWNED_EVIDENCE = None


def command(args, *, cwd=None, env=None, timeout=120):
    result = subprocess.run(args, cwd=cwd, env=env, capture_output=True, timeout=timeout)
    if result.returncode:
        raise ValueError("source_bundle_command_failed")
    return result.stdout


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, ensure_ascii=True, allow_nan=False, sort_keys=True)
        output.write("\n")


def fresh_directory(path):
    if path.exists() or path.is_symlink() or ".." in path.parts:
        raise ValueError("source_bundle_output_not_fresh")
    ancestor = path.parent
    while not ancestor.exists():
        ancestor = ancestor.parent
    PACKAGE._ordinary(ancestor)
    path.mkdir(parents=True, exist_ok=False)
    PACKAGE._ordinary(path)


def verify(args):
    global OWNED_EVIDENCE
    if os.environ.get("GITHUB_ACTIONS", "").lower() != "true" or sys.platform != "win32":
        raise ValueError("source_bundle_remote_windows_only")
    if (os.environ.get("GITHUB_REPOSITORY", "").casefold() != "fubukisama/gaussian-scene-workbench"
            or os.environ.get("GITHUB_REF") != "refs/heads/main"):
        raise ValueError("source_bundle_repository_not_main")
    root, repository, evidence = args.root.absolute(), args.repository.absolute(), args.evidence.absolute()
    if root.drive.upper() != "D:" or len(str(root)) > 40 or evidence == root or evidence.is_relative_to(root):
        raise ValueError("source_bundle_requires_short_owned_d_drive_root")
    commit, tree = args.source_commit, args.source_tree
    if not re.fullmatch(r"[a-f0-9]{40}", commit) or not re.fullmatch(r"[a-f0-9]{40}", tree):
        raise ValueError("source_bundle_identity_invalid")
    if os.environ.get("GITHUB_SHA") != commit:
        raise ValueError("source_bundle_workflow_sha_differs")
    git = lambda *values: command(["git", "-C", str(repository), *values])
    if git("rev-parse", "HEAD").decode("ascii").strip() != commit or git("rev-parse", commit + "^{tree}").decode("ascii").strip() != tree:
        raise ValueError("source_bundle_git_identity_differs")
    expected = []
    for item in git("ls-tree", "-r", "-l", "-z", "--full-tree", commit).split(b"\0"):
        if not item:
            continue
        header, raw_path = item.split(b"\t", 1)
        mode, kind, blob, size = header.decode("ascii").split()
        expected.append({"path": raw_path.decode("utf-8"), "mode": mode, "type": kind, "sha": blob, "size": int(size)})
    raw_lock = git("cat-file", "blob", commit + ":native/updater-trust/Cargo.lock")
    registry_count = len(PACKAGE.locked_registry_packages(raw_lock))
    if not registry_count:
        raise ValueError("source_bundle_registry_set_empty")
    cargo_home = args.cargo_home or os.environ.get("CARGO_HOME")
    if not cargo_home or Path(cargo_home).absolute().drive.upper() != "D:":
        raise ValueError("source_bundle_cargo_cache_requires_d_drive")
    env = dict(os.environ, RUSTUP_TOOLCHAIN="1.99.1", CARGO_HOME=str(Path(cargo_home).absolute()))
    rust_version = command(["rustc", "--version"], env=env).decode("ascii").strip()
    cargo_version = command([args.cargo, "--version"], env=env).decode("ascii").strip()
    if not rust_version.startswith("rustc 1.99.1 "):
        raise ValueError("source_bundle_pinned_rust_toolchain_unavailable")
    fresh_directory(root)
    fresh_directory(evidence)
    OWNED_EVIDENCE = evidence
    payload = root / "payload"
    (payload / "bin").mkdir(parents=True)
    executable = payload / "bin/Gaussian Scene Workbench.exe"
    with executable.open("xb") as output:
        output.write(b"GSW SOURCE-ONLY FIXTURE: NOT A PE EXECUTABLE; NEVER LAUNCH\n")
    prepared = command([sys.executable, "-I", "-B", str(Path(__file__).with_name("prepare_native_update_source.py")),
        "--repository", str(repository), "--source-commit", commit, "--source-tree", tree,
        "--output-root", str(payload / "source"), "--executable", str(executable),
        "--provenance", str(payload / "source-provenance.json"), "--manifest", str(payload / "source-manifest.json"),
        "--cargo", args.cargo, "--cargo-home", env["CARGO_HOME"]], env=env, timeout=1800)
    preparation = json.loads(prepared)
    for relative in ("COPYING", "LICENSE", "THIRD_PARTY_LICENSES.md", "native/LICENSE.md"):
        source, destination = payload / "source" / relative, payload / relative
        PACKAGE._ordinary(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with source.open("rb") as original, destination.open("xb") as output:
            while block := original.read(1024**2):
                output.write(block)
    write_json(payload / "build_manifest.json", {"product": "Gaussian Scene Workbench",
        "legacyWebRuntimeIncluded": False, "sourceOnlyFixture": True, "applicationAcceptance": False})
    write_json(payload / "backend_manifest.json", {"schema": "gsw.source_only_backend_fixture/1", "testOnly": True})
    archive = root / "source-only-fixture.zip"
    report = PACKAGE.create_archive(payload, archive, product=PACKAGE.FIXTURE_PRODUCT,
        source_commit=commit, source_tree=tree, build_id="source-proof-" + commit[:12])
    proof = PACKAGE.verify_release_archive(archive, report, expected)
    if (proof.get("registryPackageCount") != registry_count or proof.get("dependencyArchiveCount") != registry_count
            or preparation.get("dependencyArchiveCount") != registry_count
            or proof.get("trackedFileCount") != len(expected)
            or proof.get("vendorProjectionId") != "cargo-vendor-1.99.1"
            or any(type(proof.get(key)) is not int or proof[key] < 0 for key in (
                "originalRegistryFileCount", "vendoredRegistryFileCount", "filteredRegistryMetadataFileCount"))
            or proof["originalRegistryFileCount"] != proof["vendoredRegistryFileCount"] + proof["filteredRegistryMetadataFileCount"]):
        raise ValueError("source_bundle_dependency_closure_incomplete")
    with executable.open("rb") as source:
        if source.read() != b"GSW SOURCE-ONLY FIXTURE: NOT A PE EXECUTABLE; NEVER LAUNCH\n":
            raise ValueError("source_bundle_fixture_bytes_changed")
    result = {"schema": "gsw.native_source_bundle_acceptance/1", "ok": True, "scope": "source-only",
        "production": False, "applicationAcceptance": False, "payloadExecuted": False,
        "sourceCommit": commit, "sourceTree": tree, "product": PACKAGE.FIXTURE_PRODUCT,
        "registryPackageCount": registry_count, "trackedFileCount": len(expected),
        "hiddenTrackedFileCount": sum(any(part.startswith(".") for part in row["path"].split("/")) for row in expected),
        "licenseTrackedFileCount": sum(any(token in Path(row["path"]).name.casefold()
                                         for token in ("license", "copying", "notice")) for row in expected),
        "archiveLength": report["archiveLength"], "archiveSha256": report["archiveSha256"],
        "payloadManifestSha256": report["payloadManifestSha256"],
        "correspondingSourceSha256": report["correspondingSourceSha256"],
        "cargoLockSha256": hashlib.sha256(raw_lock).hexdigest(), "sourceEvidence": proof,
        "rustc": rust_version, "cargo": cargo_version,
        "runId": os.environ.get("GITHUB_RUN_ID"), "runAttempt": os.environ.get("GITHUB_RUN_ATTEMPT")}
    write_json(evidence / "source-proof.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--cargo-home", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args), ensure_ascii=True))
        return 0
    except Exception as error:
        sites = [{"module": Path(frame.filename).name, "function": frame.name, "line": frame.lineno}
                 for frame in traceback.extract_tb(error.__traceback__)
                 if Path(frame.filename).name in {"package_native_update.py", "prepare_native_update_source.py",
                                                  "verify_native_update_source_bundle.py"}][-8:]
        result = {"schema": "gsw.native_source_bundle_acceptance/1", "ok": False,
            "scope": "source-only", "production": False, "applicationAcceptance": False,
            "payloadExecuted": False, "code": getattr(error, "code", "source_bundle_acceptance_failed"),
            "errorType": type(error).__name__, "failureSite": sites}
        component = getattr(error, "sourceComponent", None)
        if isinstance(component, str) and re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.+-]{0,192}", component):
            result["crateDirectory"] = component
        difference = getattr(error, "sourceDifference", None)
        counters = ("missingCount", "extraCount", "changedCount", "knownCargoFilteredCount", "otherMissingCount")
        if (isinstance(difference, dict) and all(type(difference.get(key)) is int and 0 <= difference[key] <= 100000 for key in counters)
                and isinstance(difference.get("metadataCategories"), list)
                and set(difference["metadataCategories"]) <= {"git-directory", "gitignore", "gitattributes", "cargo-marker"}):
            result["sourceDifference"] = {key: difference[key] for key in counters}
            result["sourceDifference"]["metadataCategories"] = list(difference["metadataCategories"])
        print(json.dumps(result, ensure_ascii=True))
        if OWNED_EVIDENCE == args.evidence.absolute():
            try:
                write_json(args.evidence / "source-proof-failure.json", result)
            except OSError:
                pass
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
