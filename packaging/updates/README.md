# Native update release inputs

`release-policy.json` is an **unapproved draft**. It confers no production signing,
publication or channel-promotion authority. An approved complete policy, its
independently pinned byte digest, the current effective GitHub protection rules,
exact-source CI/artifact verification and the separately authorized protected
publishing configuration must all agree before promotion. Missing evidence fails
closed; `fullAcceptance=true` is not a public trust root.

The archive contract is `gsw.native_update_files/1`: a flat-root ZIP containing
`release-files.json` plus precisely its listed files. The manifest itself is not
self-listed. `fileCount` counts listed payload files; `unpackedLength` includes
all listed bytes **and** the manifest bytes. `payloadManifestSha256` binds the
manifest's exact bytes. `correspondingSourceSha256` binds the package's exact
`source-manifest.json` bytes; that source manifest must match the actual complete
`source/` tree, including legitimate dot paths. Hashes bind integrity, not a
publisher identity.

Production product: `Fubukisama.GaussianSceneWorkbench.Native`.
Isolated fixture product: `Fubukisama.GaussianSceneWorkbench.UpdateFixture`.
They are not interchangeable. The public client cannot take a request-supplied
root, feed, CA or developer-acceptance switch.

`fixture-requirements.txt` pins the remote-CI-only Python TUF issuer. It uses the
mature metadata API and `CryptoSigner` rather than implementing cryptographic
primitives. The chosen fixture layout uses `consistent_snapshot=false`, which
TUF permits; role versions/hashes/signatures and freshness checks remain enabled.
The ordinary metadata names are `timestamp.json`, `snapshot.json`, `targets.json`
and versioned `N.root.json`. The sole top-level application target is
`release.json`, itself protecting the exact Offer bytes. Its approved HTTPS
archive URL is independent of the TUF target path. Profiles contain only the
public root and (fixture-build only) public loopback CA, never private keys.

The network fixture server is an external remote-test adapter, not a shipped
client dependency. Its stable/preview metadata and targets are scoped under
`/repository/<channel>/`; control selects immutable test repositories A/B/badC.
`/downloads/` holds the archive assets separately. No fixture configuration can
enable public publication.

Primary API/version references, checked 2026-10-28:

- [Python-TUF release metadata](https://pypi.org/project/tuf/7.0.1/)
- [Official metadata API](https://theupdateframework.readthedocs.io/en/latest/api/api-reference.html)
- [Official manual repository example](https://github.com/theupdateframework/python-tuf/blob/v7.0.1/examples/manual_repo/basic_repo.py)
- [TUF specification](https://theupdateframework.github.io/specification/latest/)

The fixture tooling is not a TUF conformance certification and does not establish
production root custody, Authenticode trust, protected environments or public
anonymous-download acceptance. Those gates remain separate.
