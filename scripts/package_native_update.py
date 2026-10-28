"""Create a complete native update ZIP, not a publisher-authentication claim.

Developer/CI tooling only: the native updater does not execute this script.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import unicodedata
import zipfile


PRODUCT = "Fubukisama.GaussianSceneWorkbench.Native"
FIXTURE_PRODUCT = "Fubukisama.GaussianSceneWorkbench.UpdateFixture"
MAX_ARCHIVE = 4 * 1024**3
MAX_UNPACKED = 16 * 1024**3
MAX_FILES = 100_000
MAX_MANIFEST = 64 * 1024**2
MAX_DEPTH = 32
MAX_SAFE_INTEGER = 9_007_199_254_740_991
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
SHA1 = re.compile(r"[0-9a-f]{40}\Z")
BUILD_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}\Z")
REQUIRED = frozenset({"bin/Gaussian Scene Workbench.exe", "COPYING", "LICENSE",
    "THIRD_PARTY_LICENSES.md", "native/LICENSE.md", "source/native/CMakeLists.txt",
    "source-manifest.json", "build_manifest.json", "backend_manifest.json"})


class ArchiveError(ValueError):
    """Stable public CLI error, without credentialled URLs or private contents."""

    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


def _fail(code: str):
    raise ArchiveError(code)


def _pairs(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            _fail("update_json_duplicate_key")
        value[key] = item
    return value


def _json(raw: bytes):
    if len(raw) > MAX_MANIFEST:
        _fail("update_manifest_too_large")
    try:
        return json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_pairs,
                          parse_constant=lambda _: _fail("update_json_nonfinite"))
    except (UnicodeError, json.JSONDecodeError):
        _fail("update_json_invalid")


def _bytes(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def _hash(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _ordinary(path: Path):
    # Inspect the path as named, including all existing ancestors. resolve()
    # alone would conceal junctions before a caller could reject them.
    for candidate in (path,) + tuple(path.parents):
        try:
            info = candidate.lstat()
        except OSError:
            _fail("update_package_path_unavailable")
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            _fail("update_package_link_refused")
    return path.stat()


def validate_relative_path(value: str) -> str:
    """Portable Windows payload name; dot files are legitimate, aliases are not."""
    if (not isinstance(value, str) or not value or len(value.encode("utf-8")) > 1024
            or "\\" in value or ":" in value or value.startswith("/")):
        _fail("update_package_path_invalid")
    parts = value.split("/")
    if len(parts) > MAX_DEPTH:
        _fail("update_package_path_too_deep")
    for part in parts:
        if (not part or part in (".", "..") or len(part) > 255
                or len(part.encode("utf-16-le")) // 2 > 255
                or part.endswith((".", " ")) or any(ord(c) < 32 for c in part)
                or any(c in '<>"|?*' for c in part)
                or unicodedata.normalize("NFC", part) != part):
            _fail("update_package_path_invalid")
        normalized = unicodedata.normalize("NFKC", part).upper()
        stem = normalized.split(".", 1)[0]
        if (stem in {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"}
                or re.fullmatch(r"(?:COM|LPT)[1-9]", stem)):
            _fail("update_package_device_name")
        if normalized in {".", ".."}:
            _fail("update_package_path_invalid")
    return value


def _alias(value: str) -> str:
    return unicodedata.normalize("NFKC", value).casefold()


def _claim(names: dict, value: str, kind: str):
    validate_relative_path(value)
    key = _alias(value)
    if key in names and names[key] != (value, kind):
        _fail("update_package_path_alias")
    names[key] = value, kind


def _collect(root: Path) -> dict[str, Path]:
    if not stat.S_ISDIR(_ordinary(root).st_mode):
        _fail("update_package_root_invalid")
    names, result = {}, {}
    total = 0
    for directory, dirs, files in os.walk(root, followlinks=False):
        owner = Path(directory)
        _ordinary(owner)
        for name in dirs:
            path = owner / name
            relative = path.relative_to(root).as_posix()
            if name.casefold() in {".git", ".planning", "__pycache__"}:
                _fail("update_package_private_tree")
            if not stat.S_ISDIR(_ordinary(path).st_mode):
                _fail("update_package_file_type_invalid")
            _claim(names, relative, "directory")
        for name in files:
            path = owner / name
            relative = path.relative_to(root).as_posix()
            info = _ordinary(path)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink > 1:
                _fail("update_package_file_type_invalid")
            if relative == "release-files.json":
                _fail("update_package_reserved_manifest_exists")
            _claim(names, relative, "file")
            if relative in result or len(result) >= MAX_FILES or info.st_size < 0:
                _fail("update_package_resource_limit")
            total += info.st_size
            if info.st_size > MAX_ARCHIVE or total > MAX_UNPACKED:
                _fail("update_package_resource_limit")
            result[relative] = path
    return result


def _source_binding(paths: dict[str, Path]) -> str:
    if not REQUIRED <= paths.keys():
        _fail("update_package_required_file_missing")
    if paths["source-manifest.json"].stat().st_size > MAX_MANIFEST:
        _fail("update_manifest_too_large")
    source_bytes = paths["source-manifest.json"].read_bytes()
    source = _json(source_bytes)
    keys = {"schema", "executableSha256", "license", "sourceFileCount", "files"}
    if (not isinstance(source, dict) or set(source) != keys
            or source["schema"] != "gsw-native-corresponding-source-v1"
            or source["license"] != "GPL-3.0-or-later"
            or source["executableSha256"] != _hash(paths["bin/Gaussian Scene Workbench.exe"])
            or type(source["sourceFileCount"]) is not int or not isinstance(source["files"], list)
            or len(source["files"]) != source["sourceFileCount"]):
        _fail("update_package_source_identity_invalid")
    recorded, aliases = set(), {}
    for entry in source["files"]:
        if (not isinstance(entry, dict) or set(entry) != {"path", "size", "sha256"}
                or type(entry["size"]) is not int or entry["size"] < 0
                or not isinstance(entry["sha256"], str) or not SHA256.fullmatch(entry["sha256"])):
            _fail("update_package_source_manifest_invalid")
        relative = validate_relative_path(entry["path"])
        _claim(aliases, relative, "file")
        if relative in recorded:
            _fail("update_package_source_manifest_invalid")
        recorded.add(relative)
        path = paths.get("source/" + relative)
        if path is None or path.stat().st_size != entry["size"] or _hash(path) != entry["sha256"]:
            _fail("update_package_source_files_differ")
    actual = {name[7:] for name in paths if name.startswith("source/")}
    if not recorded or recorded != actual:
        _fail("update_package_source_files_differ")
    build = _json(paths["build_manifest.json"].read_bytes())
    if (not isinstance(build, dict) or build.get("product") != "Gaussian Scene Workbench"
            or build.get("legacyWebRuntimeIncluded") is not False):
        _fail("update_package_product_invalid")
    return hashlib.sha256(source_bytes).hexdigest()


def _verify_written(archive: Path, manifest: dict, manifest_bytes: bytes):
    expected = {entry["path"]: entry for entry in manifest["files"]}
    aliases = {}
    with zipfile.ZipFile(archive) as payload:
        entries = payload.infolist()
        if len(entries) != len(expected) + 1:
            _fail("update_archive_file_set_differs")
        actual = set()
        for entry in entries:
            name = validate_relative_path(entry.filename)
            _claim(aliases, name, "file")
            if name in actual or entry.is_dir() or entry.flag_bits & 1:
                _fail("update_archive_file_set_differs")
            actual.add(name)
            if name == "release-files.json":
                if payload.read(entry) != manifest_bytes:
                    _fail("update_archive_manifest_differs")
                continue
            row = expected.get(name)
            if row is None or entry.file_size != row["size"]:
                _fail("update_archive_file_set_differs")
            digest = hashlib.sha256()
            with payload.open(entry) as stream:
                while block := stream.read(1024 * 1024):
                    digest.update(block)
            if digest.hexdigest() != row["sha256"]:
                _fail("update_archive_file_digest_differs")
        if actual != set(expected) | {"release-files.json"}:
            _fail("update_archive_file_set_differs")


def create_archive(package_root: Path, output_archive: Path, *, product: str,
                   source_commit: str, source_tree: str, build_id: str,
                   display_version: str | None = None, release_sequence: int | None = None) -> dict:
    if product not in (PRODUCT, FIXTURE_PRODUCT):
        _fail("update_package_product_invalid")
    if (not SHA1.fullmatch(source_commit) or not SHA1.fullmatch(source_tree)
            or not BUILD_ID.fullmatch(build_id)):
        _fail("update_package_identity_invalid")
    validate_relative_path(build_id)
    if release_sequence is not None and (type(release_sequence) is not int
            or not 1 <= release_sequence <= MAX_SAFE_INTEGER):
        _fail("update_package_identity_invalid")
    if ".." in package_root.parts or ".." in output_archive.parts:
        _fail("update_package_path_invalid")
    root, output = package_root.absolute(), output_archive.absolute()
    _ordinary(output.parent)
    if output.exists() or output.is_symlink() or output.is_relative_to(root):
        _fail("update_archive_destination_not_fresh")
    paths = _collect(root)
    source_hash = _source_binding(paths)
    rows = [{"path": name, "size": path.stat().st_size, "sha256": _hash(path)}
            for name, path in sorted(paths.items())]
    manifest = {"schema": "gsw.native_update_files/1", "product": product,
        "platform": "windows-x64", "buildId": build_id, "sourceCommit": source_commit,
        "sourceTree": source_tree, "files": rows}
    manifest_bytes = _bytes(manifest)
    if len(manifest_bytes) > MAX_MANIFEST:
        _fail("update_manifest_too_large")
    unpacked = sum(row["size"] for row in rows) + len(manifest_bytes)
    if unpacked > MAX_UNPACKED:
        _fail("update_package_resource_limit")
    created = False
    try:
        with output.open("xb") as stream:
            created = True
            with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED,
                                 compresslevel=6, allowZip64=True) as archive:
                for row in rows + [{"path": "release-files.json"}]:
                    name = row["path"]
                    entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                    entry.compress_type = zipfile.ZIP_DEFLATED
                    entry.create_system = 3
                    entry.external_attr = (stat.S_IFREG | 0o644) << 16
                    entry.file_size = len(manifest_bytes) if name == "release-files.json" else row["size"]
                    with archive.open(entry, "w") as destination:
                        if name == "release-files.json":
                            destination.write(manifest_bytes)
                        else:
                            _ordinary(paths[name])
                            with paths[name].open("rb") as source:
                                while block := source.read(1024 * 1024):
                                    destination.write(block)
                    if stream.tell() > MAX_ARCHIVE:
                        _fail("update_package_resource_limit")
            stream.flush()
            os.fsync(stream.fileno())
        if output.stat().st_size > MAX_ARCHIVE:
            _fail("update_package_resource_limit")
        _verify_written(output, manifest, manifest_bytes)
    except BaseException:
        # Only this call's CreateNew output; never remove a pre-existing file.
        if created:
            output.unlink(missing_ok=True)
        raise
    report = {"schema": "gsw.native_update_package/1", "product": product,
        "platform": "windows-x64", "buildId": build_id, "sourceCommit": source_commit,
        "sourceTree": source_tree, "archivePath": str(output), "archiveLength": output.stat().st_size,
        "archiveSha256": _hash(output), "payloadManifestSha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "correspondingSourceSha256": source_hash, "unpackedLength": unpacked, "fileCount": len(rows)}
    if display_version is not None:
        report["displayVersion"] = display_version
    if release_sequence is not None:
        report["releaseSequence"] = release_sequence
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--output-archive", type=Path, required=True)
    parser.add_argument("--product", default=PRODUCT)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--display-version")
    parser.add_argument("--release-sequence", type=int)
    arguments = parser.parse_args()
    try:
        report = create_archive(arguments.package_root, arguments.output_archive,
            product=arguments.product, source_commit=arguments.source_commit,
            source_tree=arguments.source_tree, build_id=arguments.build_id,
            display_version=arguments.display_version, release_sequence=arguments.release_sequence)
        # ASCII-escaped JSON is UTF-8-compatible even when Python -I ignores
        # PYTHONUTF8 and the Windows pipe uses a legacy locale encoding.
        print(json.dumps(report, ensure_ascii=True))
        return 0
    except ArchiveError as error:
        print(json.dumps({"schema": "gsw.native_update_package/1", "ok": False, "code": error.code}))
        return 2
    except (OSError, ValueError, TypeError, zipfile.BadZipFile):
        print(json.dumps({"schema": "gsw.native_update_package/1", "ok": False,
                          "code": "update_archive_io_or_format_error"}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
