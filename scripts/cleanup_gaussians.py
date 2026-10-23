# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""CPU-only, source-preserving mask cleanup; outputs are not quality acceptance."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from native.worker.quality_postprocess import postprocess, QualityPostprocessError


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("cleanup_duplicate_request_key")
        result[key] = value
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cancel-file", help="An existing file requests cancellation; no input or output is overwritten")
    args = parser.parse_args(argv)
    try:
        request_path = Path(args.request)
        if not request_path.is_file() or request_path.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("cleanup_request_size_or_type")
        with request_path.open(encoding="utf-8") as stream:
            request = json.load(stream, object_pairs_hook=_object,
                parse_constant=lambda value: (_ for _ in ()).throw(ValueError("cleanup_nonfinite_request")))
        cancel = Path(args.cancel_file) if args.cancel_file else None
        if cancel is not None and (not cancel.is_absolute() or cancel.drive.upper() != "D:"):
            raise ValueError("cleanup_D_cancel_file_required")
        report = postprocess(request, args.output, cancelled=(lambda: cancel.exists()) if cancel else None)
        print(json.dumps({key: report[key] for key in ("status", "cleanup_status", "model_path", "manifest_path",
            "retained_count", "deleted_count", "quality_accepted", "oracle_cleanup")}, sort_keys=True, ensure_ascii=True))
        return 0
    except KeyboardInterrupt:
        print(json.dumps({"status": "cancelled", "reason": "cleanup_cancelled", "quality_accepted": False}), file=sys.stderr)
        return 130
    except (OSError, ValueError, KeyError, TypeError) as error:
        code = error.code if isinstance(error, QualityPostprocessError) else str(error)
        cancelled = code == "cleanup_cancelled"
        print(json.dumps({"status": "cancelled" if cancelled else "failed", "reason": code, "quality_accepted": False}, ensure_ascii=True), file=sys.stderr)
        return 130 if cancelled else 1


if __name__ == "__main__":
    raise SystemExit(main())
