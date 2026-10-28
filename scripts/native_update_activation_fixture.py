"""Small archive fixture; never an accepted online signing authority."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import sys
import zipfile

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def main() -> None:
    base = Path(sys.argv[1]).resolve()
    root = base / "installation"
    (root / "bin").mkdir(parents=True)
    (root / "bin/launcher-sentinel").write_bytes(b"unchanged-bootstrap")
    (root / "versions/fixture-A").mkdir(parents=True)
    (root / "versions/fixture-A/keep.txt").write_bytes(b"original-A")
    (root / "current.json").write_text(json.dumps({"schema": "gsw.native_active_version/1", "version": "fixture-A", "executableSha256": "a" * 64}), encoding="utf-8")
    (root / "bootstrap.json").write_text('{"schema":"gsw.native_bootstrap/1","protocol":2}', encoding="utf-8")
    payload = {
        "bin/Gaussian Scene Workbench.exe": b"fixture-executable-for-archive-boundary-only",
        "source/.github/workflows/ci.yml": b"name: preserved-hidden-source\n",
        "LICENSE": b"GPL-3.0-or-later fixture\n",
        "COPYING": b"GPL-3.0-or-later fixture\n",
        "THIRD_PARTY_LICENSES.md": b"Fixture only\n",
        "native/LICENSE.md": b"GPL-3.0-or-later fixture\n",
        "source/native/CMakeLists.txt": b"# fixture source\n",
        "build_manifest.json": b'{"product":"Gaussian Scene Workbench","legacyWebRuntimeIncluded":false}',
        "backend_manifest.json": b'{"schema":"fixture-backend/1"}',
    }
    source = {"schema": "gsw-native-corresponding-source-v1", "executableSha256": sha(payload["bin/Gaussian Scene Workbench.exe"]), "license": "GPL-3.0-or-later", "sourceFileCount": 2, "files": [{"path": p[7:], "size": len(v), "sha256": sha(v)} for p, v in sorted(payload.items()) if p.startswith("source/")]}
    payload["source-manifest.json"] = json.dumps(source, separators=(",", ":")).encode()
    manifest = {"schema": "gsw.native_update_files/1", "product": "Fubukisama.GaussianSceneWorkbench.UpdateFixture", "platform": "windows-x64", "buildId": "fixture-B", "sourceCommit": "b" * 40, "sourceTree": "c" * 40, "files": [{"path": p, "size": len(v), "sha256": sha(v)} for p, v in sorted(payload.items())]}
    raw = json.dumps(manifest, separators=(",", ":")).encode()
    with zipfile.ZipFile(base / "B.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for p, v in payload.items():
            archive.writestr(p, v)
        archive.writestr("release-files.json", raw)
    data = (base / "B.zip").read_bytes()
    offer = {"schema": "gsw.update_offer/1", "product": manifest["product"], "platform": "windows-x64", "channel": "stable", "displayVersion": "B", "buildId": "fixture-B", "releaseSequence": 2, "metadataRevision": 2, "sourceCommit": "b" * 40, "sourceTree": "c" * 40, "archiveUrl": "https://example.invalid/B.zip", "archiveLength": len(data), "archiveSha256": sha(data), "payloadManifestSha256": sha(raw), "correspondingSourceSha256": sha(payload["source-manifest.json"]), "unpackedLength": len(raw) + sum(map(len, payload.values())), "fileCount": len(payload), "minimumBootstrapProtocol": 2}
    (base / "offer.json").write_text(json.dumps(offer), encoding="utf-8")

if __name__ == "__main__":
    main()
