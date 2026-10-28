# Release eligibility is not publication

The checked-in policy is deliberately `approved=false`. No ordinary push, local
acceptance receipt, passing fixture, CLI argument, or artifact upload grants a
signing key, creates a public Release, or promotes `stable`/`preview`.

An authorized operator must separately approve the complete policy and pin the
SHA-256 of its exact bytes in the protected publisher configuration. Approval is
not inferred from the workflow names below. The gate must read that same policy
from the exact source commit and reject any byte difference.

## Read-only source and check evidence

The candidate must be the current native `main` commit and exact Git tree of the
pinned repository ID. Source, build, display version and release sequence are
different identities; a repeated display version cannot select an old artifact.

`workflowInventory` enumerates **every** committed `.github/workflows/*.yml` or
`.yaml` blob by Git blob SHA. Each has scope `required`, `diagnostic`, or
`not_applicable`; the last two need a reviewed reason. New, removed, renamed or
modified workflows require another policy review. All `required` workflows must
have named jobs in `requiredJobs`, with the official Actions application ID,
workflow path and allowed event. An empty or incomplete required set fails.

The effective required checks are the union of the approved policy and GitHub's
current classic branch protection plus all active repository/organization branch
rules. A permission error or an unbound required rule is a blocker, not evidence
that no rule exists. A 404 is accepted as "unprotected" only when GitHub explicitly
returns that meaning for an otherwise verified, readable repository.

For each workflow, use its latest eligible run at the exact SHA on `main`; do
not choose an earlier green run over a newer failed, cancelled or pending run.
Use the run's **current attempt**, fetch that attempt's jobs, and join each
required job to its exact check-run URL. Require completed/success for the run,
job and official check, and match repository, SHA, workflow, event and app ID.
Skipped/neutral are not success. Unbound legacy status contexts fail closed.

## Artifact and corresponding source

The approved artifact producer names its workflow, job, artifact name prefix,
archive member and report member. Candidate names bind the full source SHA, run
ID and attempt; artifact REST identity, timestamps, expiry and digest must agree.
The gate downloads the actual immutable artifact ID. GitHub's artifact digest is
the digest of the **outer artifact ZIP**, not the inner native update archive.
Both layers, and the candidate report's run/attempt/source/build identity, must
be verified. A stale artifact from an earlier attempt is not a current result.

The inner native ZIP must satisfy the complete manifest contract in README.md.
The exact `source-manifest.json` byte hash protects its actual source file set;
it does not alone prove that the source came from Git.
`source-provenance.json` must therefore map the full tracked repository blob set
to `source/<repository-relative-path>`, using Git's blob SHA calculation on each
actual archived file and the independently read exact Git tree. Generated Cargo
vendor sources are separate, fully enumerated dependency inputs bound to the
tracked lockfile, registry checksums, licenses and vendoring provenance. Missing
source, unenumerated generated files, unsupported Git links, or an unavailable
dependency-source proof blocks formal eligibility.

No fixture result is production publisher authentication. The isolated fixture
has a distinct product ID, ephemeral keys and public loopback CA; those keys and
CA never configure the production client or publisher.

## Separate authorized publisher

The eventual signing/publishing job must run from a separately protected entry,
with an independently approved exact policy identity and least-privilege
credentials unavailable to ordinary build/test jobs. It must repeat eligibility
checks immediately before signing and publish immutable assets before signed
channel metadata. Public anonymous download, production TUF root custody and
rotation, Authenticode and bootstrap migration remain separate acceptance gates.
This repository's current update workflow is test-only and does not implement
or authorize that publisher.

Official API boundaries (read-only):

- [Effective branch rules, including organization rules](https://docs.github.com/en/rest/repos/rules#get-rules-for-a-branch)
- [Classic branch protection](https://docs.github.com/en/rest/branches/branch-protection#get-branch-protection)
- [Jobs for a specified run attempt](https://docs.github.com/en/rest/actions/workflow-jobs#list-jobs-for-a-workflow-run-attempt)
- [Check-run identity](https://docs.github.com/en/rest/checks/runs#get-a-check-run)
- [Immutable artifact identity and download](https://docs.github.com/en/rest/actions/artifacts)
