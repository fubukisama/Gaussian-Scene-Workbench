"""Issue ephemeral, test-only TUF repositories for remote native update tests.

This is not a production signer. It accepts no production product/key/profile.
Start native_update_network_fixture.py first, then pass its ready record here.
The TUF private keys live only in this issuer process; profiles contain public
root metadata and the network fixture's public CA certificate only.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from urllib.parse import urlsplit

from securesystemslib.signer import CryptoSigner
from tuf.api.metadata import Metadata, MetaFile, Root, Snapshot, TargetFile, Targets, Timestamp
from tuf.api.serialization.json import JSONSerializer


PRODUCT = "Fubukisama.GaussianSceneWorkbench.UpdateFixture"
PLATFORM = "windows-x64"
SERIALIZER = JSONSerializer(compact=True)


def _fresh_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(path.name)
    temporary = path.with_name(path.name + ".writing")
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, allow_nan=False, sort_keys=True)
        stream.write("\n")
    temporary.rename(path)


def _digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _wire(metadata) -> bytes:
    return metadata.to_bytes(SERIALIZER)


def _signed(metadata, signer) -> bytes:
    metadata.sign(signer)
    return _wire(metadata)


def _write(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)


def _base(server):
    if server.get("schema") != "gsw.update_network_fixture/1" or server.get("testOnly") is not True:
        raise ValueError("expected_test_only_https_server")
    value = server["baseUrl"]
    parsed = urlsplit(value)
    if (parsed.scheme != "https" or parsed.username or parsed.password or parsed.query
            or parsed.fragment or parsed.path not in ("", "/") or not parsed.port):
        raise ValueError("fixture_base_url_invalid")
    if parsed.hostname != "localhost" and not ipaddress.ip_address(parsed.hostname).is_loopback:
        raise ValueError("fixture_must_be_loopback")
    return value.rstrip("/")


def _archive(package: Path, destination: Path, commit: str, tree: str,
             build_id: str, version: str, sequence: int):
    result = subprocess.run([sys.executable, "-I", "-B",
        str(Path(__file__).with_name("package_native_update.py")),
        "--package-root", str(package), "--output-archive", str(destination),
        "--source-commit", commit, "--source-tree", tree, "--build-id", build_id,
        "--product", PRODUCT, "--display-version", version,
        "--release-sequence", str(sequence)], capture_output=True, text=True,
        encoding="utf-8", timeout=240)
    if result.returncode != 0:
        # Do not include environment, credentialled URLs or package contents.
        raise ValueError("fixture_package_creation_failed")
    report = json.loads(result.stdout)
    if report.get("product") != PRODUCT or report.get("buildId") != build_id:
        raise ValueError("fixture_package_identity_differs")
    return report


def _offer(report, base_url: str, asset: str, channel: str, revision: int):
    keys = ("product", "platform", "displayVersion", "buildId", "releaseSequence",
            "sourceCommit", "sourceTree", "archiveLength", "archiveSha256",
            "payloadManifestSha256", "correspondingSourceSha256", "unpackedLength", "fileCount")
    result = {key: report[key] for key in keys}
    result.update(schema="gsw.update_offer/1", channel=channel, metadataRevision=revision,
        archiveUrl=base_url + "/downloads/" + asset, minimumBootstrapProtocol=2,
        revokedBuildIds=[], releaseNotes={
            "zh_CN": "仅隔离测试：同一入口切换完整版本；不触及用户数据。",
            "en_US": "Isolated fixture only: full slot update through the same entry.",
            "ja_JP": "隔離テスト専用：同じ起動口から完全なバージョンを切り替えます。"})
    return result


def _repository(directory: Path, offer: dict, root_bytes: bytes, signers: dict,
                now: datetime, version: int, *, expired=False, wrong_signature=False,
                duplicate_offer=False, mix_snapshot=False, wrong_timestamp_signature=False,
                root_chain=None, timestamp_lifetime_seconds=None):
    target_bytes = json.dumps(offer, ensure_ascii=False, allow_nan=False,
                             sort_keys=True, separators=(",", ":")).encode("utf-8")
    if duplicate_offer:
        replacement = json.dumps(offer["product"], ensure_ascii=False).encode("utf-8")
        target_bytes = target_bytes.replace(b'"product":', b'"product":' + replacement + b',"product":', 1)
    metadata = directory / "metadata"
    targets = directory / "targets"
    _write(targets / "release.json", target_bytes)
    target = Metadata(Targets(version=version, expires=now + timedelta(days=7)))
    target.signed.targets["release.json"] = TargetFile.from_data("release.json", target_bytes, ["sha256"])
    target_bytes_wire = _signed(target, signers["targets"])
    if wrong_signature:
        invalid = json.loads(target_bytes_wire)
        invalid["signatures"][0]["sig"] = "00" * 64
        target_bytes_wire = json.dumps(invalid, sort_keys=True, separators=(",", ":")).encode("utf-8")
    snapshot = Metadata(Snapshot(version=version, expires=now + timedelta(days=7)))
    snapshot.signed.meta["targets.json"] = MetaFile.from_data(version, target_bytes_wire, ["sha256"])
    if mix_snapshot:
        snapshot.signed.meta["targets.json"].hashes["sha256"] = "00" * 32
    snapshot_bytes = _signed(snapshot, signers["snapshot"])
    lifetime = timedelta(hours=-1) if expired else (timedelta(seconds=timestamp_lifetime_seconds)
        if timestamp_lifetime_seconds is not None else timedelta(days=1))
    timestamp = Metadata(Timestamp(version=version, expires=now + lifetime))
    timestamp.signed.snapshot_meta = MetaFile.from_data(version, snapshot_bytes, ["sha256"])
    timestamp_bytes = _signed(timestamp, signers["timestamp"])
    if wrong_timestamp_signature:
        invalid = json.loads(timestamp_bytes)
        invalid["signatures"][0]["sig"] = "00" * 64
        timestamp_bytes = json.dumps(invalid, sort_keys=True, separators=(",", ":")).encode("utf-8")
    files = {"root.json": root_bytes,
                       "targets.json": target_bytes_wire, f"{version}.targets.json": target_bytes_wire,
                       "snapshot.json": snapshot_bytes, f"{version}.snapshot.json": snapshot_bytes,
                       "timestamp.json": timestamp_bytes}
    files.update({f"{number}.root.json": data for number, data in (root_chain or {1: root_bytes}).items()})
    for name, data in files.items():
        _write(metadata / name, data)


def _rotated_root(signers: dict, second_root, signing, now: datetime, *, version=2):
    trust = Metadata(Root(version=version, expires=now + timedelta(days=365), consistent_snapshot=False))
    for role, signer in signers.items():
        trust.signed.add_key(signer.public_key, role)
    trust.signed.add_key(second_root.public_key, "root")
    trust.signed.roles["root"].threshold = 2
    for index, signer in enumerate(signing):
        trust.sign(signer, append=index != 0)
    return _wire(trust)


def prepare(args):
    if not args.fixture_only:
        raise ValueError("fixture_only_acknowledgement_required")
    if not re.fullmatch(r"[a-f0-9]{40}", args.source_commit) or not re.fullmatch(r"[a-f0-9]{40}", args.source_tree):
        raise ValueError("fixture_source_identity_invalid")
    root = args.output_root.absolute()
    root.mkdir(parents=True, exist_ok=True)
    if (root / "profile.json").exists() or (root / "fixture.json").exists():
        raise ValueError("fixture_output_already_issued")
    server = json.loads(args.server_info.read_text(encoding="utf-8-sig"))
    base_url = _base(server)
    public = Path(server["contentRoot"]).absolute()
    for name in ("A", "B", "badC", "wrong-key", "expired", "mix", "wrong-product",
                 "wrong-platform", "wrong-channel", "duplicate-offer", "sequence-overflow"):
        if (public / name).exists():
            raise ValueError("fixture_repository_already_exists")
    ca = Path(server["caPath"]).read_text(encoding="ascii")
    if "BEGIN CERTIFICATE" not in ca or "PRIVATE KEY" in ca or len(ca) > 65536:
        raise ValueError("fixture_ca_invalid")
    prepared = root / "archive-builds"
    prepared.mkdir(exist_ok=False)
    downloads = public / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    reports, assets = {}, {}
    for name, package, sequence in (("A", args.package_a, 1), ("B", args.package_b, 2),
                                     ("badC", args.package_b, 3)):
        report = _archive(package, prepared / f"{name}.zip", args.source_commit, args.source_tree,
                          "fixture-" + name.lower(), args.display_version, sequence)
        asset = report["buildId"] + "-" + report["archiveSha256"][:12] + ".zip"
        destination = downloads / asset
        if destination.exists():
            raise ValueError("fixture_asset_already_exists")
        shutil.copyfile(report["archivePath"], destination)
        if _digest(destination) != report["archiveSha256"]:
            raise ValueError("fixture_asset_copy_differs")
        if name == "badC":
            # Only the test asset is corrupted after its valid bytes were bound
            # into the Offer: metadata still authorizes the original digest.
            with destination.open("r+b") as stream:
                offset = report["archiveLength"] // 2
                stream.seek(offset)
                old = stream.read(1)
                stream.seek(offset)
                stream.write(bytes([old[0] ^ 1]))
        reports[name], assets[name] = report, asset

    now = datetime.now(timezone.utc).replace(microsecond=0)
    signers = {role: CryptoSigner.generate_ed25519() for role in ("root", "targets", "snapshot", "timestamp")}
    second_root = CryptoSigner.generate_ed25519()
    trust = Metadata(Root(version=1, expires=now + timedelta(days=365), consistent_snapshot=False))
    for role, signer in signers.items():
        trust.signed.add_key(signer.public_key, role)
    trust.signed.add_key(second_root.public_key, "root")
    trust.signed.roles["root"].threshold = 2
    trust.sign(signers["root"])
    trust.sign(second_root, append=True)
    root_bytes = _wire(trust)
    rotated = {role: CryptoSigner.generate_ed25519() for role in ("root", "targets", "snapshot", "timestamp")}
    rotated_second = CryptoSigner.generate_ed25519()
    old_threshold = [signers["root"], second_root]
    new_threshold = [rotated["root"], rotated_second]
    rotated_bytes = _rotated_root(rotated, rotated_second, old_threshold + new_threshold, now)
    rotation_roots = {
        "rotated": rotated_bytes,
        "rotation-fetch-fails": rotated_bytes,
        "rotated-root-omitted": rotated_bytes,
        "root-old-threshold-only": _rotated_root(rotated, rotated_second, old_threshold, now),
        "root-new-threshold-only": _rotated_root(rotated, rotated_second, new_threshold, now),
        "root-new-threshold-short": _rotated_root(rotated, rotated_second, old_threshold + new_threshold[:1], now),
        "root-version-skipped": _rotated_root(rotated, rotated_second, old_threshold + new_threshold, now, version=3),
    }
    for channel in ("stable", "preview"):
        for name, revision in (("A", 1), ("B", 2), ("badC", 3)):
            _repository(public / name / channel,
                _offer(reports[name], base_url, assets[name], channel, revision),
                root_bytes, signers, now, revision)
        for name in ("wrong-key", "expired", "mix", "wrong-product", "wrong-platform",
                     "wrong-channel", "duplicate-offer", "sequence-overflow"):
            offer = _offer(reports["B"], base_url, assets["B"], channel, 4)
            offer["releaseSequence"] = 4
            if name == "wrong-product":
                offer["product"] = "Untrusted.OtherProduct"
            elif name == "wrong-platform":
                offer["platform"] = "linux-x64"
            elif name == "wrong-channel":
                offer["channel"] = "preview" if channel == "stable" else "stable"
            elif name == "sequence-overflow":
                offer["releaseSequence"] = 9_007_199_254_740_992
            _repository(public / name / channel, offer, root_bytes, signers, now, 4,
                expired=name == "expired", wrong_signature=name == "wrong-key",
                duplicate_offer=name == "duplicate-offer", mix_snapshot=name == "mix")
        for name, next_root in rotation_roots.items():
            chain = {1: root_bytes}
            if name != "rotated-root-omitted":
                chain[2] = next_root
            _repository(public / name / channel,
                _offer(reports["B"], base_url, assets["B"], channel, 5), next_root, rotated, now, 5,
                root_chain=chain, wrong_timestamp_signature=name == "rotation-fetch-fails")
        for name, revision in (("revoked-a", 6), ("revocation-omitted", 7), ("revoked-b", 8),
                               ("release-sequence-rollback", 8), ("metadata-revision-rollback", 8),
                               ("build-id-reused", 8), ("failed-after-revocation", 9)):
            offer = _offer(reports["A"] if name == "release-sequence-rollback" else reports["B"],
                base_url, assets["A"] if name == "release-sequence-rollback" else assets["B"], channel, revision)
            if name == "revoked-a":
                offer["revokedBuildIds"] = ["fixture-a"]
            elif name == "revoked-b":
                offer["revokedBuildIds"] = ["fixture-b"]
            elif name == "metadata-revision-rollback":
                offer["metadataRevision"] = 1
            elif name == "build-id-reused":
                offer["releaseSequence"] = 3
                offer["archiveSha256"] = "00" * 32
            _repository(public / name / channel, offer, root_bytes, signers, now, revision,
                        wrong_timestamp_signature=name == "failed-after-revocation")
    profile = {"schema": "gsw.update_profile/1", "product": PRODUCT, "platform": PLATFORM,
        "channels": {channel: {"metadataBase": base_url + f"/repository/{channel}/metadata/",
                              "targetsBase": base_url + f"/repository/{channel}/targets/"}
                     for channel in ("stable", "preview")},
        "approvedHosts": [urlsplit(base_url).hostname], "seedRoot": json.loads(root_bytes),
        "fixtureCaPem": ca}
    _fresh_json(root / "profile.json", profile)
    result = {"schema": "gsw.update_fixture/1", "testOnly": True, "production": False,
        "product": PRODUCT, "platform": PLATFORM, "profilePath": str(root / "profile.json"),
        "profileSha256": _digest(root / "profile.json"), "contentRoot": str(public),
        "controlFile": server["controlFile"], "baseUrl": base_url,
        "consistentSnapshot": False, "rootThreshold": 2,
        "repositories": ["A", "B", "badC", "wrong-key", "expired", "mix", "wrong-product",
                         "wrong-platform", "wrong-channel", "duplicate-offer", "sequence-overflow",
                         *rotation_roots, "revoked-a", "revocation-omitted", "revoked-b",
                         "release-sequence-rollback", "metadata-revision-rollback", "build-id-reused", "failed-after-revocation"],
        "archives": reports, "sourceCommit": args.source_commit, "sourceTree": args.source_tree}
    if args.short_expiry_trigger_file is not None:
        trigger = args.short_expiry_trigger_file.absolute()
        ready = args.short_expiry_ready_file.absolute() if args.short_expiry_ready_file else root / "short-expiry-ready.json"
        if trigger.parent != root or ready.parent != root or trigger.exists() or ready.exists():
            raise ValueError("short_expiry_control_path_invalid")
        result.update(shortExpiryTrigger=str(trigger), shortExpiryReady=str(ready), issuerPid=os.getpid())
        result["repositories"].append("short-expiry")
    _fresh_json(root / "fixture.json", result)
    if args.short_expiry_trigger_file is not None:
        # Keys remain ONLY in this bounded test process's memory. Sign the short
        # timestamp AFTER the native binary has compiled and the driver signals;
        # otherwise a cold CI build could consume the entire intended lifetime.
        deadline = time.monotonic() + 10 * 60
        while not trigger.exists():
            if time.monotonic() >= deadline:
                raise ValueError("short_expiry_trigger_timeout")
            time.sleep(.1)
        if trigger.stat().st_size > 1024 or json.loads(trigger.read_text(encoding="utf-8")) != {"issue": True}:
            raise ValueError("short_expiry_trigger_invalid")
        short_now = datetime.now(timezone.utc).replace(microsecond=0)
        for channel in ("stable", "preview"):
            _repository(public / "short-expiry" / channel,
                _offer(reports["A"], base_url, assets["A"], channel, 10),
                root_bytes, signers, short_now, 10, timestamp_lifetime_seconds=60)
        _fresh_json(ready, {"schema": "gsw.short_expiry_fixture/1", "testOnly": True,
            "issuedAt": short_now.isoformat().replace("+00:00", "Z"),
            "expires": (short_now + timedelta(seconds=60)).isoformat().replace("+00:00", "Z")})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-only", action="store_true")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--server-info", type=Path, required=True)
    parser.add_argument("--package-a", type=Path, required=True)
    parser.add_argument("--package-b", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--display-version", default="0.3.8-native-preview")
    parser.add_argument("--short-expiry-trigger-file", type=Path)
    parser.add_argument("--short-expiry-ready-file", type=Path)
    try:
        print(json.dumps(prepare(parser.parse_args()), ensure_ascii=True))
        return 0
    except Exception as error:
        print(json.dumps({"schema": "gsw.update_fixture/1", "testOnly": True,
                          "ok": False, "code": "fixture_issuance_failed",
                          "errorType": type(error).__name__}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
