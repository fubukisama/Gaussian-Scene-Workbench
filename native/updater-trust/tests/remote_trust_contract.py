"""Remote-only public C-ABI tracer through the fixture CLI, not Rust internals.

The external HTTPS repository and ephemeral public profile are prepared by the
fixture issuer. No keys are created here. Every process is test-owned and no
installed GSW, project, training process or production release is touched.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import unittest


class TrustContract(unittest.TestCase):
    cli: Path
    fixture: dict
    root: Path

    def select(self, repository: str):
        control = Path(self.fixture["controlFile"])
        control.write_text(json.dumps({"repository": repository}), encoding="utf-8")

    def request(self, operation: str, root: Path, expected=None, *, channel="stable", environment=None):
        request = {"schema": 1, "operation": operation, "installation_root": str(root.absolute()), "channel": channel}
        if expected is not None:
            request["expected_build_id"] = expected
        return self.raw_request(json.dumps(request).encode(), environment=environment)

    def raw_request(self, raw: bytes, *, environment=None):
        result = subprocess.run([str(self.cli), "-"], input=raw,
            capture_output=True, timeout=100, env=environment or os.environ.copy())
        self.assertLessEqual(len(result.stdout), 4 * 1024 * 1024)
        response = json.loads(result.stdout)
        self.assertEqual(response["schema"], 1)
        self.assertEqual(result.returncode, 0 if response["ok"] else 2, result.stderr.decode(errors="replace"))
        return response

    def test_accepts_real_A_then_B_revalidates_cached_B_and_rejects_network_replay(self):
        installation = self.root / "a-to-b"
        installation.mkdir()
        self.select("A")
        a = self.request("resolve", installation)
        self.assertTrue(a["ok"], a)
        self.assertEqual(a["offer"]["buildId"], "fixture-a")
        self.assertEqual(a["offer"]["releaseSequence"], 1)
        self.assertEqual(a["offer"]["product"], "Fubukisama.GaussianSceneWorkbench.UpdateFixture")
        self.assertIn("BEGIN CERTIFICATE", a["transportPolicy"]["fixtureCaPem"])
        self.select("B")
        b = self.request("resolve", installation)
        self.assertTrue(b["ok"], b)
        self.assertEqual(b["offer"]["buildId"], "fixture-b")
        self.assertEqual(b["offer"]["releaseSequence"], 2)
        self.select("A")
        cached = self.request("verify_cached", installation, "fixture-b")
        self.assertTrue(cached["ok"], cached)
        self.assertEqual(cached["descriptorSha256"], b["descriptorSha256"])
        replay = self.request("resolve", installation)
        self.assertFalse(replay["ok"], replay)
        state = self.request("inspect_accepted", installation)
        self.assertTrue(state["ok"], state)
        self.assertEqual(state["acceptedState"]["highestByChannel"]["stable"]["releaseSequence"], 2)
        self.assertEqual(set(state["acceptedState"]["accepted"]), {"fixture-a", "fixture-b"})
        self.assertIn(a["descriptorSha256"], state["acceptedState"]["accepted"]["fixture-a"]["descriptorSha256s"])
        self.assertIn(b["descriptorSha256"], state["acceptedState"]["accepted"]["fixture-b"]["descriptorSha256s"])

    def test_rejects_invalid_signature_expiry_mix_and_descriptor_profiles(self):
        scenarios = {"wrong-key": "TUF_REJECTED", "expired": "TUF_REJECTED", "mix": "TUF_REJECTED",
            "wrong-product": "OFFER_REJECTED", "wrong-platform": "OFFER_REJECTED", "wrong-channel": "OFFER_REJECTED",
            "duplicate-offer": "JSON_REJECTED", "sequence-overflow": "JSON_REJECTED"}
        for scenario, code in scenarios.items():
            with self.subTest(repository=scenario):
                installation = self.root / ("reject-" + scenario)
                installation.mkdir()
                self.select(scenario)
                rejected = self.request("resolve", installation)
                self.assertFalse(rejected["ok"], rejected)
                self.assertEqual(rejected["code"], code)
                state = self.request("inspect_accepted", installation)
                self.assertTrue(state["ok"], state)
                self.assertEqual(state["acceptedState"]["accepted"], {})

    def test_rejects_runtime_trust_overrides_duplicate_request_and_cancels(self):
        installation = self.root / "intent-only"
        installation.mkdir()
        base = {"schema": 1, "operation": "resolve", "installation_root": str(installation), "channel": "stable"}
        for name, value in {"root": {}, "feed": "https://example.invalid/", "ca": "untrusted", "unsafe_expiry": True,
                            "now": "3026-01-01T00:00:00Z"}.items():
            with self.subTest(override=name):
                result = self.raw_request(json.dumps(dict(base, **{name: value})).encode())
                self.assertEqual((result["ok"], result["code"]), (False, "INVALID_REQUEST"))
        duplicate = json.dumps(base).replace('"schema": 1', '"schema":1,"schema":1').encode()
        self.assertEqual(self.raw_request(duplicate)["code"], "INVALID_REQUEST")
        environment = os.environ.copy()
        environment["GSW_UPDATE_FIXTURE_CANCEL_AFTER_MS"] = "0"
        self.assertEqual(self.request("resolve", installation, environment=environment)["code"], "CANCELLED")
        self.assertFalse((installation / ".update-trust-anchor").exists())

    def test_atomic_accept_before_failure_keeps_old_state_and_after_failure_keeps_new_state(self):
        self.select("B")
        for phase in ("before-replace", "after-replace"):
            with self.subTest(fault=phase):
                installation = self.root / ("atomic-" + phase)
                installation.mkdir()
                environment = os.environ.copy()
                environment["GSW_UPDATE_FIXTURE_STORE_FAULT"] = "accepted-offer:" + phase
                rejected = self.request("resolve", installation, environment=environment)
                self.assertEqual((rejected["ok"], rejected["code"]), (False, "TRUST_STATE_WRITE_FAILED"))
                state = self.request("inspect_accepted", installation)
                self.assertTrue(state["ok"], state)
                if phase == "before-replace":
                    self.assertEqual(state["acceptedState"]["accepted"], {})
                    retry = self.request("resolve", installation)
                    self.assertTrue(retry["ok"], retry)
                else:
                    self.assertEqual(state["acceptedState"]["highestByChannel"]["stable"]["releaseSequence"], 2)
                    self.assertIn("fixture-b", state["acceptedState"]["accepted"])

    def test_missing_whole_store_or_corruption_never_resets_accepted_history(self):
        self.select("B")
        for failure in ("directory-lost", "journal-corrupt"):
            with self.subTest(failure=failure):
                installation = self.root / failure
                installation.mkdir()
                accepted = self.request("resolve", installation)
                self.assertTrue(accepted["ok"], accepted)
                directory = installation / "updates/trust"
                if failure == "directory-lost":
                    directory.rename(directory.with_name("trust.saved"))
                else:
                    (directory / "trust-journal.json").write_bytes(b'{"corrupted":')
                rejected = self.request("resolve", installation)
                self.assertFalse(rejected["ok"], rejected)
                self.assertIn(rejected["code"], {"TRUST_STATE_UNAVAILABLE", "TRUST_STATE_CORRUPT"})
                if failure == "directory-lost":
                    self.assertFalse(directory.exists())
                else:
                    self.assertEqual((directory / "trust-journal.json").read_bytes(), b'{"corrupted":')

    def test_accepted_state_is_read_only_and_bad_archive_is_not_a_signature_failure(self):
        installation = self.root / "trust-versus-integrity"
        installation.mkdir()
        self.select("badC")
        response = self.request("resolve", installation)
        self.assertTrue(response["ok"], response)
        self.assertEqual(response["offer"]["buildId"], "fixture-badc")
        # The archive has been corrupted by the issuer AFTER binding. Authentic
        # metadata is not proof that a later download will have the right bytes.
        journal = installation / "updates/trust/trust-journal.json"
        before = journal.read_bytes(), journal.stat().st_mtime_ns
        state = self.request("inspect_accepted", installation)
        self.assertTrue(state["ok"], state)
        self.assertEqual((journal.read_bytes(), journal.stat().st_mtime_ns), before)

    def test_root_rotation_requires_old_and_new_thresholds_and_exact_next_version(self):
        for repository in ("root-old-threshold-only", "root-new-threshold-only", "root-new-threshold-short", "root-version-skipped"):
            with self.subTest(repository=repository):
                installation = self.root / repository
                installation.mkdir()
                self.select("B")
                self.assertTrue(self.request("resolve", installation)["ok"])
                self.select(repository)
                rejected = self.request("resolve", installation)
                self.assertEqual((rejected["ok"], rejected["code"]), (False, "TUF_REJECTED"))
                # A failed candidate root must not invalidate the accepted old
                # chain or manufacture acceptance of the candidate's new keys.
                cached = self.request("verify_cached", installation, "fixture-b")
                self.assertTrue(cached["ok"], cached)

    def test_accepted_rotated_root_survives_later_fetch_failure_and_restart(self):
        installation = self.root / "rotation-adoption"
        installation.mkdir()
        self.select("B")
        original = self.request("resolve", installation)
        self.assertTrue(original["ok"], original)
        self.select("rotation-fetch-fails")
        rejected = self.request("resolve", installation)
        self.assertEqual((rejected["ok"], rejected["code"]), (False, "TUF_REJECTED"))
        # This repository deliberately DOES NOT serve 2.root.json. It can only
        # validate its new signing keys after the previous process durably
        # adopted root 2 before the timestamp failure, not by rediscovery.
        self.select("rotated-root-omitted")
        rotated = self.request("resolve", installation)
        self.assertTrue(rotated["ok"], rotated)
        self.assertEqual(rotated["offer"]["metadataRevision"], 5)
        state = self.request("inspect_accepted", installation)
        self.assertTrue(state["ok"], state)
        digests = state["acceptedState"]["accepted"]["fixture-b"]["descriptorSha256s"]
        self.assertIn(original["descriptorSha256"], digests)
        self.assertIn(rotated["descriptorSha256"], digests)
        self.select("B")
        self.assertFalse(self.request("resolve", installation)["ok"])
        self.assertTrue(self.request("verify_cached", installation, "fixture-b")["ok"])

    def test_revocations_survive_omission_network_failure_channels_and_self_revocation(self):
        installation = self.root / "revocation-union"
        installation.mkdir()
        self.select("A")
        self.assertTrue(self.request("resolve", installation)["ok"])
        self.select("B")
        self.assertTrue(self.request("resolve", installation)["ok"])
        self.select("revoked-a")
        self.assertTrue(self.request("resolve", installation)["ok"])
        self.select("A")
        # Preview has independent fresh TUF versions, but a global revoked ID.
        preview = self.request("resolve", installation, channel="preview")
        self.assertEqual((preview["ok"], preview["code"]), (False, "BUILD_REVOKED"))
        self.select("revocation-omitted")
        self.assertTrue(self.request("resolve", installation)["ok"])
        self.select("failed-after-revocation")
        self.assertFalse(self.request("resolve", installation)["ok"])
        state = self.request("inspect_accepted", installation)
        self.assertEqual(state["acceptedState"]["revokedBuildIds"], ["fixture-a"])
        self.select("revoked-b")
        revoked = self.request("resolve", installation)
        self.assertEqual((revoked["ok"], revoked["code"]), (False, "BUILD_REVOKED"))
        state = self.request("inspect_accepted", installation)
        self.assertEqual(set(state["acceptedState"]["revokedBuildIds"]), {"fixture-a", "fixture-b"})
        self.assertEqual(state["acceptedState"]["highestByChannel"]["stable"]["metadataRevision"], 8)

    def test_fresh_tuf_versions_do_not_bypass_application_high_water_or_immutable_ids(self):
        for repository, code in {"release-sequence-rollback": "RELEASE_ROLLBACK_REJECTED",
                                 "metadata-revision-rollback": "RELEASE_ROLLBACK_REJECTED",
                                 "build-id-reused": "BUILD_ID_REUSED"}.items():
            with self.subTest(repository=repository):
                installation = self.root / repository
                installation.mkdir()
                self.select("B")
                accepted = self.request("resolve", installation)
                self.assertTrue(accepted["ok"], accepted)
                self.select(repository)
                rejected = self.request("resolve", installation)
                self.assertEqual((rejected["ok"], rejected["code"]), (False, code))
                state = self.request("inspect_accepted", installation)
                self.assertEqual(state["acceptedState"]["highestByChannel"]["stable"]["releaseSequence"], 2)
                self.assertEqual(set(state["acceptedState"]["accepted"]), {"fixture-b"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(args.fixture.read_text(encoding="utf-8-sig"))
    if fixture.get("schema") != "gsw.update_fixture/1" or fixture.get("testOnly") is not True:
        raise ValueError("isolated fixture required")
    args.work_root.mkdir(parents=True, exist_ok=False)
    TrustContract.cli, TrustContract.fixture, TrustContract.root = args.cli.absolute(), fixture, args.work_root.absolute()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TrustContract)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
