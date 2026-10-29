"""Create tiny owned byte fixtures for the actual Actions artifact boundary.

This is not an application package, installer, backend capability or launch test.
"""

import argparse
import hashlib
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root
    if not root.is_absolute() or root.exists() or not root.parent.is_dir():
        parser.error("--root must be a fresh owned absolute directory")
    root.mkdir()
    fixture_files = {
        "bin/Gaussian Scene Workbench.exe": b"GSW ARTIFACT FIXTURE - NOT EXECUTABLE\n",
        "gaussian-splatting/train.py": b"# byte fixture, never imported\n",
        "gaussian-splatting/.gitignore": b"build/\n",
        "gaussian-splatting/.gitmodules": b"# retained build metadata fixture\n",
        "gaussian-splatting/submodules/simple-knn/simple_knn/.gitkeep": b"",
        "source/.github/workflows/fixture.yml": b"# corresponding-source fixture\n",
        "source/native/CMakeLists.txt": b"# byte fixture, never configured\n",
        "source/native/.gitignore": b"build/\n",
    }
    for relative, data in fixture_files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    records = lambda prefix, size_key: [
        {"path": path[len(prefix):], size_key: len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for path, data in sorted(fixture_files.items()) if path.startswith(prefix)]
    # Backend paths are package-relative, unlike source-relative source records.
    backend = {"schemaVersion": 2, "files": records("gaussian-splatting/", "bytes")}
    for row in backend["files"]:
        row["path"] = "gaussian-splatting/" + row["path"]
    (root / "backend_manifest.json").write_text(json.dumps(backend), encoding="utf-8")
    source = records("source/", "size")
    (root / "source-manifest.json").write_text(json.dumps({
        "schema": "gsw-native-corresponding-source-v1", "sourceFileCount": len(source),
        "executableSha256": hashlib.sha256(fixture_files["bin/Gaussian Scene Workbench.exe"]).hexdigest(),
        "license": "GPL-3.0-or-later", "files": source}), encoding="utf-8")
    print(json.dumps({"fixtureOnly": True, "applicationExecuted": False, "root": str(root)}))


if __name__ == "__main__":
    main()
