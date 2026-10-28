"""Release gate CLI contracts; execute on remote CI, never grant publishing rights."""
import json
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


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


class GitHubRunFixture(GitHubSourceFixture):
    def __init__(self, policy_bytes):
        super().__init__(policy_bytes, extra_workflow=False)

    def classic_protection(self):
        return {"required_status_checks": {"checks": [{"context": "Windows build", "app_id": 15368}]}}

    def workflow(self, path):
        return {"id": 9, "path": ".github/workflows/build.yml", "state": "active"}

    def workflow_runs(self, workflow_id, sha):
        return [self.run(100), dict(self.run(99), run_attempt=1)]

    def run(self, run_id):
        return {"id": run_id, "workflow_id": 9, "run_attempt": 2, "run_number": run_id,
            "status": "completed", "conclusion": "success", "event": "push",
            "head_branch": "main", "head_sha": "a" * 40,
            "path": ".github/workflows/build.yml", "check_suite_id": 30,
            "repository": {"id": 7, "full_name": "fixture-owner/fixture-repo"},
            "head_repository": {"id": 7, "full_name": "fixture-owner/fixture-repo"},
            "run_started_at": "2026-10-28T08:00:00Z", "updated_at": "2026-10-28T08:01:00Z"}

    def jobs(self, run_id, attempt):
        return [{"id": 20, "name": "Windows build", "status": "completed", "conclusion": "success",
            "run_id": run_id, "run_attempt": 1, "head_sha": "a" * 40,
            "check_run_url": "https://api.github.com/repos/fixture-owner/fixture-repo/check-runs/20"}]


class GitHubCandidateFixture(GitHubRunFixture):
    def __init__(self, policy_bytes, wrapper, tracked):
        super().__init__(policy_bytes)
        self.wrapper, self.tracked = wrapper, tracked

    def jobs(self, run_id, attempt):
        return [dict(row, run_attempt=attempt) for row in super().jobs(run_id, attempt)]

    def check_run(self, check_id):
        return {"id": 20, "name": "Windows build", "head_sha": "a" * 40,
            "check_suite": {"id": 30}, "app": {"id": 15368, "slug": "github-actions"},
            "status": "completed", "conclusion": "success"}

    def workflow_files(self, tree):
        return {row["path"]: row["blobSha"] for row in self.tracked if row["path"].startswith(".github/workflows/")}

    def source_tree(self, tree):
        return [{"path": row["path"], "sha": row["blobSha"], "size": row["size"],
                 "type": "blob", "mode": "100644"} for row in self.tracked]

    def artifact(self, artifact_id):
        return {"id": 55, "name": "native-update-candidate-" + "a" * 40 + "-100-2",
            "expired": False, "size_in_bytes": self.wrapper.stat().st_size,
            "digest": "sha256:" + hashlib.sha256(self.wrapper.read_bytes()).hexdigest(),
            "created_at": "2026-10-28T08:00:30Z", "updated_at": "2026-10-28T08:00:31Z",
            "workflow_run": {"id": 100, "head_sha": "a" * 40, "head_branch": "main",
                             "repository_id": 7, "head_repository_id": 7}}

    def download_artifact(self, artifact_id, destination):
        shutil.copyfile(self.wrapper, destination)


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

    def test_latest_attempt_cannot_reuse_an_old_green_job(self):
        raw = json.dumps(approved_policy(), sort_keys=True).encode("utf-8")
        with tempfile.TemporaryDirectory(prefix="gsw-release-gate-") as temporary:
            path = Path(temporary) / "policy.json"
            path.write_bytes(raw)
            with self.assertRaises(GATE.ReleaseGateError) as caught:
                GATE.inspect_release(path, "a" * 40, "b" * 40,
                    hashlib.sha256(raw).hexdigest(), github=GitHubRunFixture(raw))
            self.assertEqual(caught.exception.code, "release_job_attempt_differs")

    def test_exact_current_candidate_binds_outer_artifact_inner_archive_and_full_source(self):
        with tempfile.TemporaryDirectory(prefix="gsw-release-gate-") as temporary:
            root = Path(temporary)
            workflow = b"name: isolated build\non: workflow_dispatch\njobs: {}\n"
            # Git itself is the independent source of the expected blob IDs.
            def blob(data):
                return subprocess.check_output(["git", "hash-object", "--stdin"], input=data,
                                               timeout=10).decode("ascii").strip()
            policy = approved_policy()
            policy["workflowInventory"][".github/workflows/build.yml"]["blobSha"] = blob(workflow)
            policy["artifact"] = {"workflowPath": ".github/workflows/build.yml", "jobName": "Windows build",
                "namePrefix": "native-update-candidate", "archiveMember": "candidate.zip", "reportMember": "candidate.json"}
            raw = json.dumps(policy, sort_keys=True).encode("utf-8")
            source = {"native/CMakeLists.txt": b"# isolated source, no executable code\n",
                      ".github/workflows/build.yml": workflow,
                      "packaging/updates/release-policy.json": raw}
            tracked = [{"path": name, "blobSha": blob(data), "size": len(data)} for name, data in sorted(source.items())]
            payload = {"bin/Gaussian Scene Workbench.exe": b"owned nonexecuted release gate fixture",
                "COPYING": b"isolated GPL notice", "LICENSE": b"isolated GPL notice",
                "THIRD_PARTY_LICENSES.md": b"isolated notices", "native/LICENSE.md": b"GPL-3.0-or-later",
                "build_manifest.json": b'{"product":"Gaussian Scene Workbench","legacyWebRuntimeIncluded":false}',
                "backend_manifest.json": b'{"schema":"isolated.release-gate/1"}',
                **{"source/" + name: data for name, data in source.items()}}
            payload["source-manifest.json"] = json.dumps({"schema": "gsw-native-corresponding-source-v1",
                "license": "GPL-3.0-or-later",
                "executableSha256": hashlib.sha256(payload["bin/Gaussian Scene Workbench.exe"]).hexdigest(),
                "sourceFileCount": len(source), "files": [{"path": name, "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest()} for name, data in sorted(source.items())]}).encode()
            payload["source-provenance.json"] = json.dumps({"schema": "gsw.native_update_source_provenance/1",
                "sourceCommit": "a" * 40, "sourceTree": "b" * 40,
                "trackedFiles": tracked, "generatedDependencies": []}, sort_keys=True).encode()
            package = root / "payload"
            for name, data in payload.items():
                path = package / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            archive = root / "candidate.zip"
            made = subprocess.run([sys.executable, "-I", "-B", str(SCRIPT.with_name("package_native_update.py")),
                "--package-root", str(package), "--output-archive", str(archive), "--source-commit", "a" * 40,
                "--source-tree", "b" * 40, "--build-id", "isolated-release-1", "--display-version", "1.0",
                "--release-sequence", "1"], capture_output=True, text=True, encoding="utf-8", timeout=30)
            self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
            report = json.loads(made.stdout)
            report["buildIdentity"] = {"repositoryId": 7, "repository": "fixture-owner/fixture-repo",
                "workflowId": 9, "workflowPath": ".github/workflows/build.yml", "runId": 100,
                "runAttempt": 2, "event": "push", "headBranch": "main"}
            wrapper = root / "gha-artifact.zip"
            with zipfile.ZipFile(wrapper, "w", compression=zipfile.ZIP_DEFLATED) as zipped:
                zipped.write(archive, "candidate.zip")
                zipped.writestr("candidate.json", json.dumps(report, sort_keys=True))
            policy_path = root / "policy.json"
            policy_path.write_bytes(raw)
            result = GATE.inspect_release(policy_path, "a" * 40, "b" * 40, hashlib.sha256(raw).hexdigest(),
                github=GitHubCandidateFixture(raw, wrapper, tracked), artifact_id=55, archive_path=archive)
            self.assertTrue(result["ok"])
            self.assertEqual(result["code"], "release_eligible")
            self.assertEqual(result["artifactId"], 55)
            self.assertEqual(result["archiveSha256"], report["archiveSha256"])
            self.assertTrue(result["eligibilityOnly"])
            self.assertEqual(result["repositoryId"], 7)
            self.assertEqual(result["artifactName"], "native-update-candidate-" + "a" * 40 + "-100-2")
            workflow_result = result["workflows"][0]
            self.assertEqual(workflow_result["workflowId"], 9)
            self.assertEqual(workflow_result["event"], "push")
            self.assertEqual(workflow_result["headSha"], "a" * 40)
            self.assertEqual(workflow_result["headBranch"], "main")
            self.assertEqual(workflow_result["jobs"]["Windows build"]["appId"], 15368)
            self.assertEqual(workflow_result["jobs"]["Windows build"]["appSlug"], "github-actions")


if __name__ == "__main__":
    unittest.main()
