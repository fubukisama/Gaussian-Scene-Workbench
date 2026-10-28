"""Fail-closed release eligibility inspection, never a signing/publication command.

Developer/CI tooling only. Production is disabled until a complete approved policy
and independently pinned policy identity are configured by an authorized operator.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import quote


POLICY_PATH = "packaging/updates/release-policy.json"
MAX_POLICY = 256 * 1024
MAX_API_JSON = 16 * 1024**2
PRODUCT = "Fubukisama.GaussianSceneWorkbench.Native"
WORKFLOW_PATH = re.compile(r"\.github/workflows/[^/\\]+\.ya?ml\Z")


class ReleaseGateError(ValueError):
    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ReleaseGateError("release_policy_duplicate_key")
        result[key] = value
    return result


def _decode(raw: bytes):
    try:
        return json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_pairs,
            parse_constant=lambda _: (_ for _ in ()).throw(ReleaseGateError("release_json_nonfinite")))
    except (UnicodeError, json.JSONDecodeError):
        raise ReleaseGateError("release_json_invalid") from None


def _positive(value):
    return type(value) is int and 0 < value <= 9_007_199_254_740_991


class GitHubEvidence:
    """Official, read-only GitHub API through the existing authenticated CLI.

    There is intentionally no caller-selectable hostname, API base, token output,
    fake-evidence CLI mode, mutation method or secrets endpoint.
    """
    def __init__(self, repository: str):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ReleaseGateError("release_repository_invalid")
        self.repository_name = repository
        self.base = "repos/" + repository
        self._trees = {}

    def _get(self, path: str):
        try:
            result = subprocess.run(["gh", "api", "--hostname", "github.com",
                "-H", "Accept: application/vnd.github+json", path],
                capture_output=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            raise ReleaseGateError("release_github_unavailable") from None
        if result.returncode != 0:
            # Never forward raw stderr/credentialled redirect URLs to a receipt.
            raise ReleaseGateError("release_github_evidence_unreadable")
        if len(result.stdout) > MAX_API_JSON:
            raise ReleaseGateError("release_github_response_too_large")
        return _decode(result.stdout)

    def repository(self):
        return self._get(self.base)

    def main_ref(self):
        return self._get(self.base + "/git/ref/heads/main")

    def commit(self, sha):
        return self._get(self.base + "/commits/" + sha)

    def source_file(self, path, ref):
        data = self._get(self.base + "/contents/" + quote(path, safe="/") + "?ref=" + ref)
        if (not isinstance(data, dict) or data.get("type") != "file"
                or data.get("encoding") != "base64" or type(data.get("size")) is not int
                or not 0 <= data["size"] <= MAX_POLICY or not isinstance(data.get("content"), str)):
            raise ReleaseGateError("release_source_policy_unavailable")
        try:
            raw = base64.b64decode("".join(data["content"].split()), validate=True)
        except (ValueError, TypeError):
            raise ReleaseGateError("release_source_policy_unavailable") from None
        if len(raw) != data["size"]:
            raise ReleaseGateError("release_source_policy_unavailable")
        return raw

    def source_tree(self, tree):
        if tree not in self._trees:
            data = self._get(self.base + "/git/trees/" + tree + "?recursive=1")
            if (not isinstance(data, dict) or data.get("sha") != tree
                    or data.get("truncated") is not False or not isinstance(data.get("tree"), list)
                    or len(data["tree"]) > 100_000):
                raise ReleaseGateError("release_source_tree_incomplete")
            self._trees[tree] = data["tree"]
        return self._trees[tree]

    def workflow_files(self, tree):
        result = {}
        for entry in self.source_tree(tree):
            path = entry.get("path", "")
            if WORKFLOW_PATH.fullmatch(path):
                if (entry.get("type") != "blob" or entry.get("mode") not in ("100644", "100755")
                        or not re.fullmatch(r"[a-f0-9]{40}", entry.get("sha", "")) or path in result):
                    raise ReleaseGateError("release_workflow_inventory_invalid")
                result[path] = entry["sha"]
        return result


def _source_and_inventory(policy, raw, source_commit, source_tree, github):
    repository = github.repository()
    if (not isinstance(repository, dict) or repository.get("id") != policy.get("repositoryId")
            or not _positive(policy.get("repositoryId"))
            or repository.get("full_name", "").casefold() != policy.get("repository", "").casefold()
            or repository.get("default_branch") != "main"):
        raise ReleaseGateError("release_repository_identity_differs")
    ref = github.main_ref()
    commit = github.commit(source_commit)
    if (ref.get("object", {}).get("sha") != source_commit or commit.get("sha") != source_commit
            or commit.get("commit", {}).get("tree", {}).get("sha") != source_tree):
        raise ReleaseGateError("release_source_not_current_main")
    if github.source_file(POLICY_PATH, source_commit) != raw:
        raise ReleaseGateError("release_source_policy_differs")
    inventory = policy.get("workflowInventory")
    if not isinstance(inventory, dict) or not 1 <= len(inventory) <= 256:
        raise ReleaseGateError("release_workflow_inventory_invalid")
    expected = {}
    required = set()
    for path, row in inventory.items():
        if (not WORKFLOW_PATH.fullmatch(path) or not isinstance(row, dict)
                or not re.fullmatch(r"[a-f0-9]{40}", row.get("blobSha", ""))
                or row.get("scope") not in ("required", "diagnostic", "not_applicable")):
            raise ReleaseGateError("release_workflow_inventory_invalid")
        if row["scope"] == "required":
            required.add(path)
        elif not isinstance(row.get("reason"), str) or not row["reason"].strip():
            raise ReleaseGateError("release_workflow_inventory_invalid")
        expected[path] = row["blobSha"]
    if github.workflow_files(source_tree) != expected:
        raise ReleaseGateError("release_workflow_inventory_changed")
    jobs = policy.get("requiredJobs")
    if not isinstance(jobs, list) or not 1 <= len(jobs) <= 256 or not required:
        raise ReleaseGateError("release_required_checks_empty")
    identities, job_workflows = set(), set()
    for job in jobs:
        if (not isinstance(job, dict) or job.get("workflowPath") not in required
                or not isinstance(job.get("checkName"), str) or not job["checkName"].strip()
                or len(job["checkName"]) > 256 or job.get("appSlug") != "github-actions"
                or not _positive(job.get("appId")) or not isinstance(job.get("allowedEvents"), list)
                or not job["allowedEvents"] or not set(job["allowedEvents"]) <= {"push", "workflow_dispatch"}):
            raise ReleaseGateError("release_required_check_identity_invalid")
        identity = (job["workflowPath"], job["checkName"])
        if identity in identities:
            raise ReleaseGateError("release_required_check_identity_invalid")
        identities.add(identity)
        job_workflows.add(job["workflowPath"])
    if required != job_workflows:
        raise ReleaseGateError("release_required_checks_incomplete")


def inspect_release(policy_path: Path, source_commit: str, source_tree: str,
                    approved_policy_sha256: str | None = None, *, github=None) -> dict:
    if not re.fullmatch(r"[a-f0-9]{40}", source_commit) or not re.fullmatch(r"[a-f0-9]{40}", source_tree):
        raise ReleaseGateError("release_source_identity_invalid")
    if policy_path.is_symlink() or not policy_path.is_file() or policy_path.stat().st_size > MAX_POLICY:
        raise ReleaseGateError("release_policy_unavailable")
    try:
        raw = policy_path.read_bytes()
        policy = _decode(raw)
    except (UnicodeError, json.JSONDecodeError):
        raise ReleaseGateError("release_policy_invalid") from None
    if not isinstance(policy, dict) or policy.get("schema") != "gsw.release_acceptance_policy/1":
        raise ReleaseGateError("release_policy_invalid")
    if policy.get("approved") is not True:
        raise ReleaseGateError("release_policy_not_approved")
    if (not isinstance(approved_policy_sha256, str)
            or not re.fullmatch(r"[a-f0-9]{64}", approved_policy_sha256)
            or hashlib.sha256(raw).hexdigest() != approved_policy_sha256):
        raise ReleaseGateError("release_policy_identity_not_pinned")
    if policy.get("product") != PRODUCT or policy.get("platform") != "windows-x64":
        raise ReleaseGateError("release_product_identity_invalid")
    evidence = github if github is not None else GitHubEvidence(policy.get("repository", ""))
    _source_and_inventory(policy, raw, source_commit, source_tree, evidence)
    # Each following gate is added only after its real remote failing contract.
    # No intermediate source-only implementation can authorize publication.
    raise ReleaseGateError("release_gate_evidence_not_available")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--approved-policy-sha256")
    arguments = parser.parse_args()
    try:
        result = inspect_release(arguments.policy, arguments.source_commit,
            arguments.source_tree, arguments.approved_policy_sha256)
        print(json.dumps(result))
        return 0
    except ReleaseGateError as error:
        print(json.dumps({"schema": "gsw.release_gate_result/1", "ok": False, "code": error.code}))
        return 2
    except OSError:
        print(json.dumps({"schema": "gsw.release_gate_result/1", "ok": False,
                          "code": "release_evidence_unavailable"}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
