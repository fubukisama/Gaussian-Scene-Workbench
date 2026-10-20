"""Export/replay reusable named reconstruction recipes; never starts jobs."""
import argparse
import json
from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from native.backend.camera_dataset import _read_json
from native.backend.reconstruction_comparison import ComparisonError, export_comparison, verify_comparison
from native.worker.camera_import import _InputCancellation


def main(argv=None):
    # Machine-readable pipes are always UTF-8, independent of Windows locale
    # or an inherited PYTHONIOENCODING (including cp932).
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", line_buffering=True)
        except (AttributeError, ValueError, OSError):
            pass
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", choices=("export", "verify"), required=True)
    parser.add_argument("--request")
    parser.add_argument("--output-directory")
    parser.add_argument("--profile")
    parser.add_argument("--dataset-paths", help="JSON object mapping dataset identifiers to relocated absolute paths")
    parser.add_argument("--repository")
    args = parser.parse_args(argv)
    cancelled = _InputCancellation()
    try:
        if cancelled():
            raise ComparisonError("comparison_cancelled")
        if args.operation == "export":
            if not args.request or not args.output_directory or args.profile or args.dataset_paths:
                raise ComparisonError("comparison_export_arguments_required")
            request = Path(args.request).resolve()
            if not request.is_file() or request.stat().st_size > 16 * 1024 * 1024:
                raise ComparisonError("comparison_request_missing_or_too_large")
            result = export_comparison(_read_json(request), args.output_directory, args.repository, cancelled)
        else:
            if not args.profile or args.request or args.output_directory:
                raise ComparisonError("comparison_verify_arguments_required")
            mapping = _read_json(Path(args.dataset_paths)) if args.dataset_paths else None
            result = verify_comparison(args.profile, mapping, args.repository, cancelled)
        print(json.dumps(result, ensure_ascii=False, allow_nan=False), flush=True)
        return 0
    except Exception as error:
        code = getattr(error, "code", "comparison_operation_failed")
        print(json.dumps({"success": False, "code": code, "detail": str(getattr(error, "detail", error))},
                         ensure_ascii=False, allow_nan=False), flush=True)
        return 130 if "cancel" in code else 2


if __name__ == "__main__":
    sys.exit(main())
