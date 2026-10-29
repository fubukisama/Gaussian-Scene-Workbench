# Native artifact upload/download integrity

## Failure retained

Native Windows run `40743102181`, source
`a5393be3772b8d49c6be93a767dc7dfd5abcec5a`, passed its staging checks before
upload. The downloaded preview artifact `12843034849` nevertheless failed the
native backend manifest check: eleven `.gitignore`, `.gitmodules`, `.gitkeep`,
`.appveyor.yml` and `.travis.yml` paths were missing. Its original ZIP had 2996
entries and zero dotfile paths. Corresponding-source dotfiles were also omitted.
The original artifact/result is not repaired or reclassified by this change.
The setup executable was assembled before artifact upload and is a distinct
adapter; this finding does not by itself prove that its embedded payload lost files.

The cause is the upload action's default hidden-file filter, not local extraction.
The official [upload-artifact input contract](https://github.com/actions/upload-artifact#inputs)
defines `include-hidden-files` as false by default and requires validation of
hidden content before enabling it. The
[download-artifact contract](https://github.com/actions/download-artifact#inputs)
supports explicit artifact IDs and fail-closed digest validation.

## Delivery boundary

Only the controlled `native/artifact/` upload enables hidden files. Before
upload, `scripts/verify_native_artifact.py` rejects actual `.git` metadata,
links/reparse points, unsafe manifest paths and case collisions, then verifies
all backend and corresponding-source records and their executable binding.
It does not claim to be a general secret scanner, signature validator, runtime
test or product acceptance gate. The existing native-only boundary remains.

After upload, CI downloads that exact artifact ID into a fresh runner-owned
directory. It independently compares every staged/downloaded path, byte length
and SHA-256, including runtime files not named by the two source manifests, and
re-runs the native-only package check. Separate fresh JSON receipts retain the
result. No packaged application, installer, training or user data is executed.

The small `Native Artifact Roundtrip` workflow exercises the actual GitHub
upload/download boundary twice with an explicitly non-executable byte fixture:
the previous default filter must fail on an omitted hidden backend file, while
the corrected option must preserve every byte and both manifests. Public CLI
contracts also reject changed source bytes, actual Git metadata and unmanifested
runtime loss. This fixture does not certify a complete native candidate package.

## Acceptance status

Implemented; local syntax/diff validation only. Remote roundtrip and full native
package readback are **NOT_RUN** until matching runs finish. No local automated
tests, application, installer, publisher or GUI was executed. No candidate was
installed, no active pointer/shortcut was changed, and no old result was erased.
