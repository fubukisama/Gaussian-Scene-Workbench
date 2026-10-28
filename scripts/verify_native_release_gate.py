"""Fail-closed release eligibility inspection, never a signing/publication command.

Developer/CI tooling only. Production is disabled until a complete approved policy
and independently pinned policy identity are configured by an authorized operator.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


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


def inspect_release(policy_path: Path, source_commit: str, source_tree: str,
                    approved_policy_sha256: str | None = None) -> dict:
    if not re.fullmatch(r"[a-f0-9]{40}", source_commit) or not re.fullmatch(r"[a-f0-9]{40}", source_tree):
        raise ReleaseGateError("release_source_identity_invalid")
    if policy_path.is_symlink() or not policy_path.is_file() or policy_path.stat().st_size > 256 * 1024:
        raise ReleaseGateError("release_policy_unavailable")
    try:
        raw = policy_path.read_bytes()
        policy = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_pairs,
            parse_constant=lambda _: (_ for _ in ()).throw(ReleaseGateError("release_policy_nonfinite")))
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
    # Deliberately fail closed for the first vertical slice. The next public
    # contract adds live exact-attempt/check/artifact evidence verification;
    # a local JSON boolean must never temporarily authorize production.
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
