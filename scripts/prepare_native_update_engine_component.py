"""Package the default-NOT_CONFIGURED native update engine for research preview.

Developer packaging only, never a public installer or profile-adoption command.
The first remote tracer deliberately reports a typed unimplemented capability.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    prepare = commands.add_parser("prepare")
    for name in ("runtime-root", "repository", "trust-library", "cmake-cache", "build-executable",
                 "toolchain-receipt", "cargo-home", "work-root", "output-archive", "output-manifest"):
        prepare.add_argument("--" + name, type=Path, required=True)
    prepare.add_argument("--source-commit", required=True)
    prepare.add_argument("--source-tree", required=True)
    prepare.add_argument("--cargo", default="cargo")
    parser.parse_args()
    print(json.dumps({"schema": "gsw.native_update_engine_result/1", "ok": False,
                      "code": "ENGINE_COMPONENT_NOT_IMPLEMENTED", "configurationStatus": "NOT_CONFIGURED",
                      "publicDistributionReady": False, "publisherAuthenticated": False}))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
