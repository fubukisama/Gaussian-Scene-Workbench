"""Read-only artifact round-trip verification; never launches packaged code.

The optional baseline is the controlled staging directory from the same job.
It catches files omitted during upload, including hidden corresponding source.
Manifests bind bytes, not publisher authenticity or application acceptance.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys


def file_path(path: Path) -> Path:
    # Inspect deep corresponding-source trees without depending on the host's
    # LongPathsEnabled registry policy. This changes only filesystem addressing.
    value = str(path)
    if os.name == "nt" and path.is_absolute() and not value.startswith("\\\\?\\"):
        return Path("\\\\?\\UNC\\" + value[2:] if value.startswith("\\\\") else "\\\\?\\" + value)
    return path


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def child(root: Path, relative: object) -> Path:
    if (not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative
            or PurePosixPath(relative).is_absolute()
            or any(part in ("", ".", "..", ".git") for part in relative.split("/"))):
        raise ValueError("unsafe manifest path: " + repr(relative))
    path = root / relative
    path.resolve().relative_to(root.resolve())
    return path


def inventory(root: Path) -> dict[str, tuple[int, str]]:
    if not root.is_absolute() or not root.is_dir() or root.is_symlink():
        raise ValueError("an existing absolute owned package directory is required")
    result: dict[str, tuple[int, str]] = {}
    folded: set[str] = set()
    for parent, directories, files in os.walk(root, followlinks=False):
        for name in directories + files:
            path = Path(parent) / name
            metadata = path.lstat()
            if name == ".git":
                raise ValueError("Git metadata must not enter an artifact: " + path.relative_to(root).as_posix())
            if path.is_symlink() or getattr(metadata, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400):
                raise ValueError("links/reparse points are forbidden in the controlled artifact")
        for name in files:
            path = Path(parent) / name
            relative = path.relative_to(root).as_posix()
            if relative.casefold() in folded:
                raise ValueError("case-colliding artifact path: " + relative)
            folded.add(relative.casefold())
            result[relative] = (path.stat().st_size, digest(path))
    return result


def read_json(path: Path) -> dict:
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("manifest exceeds its bounded read limit")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError("manifest must be an object")
    return value


def verify_records(root: Path, records: object, size_key: str, prefix: str = "") -> int:
    if not isinstance(records, list) or not records:
        raise ValueError("nonempty manifest file records are required")
    seen: set[str] = set()
    for row in records:
        if not isinstance(row, dict):
            raise ValueError("invalid manifest record")
        relative = row.get("path")
        path = child(root, relative)
        assert isinstance(relative, str)
        if relative.casefold() in seen:
            raise ValueError("duplicate manifest file: " + prefix + relative)
        seen.add(relative.casefold())
        expected = row.get("sha256")
        size = row.get(size_key)
        if type(size) is not int or size < 0 or not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("invalid size/hash in manifest: " + prefix + relative)
        if not path.is_file():
            raise ValueError("manifest file missing: " + prefix + relative)
        if path.stat().st_size != size or digest(path) != expected:
            raise ValueError("manifest file bytes mismatch: " + prefix + relative)
    return len(records)


def verify(root: Path, baseline: Path | None) -> dict:
    actual = inventory(root)
    backend = read_json(root / "backend_manifest.json")
    if backend.get("schemaVersion") != 2:
        raise ValueError("native backend manifest schema mismatch")
    backend_count = verify_records(root, backend.get("files"), "bytes")
    source = read_json(root / "source-manifest.json")
    if source.get("schema") != "gsw-native-corresponding-source-v1":
        raise ValueError("corresponding source manifest schema mismatch")
    source_count = verify_records(root / "source", source.get("files"), "size", "source/")
    if type(source.get("sourceFileCount")) is not int or source.get("sourceFileCount") != source_count:
        raise ValueError("corresponding source manifest count mismatch")
    if set(inventory(root / "source")) != {row["path"] for row in source["files"]}:
        raise ValueError("corresponding source contains unlisted files")
    executable = root / "bin/Gaussian Scene Workbench.exe"
    if not executable.is_file() or digest(executable) != source.get("executableSha256"):
        raise ValueError("packaged executable/corresponding source binding mismatch")
    if baseline is not None:
        expected = inventory(baseline)
        missing = sorted(expected.keys() - actual.keys())
        extra = sorted(actual.keys() - expected.keys())
        changed = sorted(path for path in actual.keys() & expected.keys() if actual[path] != expected[path])
        if missing or extra or changed:
            raise ValueError("artifact round-trip mismatch: " + json.dumps(
                {"missing": missing[:20], "extra": extra[:20], "changed": changed[:20]}, ensure_ascii=False))
    return {"fileCount": len(actual), "backendFilesVerified": backend_count,
            "sourceFilesVerified": source_count, "baselineCompared": baseline is not None,
            "hiddenFileCount": sum(any(part.startswith(".") for part in path.split("/")) for path in actual),
            "executableSha256": digest(executable)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    args.root = file_path(args.root)
    args.baseline = file_path(args.baseline) if args.baseline is not None else None
    args.output = file_path(args.output)
    if not args.output.is_absolute() or not args.output.parent.is_dir() or args.output.exists():
        parser.error("--output must be a fresh absolute receipt in an existing owned evidence directory")
    for root in (args.root, args.baseline):
        if root is not None:
            try:
                args.output.resolve().relative_to(root.resolve())
            except ValueError:
                pass
            else:
                parser.error("--output must be outside both read-only package directories")
    result = {"schema": "gsw.native_artifact_roundtrip/1", "root": str(args.root),
              "baseline": str(args.baseline) if args.baseline else None,
              "checkerSourceCommit": os.environ.get("GITHUB_SHA"),
              "applicationExecuted": False, "authenticityClaim": False}
    try:
        result.update(verify(args.root, args.baseline))
        result["status"] = "PASS"
    except (OSError, ValueError, TypeError, KeyError) as error:
        result.update(status="FAIL", error=str(error))
    with args.output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result["status"] == "PASS" else 2


if __name__ == "__main__":
    sys.exit(main())
