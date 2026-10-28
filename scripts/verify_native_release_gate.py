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
from urllib.parse import urlsplit


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

    def _get(self, path: str, *, explicit_unprotected=False):
        try:
            result = subprocess.run(["gh", "api", "--hostname", "github.com",
                "-H", "Accept: application/vnd.github+json", path],
                capture_output=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            raise ReleaseGateError("release_github_unavailable") from None
        if result.returncode != 0:
            if explicit_unprotected and len(result.stdout) <= MAX_API_JSON:
                try:
                    error = _decode(result.stdout)
                    if (isinstance(error, dict) and error.get("message") == "Branch not protected"
                            and error.get("status") in (404, "404")):
                        return None
                except ReleaseGateError:
                    pass
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

    def classic_protection(self):
        return self._get(self.base + "/branches/main/protection", explicit_unprotected=True)

    def branch_rules(self):
        result = []
        for page in range(1, 11):
            rows = self._get(self.base + f"/rules/branches/main?per_page=100&page={page}")
            if not isinstance(rows, list) or len(rows) > 100:
                raise ReleaseGateError("release_protection_evidence_invalid")
            result.extend(rows)
            if len(rows) < 100:
                return result
        raise ReleaseGateError("release_protection_evidence_incomplete")

    def _pages(self, path, key=None):
        result = []
        for page in range(1, 11):
            separator = "&" if "?" in path else "?"
            data = self._get(path + f"{separator}per_page=100&page={page}")
            rows = data.get(key) if key and isinstance(data, dict) else data
            if (not isinstance(rows, list) or len(rows) > 100
                    or (key and type(data.get("total_count")) is int and data["total_count"] > 1000)):
                raise ReleaseGateError("release_github_list_incomplete")
            result.extend(rows)
            if len(rows) < 100:
                if key and type(data.get("total_count")) is int and len(result) != data["total_count"]:
                    raise ReleaseGateError("release_github_list_incomplete")
                return result
        raise ReleaseGateError("release_github_list_incomplete")

    def workflow(self, path):
        return self._get(self.base + "/actions/workflows/" + quote(path.rsplit("/", 1)[-1], safe=""))

    def workflow_runs(self, workflow_id, sha):
        return self._pages(self.base + f"/actions/workflows/{workflow_id}/runs?head_sha={sha}&branch=main",
                           "workflow_runs")

    def run(self, run_id):
        return self._get(self.base + f"/actions/runs/{run_id}")

    def jobs(self, run_id, attempt):
        return self._pages(self.base + f"/actions/runs/{run_id}/attempts/{attempt}/jobs", "jobs")

    def check_run(self, check_id):
        return self._get(self.base + f"/check-runs/{check_id}")

    def statuses(self, sha):
        return self._pages(self.base + f"/commits/{sha}/statuses")


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


def _protection_union(policy, github):
    """Policy is a minimum, never a substitute for current effective rules."""
    protected = []
    classic = github.classic_protection()
    if classic is not None:
        if not isinstance(classic, dict):
            raise ReleaseGateError("release_protection_evidence_invalid")
        checks = classic.get("required_status_checks")
        if checks is not None:
            if not isinstance(checks, dict):
                raise ReleaseGateError("release_protection_evidence_invalid")
            rows, contexts = checks.get("checks", []), checks.get("contexts", [])
            if not isinstance(rows, list) or not isinstance(contexts, list):
                raise ReleaseGateError("release_protection_evidence_invalid")
            for row in rows:
                if not isinstance(row, dict):
                    raise ReleaseGateError("release_protection_evidence_invalid")
                protected.append((row.get("context"), row.get("app_id")))
            for context in contexts:
                protected.append((context, None))
        deployments = classic.get("required_deployments")
        if deployments and (not isinstance(deployments, dict) or deployments.get("environments")):
            raise ReleaseGateError("release_required_deployment_unbound")
    rules = github.branch_rules()
    if not isinstance(rules, list) or len(rules) > 1000:
        raise ReleaseGateError("release_protection_evidence_invalid")
    for rule in rules:
        if not isinstance(rule, dict):
            raise ReleaseGateError("release_protection_evidence_invalid")
        kind, parameters = rule.get("type"), rule.get("parameters", {})
        if kind == "required_status_checks":
            if not isinstance(parameters, dict) or not isinstance(parameters.get("required_status_checks"), list):
                raise ReleaseGateError("release_protection_evidence_invalid")
            for row in parameters["required_status_checks"]:
                if not isinstance(row, dict):
                    raise ReleaseGateError("release_protection_evidence_invalid")
                protected.append((row.get("context"), row.get("integration_id")))
        elif kind == "workflows":
            if not isinstance(parameters, dict) or not isinstance(parameters.get("workflows"), list):
                raise ReleaseGateError("release_protection_evidence_invalid")
            for workflow in parameters["workflows"]:
                if (not isinstance(workflow, dict)
                        or workflow.get("repository_id") != policy["repositoryId"]
                        or workflow.get("ref") not in (None, "refs/heads/main")
                        or not any(job["workflowPath"] == workflow.get("path") for job in policy["requiredJobs"])):
                    raise ReleaseGateError("release_required_workflow_unbound")
        elif kind not in {"creation", "update", "deletion", "required_linear_history",
                          "required_signatures", "pull_request", "non_fast_forward",
                          "required_review_thread_resolution", "required_conversation_resolution"}:
            # Scanning/deployment/unknown future gates require a dedicated proof,
            # not a silent assumption that a similarly named job covers them.
            raise ReleaseGateError("release_protection_rule_unbound")
    statuses = policy.get("requiredStatuses")
    if not isinstance(statuses, list) or len(statuses) > 256:
        raise ReleaseGateError("release_status_policy_invalid")
    contexts = set()
    for status in statuses:
        if (not isinstance(status, dict) or not isinstance(status.get("context"), str)
                or not status["context"].strip() or status["context"] in contexts
                or not _positive(status.get("creatorId"))
                or not isinstance(status.get("creatorLogin"), str) or not status["creatorLogin"]):
            raise ReleaseGateError("release_status_policy_invalid")
        contexts.add(status["context"])
    for context, app_id in protected:
        if not isinstance(context, str) or not context or (app_id is not None and not _positive(app_id)):
            raise ReleaseGateError("release_protection_evidence_invalid")
        matches = [job for job in policy["requiredJobs"] if job["checkName"] == context
                   and (app_id is None or job["appId"] == app_id)]
        status_bound = app_id is None and context in contexts
        if len(matches) != 1 and not (not matches and status_bound):
            raise ReleaseGateError("release_required_check_unbound")
    return [{"context": context, "appId": app_id} for context, app_id in protected]


def _select_run(github, policy, workflow_path, requirements, source_commit):
    workflow = github.workflow(workflow_path)
    if (not isinstance(workflow, dict) or workflow.get("path") != workflow_path
            or workflow.get("state") != "active" or not _positive(workflow.get("id"))):
        raise ReleaseGateError("release_workflow_identity_differs")
    events = set.intersection(*(set(job["allowedEvents"]) for job in requirements))
    if not events:
        raise ReleaseGateError("release_workflow_event_policy_invalid")
    rows = github.workflow_runs(workflow["id"], source_commit)
    if not isinstance(rows, list) or len(rows) > 1000:
        raise ReleaseGateError("release_workflow_runs_incomplete")
    eligible = [row for row in rows if isinstance(row, dict)
                and row.get("head_sha") == source_commit and row.get("head_branch") == "main"
                and row.get("event") in events]
    if not eligible or any(not _positive(row.get("id")) for row in eligible):
        raise ReleaseGateError("release_required_run_missing")
    selected = max(eligible, key=lambda row: row["id"])
    run = github.run(selected["id"])
    if (not isinstance(run, dict) or run.get("id") != selected["id"]
            or run.get("workflow_id") != workflow["id"] or run.get("path") != workflow_path
            or run.get("head_sha") != source_commit or run.get("head_branch") != "main"
            or run.get("event") not in events or not _positive(run.get("run_attempt"))
            or run.get("repository", {}).get("id") != policy["repositoryId"]
            or run.get("head_repository", {}).get("id") != policy["repositoryId"]
            or not _positive(run.get("check_suite_id"))):
        raise ReleaseGateError("release_run_identity_differs")
    if run.get("status") != "completed" or run.get("conclusion") != "success":
        raise ReleaseGateError("release_required_run_not_success")
    return run


def _check_run_id(url, repository):
    if not isinstance(url, str):
        raise ReleaseGateError("release_check_run_identity_invalid")
    parsed = urlsplit(url)
    path = re.fullmatch(r"/repos/" + re.escape(repository) + r"/check-runs/([0-9]+)", parsed.path, re.I)
    if (parsed.scheme != "https" or parsed.hostname != "api.github.com" or parsed.port is not None
            or parsed.username or parsed.password or parsed.query or parsed.fragment or not path):
        raise ReleaseGateError("release_check_run_identity_invalid")
    value = int(path.group(1))
    if not _positive(value):
        raise ReleaseGateError("release_check_run_identity_invalid")
    return value


def _required_results(policy, source_commit, github):
    groups = {}
    for job in policy["requiredJobs"]:
        groups.setdefault(job["workflowPath"], []).append(job)
    verified = {}
    for path, requirements in sorted(groups.items()):
        run = _select_run(github, policy, path, requirements, source_commit)
        rows = github.jobs(run["id"], run["run_attempt"])
        if not isinstance(rows, list) or len(rows) > 1000:
            raise ReleaseGateError("release_jobs_incomplete")
        jobs = {}
        for requirement in requirements:
            matches = [row for row in rows if isinstance(row, dict) and row.get("name") == requirement["checkName"]]
            if len(matches) != 1:
                raise ReleaseGateError("release_required_job_missing_or_ambiguous")
            job = matches[0]
            if job.get("run_attempt") != run["run_attempt"] or job.get("run_id") != run["id"]:
                raise ReleaseGateError("release_job_attempt_differs")
            if job.get("head_sha") != source_commit or not _positive(job.get("id")):
                raise ReleaseGateError("release_job_source_differs")
            if job.get("status") != "completed" or job.get("conclusion") != "success":
                raise ReleaseGateError("release_required_job_not_success")
            check_id = _check_run_id(job.get("check_run_url"), policy["repository"])
            check = github.check_run(check_id)
            if (not isinstance(check, dict) or check.get("id") != check_id
                    or check.get("name") != requirement["checkName"] or check.get("head_sha") != source_commit
                    or check.get("check_suite", {}).get("id") != run["check_suite_id"]
                    or check.get("app", {}).get("slug") != "github-actions"
                    or check.get("app", {}).get("id") != requirement["appId"]
                    or requirement["appId"] != 15368):
                raise ReleaseGateError("release_check_publisher_identity_differs")
            if check.get("status") != "completed" or check.get("conclusion") != "success":
                raise ReleaseGateError("release_required_check_not_success")
            jobs[requirement["checkName"]] = {"jobId": job["id"], "checkRunId": check_id}
        verified[path] = {"run": run, "jobs": jobs}
    statuses = []
    if policy["requiredStatuses"]:
        rows = github.statuses(source_commit)
        if not isinstance(rows, list) or len(rows) > 1000:
            raise ReleaseGateError("release_statuses_incomplete")
        for requirement in policy["requiredStatuses"]:
            matches = [row for row in rows if isinstance(row, dict) and row.get("context") == requirement["context"]]
            if not matches or any(not _positive(row.get("id")) for row in matches):
                raise ReleaseGateError("release_required_status_missing")
            status = max(matches, key=lambda row: row["id"])
            if (status.get("creator", {}).get("id") != requirement["creatorId"]
                    or status.get("creator", {}).get("login", "").casefold() != requirement["creatorLogin"].casefold()
                    or status.get("state") != "success"):
                raise ReleaseGateError("release_required_status_not_success")
            statuses.append({"context": requirement["context"], "id": status["id"]})
    return verified, statuses


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
    _protection_union(policy, evidence)
    _required_results(policy, source_commit, evidence)
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
