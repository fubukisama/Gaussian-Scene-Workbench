"""REMOTE CI ONLY: reject unsafe compile-time inputs through developer probes.

No keys are generated and no network or installation operation is performed.
The base input must contain only a CI ephemeral public test root and reserved
.invalid hostnames. Never point this test at a production-library target.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import subprocess
import unittest


class ProfileInputRejections(unittest.TestCase):
    cargo_command: list[str]
    manifest: Path
    target: Path
    root: Path
    base: dict

    def build(self, name, *, profile=None, raw=None, mixed=False, missing=False, relative=False, unset=False):
        path = self.root / (name + ".json")
        if not missing:
            path.write_bytes(raw if raw is not None else json.dumps(profile if profile is not None else self.base).encode())
        environment = os.environ.copy()
        environment.pop("GSW_UPDATER_FIXTURE_PROFILE_PATH", None)
        environment["GSW_UPDATER_PRODUCTION_PROFILE_PATH"] = path.name if relative else str(path)
        if unset:
            environment.pop("GSW_UPDATER_PRODUCTION_PROFILE_PATH")
        features = "profile-probe,production-profile" + (",fixture-profile" if mixed else "")
        command = [*self.cargo_command, "build", "--manifest-path", str(self.manifest),
                   "--locked", "--release", "--bin", "gsw-trust-profile-probe",
                   "--features", features, "--target-dir", str(self.target)]
        result = subprocess.run(command, capture_output=True, timeout=600, env=environment)
        (self.root / (name + ".build.log")).write_bytes(result.stdout + result.stderr)
        return result

    def test_feature_mixing_and_nonabsolute_or_missing_input_fail_to_build(self):
        for name, options, expected in (
            ("mixed-features", {"mixed": True}, b"mutually exclusive"),
            ("unset-input", {"unset": True}, b"explicit compile-time profile path"),
            ("relative-path", {"relative": True}, b"absolute build input"),
            ("missing-input", {"missing": True}, b"open profile build input"),
            ("oversize-input", {"raw": b" " * (2 * 1024 * 1024 + 1)}, b"profile build input too large"),
        ):
            with self.subTest(case=name):
                result = self.build(name, **options)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)

    def test_rejects_unsafe_profile_data_without_network_or_installation(self):
        cases = {}

        def changed(name, change):
            profile = deepcopy(self.base)
            change(profile)
            cases[name] = profile

        changed("fixture-product", lambda p: p.update(product="Fubukisama.GaussianSceneWorkbench.UpdateFixture"))
        changed("wrong-schema", lambda p: p.update(schema="gsw.update_profile/2"))
        changed("fixture-ca", lambda p: p.update(fixtureCaPem="test-only-forbidden-ca"))
        changed("fixture-ca-null", lambda p: p.update(fixtureCaPem=None))
        changed("unknown-field", lambda p: p.update(rootPath="not-a-runtime-input"))
        changed("wrong-platform", lambda p: p.update(platform="linux-x64"))
        changed("missing-preview", lambda p: p["channels"].pop("preview"))
        changed("extra-channel", lambda p: p["channels"].update(nightly=deepcopy(p["channels"]["stable"])))
        changed("unapproved-host", lambda p: p.update(approvedHosts=["unapproved.example.invalid"]))
        changed("duplicate-host", lambda p: p["approvedHosts"].append(p["approvedHosts"][0]))
        for name, host in (("loopback-ip", "127.0.0.1"), ("odd-numeric-ip", "127.1"),
                           ("numeric-ip", "2130706433"), ("localhost", "localhost"),
                           ("localhost-child", "updates.localhost"), ("private-dns", "updates.local"),
                           ("bad-label", "-updates.example.invalid"), ("empty-label", "updates..invalid")):
            changed(name, lambda p, host=host: p["approvedHosts"].append(host))
        changed("http-url", lambda p: p["channels"]["stable"].update(metadataBase="http://updates.example.invalid/metadata/"))
        changed("credential-url", lambda p: p["channels"]["stable"].update(metadataBase="https://user:discarded@updates.example.invalid/metadata/"))
        changed("unsafe-port", lambda p: p["channels"]["stable"].update(metadataBase="https://updates.example.invalid:8443/metadata/"))
        changed("query-url", lambda p: p["channels"]["stable"].update(metadataBase="https://updates.example.invalid/metadata/?x=1"))
        changed("fragment-url", lambda p: p["channels"]["stable"].update(metadataBase="https://updates.example.invalid/metadata/#x"))
        changed("unknown-channel-field", lambda p: p["channels"]["stable"].update(caPem="forbidden"))
        changed("root-threshold-one", lambda p: p["seedRoot"]["signed"]["roles"]["root"].update(threshold=1))
        changed("insufficient-root-signatures", lambda p: p["seedRoot"].update(signatures=p["seedRoot"]["signatures"][:1]))
        changed("bad-root-signature", lambda p: p["seedRoot"]["signatures"][0].update(sig="00" * 64))
        changed("duplicate-root-signature", lambda p: p["seedRoot"]["signatures"].append(deepcopy(p["seedRoot"]["signatures"][0])))
        changed("unknown-root-field", lambda p: p["seedRoot"]["signed"].update(unsafeExpiry=True))
        for name, profile in cases.items():
            with self.subTest(case=name):
                result = self.build(name, profile=profile)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace")[-8000:])
                self.assert_rejected(name)
        duplicate = b'{"schema":"gsw.update_profile/1",' + json.dumps(self.base).encode()[1:]
        result = self.build("duplicate-json-key", raw=duplicate)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace")[-8000:])
        self.assert_rejected("duplicate-json-key")

    def assert_rejected(self, name):
        executable = self.target / "release" / ("gsw-trust-profile-probe.exe" if os.name == "nt" else "gsw-trust-profile-probe")
        result = subprocess.run([str(executable)], capture_output=True, timeout=15)
        (self.root / (name + ".probe.json")).write_bytes(result.stdout)
        self.assertEqual(result.returncode, 2, result.stderr.decode(errors="replace"))
        self.assertEqual(json.loads(result.stdout),
                         {"schema": "gsw.profile_probe/1", "ok": False, "code": "PROFILE_INVALID"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default="rustup")
    parser.add_argument("--toolchain", default="1.99.1")
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--target-dir", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    arguments = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") != "true":
        parser.error("remote CI execution is required")
    base = json.loads(arguments.profile.read_text(encoding="utf-8-sig"))
    if (base.get("product") != "Fubukisama.GaussianSceneWorkbench.Native" or "fixtureCaPem" in base
            or not base.get("approvedHosts") or any(not h.endswith(".invalid") for h in base["approvedHosts"])):
        parser.error("only a reserved-host CI public test profile is permitted")
    target = arguments.target_dir.absolute()
    if not target.name.startswith("profile-"):
        parser.error("target directory must have a distinct profile- developer-probe name")
    root = arguments.work_root.absolute()
    root.mkdir(parents=True, exist_ok=False)
    ProfileInputRejections.cargo_command = [arguments.cargo, "run", arguments.toolchain, "cargo"]
    ProfileInputRejections.manifest = arguments.manifest.absolute()
    ProfileInputRejections.target = target
    ProfileInputRejections.root = root
    ProfileInputRejections.base = base
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ProfileInputRejections)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 2


if __name__ == "__main__":
    raise SystemExit(main())
