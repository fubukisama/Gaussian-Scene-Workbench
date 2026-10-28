"""Release gate CLI contracts; execute on remote CI, never grant publishing rights."""
import json
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("verify_native_release_gate.py")
SPEC = importlib.util.spec_from_file_location("gsw_release_gate", SCRIPT)
GATE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


class GitHubSourceFixture:
    """Read-only GitHub boundary, not an imitation of gate internals."""
    def __init__(self, policy_bytes, *, extra_workflow=True):
        self.policy_bytes = policy_bytes
        self.extra_workflow = extra_workflow

    def repository(self):
        return {"id": 7, "full_name": "fixture-owner/fixture-repo", "default_branch": "main"}

    def main_ref(self):
        return {"object": {"sha": "a" * 40}}

    def commit(self, sha):
        return {"sha": "a" * 40, "commit": {"tree": {"sha": "b" * 40}}}

    def source_file(self, path, ref):
        return self.policy_bytes

    def workflow_files(self, tree):
        result = {".github/workflows/build.yml": "c" * 40}
        if self.extra_workflow:
            result[".github/workflows/new-check.yml"] = "d" * 40
        return result

    def classic_protection(self):
        return {"required_status_checks": {"checks": [
            {"context": "Security scan not reviewed in the policy", "app_id": 15368}]}}

    def branch_rules(self):
        return []


def approved_policy():
    return {"schema": "gsw.release_acceptance_policy/1", "approved": True,
        "policyId": "isolated-policy", "repository": "fixture-owner/fixture-repo",
        "repositoryId": 7, "product": "Fubukisama.GaussianSceneWorkbench.Native",
        "platform": "windows-x64", "requiredJobs": [
            {"workflowPath": ".github/workflows/build.yml", "checkName": "Windows build",
             "appSlug": "github-actions", "appId": 15368, "allowedEvents": ["push"]}],
        "requiredStatuses": [], "workflowInventory": {
            ".github/workflows/build.yml": {"blobSha": "c" * 40, "scope": "required"}}}


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

    def test_approved_policy_does_not_ignore_an_unreviewed_workflow(self):
        policy = approved_policy()
        raw = json.dumps(policy, sort_keys=True).encode("utf-8")
        with tempfile.TemporaryDirectory(prefix="gsw-release-gate-") as temporary:
            path = Path(temporary) / "policy.json"
            path.write_bytes(raw)
            with self.assertRaises(GATE.ReleaseGateError) as caught:
                GATE.inspect_release(path, "a" * 40, "b" * 40,
                    hashlib.sha256(raw).hexdigest(), github=GitHubSourceFixture(raw))
            self.assertEqual(caught.exception.code, "release_workflow_inventory_changed")

    def test_protection_cannot_add_an_unbound_required_check(self):
        raw = json.dumps(approved_policy(), sort_keys=True).encode("utf-8")
        with tempfile.TemporaryDirectory(prefix="gsw-release-gate-") as temporary:
            path = Path(temporary) / "policy.json"
            path.write_bytes(raw)
            with self.assertRaises(GATE.ReleaseGateError) as caught:
                GATE.inspect_release(path, "a" * 40, "b" * 40,
                    hashlib.sha256(raw).hexdigest(),
                    github=GitHubSourceFixture(raw, extra_workflow=False))
            self.assertEqual(caught.exception.code, "release_required_check_unbound")


if __name__ == "__main__":
    unittest.main()
