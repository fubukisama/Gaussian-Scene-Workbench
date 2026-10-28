"""Release gate CLI contracts; execute on remote CI, never grant publishing rights."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("verify_native_release_gate.py")


class NativeReleaseGateTests(unittest.TestCase):
    def test_unapproved_policy_fails_closed_before_github_access(self):
        with tempfile.TemporaryDirectory(prefix="gsw-release-gate-") as temporary:
            policy = Path(temporary) / "policy.json"
            policy.write_text(json.dumps({"schema": "gsw.release_acceptance_policy/1",
                "approved": False}), encoding="utf-8")
            environment = dict(os.environ, PATH="")
            environment.pop("GH_TOKEN", None)
            environment.pop("GITHUB_TOKEN", None)
            result = subprocess.run([sys.executable, "-I", "-B", str(SCRIPT),
                "--policy", str(policy), "--source-commit", "a" * 40,
                "--source-tree", "b" * 40], capture_output=True, text=True,
                encoding="utf-8", timeout=15, env=environment)
            self.assertNotEqual(result.returncode, 0)
            response = json.loads(result.stdout)
            self.assertFalse(response["ok"])
            self.assertEqual(response["code"], "release_policy_not_approved")
            self.assertNotIn("token", response)


if __name__ == "__main__":
    unittest.main()
