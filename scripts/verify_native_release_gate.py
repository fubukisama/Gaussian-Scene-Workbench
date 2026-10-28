"""Fail-closed release eligibility inspection, never a signing/publication command.

Developer/CI tooling only. Production is disabled until a complete approved policy
and independently pinned policy identity are configured by an authorized operator.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import time
from urllib.parse import quote
from urllib.parse import urlsplit
import zipfile


POLICY_PATH = "packaging/updates/release-policy.json"
MAX_POLICY = 256 * 1024
MAX_API_JSON = 16 * 1024**2
MAX_ARCHIVE = 4 * 1024**3
MAX_ARTIFACT = MAX_ARCHIVE + 256 * 1024**2
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

    def artifact(self, artifact_id):
        return self._get(self.base + f"/actions/artifacts/{artifact_id}")

    def download_artifact(self, artifact_id, destination):
        # gh owns authenticated API redirects. No token is retrieved or copied
        # into our CLI arguments, logs or cross-host headers.
        with destination.open("xb") as output:
            process = subprocess.Popen(["gh", "api", "--hostname", "github.com", "--allow-escape-sequences",
                self.base + f"/actions/artifacts/{artifact_id}/zip"], stdout=output,
                stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            try:
                deadline = time.monotonic() + 600
                while process.poll() is None:
                    if destination.stat().st_size > MAX_ARTIFACT or time.monotonic() > deadline:
                        raise ReleaseGateError("release_artifact_download_limit")
                    time.sleep(.1)
                if process.returncode or destination.stat().st_size > MAX_ARTIFACT:
                    raise ReleaseGateError("release_artifact_download_failed")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)


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
            jobs[requirement["checkName"]] = {"jobId": job["id"], "checkRunId": check_id,
                "appId": check["app"]["id"], "appSlug": check["app"]["slug"]}
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
            statuses.append({"context": requirement["context"], "id": status["id"],
                "creatorId": status["creator"]["id"], "creatorLogin": status["creator"]["login"]})
    return verified, statuses


def _sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _instant(value):
    try:
        if not isinstance(value, str) or len(value) > 64:
            raise ValueError
        result = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if result.tzinfo is None:
            raise ValueError
        return result.astimezone(timezone.utc)
    except ValueError:
        raise ReleaseGateError("release_evidence_timestamp_invalid") from None


def _artifact_metadata(policy, source_commit, artifact_id, verified, github):
    producer = policy.get("artifact")
    if (not isinstance(producer, dict) or set(producer) != {"workflowPath", "jobName", "namePrefix", "archiveMember", "reportMember"}
            or producer["workflowPath"] not in verified
            or producer["jobName"] not in verified[producer["workflowPath"]]["jobs"]
            or not isinstance(producer["namePrefix"], str)
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}", producer["namePrefix"])
            or producer["archiveMember"] != "candidate.zip" or producer["reportMember"] != "candidate.json"):
        raise ReleaseGateError("release_artifact_policy_invalid")
    run = verified[producer["workflowPath"]]["run"]
    artifact = github.artifact(artifact_id)
    expected_name = f"{producer['namePrefix']}-{source_commit}-{run['id']}-{run['run_attempt']}"
    if (not isinstance(artifact, dict) or artifact.get("id") != artifact_id
            or artifact.get("name") != expected_name or artifact.get("expired") is not False
            or type(artifact.get("size_in_bytes")) is not int or not 0 < artifact["size_in_bytes"] <= MAX_ARTIFACT
            or not isinstance(artifact.get("digest"), str)
            or not re.fullmatch(r"sha256:[a-f0-9]{64}", artifact["digest"])
            or artifact.get("workflow_run", {}).get("id") != run["id"]
            or artifact.get("workflow_run", {}).get("head_sha") != source_commit
            or artifact.get("workflow_run", {}).get("head_branch") != "main"
            or artifact.get("workflow_run", {}).get("repository_id") != policy["repositoryId"]
            or artifact.get("workflow_run", {}).get("head_repository_id") != policy["repositoryId"]):
        raise ReleaseGateError("release_artifact_identity_differs")
    if not (_instant(run.get("run_started_at")) <= _instant(artifact.get("created_at"))
            <= _instant(artifact.get("updated_at")) <= _instant(run.get("updated_at"))):
        raise ReleaseGateError("release_artifact_attempt_differs")
    return artifact, run


def _package_module():
    spec = importlib.util.spec_from_file_location("gsw_release_archive", Path(__file__).with_name("package_native_update.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _artifact_payload(policy, source_commit, source_tree, artifact_id, archive_path, verified, github):
    if not _positive(artifact_id) or not isinstance(archive_path, Path):
        raise ReleaseGateError("release_artifact_input_missing")
    package = _package_module()
    try:
        info = package._ordinary(archive_path.absolute())
    except (OSError, package.ArchiveError):
        raise ReleaseGateError("release_candidate_archive_unavailable") from None
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or not 0 < info.st_size <= MAX_ARCHIVE:
        raise ReleaseGateError("release_candidate_archive_invalid")
    local_sha = _sha256(archive_path)
    artifact, run = _artifact_metadata(policy, source_commit, artifact_id, verified, github)
    with tempfile.TemporaryDirectory(prefix="gsw-release-evidence-", dir=archive_path.absolute().parent) as temporary:
        root = Path(temporary)
        wrapper = root / "artifact.zip"
        github.download_artifact(artifact_id, wrapper)
        if (wrapper.is_symlink() or not wrapper.is_file() or wrapper.stat().st_size != artifact["size_in_bytes"]
                or _sha256(wrapper) != artifact["digest"][7:]):
            raise ReleaseGateError("release_artifact_digest_differs")
        inner = root / "candidate.zip"
        with zipfile.ZipFile(wrapper) as bundle:
            rows = bundle.infolist()
            if (len(rows) != 2 or {row.filename for row in rows} != {"candidate.zip", "candidate.json"}
                    or any(row.is_dir() or row.flag_bits & 1 or row.compress_type not in
                           (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) for row in rows)
                    or bundle.getinfo("candidate.json").file_size > 1024**2
                    or not 0 < bundle.getinfo("candidate.zip").file_size <= MAX_ARCHIVE):
                raise ReleaseGateError("release_artifact_members_invalid")
            report = _decode(bundle.read("candidate.json"))
            with bundle.open("candidate.zip") as source, inner.open("xb") as output:
                remaining = MAX_ARCHIVE
                while block := source.read(min(1024**2, remaining + 1)):
                    if len(block) > remaining:
                        raise ReleaseGateError("release_artifact_download_limit")
                    output.write(block)
                    remaining -= len(block)
        expected_build = {"repositoryId": policy["repositoryId"], "repository": policy["repository"],
            "workflowId": run["workflow_id"], "workflowPath": run["path"], "runId": run["id"],
            "runAttempt": run["run_attempt"], "event": run["event"], "headBranch": "main"}
        if (not isinstance(report, dict) or report.get("schema") != "gsw.native_update_package/1"
                or report.get("product") != PRODUCT or report.get("platform") != "windows-x64"
                or report.get("sourceCommit") != source_commit or report.get("sourceTree") != source_tree
                or not isinstance(report.get("buildIdentity"), dict)
                or any(type(report["buildIdentity"].get(key)) is not int
                       for key in ("repositoryId", "workflowId", "runId", "runAttempt"))
                or report.get("buildIdentity") != expected_build
                or not isinstance(report.get("buildId"), str)
                or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}", report["buildId"])
                or not isinstance(report.get("displayVersion"), str) or not report["displayVersion"].strip()
                or len(report["displayVersion"]) > 128 or any(ord(c) < 32 for c in report["displayVersion"])
                or not _positive(report.get("releaseSequence"))
                or type(report.get("archiveLength")) is not int or report["archiveLength"] != info.st_size
                or inner.stat().st_size != info.st_size or _sha256(inner) != local_sha
                or report.get("archiveSha256") != local_sha
                or type(report.get("fileCount")) is not int or not 0 < report["fileCount"] <= 100_000
                or type(report.get("unpackedLength")) is not int or not 0 < report["unpackedLength"] <= 16 * 1024**3
                or any(not isinstance(report.get(key), str) or not re.fullmatch(r"[a-f0-9]{64}", report[key])
                       for key in ("payloadManifestSha256", "correspondingSourceSha256"))):
            raise ReleaseGateError("release_candidate_identity_differs")
        try:
            package.validate_relative_path(report["buildId"])
            source_evidence = package.verify_release_archive(inner, report, github.source_tree(source_tree))
        except (package.ArchiveError, zipfile.BadZipFile, OSError, ValueError, TypeError, KeyError):
            raise ReleaseGateError("release_archive_or_source_rejected") from None
    return report, artifact, source_evidence


def inspect_release(policy_path: Path, source_commit: str, source_tree: str,
                    approved_policy_sha256: str | None = None, *, github=None,
                    artifact_id: int | None = None, archive_path: Path | None = None) -> dict:
    if not re.fullmatch(r"[a-f0-9]{40}", source_commit) or not re.fullmatch(r"[a-f0-9]{40}", source_tree):
        raise ReleaseGateError("release_source_identity_invalid")
    if policy_path.is_symlink() or not policy_path.is_file() or policy_path.stat().st_size > MAX_POLICY:
        raise ReleaseGateError("release_policy_unavailable")
    try:
        with policy_path.open("rb") as stream:
            raw = stream.read(MAX_POLICY + 1)
        if len(raw) > MAX_POLICY:
            raise ReleaseGateError("release_policy_unavailable")
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
    protected = _protection_union(policy, evidence)
    verified, statuses = _required_results(policy, source_commit, evidence)
    report, artifact, source_evidence = _artifact_payload(policy, source_commit, source_tree,
        artifact_id, archive_path, verified, evidence)
    # Repeat the live minimum just before issuing an eligibility-only receipt.
    # Publication must re-run this gate; a receipt is not a signing credential.
    _source_and_inventory(policy, raw, source_commit, source_tree, evidence)
    protected_again = _protection_union(policy, evidence)
    verified_again, statuses_again = _required_results(policy, source_commit, evidence)
    artifact_again, _ = _artifact_metadata(policy, source_commit, artifact_id, verified_again, evidence)
    identities = lambda rows: {path: (row["run"]["id"], row["run"]["run_attempt"], row["jobs"])
                               for path, row in rows.items()}
    canonical = lambda value: json.dumps(value, sort_keys=True, separators=(",", ":"))
    if (identities(verified) != identities(verified_again) or canonical(protected) != canonical(protected_again)
            or statuses != statuses_again or artifact_again != artifact):
        raise ReleaseGateError("release_evidence_changed_during_inspection")
    return {"schema": "gsw.release_gate_result/1", "ok": True, "code": "release_eligible",
        "eligibilityOnly": True, "published": False, "signingAuthorized": False,
        "repositoryId": policy["repositoryId"], "repository": policy["repository"],
        "sourceCommit": source_commit, "sourceTree": source_tree, "product": PRODUCT,
        "platform": "windows-x64", "buildId": report["buildId"], "displayVersion": report["displayVersion"],
        "releaseSequence": report["releaseSequence"], "artifactId": artifact_id, "artifactName": artifact["name"],
        "artifactDigest": artifact["digest"], "artifactCreatedAt": artifact["created_at"], "archiveLength": report["archiveLength"],
        "archiveSha256": report["archiveSha256"], "payloadManifestSha256": report["payloadManifestSha256"],
        "correspondingSourceSha256": report["correspondingSourceSha256"],
        "policySha256": approved_policy_sha256, "sourceEvidence": source_evidence,
        "protectedChecks": protected, "statuses": statuses,
        "workflows": [{"path": path, "runId": row["run"]["id"], "attempt": row["run"]["run_attempt"],
                       "workflowId": row["run"]["workflow_id"], "event": row["run"]["event"],
                       "headSha": row["run"]["head_sha"], "headBranch": row["run"]["head_branch"],
                       "jobs": row["jobs"]} for path, row in sorted(verified.items())],
        "verifiedAt": datetime.now(timezone.utc).isoformat()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--approved-policy-sha256")
    parser.add_argument("--artifact-id", type=int)
    parser.add_argument("--archive", type=Path)
    arguments = parser.parse_args()
    try:
        result = inspect_release(arguments.policy, arguments.source_commit,
            arguments.source_tree, arguments.approved_policy_sha256,
            artifact_id=arguments.artifact_id, archive_path=arguments.archive)
        print(json.dumps(result, ensure_ascii=True))
        return 0
    except ReleaseGateError as error:
        print(json.dumps({"schema": "gsw.release_gate_result/1", "ok": False, "code": error.code}))
        return 2
    except (OSError, ValueError, TypeError, KeyError, AttributeError, zipfile.BadZipFile, subprocess.SubprocessError):
        print(json.dumps({"schema": "gsw.release_gate_result/1", "ok": False,
                          "code": "release_evidence_unavailable"}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
