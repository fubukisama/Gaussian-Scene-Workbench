"""Prepare a remote-only HTTPS/TUF trust fixture; never run installed GSW.

Synthetic package EXE bytes are NOT executable application acceptance. This
preparation proves only the native trust boundary in a separate CI test tree.
The later stable-launcher activation seam uses its own real executable fixture.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


def write_json(path: Path, value):
    with path.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, ensure_ascii=True, sort_keys=True)
        output.write("\n")


def package(root: Path, label: str):
    payload = {
        "bin/Gaussian Scene Workbench.exe": ("Synthetic trust-only package " + label).encode(),
        "COPYING": b"GPL-3.0-or-later; isolated fixture only\n",
        "LICENSE": b"GPL-3.0-or-later; isolated fixture only\n",
        "THIRD_PARTY_LICENSES.md": b"No bundled third-party implementation in this synthetic payload.\n",
        "native/LICENSE.md": b"GPL-3.0-or-later; isolated fixture only\n",
        "source/native/CMakeLists.txt": b"# Complete synthetic source boundary, not the GSW application\n",
        "source/.github/workflows/fixture.yml": b"name: synthetic-hidden-source\n",
        "source/native/fixture.txt": ("Synthetic original source " + label + "\n").encode(),
        "build_manifest.json": b'{"product":"Gaussian Scene Workbench","legacyWebRuntimeIncluded":false}',
        "backend_manifest.json": b'{"schema":"gsw.synthetic_trust_backend/1","testOnly":true}',
    }
    source = {"schema": "gsw-native-corresponding-source-v1", "license": "GPL-3.0-or-later",
        "executableSha256": hashlib.sha256(payload["bin/Gaussian Scene Workbench.exe"]).hexdigest(),
        "sourceFileCount": sum(name.startswith("source/") for name in payload),
        "files": [{"path": name.removeprefix("source/"), "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in sorted(payload.items()) if name.startswith("source/")]}
    payload["source-manifest.json"] = json.dumps(source, sort_keys=True, separators=(",", ":")).encode()
    root.mkdir(parents=True, exist_ok=False)
    for name, data in payload.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as output:
            output.write(data)


def prepare(args):
    if not re.fullmatch("[0-9a-f]{40}", args.source_commit) or not re.fullmatch("[0-9a-f]{40}", args.source_tree):
        raise ValueError("invalid_source_identity")
    root = args.root.absolute()
    root.mkdir(parents=True, exist_ok=False)
    a, b = root / "package-A", root / "package-B"
    package(a, "A")
    package(b, "B")
    server_root = root / "https"
    ready = root / "server-info.json"
    server_script = Path(__file__).with_name("native_update_network_fixture.py").absolute()
    server_command = [sys.executable, "-I", "-B", str(server_script), "--root", str(server_root), "--ready", str(ready)]
    flags = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    with (root / "server-stdout.log").open("xb") as stdout, (root / "server-stderr.log").open("xb") as stderr:
        server = subprocess.Popen(server_command, stdout=stdout, stderr=stderr, creationflags=flags, close_fds=True)
    issuer_process = None
    try:
        deadline = time.monotonic() + 20
        while not ready.exists():
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("isolated_https_server_unavailable")
            time.sleep(.1)
        issuer = Path(__file__).with_name("create_native_update_fixture.py").absolute()
        issued = root / "issued"
        issuer_command = [sys.executable, "-I", "-B", str(issuer), "--fixture-only",
            "--output-root", str(issued), "--server-info", str(ready), "--package-a", str(a), "--package-b", str(b),
            "--source-commit", args.source_commit, "--source-tree", args.source_tree]
        if args.with_short_expiry:
            issuer_command += ["--short-expiry-trigger-file", str(issued / "short-expiry-trigger.json")]
            with (root / "issuer-stdout.log").open("xb") as stdout, (root / "issuer-stderr.log").open("xb") as stderr:
                issuer_process = subprocess.Popen(issuer_command, stdout=stdout, stderr=stderr,
                    creationflags=flags, close_fds=True)
            deadline = time.monotonic() + 40
            while not (issued / "fixture.json").exists():
                if issuer_process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("isolated_tuf_issuer_failed")
                time.sleep(.1)
        else:
            result = subprocess.run(issuer_command, capture_output=True, timeout=120, encoding="utf-8")
            if result.returncode != 0:
                try:
                    failure = json.loads(result.stdout[:8192])
                    write_json(root / "issuer-failure.json", {key: failure.get(key) for key in ("schema", "ok", "code", "errorType")})
                except (ValueError, OSError):
                    pass
                raise RuntimeError("isolated_tuf_issuer_failed")
        fixture = json.loads((issued / "fixture.json").read_text(encoding="utf-8"))
        report = {"schema": "gsw.trust_ci_preparation/1", "testOnly": True, "production": False,
            "scope": "synthetic-native-trust-only", "root": str(root), "serverPid": server.pid,
            "serverInfo": str(ready), "profile": str(issued / "profile.json"), "fixture": str(issued / "fixture.json"),
            "controlFile": fixture["controlFile"], "sourceCommit": args.source_commit, "sourceTree": args.source_tree,
            "issuerPid": issuer_process.pid if issuer_process is not None else None}
        write_json(root / "preparation.json", report)
        return report
    except BaseException:
        if issuer_process is not None and issuer_process.poll() is None:
            issuer_process.terminate()
            issuer_process.wait(timeout=10)
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=10)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--with-short-expiry", action="store_true")
    try:
        print(json.dumps(prepare(parser.parse_args()), ensure_ascii=True))
        return 0
    except Exception as error:
        print(json.dumps({"schema": "gsw.trust_ci_preparation/1", "testOnly": True, "ok": False,
                          "code": "trust_fixture_preparation_failed", "errorType": type(error).__name__}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
