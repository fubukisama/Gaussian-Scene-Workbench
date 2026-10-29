"""Package/verify/merge a default-disabled native update engine for research preview.

Developer tooling only. Never starts a helper/application, installs into an
existing root, configures a trust profile, signs, or publishes.
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
import struct
import subprocess
import sys
import zipfile

SPEC = importlib.util.spec_from_file_location("gsw_engine_archive", Path(__file__).with_name("package_native_update.py"))
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)
HELPER = "bin/Gaussian Scene Workbench Update Helper.exe"
APP = "bin/Gaussian Scene Workbench.exe"
SOURCE = "native-update-engine-source/"
MANIFEST = "native-update-engine-component.json"
BUILD = "native-update-engine-build.json"
SOURCE_MANIFEST = "native-update-engine-source-manifest.json"
PROVENANCE = "native-update-engine-source-provenance.json"
CRT = "native-update-engine-crt.json"
README = "native-update-engine-README.txt"
LICENSES = ("COPYING", "LICENSE", "THIRD_PARTY_LICENSES.md", "native/LICENSE.md")
PROFILE_ENV = ("GSW_UPDATER_FIXTURE_PROFILE_PATH", "GSW_UPDATER_PRODUCTION_PROFILE_PATH")
META_KEYS = {"schema", "product", "platform", "sourceCommit", "sourceTree", "configurationStatus",
    "publicDistributionReady", "publisherAuthenticated", "fixtureBuild", "minimumBootstrapProtocol",
    "helperPath", "helperSha256", "trustLibrarySha256", "buildReceiptSha256", "crtReceiptSha256",
    "sourceManifestSha256", "sourceProvenanceSha256", "files"}
DELIVERY_KEYS = {"archiveLength", "archiveSha256", "payloadManifestSha256"}


class EngineError(ValueError):
    def __init__(self, code):
        self.code = code
        super().__init__(code)


def require(condition, code):
    if not condition:
        raise EngineError(code)


def fs(path: Path) -> Path:
    """Internal W-API spelling; CLI identities remain ordinary local paths."""
    value = str(path)
    if os.name == "nt" and path.is_absolute() and not value.startswith("\\\\?\\"):
        return Path("\\\\?\\" + value)
    return path


def local(path: Path) -> Path:
    require(path.is_absolute() and ".." not in path.parts and not str(path).startswith("\\\\"), "ENGINE_LOCAL_PATH_REQUIRED")
    return path.absolute()


def ordinary(path: Path):
    return PACKAGE._ordinary(fs(local(path)))


def fresh(path: Path, *, directory=False):
    path = local(path)
    require(not fs(path).exists() and not fs(path).is_symlink(), "ENGINE_OUTPUT_NOT_FRESH")
    if os.name == "nt" and Path("D:/").is_dir():
        require(path.drive.casefold() == "d:", "ENGINE_OUTPUT_REQUIRES_D_DRIVE")
    ancestor = path.parent
    while not fs(ancestor).exists():
        ancestor = ancestor.parent
    ordinary(ancestor)
    if directory:
        fs(path).mkdir(parents=True, exist_ok=False)
        ordinary(path)


def raw(path: Path, limit=PACKAGE.MAX_MANIFEST) -> bytes:
    info = ordinary(path)
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size <= limit, "ENGINE_INPUT_FILE_INVALID")
    return fs(path).read_bytes()


def decoded(path: Path):
    return PACKAGE._json(raw(path))


def encoded(value) -> bytes:
    return (json.dumps(value, ensure_ascii=True, allow_nan=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def write(path: Path, data: bytes):
    fs(path.parent).mkdir(parents=True, exist_ok=True)
    with fs(path).open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def digest(path: Path) -> str:
    with fs(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inventory(root: Path) -> dict:
    require(stat.S_ISDIR(ordinary(root).st_mode), "ENGINE_INPUT_DIRECTORY_INVALID")
    result, aliases, total = {}, {}, 0
    for parent, directories, names in os.walk(fs(root), followlinks=False):
        owner = Path(parent)
        for name in directories + names:
            info = PACKAGE._ordinary(owner / name)
            require(name != ".git" and (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)), "ENGINE_INPUT_FILE_INVALID")
        for name in names:
            path = owner / name
            info = path.stat()
            relative = path.relative_to(fs(root)).as_posix()
            PACKAGE._claim_file(aliases, relative)
            require(info.st_nlink == 1 and 0 <= info.st_size <= PACKAGE.MAX_ARCHIVE, "ENGINE_INPUT_FILE_INVALID")
            total += info.st_size
            require(len(result) < PACKAGE.MAX_FILES and total <= PACKAGE.MAX_UNPACKED, "ENGINE_RESOURCE_LIMIT")
            result[relative] = {"path": relative, "size": info.st_size, "sha256": digest(path)}
    return result


def copy(source: Path, destination: Path, row=None):
    info = ordinary(source)
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "ENGINE_INPUT_FILE_INVALID")
    fs(destination.parent).mkdir(parents=True, exist_ok=True)
    checksum, received = hashlib.sha256(), 0
    with fs(source).open("rb") as original, fs(destination).open("xb") as output:
        opened = os.fstat(original.fileno())
        require((opened.st_dev, opened.st_ino, opened.st_size) == (info.st_dev, info.st_ino, info.st_size), "ENGINE_INPUT_CHANGED")
        while block := original.read(1024**2):
            received += len(block)
            require(received <= info.st_size, "ENGINE_INPUT_CHANGED")
            checksum.update(block)
            output.write(block)
    require(received == info.st_size, "ENGINE_INPUT_CHANGED")
    if row is not None:
        require(received == row["size"] and checksum.hexdigest() == row["sha256"], "ENGINE_INPUT_CHANGED")


def command(arguments, *, env=None, timeout=120):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, env=env, timeout=timeout)
    require(result.returncode == 0 and len(result.stdout) <= PACKAGE.MAX_MANIFEST, "ENGINE_TOOL_COMMAND_FAILED")
    return result.stdout


def git_tree(repository: Path, commit: str, tree: str):
    ordinary(repository)
    require(PACKAGE.SHA1.fullmatch(commit) and PACKAGE.SHA1.fullmatch(tree), "ENGINE_SOURCE_IDENTITY_INVALID")
    actual = command(["git", "-C", repository, "rev-parse", commit + "^{tree}"]).decode("ascii").strip()
    require(actual == tree, "ENGINE_SOURCE_IDENTITY_INVALID")
    expected = []
    for item in command(["git", "-C", repository, "ls-tree", "-r", "-l", "-z", "--full-tree", commit]).split(b"\0"):
        if item:
            header, path = item.split(b"\t", 1)
            mode, kind, blob, size = header.decode("ascii").split()
            expected.append({"path": path.decode("utf-8"), "mode": mode, "type": kind, "sha": blob, "size": int(size)})
    return expected


def pe_x64(path: Path):
    with fs(path).open("rb") as stream:
        header = stream.read(64)
        require(len(header) == 64 and header[:2] == b"MZ", "ENGINE_RUNTIME_PE_INVALID")
        offset = struct.unpack_from("<I", header, 60)[0]
        require(offset <= ordinary(path).st_size - 6, "ENGINE_RUNTIME_PE_INVALID")
        stream.seek(offset)
        require(stream.read(6) == b"PE\0\0d\x86", "ENGINE_RUNTIME_PE_INVALID")


def runtime_contract(rows: dict):
    folded = {name.casefold(): row for name, row in rows.items()}
    required = {HELPER.casefold(), "bin/qt6core.dll", "bin/qt6network.dll", "bin/tls/qschannelbackend.dll",
        "bin/msvcp140.dll", "bin/msvcp140_1.dll", "bin/msvcp140_2.dll", "bin/vcruntime140.dll", "bin/vcruntime140_1.dll"}
    require(required <= folded.keys(), "ENGINE_RUNTIME_DEPENDENCY_MISSING")
    for name in rows:
        if name.startswith("bin/") and name.casefold().endswith(".exe"):
            require(name == HELPER, "ENGINE_RUNTIME_EXECUTABLE_FORBIDDEN")


def validate_build(build: dict, metadata: dict):
    require(isinstance(build, dict) and build.get("schema") == "gsw.native_update_engine_build/1", "ENGINE_BUILD_RECEIPT_INVALID")
    for key in ("sourceCommit", "sourceTree", "helperSha256", "trustLibrarySha256"):
        require(build.get(key) == metadata.get(key), "ENGINE_BUILD_IDENTITY_DIFFERS")
    require(build.get("rustFeatures") == [] and build.get("noDefaultFeatures") is True and
        build.get("fixtureBuild") is False and build.get("fixtureProfileInputPresent") is False and
        build.get("productionProfileInputPresent") is False and build.get("configurationStatus") == "NOT_CONFIGURED" and
        build.get("publicDistributionReady") is False and build.get("publisherAuthenticated") is False,
        "ENGINE_DEFAULT_CONFIGURATION_REQUIRED")
    require(isinstance(build.get("rustc"), str) and build["rustc"].startswith("rustc 1.99.1 ") and
        isinstance(build.get("cargo"), str) and build["cargo"].startswith("cargo 1.99.1 "), "ENGINE_TOOLCHAIN_INVALID")


def validate_crt(crt, rows):
    require(isinstance(crt, dict) and crt.get("schema") == "gsw-native-crt-v1" and
        crt.get("verified") is True and crt.get("architecture") == "x64" and
        isinstance(crt.get("files"), list) and len(crt["files"]) >= 5, "ENGINE_CRT_RECEIPT_INVALID")
    folded = {name.casefold(): row for name, row in rows.items()}
    for row in crt["files"]:
        require(isinstance(row, dict) and isinstance(row.get("path"), str), "ENGINE_CRT_RECEIPT_INVALID")
        actual = folded.get(row["path"].casefold())
        require(row["path"].startswith("bin/") and actual is not None and actual["sha256"] == row.get("sha256"),
            "ENGINE_CRT_RECEIPT_INVALID")


class SourceZip:
    """Read-only prefix adapter to the existing Git/raw-crate source verifier."""
    def __init__(self, archive):
        self.archive = archive

    @staticmethod
    def name(value):
        return SOURCE + value[7:] if isinstance(value, str) and value.startswith("source/") else value

    def read(self, name):
        return self.archive.read(self.name(name))

    def getinfo(self, name):
        return self.archive.getinfo(self.name(name))

    def open(self, name, *args, **kwargs):
        # The raw-crate verifier passes ZipInfo here after getinfo(); preserve it.
        return self.archive.open(self.name(name), *args, **kwargs)


def verify_component(repository: Path, archive: Path, metadata: dict):
    require(isinstance(metadata, dict) and set(metadata) == META_KEYS | DELIVERY_KEYS and
        metadata.get("schema") == "gsw.native_update_engine_component/1" and metadata.get("product") == PACKAGE.PRODUCT and
        metadata.get("platform") == "windows-x64" and metadata.get("helperPath") == HELPER and
        metadata.get("configurationStatus") == "NOT_CONFIGURED" and metadata.get("fixtureBuild") is False and
        metadata.get("minimumBootstrapProtocol") == 2 and metadata.get("publicDistributionReady") is False and
        metadata.get("publisherAuthenticated") is False, "ENGINE_COMPONENT_MANIFEST_INVALID")
    for key in ("helperSha256", "trustLibrarySha256", "buildReceiptSha256", "crtReceiptSha256", "sourceManifestSha256",
                "sourceProvenanceSha256", "archiveSha256", "payloadManifestSha256"):
        require(isinstance(metadata.get(key), str) and PACKAGE.SHA256.fullmatch(metadata[key]), "ENGINE_COMPONENT_MANIFEST_INVALID")
    length = metadata.get("archiveLength")
    require(type(length) is int and 0 < length <= PACKAGE.MAX_ARCHIVE, "ENGINE_COMPONENT_MANIFEST_INVALID")
    info = ordinary(archive)
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size == length and
        digest(archive) == metadata["archiveSha256"], "ENGINE_ARCHIVE_DIGEST_MISMATCH")
    expected = PACKAGE._manifest_rows(metadata["files"])
    require(MANIFEST not in expected, "ENGINE_COMPONENT_MANIFEST_INVALID")
    allowed_metadata = {BUILD, CRT, SOURCE_MANIFEST, PROVENANCE, README} | {
        "licenses/native-update-engine/" + name for name in LICENSES}
    require(all(name.startswith(("bin/", SOURCE)) or name in allowed_metadata for name in expected),
        "ENGINE_COMPONENT_FILE_FORBIDDEN")
    internal = {key: value for key, value in metadata.items() if key not in DELIVERY_KEYS}
    hashes, captured, aliases, seen, total = {}, {}, {}, set(), 0
    capture = {BUILD, CRT, SOURCE_MANIFEST, PROVENANCE}
    with fs(archive).open("rb") as held, zipfile.ZipFile(held) as payload:
        require(len(payload.infolist()) == len(expected) + 1, "ENGINE_ARCHIVE_FILE_SET_DIFFERS")
        for entry in payload.infolist():
            name = PACKAGE.validate_relative_path(entry.filename)
            PACKAGE._claim_file(aliases, name)
            kind = (entry.external_attr >> 16) & 0o170000
            require(name not in seen and not entry.is_dir() and not entry.flag_bits & 1 and
                kind in (0, stat.S_IFREG) and not entry.external_attr & 0x10 and
                entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED), "ENGINE_ARCHIVE_ENTRY_INVALID")
            seen.add(name)
            total += entry.file_size
            require(0 <= entry.file_size <= PACKAGE.MAX_ARCHIVE and total <= PACKAGE.MAX_UNPACKED, "ENGINE_RESOURCE_LIMIT")
            if name == MANIFEST:
                require(entry.file_size <= PACKAGE.MAX_MANIFEST, "ENGINE_RESOURCE_LIMIT")
                content = payload.read(entry)
                require(hashlib.sha256(content).hexdigest() == metadata["payloadManifestSha256"] and
                    PACKAGE._json(content) == internal, "ENGINE_COMPONENT_MANIFEST_INVALID")
                continue
            row = expected.get(name)
            require(row is not None and row["size"] == entry.file_size, "ENGINE_ARCHIVE_FILE_SET_DIFFERS")
            checksum, blob, received, content = hashlib.sha256(), hashlib.sha1(f"blob {entry.file_size}\0".encode("ascii")), 0, bytearray()
            with payload.open(entry) as stream:
                while block := stream.read(min(1024**2, entry.file_size - received + 1)):
                    received += len(block)
                    require(received <= entry.file_size, "ENGINE_RESOURCE_LIMIT")
                    checksum.update(block)
                    blob.update(block)
                    if name in capture:
                        require(received <= PACKAGE.MAX_MANIFEST, "ENGINE_RESOURCE_LIMIT")
                        content.extend(block)
            require(received == row["size"] and checksum.hexdigest() == row["sha256"], "ENGINE_ARCHIVE_FILE_DIGEST_DIFFERS")
            hashes[name] = {"size": received, "sha256": checksum.hexdigest(), "blobSha": blob.hexdigest()}
            if name in capture:
                captured[name] = bytes(content)
        require(seen == set(expected) | {MANIFEST}, "ENGINE_ARCHIVE_FILE_SET_DIFFERS")
        runtime_contract(hashes)
        require(hashes[HELPER]["sha256"] == metadata["helperSha256"], "ENGINE_BUILD_IDENTITY_DIFFERS")
        for name, field in ((BUILD, "buildReceiptSha256"), (CRT, "crtReceiptSha256"),
                            (SOURCE_MANIFEST, "sourceManifestSha256"), (PROVENANCE, "sourceProvenanceSha256")):
            require(name in hashes and hashes[name]["sha256"] == metadata[field], "ENGINE_COMPONENT_MANIFEST_INVALID")
        build = PACKAGE._json(captured[BUILD])
        validate_build(build, metadata)
        require(build.get("cargoFingerprintFeatures") == [] and build.get("cxxCompilerId") == "MSVC" and
            isinstance(build.get("cxxCompilerVersion"), str), "ENGINE_TOOLCHAIN_INVALID")
        validate_crt(PACKAGE._json(captured[CRT]), hashes)
        source = PACKAGE._json(captured[SOURCE_MANIFEST])
        require(isinstance(source, dict) and set(source) == {"schema", "license", "executableSha256", "sourceFileCount", "files"} and
            source["schema"] == "gsw-native-corresponding-source-v1" and source["license"] == "GPL-3.0-or-later" and
            source["executableSha256"] == metadata["helperSha256"], "ENGINE_SOURCE_LEDGER_INVALID")
        source_rows = PACKAGE._manifest_rows(source["files"])
        require(type(source["sourceFileCount"]) is int and source["sourceFileCount"] == len(source_rows) and
            set(source_rows) == {name[len(SOURCE):] for name in hashes if name.startswith(SOURCE)} and
            all(hashes[SOURCE + name]["size"] == row["size"] and hashes[SOURCE + name]["sha256"] == row["sha256"]
                for name, row in source_rows.items()), "ENGINE_SOURCE_LEDGER_INVALID")
        for name in LICENSES:
            destination = "licenses/native-update-engine/" + name
            require(destination in hashes and SOURCE + name in hashes and
                hashes[destination]["sha256"] == hashes[SOURCE + name]["sha256"], "ENGINE_LICENSE_FILES_MISSING")
        source_hashes = {"source/" + name[len(SOURCE):]: row for name, row in hashes.items() if name.startswith(SOURCE)}
        source_hashes["source-manifest.json"], source_hashes["source-provenance.json"] = hashes[SOURCE_MANIFEST], hashes[PROVENANCE]
        proof = PACKAGE._verify_git_source(metadata, {"source-provenance.json": captured[PROVENANCE]}, source_hashes,
            git_tree(repository, metadata["sourceCommit"], metadata["sourceTree"]), SourceZip(payload))
        require(digest(archive) == metadata["archiveSha256"], "ENGINE_ARCHIVE_DIGEST_MISMATCH")
    return proof


def prepare(args):
    for path in (args.runtime_root, args.repository, args.trust_library, args.cmake_cache, args.build_executable,
                 args.toolchain_receipt, args.cargo_home, args.work_root, args.output_archive, args.output_manifest):
        local(path)
    fresh(args.work_root)
    fresh(args.output_archive)
    fresh(args.output_manifest)
    require(not any(path == args.runtime_root or path.is_relative_to(args.runtime_root)
        for path in (args.work_root, args.output_archive, args.output_manifest)), "ENGINE_OUTPUT_INSIDE_INPUT")
    git_tree(args.repository, args.source_commit, args.source_tree)
    require(command(["git", "-C", args.repository, "rev-parse", "HEAD"]).decode("ascii").strip() == args.source_commit,
        "ENGINE_BUILD_SOURCE_DIFFERS")
    command(["git", "-C", args.repository, "diff", "--exit-code", args.source_commit, "--",
        "native/src", "native/updater", "native/updater-trust", "native/third_party"])
    rows = inventory(args.runtime_root)
    runtime_contract(rows)
    require(all(name.startswith("bin/") or name == CRT for name in rows), "ENGINE_RUNTIME_FILE_FORBIDDEN")
    for name in rows:
        if name.casefold().endswith((".exe", ".dll")):
            pe_x64(args.runtime_root / name)
    require(args.build_executable.name == Path(HELPER).name and digest(args.build_executable) == rows[HELPER]["sha256"], "ENGINE_BUILD_IDENTITY_DIFFERS")
    require(args.trust_library.name == "gsw_updater_trust.lib" and ordinary(args.trust_library).st_size > 0, "ENGINE_BUILD_IDENTITY_DIFFERS")
    cache_raw = raw(args.cmake_cache)
    cache = dict(match.groups() for line in cache_raw.decode("utf-8").splitlines()
                 if (match := re.match(r"^([^#/:][^:]*):[^=]+=(.*)$", line)))
    require(cache.get("GSW_UPDATER_FIXTURE_BUILD") == "OFF" and cache.get("BUILD_TESTING") == "OFF" and
        Path(cache.get("GSW_TRUST_LIBRARY", "")).absolute() == args.trust_library.absolute() and
        Path(cache.get("CMAKE_HOME_DIRECTORY", "")).absolute() == (args.repository / "native/updater").absolute(),
        "ENGINE_DEFAULT_BUILD_REQUIRED")
    fingerprints = list(fs(args.trust_library.parent / ".fingerprint").glob("gsw-updater-trust-*/lib-gsw_updater_trust.json"))
    require(len(fingerprints) == 1, "ENGINE_DEFAULT_BUILD_REQUIRED")
    fingerprint_raw = raw(args.trust_library.parent / ".fingerprint" / fingerprints[0].parent.name / fingerprints[0].name)
    features = PACKAGE._json(fingerprint_raw).get("features")
    require((json.loads(features) if isinstance(features, str) else features) == [], "ENGINE_DEFAULT_BUILD_REQUIRED")
    build = decoded(args.toolchain_receipt)
    identity = {"sourceCommit": args.source_commit, "sourceTree": args.source_tree,
        "helperSha256": rows[HELPER]["sha256"], "trustLibrarySha256": digest(args.trust_library)}
    validate_build(build, identity)
    compilers = list(fs(args.cmake_cache.parent / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    require(len(compilers) == 1, "ENGINE_TOOLCHAIN_INVALID")
    compiler_raw = raw(args.cmake_cache.parent / "CMakeFiles" / compilers[0].parent.name / compilers[0].name)
    compiler = dict(re.findall(r'set\((CMAKE_CXX_COMPILER_(?:ID|VERSION)) "([^"]+)"\)', compiler_raw.decode("utf-8")))
    require(compiler.get("CMAKE_CXX_COMPILER_ID") == "MSVC" and compiler.get("CMAKE_CXX_COMPILER_VERSION"), "ENGINE_TOOLCHAIN_INVALID")
    build = {**build, "cmakeCacheSha256": hashlib.sha256(cache_raw).hexdigest(),
        "cargoFingerprintSha256": hashlib.sha256(fingerprint_raw).hexdigest(), "cargoFingerprintFeatures": [],
        "cxxCompilerId": compiler["CMAKE_CXX_COMPILER_ID"], "cxxCompilerVersion": compiler["CMAKE_CXX_COMPILER_VERSION"],
        "cxxCompilerIdentificationSha256": hashlib.sha256(compiler_raw).hexdigest(),
        "configurationEvidence": "explicit-no-default-features-and-fixture-off-build"}
    crt = decoded(args.runtime_root / CRT)
    validate_crt(crt, rows)
    fresh(args.work_root, directory=True)
    source, meta = args.work_root / "s", args.work_root / "m"
    fs(meta).mkdir()
    write(meta / BUILD, encoded(build))
    env = dict(os.environ, CARGO_HOME=str(args.cargo_home), RUSTUP_TOOLCHAIN="1.99.1")
    for name in PROFILE_ENV:
        env.pop(name, None)
    response = PACKAGE._json(command([sys.executable, "-I", "-B", args.repository / "scripts/prepare_native_update_source.py",
        "--repository", args.repository, "--source-commit", args.source_commit, "--source-tree", args.source_tree,
        "--output-root", source, "--executable", args.build_executable, "--manifest", meta / SOURCE_MANIFEST,
        "--provenance", meta / PROVENANCE, "--cargo", args.cargo, "--cargo-home", args.cargo_home], env=env, timeout=1800))
    require(response.get("ok") is True, "ENGINE_SOURCE_PREPARATION_FAILED")
    write(meta / README, ("Native update engine: default NOT_CONFIGURED; research preview only. No publisher authentication or public installation acceptance.\n"
        "原生更新引擎：默认 NOT_CONFIGURED；仅用于研究预览，不代表发布者认证或公开安装验收。\n"
        "ネイティブ更新エンジン：既定は NOT_CONFIGURED。研究プレビュー専用で、発行元認証・公開インストール受入れを示しません。\n").encode("utf-8"))
    mapping = {name: args.runtime_root / name for name in rows}
    mapping.update({SOURCE + name: source / name for name in inventory(source)})
    mapping.update({name: meta / name for name in (BUILD, SOURCE_MANIFEST, PROVENANCE, README)})
    mapping.update({"licenses/native-update-engine/" + name: source / name for name in LICENSES})
    aliases, files = {}, []
    for name, path in sorted(mapping.items()):
        PACKAGE._claim_file(aliases, name)
        info = ordinary(path)
        files.append({"path": name, "size": info.st_size, "sha256": digest(path)})
    require(len(files) <= PACKAGE.MAX_FILES and sum(row["size"] for row in files) <= PACKAGE.MAX_UNPACKED, "ENGINE_RESOURCE_LIMIT")
    metadata = {"schema": "gsw.native_update_engine_component/1", "product": PACKAGE.PRODUCT, "platform": "windows-x64",
        **identity, "configurationStatus": "NOT_CONFIGURED", "publicDistributionReady": False,
        "publisherAuthenticated": False, "fixtureBuild": False, "minimumBootstrapProtocol": 2, "helperPath": HELPER,
        "buildReceiptSha256": digest(meta / BUILD), "crtReceiptSha256": digest(args.runtime_root / CRT),
        "sourceManifestSha256": digest(meta / SOURCE_MANIFEST), "sourceProvenanceSha256": digest(meta / PROVENANCE), "files": files}
    manifest_bytes = encoded(metadata)
    with fs(args.output_archive).open("xb") as output, zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED,
            compresslevel=6, allowZip64=True) as archive:
        for row in files + [{"path": MANIFEST}]:
            name = row["path"]
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.create_system, entry.external_attr, entry.compress_type = 3, (stat.S_IFREG | 0o644) << 16, zipfile.ZIP_DEFLATED
            with archive.open(entry, "w") as destination:
                if name == MANIFEST:
                    destination.write(manifest_bytes)
                else:
                    with fs(mapping[name]).open("rb") as original:
                        while block := original.read(1024**2):
                            destination.write(block)
            require(output.tell() <= PACKAGE.MAX_ARCHIVE, "ENGINE_RESOURCE_LIMIT")
    metadata.update(archiveLength=ordinary(args.output_archive).st_size, archiveSha256=digest(args.output_archive),
        payloadManifestSha256=hashlib.sha256(manifest_bytes).hexdigest())
    proof = verify_component(args.repository, args.output_archive, metadata)
    write(args.output_manifest, encoded(metadata))
    return {"code": "ENGINE_COMPONENT_PREPARED", "archiveSha256": metadata["archiveSha256"],
        "helperSha256": identity["helperSha256"], "sourceProof": proof}


def extract(archive, entry, destination: Path, row: dict):
    PACKAGE.validate_relative_path(entry.filename)
    require(entry.file_size == row["size"], "ENGINE_ARCHIVE_FILE_SET_DIFFERS")
    fs(destination.parent).mkdir(parents=True, exist_ok=True)
    checksum, received = hashlib.sha256(), 0
    with archive.open(entry) as source, fs(destination).open("xb") as output:
        while block := source.read(min(1024**2, row["size"] - received + 1)):
            received += len(block)
            require(received <= row["size"], "ENGINE_RESOURCE_LIMIT")
            checksum.update(block)
            output.write(block)
    require(received == row["size"] and checksum.hexdigest() == row["sha256"], "ENGINE_ARCHIVE_FILE_DIGEST_DIFFERS")


def merge(args):
    metadata = decoded(args.manifest)
    proof = verify_component(args.repository, args.archive, metadata)
    local(args.application_root)
    fresh(args.output_root)
    require(args.output_root != args.application_root and not args.output_root.is_relative_to(args.application_root),
        "ENGINE_OUTPUT_INSIDE_INPUT")
    application = inventory(args.application_root)
    identity = decoded(args.application_identity)
    require(isinstance(identity, dict) and identity.get("schema") == "gsw.native_application_artifact/1" and
        identity.get("sourceCommit") == metadata["sourceCommit"] and identity.get("sourceTree") == metadata["sourceTree"],
        "ENGINE_APPLICATION_SOURCE_DIFFERS")
    require(APP in application and identity.get("applicationSha256") == application[APP]["sha256"] and
        str(identity.get("runId", "")).isdigit() and int(identity["runId"]) > 0 and
        str(identity.get("artifactId", "")).isdigit() and int(identity["artifactId"]) > 0,
        "ENGINE_APPLICATION_IDENTITY_INVALID")
    require(not ({"current.json", "bootstrap.json", ".publish.lock", ".update-trust-anchor"} & application.keys()) and
        not any(name.startswith(("versions/", "updates/", SOURCE)) or name == SOURCE.rstrip("/") for name in application),
        "ENGINE_APPLICATION_PACKAGE_REQUIRED")
    original_manifest = raw(args.application_root / "source-manifest.json")
    ledger = PACKAGE._json(original_manifest)
    require(isinstance(ledger, dict) and set(ledger) == {"schema", "license", "executableSha256", "sourceFileCount", "files"} and
        ledger["schema"] == "gsw-native-corresponding-source-v1" and ledger["license"] == "GPL-3.0-or-later" and
        ledger["executableSha256"] == application[APP]["sha256"], "ENGINE_APPLICATION_SOURCE_LEDGER_INVALID")
    old_source = PACKAGE._manifest_rows(ledger["files"])
    require(type(ledger["sourceFileCount"]) is int and ledger["sourceFileCount"] == len(old_source) and
        set(old_source) == {name[7:] for name in application if name.startswith("source/")} and
        all(application["source/" + name]["size"] == row["size"] and application["source/" + name]["sha256"] == row["sha256"]
            for name, row in old_source.items()), "ENGINE_APPLICATION_SOURCE_LEDGER_INVALID")
    folded = {name.casefold(): row for name, row in application.items()}
    shared = []
    for row in metadata["files"]:
        name, existing = row["path"], folded.get(row["path"].casefold())
        if existing:
            require(name.startswith("bin/") and existing["size"] == row["size"] and existing["sha256"] == row["sha256"],
                "ENGINE_RUNTIME_CONFLICT" if name.startswith("bin/") else "ENGINE_COMPONENT_PATH_CONFLICT")
            shared.append(existing["path"])
    reserved = {MANIFEST, "native-update-engine-merge.json", "native-update-engine-original-application-source.json",
        "native-update-engine-original-application-provenance.json"}
    require(not {name.casefold() for name in reserved} & folded.keys(), "ENGINE_COMPONENT_PATH_CONFLICT")
    # Check aliases/file-parent conflicts across both inputs before any output.
    planned = {name: row for name, row in application.items()
        if not name.startswith("source/") and name not in ("source-manifest.json", "source-provenance.json")}
    planned.update({row["path"]: row for row in metadata["files"] if row["path"].casefold() not in folded})
    planned.update({"source/" + row["path"][len(SOURCE):]: row for row in metadata["files"] if row["path"].startswith(SOURCE)})
    aliases = {}
    for name in sorted(planned):
        PACKAGE._claim_file(aliases, name)
    require(len(planned) + len(reserved) + 2 <= PACKAGE.MAX_FILES and
        sum(row["size"] for row in planned.values()) <= PACKAGE.MAX_UNPACKED, "ENGINE_RESOURCE_LIMIT")
    fresh(args.output_root, directory=True)
    for name, row in application.items():
        if not name.startswith("source/") and name not in ("source-manifest.json", "source-provenance.json"):
            copy(args.application_root / name, args.output_root / name, row)
    expected = {row["path"]: row for row in metadata["files"]}
    expected_names = set(expected) | {MANIFEST}
    with fs(args.archive).open("rb") as held, zipfile.ZipFile(held) as archive:
        names, aliases = set(), {}
        for entry in archive.infolist():
            name = PACKAGE.validate_relative_path(entry.filename)
            PACKAGE._claim_file(aliases, name)
            require(name not in names and name in expected_names, "ENGINE_ARCHIVE_FILE_SET_DIFFERS")
            names.add(name)
            row = expected[name] if name != MANIFEST else {"size": entry.file_size, "sha256": metadata["payloadManifestSha256"]}
            require(not entry.is_dir() and not entry.flag_bits & 1 and entry.file_size <= PACKAGE.MAX_ARCHIVE,
                "ENGINE_ARCHIVE_ENTRY_INVALID")
            if name.casefold() not in folded:
                extract(archive, entry, args.output_root / name, row)
            if name.startswith(SOURCE):
                copy(args.output_root / name, args.output_root / "source" / name[len(SOURCE):], row)
        require(names == expected_names, "ENGINE_ARCHIVE_FILE_SET_DIFFERS")
        write(args.output_root / "source-provenance.json", archive.read(PROVENANCE))
    require(digest(args.archive) == metadata["archiveSha256"], "ENGINE_ARCHIVE_DIGEST_MISMATCH")
    canonical = inventory(args.output_root / "source")
    new_ledger = {"schema": "gsw-native-corresponding-source-v1", "license": "GPL-3.0-or-later",
        "executableSha256": application[APP]["sha256"], "sourceFileCount": len(canonical), "files": [canonical[name] for name in sorted(canonical)]}
    write(args.output_root / "source-manifest.json", encoded(new_ledger))
    write(args.output_root / "native-update-engine-original-application-source.json", original_manifest)
    if "source-provenance.json" in application:
        copy(args.application_root / "source-provenance.json", args.output_root / "native-update-engine-original-application-provenance.json")
    require(inventory(args.application_root) == application, "ENGINE_APPLICATION_INPUT_CHANGED")
    receipt = {"schema": "gsw.native_update_engine_merge/1", "ok": True, "sourceCommit": metadata["sourceCommit"],
        "sourceTree": metadata["sourceTree"], "applicationSha256": application[APP]["sha256"],
        "applicationIdentitySha256": digest(args.application_identity), "engineArchiveSha256": metadata["archiveSha256"],
        "configurationStatus": "NOT_CONFIGURED", "publicDistributionReady": False, "publisherAuthenticated": False,
        "applicationStarted": False, "inputApplicationPreserved": True, "canonicalSourceRebuilt": True,
        "sharedRuntimeFiles": sorted(shared), "sourceFileCount": len(canonical),
        "sourceManifestSha256": digest(args.output_root / "source-manifest.json"), "sourceProof": proof}
    write(args.output_root / "native-update-engine-merge.json", encoded(receipt))
    return {"code": "ENGINE_COMPONENT_MERGED", "mergeReceipt": receipt}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    prepare_parser = commands.add_parser("prepare")
    for name in ("runtime-root", "repository", "trust-library", "cmake-cache", "build-executable",
                 "toolchain-receipt", "cargo-home", "work-root", "output-archive", "output-manifest"):
        prepare_parser.add_argument("--" + name, type=Path, required=True)
    prepare_parser.add_argument("--source-commit", required=True)
    prepare_parser.add_argument("--source-tree", required=True)
    prepare_parser.add_argument("--cargo", default="cargo")
    for operation in ("verify", "merge"):
        target = commands.add_parser(operation)
        for name in ("repository", "archive", "manifest"):
            target.add_argument("--" + name, type=Path, required=True)
        if operation == "merge":
            for name in ("application-root", "application-identity", "output-root"):
                target.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    response = {"schema": "gsw.native_update_engine_result/1", "ok": False, "configurationStatus": "NOT_CONFIGURED",
        "publicDistributionReady": False, "publisherAuthenticated": False, "applicationStarted": False}
    try:
        if args.operation == "prepare":
            outcome = prepare(args)
        elif args.operation == "merge":
            outcome = merge(args)
        else:
            outcome = {"code": "ENGINE_COMPONENT_VERIFIED", "sourceProof": verify_component(args.repository, args.archive, decoded(args.manifest))}
        response.update(outcome, ok=True)
    except Exception as error:
        response.update(code=getattr(error, "code", "ENGINE_COMPONENT_FAILED"), errorType=type(error).__name__)
    print(json.dumps(response, ensure_ascii=True, allow_nan=False))
    return 0 if response["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
