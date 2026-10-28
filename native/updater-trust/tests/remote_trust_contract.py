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
        result = subprocess.run([str(self.cli), "-"], input=json.dumps(request).encode(),
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
