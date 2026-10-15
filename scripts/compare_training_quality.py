# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Read-only comparison of explicit baseline/candidate quality measurements.

Exit codes: 0=contract passed, 1=regressed, 2=invalid input/output,
3=unverified, 4=not comparable. No renderer, trainer, GPU or GUI is started.
Reports are supplied measurements, not measurements produced by this tool.
Output files must be new; existing reports are never overwritten.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from native.worker.training_quality import compare_quality


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def _invalid_constant(value):
    raise ValueError("Nonstandard JSON numeric constant is forbidden: " + value)


def _read(path):
    source = Path(path)
    if source.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("Quality measurement reports must not exceed 16 MiB")
    return json.loads(source.read_text(encoding="utf-8-sig"),
                      object_pairs_hook=_unique_object, parse_constant=_invalid_constant)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, help="Baseline measurement report JSON")
    parser.add_argument("--candidate", required=True, help="Candidate measurement report JSON")
    parser.add_argument("--contract", required=True, help="Explicit user quality contract JSON")
    parser.add_argument("--output", help="Optional new JSON output path; existing files are refused")
    args = parser.parse_args(argv)
    try:
        result = compare_quality(_read(args.baseline), _read(args.candidate), _read(args.contract))
        output = json.dumps(result, indent=2, sort_keys=True, ensure_ascii=True, allow_nan=False) + "\n"
        if args.output:
            with Path(args.output).open("x", encoding="utf-8", newline="\n") as handle:
                handle.write(output)
        else:
            sys.stdout.write(output)
        return {"pass": 0, "regressed": 1, "unverified": 3, "not_comparable": 4}[result["status"]]
    except (OSError, ValueError, RecursionError) as error:
        sys.stderr.write("Quality comparison failed: " + str(error) + "\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
