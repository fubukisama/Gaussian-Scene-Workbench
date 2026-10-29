"""Create a complete native update ZIP, not a publisher-authentication claim.

Developer/CI tooling only: the native updater does not execute this script.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tarfile
import tomllib
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
MAX_CRATE_UNPACKED = 512 * 1024**2
MAX_DEPENDENCY_UNPACKED = 2 * 1024**3
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


def _claim_file(names: dict, name: str):
    """A file cannot also be another file's parent, including case aliases."""
    parts = validate_relative_path(name).split("/")
    for count in range(1, len(parts)):
        _claim(names, "/".join(parts[:count]), "directory")
    _claim(names, name, "file")


def _manifest_rows(rows) -> dict:
    if not isinstance(rows, list) or not 0 < len(rows) <= MAX_FILES:
        _fail("update_archive_manifest_invalid")
    result, aliases = {}, {}
    for row in rows:
        if (not isinstance(row, dict) or set(row) != {"path", "size", "sha256"}
                or type(row["size"]) is not int or not 0 <= row["size"] <= MAX_ARCHIVE
                or not isinstance(row["sha256"], str) or not SHA256.fullmatch(row["sha256"])):
            _fail("update_archive_manifest_invalid")
        name = validate_relative_path(row["path"])
        _claim_file(aliases, name)
        if name in result:
            _fail("update_archive_file_set_differs")
        result[name] = row
    return result


def _source_content(payload, name: str, hashes: dict) -> bytes:
    """Bounded read of an already hash-checked source member, never a local path."""
    key = "source/" + name
    row = hashes.get(key)
    if row is None or row["size"] > MAX_MANIFEST:
        _fail("update_source_dependency_evidence_unavailable")
    raw = payload.read(key)
    if len(raw) != row["size"] or hashlib.sha256(raw).hexdigest() != row["sha256"]:
        _fail("update_source_dependency_files_differ")
    return raw


def _toml(raw: bytes) -> dict:
    try:
        return tomllib.loads(raw.decode("utf-8"))
    except (UnicodeError, tomllib.TOMLDecodeError):
        _fail("update_source_dependency_metadata_invalid")


def _cargo_identity(package) -> tuple[str, str]:
    if (not isinstance(package, dict) or not isinstance(package.get("name"), str)
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", package["name"])
            or not isinstance(package.get("version"), str)
            or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?(?:\+[A-Za-z0-9.-]+)?", package["version"])):
        _fail("update_source_dependency_metadata_invalid")
    return package["name"], package["version"]


def locked_registry_packages(raw: bytes) -> dict:
    """Preflight/cache inventory; release eligibility still uses the full source verifier."""
    if len(raw) > MAX_MANIFEST:
        _fail("update_source_dependency_resource_limit")
    lock = _toml(raw)
    packages = lock.get("package")
    if (type(lock.get("version")) is not int or lock["version"] not in (3, 4)
            or not isinstance(packages, list) or not 0 < len(packages) <= MAX_FILES):
        _fail("update_source_dependency_metadata_invalid")
    result, identities = {}, set()
    for package in packages:
        identity = _cargo_identity(package)
        if identity in identities:
            _fail("update_source_dependency_metadata_invalid")
        identities.add(identity)
        if "source" not in package:
            if "checksum" in package:
                _fail("update_source_dependency_metadata_invalid")
            continue
        if (package["source"] != "registry+https://github.com/rust-lang/crates.io-index"
                or not isinstance(package.get("checksum"), str) or not SHA256.fullmatch(package["checksum"])):
            _fail("update_source_dependency_evidence_unavailable")
        validate_relative_path(identity[0] + "-" + identity[1] + ".crate")
        result[identity] = package["checksum"]
    return result


def _cargo_path_manifest(owner: str, relative) -> str:
    """Resolve only canonical portable relative directories, never OS aliases."""
    if (not isinstance(relative, str) or not relative or len(relative.encode("utf-8")) > 1024
            or relative.startswith("/") or "\\" in relative or ":" in relative):
        _fail("update_source_dependency_path_invalid")
    parts, parent_only = owner.split("/")[:-1], True
    for component in relative.split("/"):
        if component == "..":
            if not parent_only or not parts:
                _fail("update_source_dependency_path_invalid")
            parts.pop()
        else:
            validate_relative_path(component)
            parts.append(component)
            parent_only = False
    return validate_relative_path("/".join(parts + ["Cargo.toml"]))


def _local_cargo_packages(payload, hashes: dict, recorded: set, root: str) -> tuple[set, tuple]:
    """Narrow manifest profile: explicit Git-local path dependencies only.

    All dependency/build/dev and target tables are inspected, including optional
    and inactive targets. This is not a general Cargo resolver: workspace
    inheritance, patches/replacements, Git and alternate registries fail closed.
    """
    identities, visited, pending, first = {}, set(), [(root, None, None)], None
    dependency_keys = ("dependencies", "build-dependencies", "dev-dependencies")
    while pending:
        name, required_name, required_version = pending.pop()
        if name not in recorded:
            _fail("update_source_dependency_evidence_unavailable")
        document = _toml(_source_content(payload, name, hashes))
        package = document.get("package")
        identity = _cargo_identity(package)
        if (any(key in document for key in ("workspace", "patch", "replace", "build_dependencies", "dev_dependencies"))
                or "workspace" in package
                or any(isinstance(value, dict) and "workspace" in value for value in package.values())
                or isinstance(document.get("lints"), dict) and "workspace" in document["lints"]):
            _fail("update_source_dependency_profile_unsupported")
        if required_name is not None and identity[0] != required_name:
            _fail("update_source_dependency_metadata_invalid")
        # Accept a version literal (optionally prefixed '=') only when it
        # matches this identity; do not imitate Cargo's broader semver resolver.
        if required_version is not None and required_version not in (identity[1], "=" + identity[1]):
            _fail("update_source_dependency_profile_unsupported")
        if identity in identities and identities[identity] != name:
            _fail("update_source_dependency_metadata_invalid")
        identities[identity] = name
        if first is None:
            first = identity
        if name in visited:
            continue
        visited.add(name)
        if len(visited) > MAX_FILES:
            _fail("update_source_dependency_resource_limit")
        # Cargo discovers enclosing workspace roots. Do not silently omit an
        # ancestor's unsupported dependency override/inheritance semantics.
        components = name.split("/")[:-1]
        for count in range(len(components)):
            ancestor = "/".join(components[:count] + ["Cargo.toml"])
            if ancestor in recorded:
                parent = _toml(_source_content(payload, ancestor, hashes))
                if any(key in parent for key in ("workspace", "patch", "replace")):
                    _fail("update_source_dependency_profile_unsupported")
        tables = [document]
        targets = document.get("target", {})
        if not isinstance(targets, dict) or any(not isinstance(value, dict) for value in targets.values()):
            _fail("update_source_dependency_metadata_invalid")
        for target in targets.values():
            if set(target) - set(dependency_keys):
                _fail("update_source_dependency_profile_unsupported")
            tables.append(target)
        for table in tables:
            for key in dependency_keys:
                dependencies = table.get(key, {})
                if not isinstance(dependencies, dict):
                    _fail("update_source_dependency_metadata_invalid")
                for alias, dependency in dependencies.items():
                    if isinstance(dependency, str):
                        continue  # Registry resolution remains independently lock/archive-bound.
                    if not isinstance(dependency, dict):
                        _fail("update_source_dependency_metadata_invalid")
                    if any(key in dependency for key in ("workspace", "git", "rev", "branch", "tag", "registry", "registry-index")):
                        _fail("update_source_dependency_profile_unsupported")
                    if "path" in dependency:
                        target = _cargo_path_manifest(name, dependency["path"])
                        pending.append((target, dependency.get("package", alias), dependency.get("version")))
                    elif not isinstance(dependency.get("version"), str):
                        _fail("update_source_dependency_metadata_invalid")
    return set(identities), first


class _DependencyReader:
    """Cap gzip expansion, including TAR metadata/padding, before TAR parsing."""
    def __init__(self, stream, budget):
        self.stream, self.budget = stream, budget
        self.remaining = MAX_CRATE_UNPACKED

    def read(self, size):
        amount = min(size, self.remaining + 1, self.budget[0] + 1, MAX_MANIFEST + 1)
        raw = self.stream.read(amount)
        if len(raw) > self.remaining or len(raw) > self.budget[0]:
            _fail("update_source_dependency_resource_limit")
        self.remaining -= len(raw)
        self.budget[0] -= len(raw)
        return raw


def _crate_members(payload, path: str, package: str, budget) -> dict:
    """Inspect the exact lock-bound registry archive without extracting it."""
    entry = payload.getinfo("source/" + path)
    if not 0 < entry.file_size <= MAX_MANIFEST:
        _fail("update_source_dependency_resource_limit")
    members, seen, aliases, total = {}, set(), {}, 0
    try:
        with payload.open(entry) as compressed, gzip.GzipFile(fileobj=compressed) as inflated:
            bounded = _DependencyReader(inflated, budget)
            # Streaming TAR never executes/extracts members. Scan through zero
            # blocks too, so a second concatenated TAR cannot hide extra files.
            with tarfile.open(fileobj=bounded, mode="r|", ignore_zeros=True) as crate:
                for member in crate:
                    name = member.name.removesuffix("/") if member.isdir() else member.name
                    validate_relative_path(name)
                    if name in seen or len(seen) >= MAX_FILES or member.issparse():
                        _fail("update_source_dependency_archive_invalid")
                    seen.add(name)
                    if not member.isdir() and not member.isfile():
                        _fail("update_source_dependency_archive_invalid")
                    if not (name == package or name.startswith(package + "/")):
                        _fail("update_source_dependency_archive_invalid")
                    if member.isdir():
                        _claim(aliases, name, "directory")
                        continue
                    _claim_file(aliases, name)
                    relative = name[len(package) + 1:]
                    if (not relative or relative == ".cargo-checksum.json" or
                            not 0 <= member.size <= MAX_MANIFEST or total + member.size > MAX_CRATE_UNPACKED):
                        _fail("update_source_dependency_resource_limit")
                    total += member.size
                    checksum, received = hashlib.sha256(), 0
                    stream = crate.extractfile(member)  # Read-only stream; no filesystem extraction.
                    if stream is None:
                        _fail("update_source_dependency_archive_invalid")
                    with stream:
                        while block := stream.read(min(1024**2, member.size - received + 1)):
                            received += len(block)
                            if received > member.size:
                                _fail("update_source_dependency_resource_limit")
                            checksum.update(block)
                    if received != member.size:
                        _fail("update_source_dependency_archive_invalid")
                    members[relative] = {"size": received, "sha256": checksum.hexdigest()}
            while bounded.read(1024**2):
                pass  # Complete gzip CRC/truncation checks inside the same expansion budget.
    except ArchiveError:
        raise
    except (tarfile.TarError, OSError, EOFError, UnicodeError, ValueError):
        _fail("update_source_dependency_archive_invalid")
    if "Cargo.toml" not in members:
        _fail("update_source_dependency_archive_invalid")
    return members


def _verify_dependencies(payload, hashes: dict, recorded: set, rows: list) -> dict:
    """Git lock -> original crate digest -> every vendor byte, not self-reporting.

    Only the existing crates.io registry profile is supported. Missing raw
    archives, Git dependencies and arbitrary path remapping remain fail-closed.
    """
    root = "native/updater-trust/"
    lock_name, manifest_name = root + "Cargo.lock", root + "Cargo.toml"
    if lock_name not in recorded or manifest_name not in recorded:
        _fail("update_source_dependency_evidence_unavailable")
    lock = _toml(_source_content(payload, lock_name, hashes))
    local_packages, root_identity = _local_cargo_packages(payload, hashes, recorded, manifest_name)
    packages = lock.get("package")
    if not isinstance(packages, list) or not 0 < len(packages) <= MAX_FILES:
        _fail("update_source_dependency_metadata_invalid")
    locked, identities, local_locked = {}, set(), set()
    for package in packages:
        identity = _cargo_identity(package)
        if identity in identities:
            _fail("update_source_dependency_metadata_invalid")
        identities.add(identity)
        if "source" not in package:
            if "checksum" in package:
                _fail("update_source_dependency_metadata_invalid")
            if identity not in local_packages:
                _fail("update_source_dependency_evidence_unavailable")
            local_locked.add(identity)
            continue
        if (package["source"] != "registry+https://github.com/rust-lang/crates.io-index"
                or not isinstance(package.get("checksum"), str) or not SHA256.fullmatch(package["checksum"])
                or package["checksum"] in locked):
            _fail("update_source_dependency_evidence_unavailable")
        locked[package["checksum"]] = identity
    if local_packages - {root_identity} - local_locked:
        _fail("update_source_dependency_evidence_unavailable")
    generated = {}
    if len(rows) > MAX_FILES:
        _fail("update_source_provenance_invalid")
    for row in rows:
        if (not isinstance(row, dict) or set(row) != {"path", "size", "sha256", "kind"}
                or type(row["size"]) is not int or not 0 <= row["size"] <= MAX_ARCHIVE
                or not isinstance(row["sha256"], str) or not SHA256.fullmatch(row["sha256"])
                or row["kind"] not in ("cargo-vendor", "cargo-crate-archive")):
            _fail("update_source_provenance_invalid")
        name = validate_relative_path(row["path"])
        actual = hashes.get("source/" + name)
        if (name in recorded or name in generated or actual is None
                or actual["size"] != row["size"] or actual["sha256"] != row["sha256"]):
            _fail("update_source_dependency_files_differ")
        generated[name] = row
    if not locked:
        if generated:
            _fail("update_source_dependency_evidence_unavailable")
        return {"generated": set(), "registryPackageCount": 0, "dependencyArchiveCount": 0}
    config_name = root + ".cargo/config.toml"
    if config_name not in generated or generated[config_name]["kind"] != "cargo-vendor":
        _fail("update_source_dependency_evidence_unavailable")
    if _toml(_source_content(payload, config_name, hashes)) != {"source": {
            "crates-io": {"replace-with": "vendored-sources"}, "vendored-sources": {"directory": "vendor"}}}:
        _fail("update_source_dependency_metadata_invalid")
    checked, vendor_roots, budget = {config_name}, {}, [MAX_DEPENDENCY_UNPACKED]
    for name, row in generated.items():
        if name.startswith(root + "vendor/") and name.endswith("/.cargo-checksum.json"):
            directory = name[len(root + "vendor/"):-len("/.cargo-checksum.json")]
            if "/" in directory or row["kind"] != "cargo-vendor":
                _fail("update_source_dependency_metadata_invalid")
            checksum = _json(_source_content(payload, name, hashes))
            reason = None
            if (not isinstance(checksum, dict) or not {"package", "files"} <= set(checksum)
                    or set(checksum) - {"package", "files", "$comment"}):
                reason = "schema_invalid"
            elif "$comment" in checksum and (not isinstance(checksum["$comment"], str) or len(checksum["$comment"]) > 1024):
                reason = "comment_invalid"
            elif not isinstance(checksum["package"], str):
                reason = "package_type_invalid"
            elif checksum["package"] not in locked:
                reason = "package_not_in_lock"
            elif checksum["package"] in vendor_roots:
                reason = "package_duplicate"
            elif not isinstance(checksum["files"], dict):
                reason = "files_type_invalid"
            if reason is not None:
                error = ArchiveError("update_source_vendor_checksum_" + reason)
                if re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.+-]{0,192}", directory):
                    error.sourceComponent = directory
                raise error
            vendor_roots[checksum["package"]] = directory, checksum
    if set(vendor_roots) != set(locked):
        _fail("update_source_dependency_evidence_unavailable")
    for digest, (name, version) in locked.items():
        package = name + "-" + version
        archive_name = root + "vendor-archives/" + package + ".crate"
        archive_row = generated.get(archive_name)
        if (archive_row is None or archive_row["kind"] != "cargo-crate-archive"
                or archive_row["sha256"] != digest):
            _fail("update_source_dependency_evidence_unavailable")
        directory, checksum = vendor_roots[digest]
        if directory not in (name, package):
            _fail("update_source_dependency_files_differ")
        members = _crate_members(payload, archive_name, package, budget)
        prefix = root + "vendor/" + directory + "/"
        if checksum["files"] != {path: row["sha256"] for path, row in members.items()}:
            declared, expected_files = checksum["files"], {path: row["sha256"] for path, row in members.items()}
            missing, extra = set(expected_files) - set(declared), set(declared) - set(expected_files)
            known, categories = set(), set()
            for relative in missing:
                parts = relative.split("/")
                if ".git" in parts:
                    known.add(relative); categories.add("git-directory")
                elif parts[-1] == ".gitignore":
                    known.add(relative); categories.add("gitignore")
                elif parts[-1] == ".gitattributes":
                    known.add(relative); categories.add("gitattributes")
                elif relative == ".cargo-ok":
                    known.add(relative); categories.add("cargo-marker")
            error = ArchiveError("update_source_dependency_files_differ")
            if re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.+-]{0,192}", directory):
                error.sourceComponent = directory
            error.sourceDifference = {"missingCount": len(missing), "extraCount": len(extra),
                "changedCount": sum(declared[name] != expected_files[name] for name in set(declared) & set(expected_files)),
                "knownCargoFilteredCount": len(known), "otherMissingCount": len(missing - known),
                "metadataCategories": sorted(categories)}
            raise error
        for path, row in members.items():
            actual = generated.get(prefix + path)
            if (actual is None or actual["kind"] != "cargo-vendor" or actual["size"] != row["size"]
                    or actual["sha256"] != row["sha256"]):
                _fail("update_source_dependency_files_differ")
            checked.add(prefix + path)
        crate_package = _toml(_source_content(payload, prefix + "Cargo.toml", hashes)).get("package")
        if not isinstance(crate_package, dict) or crate_package.get("name") != name or crate_package.get("version") != version:
            _fail("update_source_dependency_metadata_invalid")
        checked.update({archive_name, prefix + ".cargo-checksum.json"})
    if checked != set(generated):
        _fail("update_source_dependency_files_differ")
    return {"generated": checked, "registryPackageCount": len(locked), "dependencyArchiveCount": len(locked)}


def _verify_git_source(metadata: dict, captured: dict, hashes: dict, expected_tree, payload) -> dict:
    """Bind archived source to independent Git blobs and locked raw crate bytes."""
    provenance = _json(captured["source-provenance.json"])
    if (not isinstance(provenance, dict) or set(provenance) != {
            "schema", "sourceCommit", "sourceTree", "trackedFiles", "generatedDependencies"}
            or provenance["schema"] != "gsw.native_update_source_provenance/1"
            or provenance["sourceCommit"] != metadata["sourceCommit"]
            or provenance["sourceTree"] != metadata["sourceTree"]
            or not isinstance(provenance["trackedFiles"], list)
            or not isinstance(provenance["generatedDependencies"], list)
            or not isinstance(expected_tree, list) or not 0 < len(expected_tree) <= MAX_FILES):
        _fail("update_source_provenance_invalid")
    expected, tree_names, aliases = {}, set(), {}
    for entry in expected_tree:
        if not isinstance(entry, dict):
            _fail("update_source_tree_invalid")
        name = validate_relative_path(entry.get("path"))
        if name in tree_names or not isinstance(entry.get("sha"), str) or not SHA1.fullmatch(entry["sha"]):
            _fail("update_source_tree_invalid")
        tree_names.add(name)
        if entry.get("type") == "tree" and entry.get("mode") == "040000":
            _claim(aliases, name, "directory")
            continue
        if (entry.get("type") != "blob" or entry.get("mode") not in ("100644", "100755")
                or type(entry.get("size")) is not int or not 0 <= entry["size"] <= MAX_ARCHIVE):
            _fail("update_source_tree_unsupported_entry")
        _claim_file(aliases, name)
        expected[name] = entry
    if not expected:
        _fail("update_source_tree_invalid")
    recorded = set()
    for row in provenance["trackedFiles"]:
        if (not isinstance(row, dict) or set(row) != {"path", "blobSha", "size"}
                or not isinstance(row.get("blobSha"), str) or not SHA1.fullmatch(row["blobSha"])
                or type(row.get("size")) is not int):
            _fail("update_source_provenance_invalid")
        name = validate_relative_path(row["path"])
        entry, actual = expected.get(name), hashes.get("source/" + name)
        if (name in recorded or entry is None or actual is None or row["size"] != entry["size"]
                or row["blobSha"] != entry["sha"] or actual["size"] != entry["size"]
                or actual["blobSha"] != entry["sha"]):
            _fail("update_source_git_blob_differs")
        recorded.add(name)
    if recorded != set(expected):
        _fail("update_source_tree_file_set_differs")
    dependency = {"generated": set(), "registryPackageCount": 0, "dependencyArchiveCount": 0}
    if provenance["generatedDependencies"] or "native/updater-trust/Cargo.lock" in recorded:
        dependency = _verify_dependencies(payload, hashes, recorded, provenance["generatedDependencies"])
    actual_source = {name[7:] for name in hashes if name.startswith("source/")}
    if actual_source != recorded | dependency["generated"]:
        _fail("update_source_tree_file_set_differs")
    return {"schema": "gsw.native_update_source_binding/1",
        "sourceCommit": metadata["sourceCommit"], "sourceTree": metadata["sourceTree"],
        "trackedFileCount": len(recorded), "generatedFileCount": len(dependency["generated"]),
        "registryPackageCount": dependency["registryPackageCount"], "dependencyArchiveCount": dependency["dependencyArchiveCount"],
        "sourceManifestSha256": hashes["source-manifest.json"]["sha256"],
        "sourceProvenanceSha256": hashes["source-provenance.json"]["sha256"]}


def verify_release_archive(archive: Path, report: dict, expected_tree) -> dict:
    """Read back the actual immutable archive without extracting or executing it.

    The caller separately authenticates the GitHub run/artifact and approved
    policy. This seam verifies its exact inner payload and independently read
    Git tree; it is not a signing or public-promotion capability.
    """
    if (not isinstance(report, dict) or report.get("schema") != "gsw.native_update_package/1"
            or report.get("product") not in (PRODUCT, FIXTURE_PRODUCT)
            or report.get("platform") != "windows-x64"
            or any(not isinstance(report.get(key), str) or not SHA1.fullmatch(report[key])
                   for key in ("sourceCommit", "sourceTree"))
            or not isinstance(report.get("buildId"), str) or not BUILD_ID.fullmatch(report["buildId"])
            or any(not isinstance(report.get(key), str) or not SHA256.fullmatch(report[key])
                   for key in ("archiveSha256", "payloadManifestSha256", "correspondingSourceSha256"))
            or type(report.get("archiveLength")) is not int or not 0 < report["archiveLength"] <= MAX_ARCHIVE
            or type(report.get("unpackedLength")) is not int or not 0 < report["unpackedLength"] <= MAX_UNPACKED
            or type(report.get("fileCount")) is not int or not 0 < report["fileCount"] <= MAX_FILES):
        _fail("update_archive_report_invalid")
    validate_relative_path(report["buildId"])
    info = _ordinary(archive.absolute())
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size != report["archiveLength"]:
        _fail("update_archive_identity_differs")
    captured, hashes = {}, {}
    capture_names = {"source-manifest.json", "source-provenance.json", "build_manifest.json"}
    with archive.open("rb") as held:
        opened = os.fstat(held.fileno())
        if ((opened.st_dev, opened.st_ino, opened.st_size) != (info.st_dev, info.st_ino, info.st_size)
                or opened.st_nlink != 1 or hashlib.file_digest(held, "sha256").hexdigest() != report["archiveSha256"]):
            _fail("update_archive_identity_differs")
        held.seek(0)
        with zipfile.ZipFile(held) as payload:
            entries, aliases, actual, total = payload.infolist(), {}, {}, 0
            if len(entries) != report["fileCount"] + 1:
                _fail("update_archive_file_set_differs")
            for entry in entries:
                name = validate_relative_path(entry.orig_filename)
                _claim_file(aliases, name)
                if not entry.flag_bits & 0x800 and not name.isascii():
                    _fail("update_archive_path_encoding_invalid")
                kind = stat.S_IFMT(entry.external_attr >> 16)
                if (entry.filename != name or name in actual or entry.is_dir() or entry.flag_bits & 1
                        or kind not in (0, stat.S_IFREG) or entry.external_attr & 0x10
                        or entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)):
                    _fail("update_archive_type_or_file_set_invalid")
                if (not 0 <= entry.file_size <= MAX_ARCHIVE or not 0 <= entry.compress_size <= MAX_ARCHIVE
                        or total + entry.file_size > report["unpackedLength"]):
                    _fail("update_archive_resource_limit")
                total += entry.file_size
                actual[name] = entry
            if total != report["unpackedLength"] or "release-files.json" not in actual:
                _fail("update_archive_file_set_differs")
            manifest_entry = actual["release-files.json"]
            if manifest_entry.file_size > MAX_MANIFEST:
                _fail("update_manifest_too_large")
            manifest_bytes = payload.read(manifest_entry)
            if hashlib.sha256(manifest_bytes).hexdigest() != report["payloadManifestSha256"]:
                _fail("update_archive_manifest_differs")
            manifest = _json(manifest_bytes)
            if (not isinstance(manifest, dict) or set(manifest) != {"schema", "product", "platform", "buildId", "sourceCommit", "sourceTree", "files"}
                    or manifest["schema"] != "gsw.native_update_files/1"
                    or any(manifest[key] != report[key] for key in ("product", "platform", "buildId", "sourceCommit", "sourceTree"))):
                _fail("update_archive_manifest_invalid")
            expected = _manifest_rows(manifest["files"])
            if (len(expected) != report["fileCount"] or "release-files.json" in expected
                    or set(actual) != set(expected) | {"release-files.json"}
                    or not (REQUIRED | {"source-provenance.json"}) <= expected.keys()):
                _fail("update_archive_file_set_differs")
            for name, row in expected.items():
                entry = actual[name]
                if entry.file_size != row["size"]:
                    _fail("update_archive_file_set_differs")
                checksum = hashlib.sha256()
                blob = hashlib.sha1(f"blob {entry.file_size}\0".encode("ascii"))
                content, received = bytearray(), 0
                with payload.open(entry) as stream:
                    while block := stream.read(min(1024**2, entry.file_size - received + 1)):
                        received += len(block)
                        if received > entry.file_size:
                            _fail("update_archive_resource_limit")
                        checksum.update(block); blob.update(block)
                        if name in capture_names:
                            if received > MAX_MANIFEST:
                                _fail("update_manifest_too_large")
                            content.extend(block)
                if received != row["size"] or checksum.hexdigest() != row["sha256"]:
                    _fail("update_archive_file_digest_differs")
                hashes[name] = {"size": received, "sha256": checksum.hexdigest(), "blobSha": blob.hexdigest()}
                if name in capture_names:
                    captured[name] = bytes(content)
        if hashes["source-manifest.json"]["sha256"] != report["correspondingSourceSha256"]:
            _fail("update_package_source_identity_invalid")
        source = _json(captured["source-manifest.json"])
        if (not isinstance(source, dict) or set(source) != {"schema", "executableSha256", "license", "sourceFileCount", "files"}
                or source["schema"] != "gsw-native-corresponding-source-v1" or source["license"] != "GPL-3.0-or-later"
                or source["executableSha256"] != hashes["bin/Gaussian Scene Workbench.exe"]["sha256"]
                or type(source["sourceFileCount"]) is not int):
            _fail("update_package_source_identity_invalid")
        source_rows = _manifest_rows(source["files"])
        if (len(source_rows) != source["sourceFileCount"]
                or set(source_rows) != {name[7:] for name in hashes if name.startswith("source/")}
                or any(row["size"] != hashes["source/" + name]["size"]
                       or row["sha256"] != hashes["source/" + name]["sha256"] for name, row in source_rows.items())):
            _fail("update_package_source_files_differ")
        build = _json(captured["build_manifest.json"])
        if (not isinstance(build, dict) or build.get("product") != "Gaussian Scene Workbench"
                or build.get("legacyWebRuntimeIncluded") is not False):
            _fail("update_package_product_invalid")
        with zipfile.ZipFile(held) as payload:
            evidence = _verify_git_source(report, captured, hashes, expected_tree, payload)
        held.seek(0)
        if hashlib.file_digest(held, "sha256").hexdigest() != report["archiveSha256"]:
            _fail("update_archive_identity_differs")
    return evidence


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
