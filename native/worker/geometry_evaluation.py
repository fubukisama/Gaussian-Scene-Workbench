"""Independent geometry evaluation CLI; never starts training or a renderer."""
import argparse
import json
from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from native.backend.geometry_evaluation import GeometryEvaluationError, evaluate_geometry
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
    parser.add_argument("--config", required=True)
    parser.add_argument("--output-directory", required=True)
    args = parser.parse_args(argv)
    cancelled = _InputCancellation()
    try:
        if cancelled():
            raise GeometryEvaluationError("geometry_evaluation_cancelled")
        path = Path(args.config).resolve()
        if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
            raise GeometryEvaluationError("geometry_evaluation_request_missing_or_too_large")

        def object_pairs(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise GeometryEvaluationError("geometry_duplicate_json_key", key)
                result[key] = value
            return result

        def constant(value):
            raise GeometryEvaluationError("geometry_nonfinite_request", value)

        with path.open(encoding="utf-8-sig") as stream:
            request = json.load(stream, object_pairs_hook=object_pairs, parse_constant=constant)
        result = evaluate_geometry(request, args.output_directory, base_directory=path.parent,
                                   cancelled=cancelled)
        print(json.dumps(result, ensure_ascii=False, allow_nan=False), flush=True)
        return 0
    except Exception as error:
        code = getattr(error, "code", "geometry_evaluation_failed")
        print(json.dumps({"success": False, "code": code, "detail": getattr(error, "detail", str(error))},
                         ensure_ascii=False, allow_nan=False), flush=True)
        return 130 if code == "geometry_evaluation_cancelled" else 2


if __name__ == "__main__":
    sys.exit(main())
