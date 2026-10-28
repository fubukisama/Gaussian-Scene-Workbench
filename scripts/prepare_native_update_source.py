"""Materialize exact Git blobs and locked Rust corresponding source for CI.

Developer packaging only. Never called by the installed updater. The output
source directory, provenance and manifest must be fresh paths; no dirty working
tree is copied and existing user files are never replaced.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

_spec = importlib.util.spec_from_file_location("gsw_update_package", Path(__file__).with_name("package_native_update.py"))
_package = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_package)

def git(repository: Path, *args: str) -> bytes:
    result = subprocess.run(["git", "-C", str(repository), *args], capture_output=True, timeout=120)
    if result.returncode:
        raise ValueError("source_git_operation_failed")
    return result.stdout

def write_json(path: Path, value):
    with path.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, ensure_ascii=True, allow_nan=False, sort_keys=True, separators=(",", ":"))
        output.write("\n")

def files(root: Path):
    for parent, directories, names in os.walk(root, followlinks=False):
        owner = Path(parent)
        _package._ordinary(owner)
        for name in directories:
            info = _package._ordinary(owner / name)
            if not stat.S_ISDIR(info.st_mode):
                raise ValueError("source_vendor_directory_invalid")
        for name in names:
            path = owner / name
            info = _package._ordinary(path)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise ValueError("source_vendor_file_invalid")
            yield path, info


def registry_archives(output: Path, cargo_home: Path):
    """Original cache bytes are admitted by the exact Git lock's checksum only."""
    lock = output / "native/updater-trust/Cargo.lock"
    info = _package._ordinary(lock)
    if not stat.S_ISREG(info.st_mode) or info.st_size > _package.MAX_MANIFEST:
        raise ValueError("source_dependency_lock_invalid")
    locked = _package.locked_registry_packages(lock.read_bytes())
    if not locked:
        raise ValueError("source_registry_dependency_set_empty")
    cache = cargo_home / "registry/cache"
    _package._ordinary(cache)
    destination = output / "native/updater-trust/vendor-archives"
    destination.mkdir(exist_ok=False)
    for (name, version), checksum in sorted(locked.items()):
        filename = _package.validate_relative_path(name + "-" + version + ".crate")
        selected = None
        for candidate in sorted(cache.glob("*/" + filename)):
            source_info = _package._ordinary(candidate)
            if (not stat.S_ISREG(source_info.st_mode) or source_info.st_nlink != 1
                    or not 0 < source_info.st_size <= _package.MAX_MANIFEST):
                raise ValueError("source_registry_cache_file_invalid")
            if _package._hash(candidate) == checksum:
                selected = candidate
                break
        if selected is None:
            raise ValueError("source_locked_registry_archive_missing")
        source_info = _package._ordinary(selected)
        with selected.open("rb") as source, (destination / filename).open("xb") as target:
            held = os.fstat(source.fileno())
            if (held.st_dev, held.st_ino, held.st_size, held.st_nlink) != (
                    source_info.st_dev, source_info.st_ino, source_info.st_size, 1):
                raise ValueError("source_registry_archive_identity_changed")
            digest, received = hashlib.sha256(), 0
            while block := source.read(1024**2):
                received += len(block)
                if received > source_info.st_size:
                    raise ValueError("source_registry_archive_identity_changed")
                digest.update(block)
                target.write(block)
            if received != source_info.st_size or digest.hexdigest() != checksum:
                raise ValueError("source_registry_archive_identity_changed")
    return len(locked)

def prepare(args):
    repository = args.repository.absolute()
    commit = args.source_commit
    if not re.fullmatch("[a-f0-9]{40}", commit):
        raise ValueError("source_commit_invalid")
    tree = git(repository, "rev-parse", f"{commit}^{{tree}}").decode("ascii").strip()
    if tree != args.source_tree:
        raise ValueError("source_tree_mismatch")
    output = args.output_root.absolute()
    for path in (output, args.provenance.absolute(), args.manifest.absolute()):
        if path.exists() or path.is_symlink():
            raise ValueError("source_output_not_fresh")
        _package._ordinary(path.parent)
    _package._ordinary(args.executable.absolute())
    rows = []
    for item in git(repository, "ls-tree", "-r", "-z", "--full-tree", commit).split(b"\0"):
        if not item:
            continue
        header, raw_path = item.split(b"\t", 1)
        mode, kind, object_id = header.decode("ascii").split(" ")
        if mode not in {"100644", "100755"} or kind != "blob":
            raise ValueError("source_gitlink_or_symlink_requires_explicit_materialization")
        path = _package.validate_relative_path(raw_path.decode("utf-8"))
        rows.append((path, object_id, mode))
    if not rows:
        raise ValueError("source_tree_empty")
    output.mkdir()
    tracked = []
    process = subprocess.Popen(["git", "-C", str(repository), "cat-file", "--batch"], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        for name, object_id, mode in rows:
            process.stdin.write(object_id.encode("ascii") + b"\n")
            process.stdin.flush()
            response = process.stdout.readline(256).decode("ascii").strip().split(" ")
            if len(response) != 3 or response[:2] != [object_id, "blob"]:
                raise ValueError("source_blob_unavailable")
            size = int(response[2])
            if size < 0 or size > _package.MAX_ARCHIVE:
                raise ValueError("source_blob_resource_limit")
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            blob_hash = hashlib.sha1(f"blob {size}\0".encode("ascii"))
            remaining = size
            with path.open("xb") as target:
                while remaining:
                    chunk = process.stdout.read(min(remaining, 1024 * 1024))
                    if not chunk:
                        raise ValueError("source_blob_truncated")
                    blob_hash.update(chunk)
                    target.write(chunk)
                    remaining -= len(chunk)
            if process.stdout.read(1) != b"\n" or blob_hash.hexdigest() != object_id:
                raise ValueError("source_blob_identity_mismatch")
            tracked.append({"path": name, "blobSha": object_id, "size": size})
        process.stdin.close()
        if process.wait(timeout=30) != 0:
            raise ValueError("source_git_operation_failed")
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
    manifest_path = output / "native/updater-trust/Cargo.toml"
    vendor = output / "native/updater-trust/vendor"
    lock_path = output / "native/updater-trust/Cargo.lock"
    if _package._ordinary(lock_path).st_size > _package.MAX_MANIFEST:
        raise ValueError("source_dependency_lock_invalid")
    _package.locked_registry_packages(lock_path.read_bytes())
    configured_home = args.cargo_home or os.environ.get("CARGO_HOME")
    if configured_home:
        cargo_home = Path(configured_home).absolute()
    elif os.name == "nt" and Path("D:/").is_dir():
        cargo_home = Path("D:/CodexData/Cache/updater-source-cargo")
    else:
        cargo_home = output.parent / ".cargo-source-cache"
    ancestor = cargo_home
    while not ancestor.exists():
        ancestor = ancestor.parent
    _package._ordinary(ancestor)
    cargo_home.mkdir(parents=True, exist_ok=True)
    _package._ordinary(cargo_home)
    result = subprocess.run([args.cargo, "vendor", "--locked", "--manifest-path", str(manifest_path), str(vendor)],
                            capture_output=True, timeout=1200, env=dict(os.environ, CARGO_HOME=str(cargo_home)))
    if result.returncode:
        raise ValueError("source_locked_vendor_failed")
    config = output / "native/updater-trust/.cargo/config.toml"
    config.parent.mkdir(exist_ok=True)
    with config.open("x", encoding="utf-8", newline="\n") as target:
        target.write('[source.crates-io]\nreplace-with = "vendored-sources"\n\n[source.vendored-sources]\ndirectory = "vendor"\n')
    archive_count = registry_archives(output, cargo_home)
    tracked_names = {row["path"] for row in tracked}
    generated, source_rows = [], []
    for path, info in files(output):
        name = _package.validate_relative_path(path.relative_to(output).as_posix())
        checksum = _package._hash(path)
        row = {"path": name, "size": info.st_size, "sha256": checksum}
        source_rows.append(row)
        if name not in tracked_names:
            if not (name.startswith("native/updater-trust/vendor/") or name.startswith("native/updater-trust/vendor-archives/")
                    or name == "native/updater-trust/.cargo/config.toml"):
                raise ValueError("source_unclassified_generated_file")
            generated.append({**row, "kind": "cargo-crate-archive" if name.startswith("native/updater-trust/vendor-archives/") else "cargo-vendor"})
    provenance = {"schema": "gsw.native_update_source_provenance/1", "sourceCommit": commit,
                  "sourceTree": tree, "trackedFiles": sorted(tracked, key=lambda row: row["path"]),
                  "generatedDependencies": sorted(generated, key=lambda row: row["path"])}
    source_manifest = {"schema": "gsw-native-corresponding-source-v1", "license": "GPL-3.0-or-later",
                       "executableSha256": _package._hash(args.executable), "sourceFileCount": len(source_rows),
                       "files": sorted(source_rows, key=lambda row: row["path"])}
    write_json(args.provenance, provenance)
    write_json(args.manifest, source_manifest)
    return {"schema": "gsw.update_source_preparation/1", "ok": True, "sourceCommit": commit,
            "sourceTree": tree, "trackedFileCount": len(tracked), "generatedFileCount": len(generated),
            "dependencyArchiveCount": archive_count,
            "sourceManifestSha256": _package._hash(args.manifest), "provenanceSha256": _package._hash(args.provenance)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--cargo-home", type=Path)
    try:
        print(json.dumps(prepare(parser.parse_args()), ensure_ascii=True))
        return 0
    except Exception as error:
        print(json.dumps({"schema": "gsw.update_source_preparation/1", "ok": False,
                          "code": "source_preparation_failed", "errorType": type(error).__name__}))
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
