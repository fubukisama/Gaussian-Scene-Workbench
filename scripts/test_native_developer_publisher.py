"""Remote Windows contract for the separate unsigned developer publisher CLI.

This is deliberately not a product/signature acceptance test. It creates only
owned, non-executable payload fixtures and never starts a GSW application.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import zipfile


PRODUCT = "Fubukisama.GaussianSceneWorkbench.UpdateFixture"
APP = "bin/Gaussian Scene Workbench.exe"
LAUNCHER = "bin/Gaussian Scene Workbench Launcher.exe"


def sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def encoded(value: dict) -> bytes:
    return json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("utf-8")


def write_json(path: Path, value: dict) -> None:
    path.write_bytes(encoded(value))


def tree_bytes(root: Path) -> dict[str, bytes]:
    return {path.relative_to(root).as_posix(): path.read_bytes()
            for path in root.rglob("*") if path.is_file()}


def make_package(base: Path, name: str, *, mutate=None) -> tuple[Path, Path, dict[str, bytes]]:
    """Independent, complete ZIP fixture, including corresponding hidden source."""
    payload = {
        APP: ("not executable: owned developer fixture " + name).encode(),
        LAUNCHER: b"not executable: immutable bootstrap payload",
        "bin/runtime-sentinel.dll": b"owned per-version runtime",
        "source/.github/workflows/ci.yml": b"name: fixture source kept\n",
        "source/native/CMakeLists.txt": b"# owned corresponding source fixture\n",
        "source/native/src/Unicode_日本語_中文.cpp": b"// owned unicode source\n",
        "COPYING": b"GPL-3.0-or-later fixture\n",
        "LICENSE": b"GPL-3.0-or-later fixture\n",
        "THIRD_PARTY_LICENSES.md": b"Owned fixture only\n",
        "native/LICENSE.md": b"GPL-3.0-or-later fixture\n",
        "build_manifest.json": encoded({"product": "Gaussian Scene Workbench",
                                         "legacyWebRuntimeIncluded": False}),
        "backend_manifest.json": encoded({"schema": "owned-fixture/1"}),
    }
    source = {"schema": "gsw-native-corresponding-source-v1", "license": "GPL-3.0-or-later",
              "executableSha256": sha(payload[APP]), "sourceFileCount": 3,
              "files": [{"path": path[7:], "size": len(raw), "sha256": sha(raw)}
                        for path, raw in sorted(payload.items()) if path.startswith("source/")]}
    payload["source-manifest.json"] = encoded(source)
    if mutate:
        mutate(payload)
    manifest = {"schema": "gsw.native_update_files/1", "product": PRODUCT,
                "platform": "windows-x64", "buildId": name, "sourceCommit": "b" * 40,
                "sourceTree": "c" * 40,
                "files": [{"path": path, "size": len(raw), "sha256": sha(raw)}
                          for path, raw in sorted(payload.items())]}
    manifest_bytes = encoded(manifest)
    archive = base / (name + ".zip")
    with archive.open("xb") as output:
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as package:
            for path, raw in list(payload.items()) + [("release-files.json", manifest_bytes)]:
                entry = zipfile.ZipInfo(path, date_time=(1980, 1, 1, 0, 0, 0))
                entry.create_system = 3
                entry.external_attr = (stat.S_IFREG | 0o644) << 16
                entry.compress_type = zipfile.ZIP_DEFLATED
                package.writestr(entry, raw)
    offer = {"schema": "gsw.update_offer/1", "product": PRODUCT, "platform": "windows-x64",
             "channel": "stable", "displayVersion": name, "buildId": name,
             "releaseSequence": 2, "metadataRevision": 2, "sourceCommit": "b" * 40,
             "sourceTree": "c" * 40, "archiveUrl": "https://example.invalid/not-used.zip",
             "archiveLength": archive.stat().st_size, "archiveSha256": sha(archive.read_bytes()),
             "payloadManifestSha256": sha(manifest_bytes),
             "correspondingSourceSha256": sha(payload["source-manifest.json"]),
             "unpackedLength": len(manifest_bytes) + sum(map(len, payload.values())),
             "fileCount": len(payload), "minimumBootstrapProtocol": 2}
    offer_path = base / (name + ".json")
    write_json(offer_path, offer)
    return archive, offer_path, payload | {"release-files.json": manifest_bytes}


def prepare_root(base: Path, name: str, protocol: int = 1) -> Path:
    root = base / name
    (root / "bin").mkdir(parents=True)
    (root / LAUNCHER).write_bytes(b"not executable: existing immutable launcher")
    (root / "bin/bootstrap-runtime.dll").write_bytes(b"existing immutable runtime")
    rows = [{"path": path.relative_to(root).as_posix(), "size": path.stat().st_size,
             "sha256": sha(path.read_bytes())} for path in sorted((root / "bin").iterdir())]
    write_json(root / "bootstrap.json", {"schema": "gsw.native_bootstrap/1",
                                          "protocol": protocol, "files": rows})
    (root / "versions/legacy-A/bin").mkdir(parents=True)
    original = root / "versions/legacy-A" / APP
    original.write_bytes(b"original in-use legacy A")
    write_json(root / "current.json", {"schema": "gsw.native_active_version/1",
                                        "version": "legacy-A", "executableSha256": sha(original.read_bytes())})
    (root / "user-data").mkdir()
    (root / "user-data/keep.gsw").write_bytes(b"untouched original user data sentinel")
    return root


@contextmanager
def held_read(path: Path):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                  ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "owned fixture read lease failed")
    try:
        yield
    finally:
        kernel.CloseHandle(handle)


class Contract:
    def __init__(self, publisher: Path, evidence: Path):
        self.publisher, self.evidence, self.calls = publisher, evidence, []

    def call(self, *arguments: str | Path, code: str | None = None, ok=False) -> dict:
        result = subprocess.run([str(self.publisher), *map(str, arguments)], capture_output=True,
                                timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        text = result.stdout.decode("utf-8", errors="strict")
        lines = text.splitlines()
        assert len(lines) == 1, (result.returncode, text, result.stderr.decode("utf-8", "replace"))
        message = json.loads(lines[0])
        assert message.get("schema") == "gsw.native_developer_publication/1", message
        assert message.get("ok") is ok and result.returncode == (0 if ok else 2), message
        assert message.get("publisherAuthenticated") is False, message
        assert message.get("admission") == "local-developer-unsigned", message
        if code:
            assert message.get("code") == code, message
        self.calls.append({"arguments": list(map(str, arguments)), "exitCode": result.returncode,
                           "response": message, "stderr": result.stderr.decode("utf-8", "replace")})
        write_json(self.evidence / "calls.json", {"schema": "gsw.developer_cli_calls/1", "calls": self.calls})
        return message

    def publish(self, root: Path, archive: Path, offer: Path, **expected) -> dict:
        return self.call("--publish-unsigned-local", root, archive, offer, **expected)


def run_contract(publisher: Path, base: Path, evidence: Path) -> None:
    contract = Contract(publisher, evidence)
    archive, offer, expected_payload = make_package(base, "developer-B")
    root = prepare_root(base, "managed 日本語 中文")
    original = tree_bytes(root / "versions/legacy-A")
    bootstrap = tree_bytes(root / "bin") | {"bootstrap.json": (root / "bootstrap.json").read_bytes()}
    with held_read(root / "versions/legacy-A" / APP):
        message = contract.publish(root, archive, offer, ok=True, code="ACTIVATED")
        assert message["buildId"] == "developer-B"
        assert tree_bytes(root / "versions/developer-B") == expected_payload
        pointer = root / "current.json"
        assert json.loads(pointer.read_bytes())["version"] == "developer-B"
        first_pointer, first_stat = pointer.read_bytes(), pointer.stat()
        contract.publish(root, archive, offer, ok=True, code="ACTIVATED")
        assert pointer.read_bytes() == first_pointer and pointer.stat().st_mtime_ns == first_stat.st_mtime_ns
        assert tree_bytes(root / "versions/legacy-A") == original
        assert tree_bytes(root / "bin") | {"bootstrap.json": (root / "bootstrap.json").read_bytes()} == bootstrap
        assert (root / "user-data/keep.gsw").read_bytes() == b"untouched original user data sentinel"

        # A bad archive may not switch an already-active healthy pointer.
        corrupt = base / "corrupt.zip"
        corrupt.write_bytes(archive.read_bytes() + b"corrupt after descriptor creation")
        contract.publish(root, corrupt, offer, code="activation_archive_digest_mismatch")
        assert pointer.read_bytes() == first_pointer

        # A complete but license-incomplete ZIP is not publishable.
        bad_archive, bad_offer, _ = make_package(base, "missing-license",
            mutate=lambda payload: payload.pop("native/LICENSE.md"))
        contract.publish(root, bad_archive, bad_offer, code="activation_required_file_missing")
        assert pointer.read_bytes() == first_pointer

        # Source bytes with a correctly hashed ZIP but stale source ledger fail.
        bad_archive, bad_offer, _ = make_package(base, "changed-source",
            mutate=lambda payload: payload.__setitem__("source/native/CMakeLists.txt", b"changed source\n"))
        contract.publish(root, bad_archive, bad_offer, code="activation_source_files_differ")
        assert pointer.read_bytes() == first_pointer

        # Recovery must not depend on parsing the corrupt launch pointer.
        pointer.write_bytes(b"{malformed pointer\n")
        contract.publish(root, archive, offer, ok=True, code="ACTIVATED")
        assert pointer.read_bytes() == first_pointer
        assert any(path.read_bytes() == b"{malformed pointer\n"
                   for path in (root / "updates/pointer-backups").glob("*.json"))
        assert tree_bytes(root / "versions/legacy-A") == original

    # The development adapter must not enter any public trust installation,
    # even when the marker is malformed or is a directory instead of a file.
    for index, (protocol, relative, marker_kind) in enumerate((
            (2, ".update-trust-anchor", "valid"), (2, ".update-trust-anchor", "corrupt"),
            (2, ".update-trust-anchor", "directory"), (1, ".update-trust-anchor", "corrupt"),
            (1, "updates/trust", "directory"), (2, "updates/trust", "corrupt"))):
        trusted = prepare_root(base, "trusted-" + str(index), protocol=protocol)
        marker = trusted / relative
        marker.parent.mkdir(exist_ok=True, parents=True)
        if marker_kind == "directory":
            marker.mkdir()
        else:
            marker.write_bytes(b"{}" if marker_kind == "valid" else b"corrupt")
        before = tree_bytes(trusted)
        contract.publish(trusted, archive, offer, code="developer_trusted_root_forbidden")
        assert tree_bytes(trusted) == before

    invalid_bootstrap = prepare_root(base, "invalid-legacy-bootstrap")
    (invalid_bootstrap / LAUNCHER).write_bytes(b"changed after bootstrap receipt")
    before_pointer = (invalid_bootstrap / "current.json").read_bytes()
    contract.publish(invalid_bootstrap, archive, offer, code="activation_bootstrap_digest_mismatch")
    assert (invalid_bootstrap / "current.json").read_bytes() == before_pointer

    # No generic operation or unacknowledged admission mode exists.
    contract.call("--publish", root, archive, offer, code="INVALID_REQUEST")
    contract.call("--publish-unsigned-local", root, archive, code="INVALID_REQUEST")
    write_json(evidence / "acceptance.json", {"schema": "gsw.developer_publisher_contract/1",
        "accepted": True, "scope": "isolated-remote-cli-fixtures", "production": False,
        "publisherAuthenticated": False, "formalGswAcceptance": False,
        "contracts": ["complete-zip", "corresponding-hidden-source", "licenses-required",
            "verified-protocol1-developer-only", "immutable-in-use-original", "repeat-publication",
            "bad-package-pointer-preserved", "corrupt-pointer-recovery", "trusted-root-refused",
            "bootstrap-integrity", "explicit-unsigned-admission"]})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-fixture-only", action="store_true", required=True)
    parser.add_argument("--publisher", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    arguments = parser.parse_args()
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_OS") != "Windows":
        parser.error("This contract runs only on an isolated Windows GitHub Actions runner.")
    if arguments.work_root.exists() or arguments.work_root.is_symlink():
        parser.error("Use a fresh owned work root; existing files are preserved.")
    arguments.work_root.mkdir(parents=True)
    arguments.evidence.mkdir(parents=True, exist_ok=True)
    run_contract(arguments.publisher.absolute(), arguments.work_root.absolute(), arguments.evidence.absolute())
    print("Native unsigned developer CLI contract PASS (isolated fixture scope only).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
