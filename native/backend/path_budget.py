"""Conservative Win32 path budgets for generated/external-tool file handoff.

This does not enable extended paths, change Windows policy, or claim that an
arbitrary external application supports long paths. Callers supply the actual
planned paths for their operation before creating expensive intermediate data.
"""

import json
import os
from pathlib import Path


class PathBudgetError(ValueError):
    """Machine-readable path failure for a localized caller to present."""

    def __init__(self, path, kind, actual, limit):
        self.detail = {"path": str(path), "kind": kind, "actual": actual,
                       "limit": limit, "unit": "utf16_code_units",
                       "policy": "legacy_win32_external_tools"}
        super().__init__(json.dumps(self.detail, ensure_ascii=False, sort_keys=True))


def require_legacy_windows_paths(files=(), directories=()):
    """Reject unsupported generated paths before output work on Windows.

    259 UTF-16 units excludes the terminating NUL from MAX_PATH=260.
    Directory creation conservatively reserves the documented 8.3 suffix
    allowance (MAX_PATH minus twelve, again excluding the terminator).
    Parent directories are checked too. No filesystem or registry is mutated.
    Platforms other than Windows retain their native path policy.
    """
    if os.name != "nt":
        return
    planned_files = [Path(path).absolute() for path in files]
    planned_directories = {Path(path).absolute() for path in directories}
    for path in planned_files:
        planned_directories.update(path.parents)
    for path in tuple(planned_directories):
        planned_directories.update(path.parents)
    for kind, limit, paths in (("file", 259, planned_files),
                               ("directory", 247, sorted(planned_directories, key=str))):
        for path in paths:
            length = len(str(path).encode("utf-16-le")) // 2
            if length > limit:
                raise PathBudgetError(path, kind, length, limit)
