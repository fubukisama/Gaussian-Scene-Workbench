"""Remote-only real Windows ACL, occupied-pointer and small-volume contracts.

Uses the separate unsigned developer publisher, never an online trust authority
or GSW payload. The low-space fixture is a 64 MiB VHD backed by a fresh D-drive
file; no physical disk is selected or formatted. Never run on a user device.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import csv
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time

SPEC = importlib.util.spec_from_file_location("gsw_owned_developer_fixture",
    Path(__file__).with_name("test_native_developer_publisher.py"))
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)
RESERVE = 512 * 1024 * 1024


def ordinary_chain(path: Path) -> None:
    if not path.is_absolute() or path.drive.casefold() != "d:" or ".." in path.parts or any(x in str(path) for x in ('"', "\r", "\n")):
        raise ValueError("SPACE_OWNED_D_PATH_REQUIRED")
    for item in (path, *path.parents):
        if item.exists():
            info = item.lstat()
            if item.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError("SPACE_REPARSE_PATH_FORBIDDEN")


def checked(arguments: list[str], *, timeout=60) -> bytes:
    result = subprocess.run(arguments, capture_output=True, timeout=timeout,
        creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise RuntimeError("SPACE_FIXTURE_TOOL_FAILED:" + Path(arguments[0]).name)
    return result.stdout


def protected(root: Path) -> dict[str, str]:
    rows = {}
    for relative in ("bin", "versions", "user-data", "current.json", "bootstrap.json"):
        path = root / relative
        files = path.rglob("*") if path.is_dir() else (path,)
        for item in files:
            if item.is_file():
                rows[item.relative_to(root).as_posix()] = FIXTURE.sha(item.read_bytes())
    return rows


def unchanged(root: Path, before: dict[str, str], candidate: str) -> None:
    assert protected(root) == before, "SPACE_PROTECTED_FILES_CHANGED"
    assert not (root / "versions" / candidate).exists(), "SPACE_FAILED_VERSION_PUBLISHED"
    assert not (root / "updates/receipts" / (candidate + ".json")).exists(), "SPACE_FAILED_RECEIPT_PUBLISHED"


@contextmanager
def deny_directory_writes(path: Path):
    # SID form avoids localized account-name parsing and grants no new rights.
    rows = list(csv.reader(checked(["whoami.exe", "/user", "/fo", "csv", "/nh"]).decode("utf-8").splitlines()))
    sid = rows[0][-1]
    assert re.fullmatch(r"S-1-(?:\d+-)+\d+", sid), "SPACE_SID_UNAVAILABLE"
    denied = False
    try:
        checked(["icacls.exe", str(path), "/deny", "*" + sid + ":(OI)(CI)(W)"])
        denied = True
        try:
            (path / "must-not-write.txt").write_bytes(b"denied probe")
        except PermissionError:
            pass
        else:
            raise AssertionError("SPACE_ACL_NOT_EFFECTIVE")
        yield
    finally:
        if denied:
            checked(["icacls.exe", str(path), "/remove:d", "*" + sid])


@contextmanager
def small_owned_volume(base: Path, evidence: Path):
    ordinary_chain(base)
    assert ctypes.windll.shell32.IsUserAnAdmin(), "SPACE_VHD_FIXTURE_REQUIRES_RUNNER_ADMIN"
    drive_mask = ctypes.windll.kernel32.GetLogicalDrives()
    letter = next((chr(n + ord("A")) for n in range(25, 16, -1) if not drive_mask & (1 << n)), None)
    assert letter is not None, "SPACE_FREE_TEMPORARY_DRIVE_UNAVAILABLE"
    vhd = base / "owned-small-volume.vhdx"
    assert not vhd.exists() and not vhd.is_symlink(), "SPACE_VHD_NOT_FRESH"
    create = base / "create-owned-vhd.txt"
    attach = base / "attach-owned-vhd.txt"
    detach = base / "detach-owned-vhd.txt"
    # Only this exact newly-created VHD is ever selected. There are no select
    # disk/volume-number, clean, physical-drive or noerr commands.
    create.write_text(f'create vdisk file="{vhd}" maximum=64 type=fixed\nexit\n', encoding="ascii")
    attach.write_text(f'select vdisk file="{vhd}"\nattach vdisk\ncreate partition primary\n'
        f'format fs=ntfs quick label=GSW_SPACE_FIXTURE\nassign letter={letter}\nexit\n', encoding="ascii")
    detach.write_text(f'select vdisk file="{vhd}"\ndetach vdisk\nexit\n', encoding="ascii")
    attached = False
    try:
        result = checked(["diskpart.exe", "/s", str(create)], timeout=120)
        (evidence / "owned-vhd-create.log").write_bytes(result)
        created = vhd.lstat()
        assert stat.S_ISREG(created.st_mode) and created.st_nlink == 1 and not (
            getattr(created, "st_file_attributes", 0) & 0x400), "SPACE_CREATED_IMAGE_INVALID"
        assert 64 * 1024 * 1024 <= created.st_size <= 80 * 1024 * 1024, "SPACE_CREATED_IMAGE_SIZE_INVALID"
        assert not ctypes.windll.kernel32.GetLogicalDrives() & (1 << (ord(letter) - ord("A"))), "SPACE_TEMPORARY_DRIVE_RACED"
        result = checked(["diskpart.exe", "/s", str(attach)], timeout=120)
        (evidence / "owned-vhd-attach.log").write_bytes(result)
        attached = True
        volume = Path(letter + ":/")
        assert volume.is_dir(), "SPACE_OWNED_VHD_NOT_MOUNTED"
        # Independently prove the temporary letter belongs to the exact newly
        # created D-drive image before writing any installation fixture there.
        probe = ("$ErrorActionPreference='Stop';"
            f"$i=Get-DiskImage -ImagePath '{vhd}';"
            "$p=@($i|Get-Disk|Get-Partition|Where-Object DriveLetter);"
            "[ordered]@{imagePath=$i.ImagePath;attached=$i.Attached;letters=@($p.DriveLetter)}|ConvertTo-Json -Compress")
        binding = json.loads(checked(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", probe]).decode("utf-8-sig"))
        assert Path(binding["imagePath"]) == vhd and binding["attached"] is True and binding["letters"] == [letter], "SPACE_IMAGE_VOLUME_BINDING_INVALID"
        capacity = shutil.disk_usage(volume)
        assert 0 < capacity.total <= 64 * 1024 * 1024 and capacity.free < RESERVE, "SPACE_VOLUME_NOT_CONSTRAINED"
        FIXTURE.write_json(evidence / "small-volume.json", {"schema": "gsw.space_fixture_volume/1",
            "backingFile": str(vhd), "temporaryVolume": str(volume), "totalBytes": capacity.total,
            "freeBytes": capacity.free, "productionReserveBytes": RESERVE, "physicalDiskSelected": False})
        yield volume
    finally:
        # A failed attach can still have attached our own file. Detach by the
        # checked fresh VHD identity only, never by disk number or drive letter.
        if vhd.is_file():
            result = subprocess.run(["diskpart.exe", "/s", str(detach)], capture_output=True,
                timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
            (evidence / "owned-vhd-detach.log").write_bytes(result.stdout)
            if attached:
                assert result.returncode == 0, "SPACE_OWNED_VHD_DETACH_FAILED"
                assert not ctypes.windll.kernel32.GetLogicalDrives() & (1 << (ord(letter) - ord("A"))), "SPACE_OWNED_VHD_STILL_MOUNTED"


def download_space_contract(driver: Path, volume: Path, base: Path, evidence: Path, root: Path) -> None:
    server_root = base / "large-download-server"
    ready = server_root / "ready.json"
    server = subprocess.Popen([sys.executable, "-I", "-B",
        str(Path(__file__).with_name("native_update_network_fixture.py")),
        "--root", str(server_root), "--ready", str(ready), "--payload-mib", "64"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        deadline = time.monotonic() + 15
        while not ready.is_file() and server.poll() is None and time.monotonic() < deadline:
            time.sleep(.05)
        assert ready.is_file(), "SPACE_DOWNLOAD_SERVER_NOT_READY"
        metadata = json.loads(ready.read_text(encoding="utf-8"))
        assert metadata["testOnly"] is True and metadata["length"] == 64 * 1024 * 1024
        destination = volume / "owned-update-download.zip"
        old_package = b"old verified destination must not be overwritten by a space failure"
        destination.write_bytes(old_package)
        before = protected(root)
        available = shutil.disk_usage(volume).free
        result = subprocess.run([str(driver), str(ready), str(destination)], capture_output=True,
            timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
        rows = [json.loads(line) for line in result.stdout.decode("utf-8").splitlines() if line.startswith("{")]
        assert len(rows) == 1, "SPACE_DOWNLOAD_RESULT_MISSING"
        response = rows[0]
        requests = Path(metadata["requestLog"])
        request_count = len(requests.read_bytes().splitlines()) if requests.is_file() else 0
        FIXTURE.write_json(evidence / "download-space.json", {"schema": "gsw.update_download_space_evidence/1",
            "testOnly": True, "expectedLength": metadata["length"], "availableBefore": available,
            "providerIdentity": "owned-native-download-fixture-only-not-TUF", "exitCode": result.returncode,
            "response": response, "httpRequestCount": request_count, "applicationStarted": False})
        assert destination.read_bytes() == old_package, "SPACE_DOWNLOAD_DESTINATION_CHANGED"
        assert protected(root) == before, "SPACE_DOWNLOAD_OLD_INSTALLATION_CHANGED"
        assert result.returncode == 2 and response.get("ok") is False, "SPACE_DOWNLOAD_DID_NOT_REFUSE"
        assert response.get("code") == "download_insufficient_space", response
        assert response.get("receivedBytes") == 0 and request_count == 0, "SPACE_DOWNLOAD_DID_NOT_PREFLIGHT"
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=10)
        server.stderr.close()


def run(publisher: Path, download_driver: Path, base: Path, evidence: Path) -> None:
    contract = FIXTURE.Contract(publisher, evidence)
    name = "space-candidate-B"
    archive, offer, _ = FIXTURE.make_package(base, name)

    root = FIXTURE.prepare_root(base, "acl-denied")
    updates = root / "updates"
    updates.mkdir()
    before = protected(root)
    with deny_directory_writes(updates):
        contract.publish(root, archive, offer, code="activation_directory_unavailable")
        unchanged(root, before, name)
    contract.publish(root, archive, offer, ok=True, code="ACTIVATED")

    root = FIXTURE.prepare_root(base, "occupied-current")
    before = protected(root)
    with FIXTURE.held_read(root / "current.json"):
        contract.publish(root, archive, offer, code="activation_atomic_write_failed")
        # A fully validated B slot/receipt may already exist, but the original
        # active pointer, all of A, bootstrap and user data must remain intact.
        actual = protected(root)
        assert all(actual.get(path) == digest for path, digest in before.items()), "SPACE_OCCUPIED_OLD_STATE_CHANGED"
        assert (root / "versions" / name / FIXTURE.APP).is_file(), "SPACE_VALIDATED_SLOT_MISSING"
    contract.publish(root, archive, offer, ok=True, code="ACTIVATED")

    with small_owned_volume(base, evidence) as volume:
        root = FIXTURE.prepare_root(volume, "gsw-owned-low-space")
        before = protected(root)
        contract.publish(root, archive, offer, code="activation_insufficient_space")
        unchanged(root, before, name)
        download_space_contract(download_driver, volume, base, evidence, root)

    FIXTURE.write_json(evidence / "acceptance.json", {"schema": "gsw.update_space_acceptance/1",
        "passed": True, "scope": "isolated-native-activation-resource-failures",
        "cases": ["real-acl-denial-and-retry", "real-held-current-pointer-and-retry", "real-small-vhd-free-space",
                  "known-length-download-preflight-on-real-constrained-volume"],
        "testOnly": True, "production": False, "publisherAuthenticated": False,
        "applicationStarted": False, "formalGswAcceptance": False})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-fixture-only", action="store_true", required=True)
    parser.add_argument("--publisher", type=Path, required=True)
    parser.add_argument("--download-fixture", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt" or os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_OS") != "Windows":
        parser.error("Run only on an isolated Windows GitHub Actions runner.")
    ordinary_chain(args.work_root)
    if args.work_root.exists() or args.work_root.is_symlink():
        parser.error("Use a fresh owned D-drive work root.")
    args.work_root.mkdir(parents=True)
    args.evidence.mkdir(parents=True, exist_ok=True)
    try:
        run(args.publisher.absolute(), args.download_fixture.absolute(), args.work_root.absolute(), args.evidence.absolute())
    except Exception as error:
        FIXTURE.write_json(args.evidence / "failure.json", {"schema": "gsw.update_space_failure/1",
            "passed": False, "testOnly": True, "errorType": type(error).__name__, "detail": str(error)[:1024]})
        raise
    print("Native activation ACL/occupied/low-space contract PASS (owned remote fixtures only).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
