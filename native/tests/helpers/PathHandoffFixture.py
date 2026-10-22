"""Owned CPU acceptance bridge for Qt -> product preparation -> COLMAP I/O.

This helper neither trains nor estimates camera poses. All outputs are new files
inside a caller-owned fixture. Product code is selected explicitly and recorded.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_new(path, value):
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)


def prepare(options):
    root = Path(options.backend_root).resolve(strict=True)
    sys.path.insert(0, str(root))
    from native.backend import camera_admission

    module = Path(camera_admission.__file__).resolve(strict=True)
    module.relative_to(root)
    source = Path(options.source).resolve(strict=True)
    raw = json.loads(source.read_text(encoding="utf-8"))
    originals = {str(source): digest(source)}
    for view in raw["views"]:
        path = (source.parent / view["image_path"]).resolve(strict=True)
        path.relative_to(source.parent)
        originals[str(path)] = digest(path)
    admitted = camera_admission.admit_locked_dataset(source)
    dataset = admitted["dataset"]
    result = camera_admission.prepare_solve_dataset_version(
        source, Path(options.destination).absolute(), dataset["data_identity"])
    expected = {view["view_id"]: view for view in dataset["views"] if view["split"] == "train"}
    if {item["viewId"] for item in result["files"]} != set(expected):
        raise AssertionError("prepared train view identities differ from the admitted split")
    for item in result["files"]:
        original = expected[item["viewId"]]
        if (item["sourceSplit"] != "train" or item["sourceImagePath"] != original["image_path"]
                or item["sha256"] != original["sha256"]
                or digest(Path(result["datasetPath"]) / item["inputImagePath"]) != original["sha256"]):
            raise AssertionError("prepared image/camera mapping or byte hash changed")
    unchanged = all(digest(path) == value for path, value in originals.items())
    if not unchanged:
        raise AssertionError("original fixture changed during admission/preparation")
    if result["scale"]["source_unit"] != "unknown" or result["scale"]["meters_per_unit"] is not None:
        raise AssertionError("a fresh solve must not invent metric scale")
    policy = None
    if sys.platform == "win32":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\FileSystem") as key:
            policy = winreg.QueryValueEx(key, "LongPathsEnabled")[0]
    return dict(result, success=True, originalsUnchanged=unchanged, sourceHashes=originals,
                longPathsEnabled=policy, sourceModule=str(module), sourceModuleSha256=digest(module),
                python=sys.executable, cameras=dataset["views"], splits=dataset["splits"],
                scope="path_input_compatibility_not_camera_solving_or_geometry_accuracy")


def inspect(options):
    preparation = json.loads(Path(options.preparation).read_text(encoding="utf-8"))
    database = Path(options.database).resolve(strict=True)
    expected = {Path(item["inputImagePath"]).name for item in preparation["files"]}
    with sqlite3.connect(database.as_uri() + "?mode=ro", uri=True) as connection:
        images = [dict(imageId=row[0], name=row[1], cameraId=row[2])
                  for row in connection.execute("SELECT image_id,name,camera_id FROM images ORDER BY image_id")]
        cameras = [dict(cameraId=row[0], model=row[1], width=row[2], height=row[3])
                   for row in connection.execute("SELECT camera_id,model,width,height FROM cameras ORDER BY camera_id")]
        keypoints = [dict(imageId=row[0], count=row[1], columns=row[2])
                     for row in connection.execute("SELECT image_id,rows,cols FROM keypoints ORDER BY image_id")]
    if {row["name"] for row in images} != expected or len(images) != len(expected):
        raise AssertionError("COLMAP readback omitted a train image or included non-train input")
    if not cameras or any(row["width"] != 128 or row["height"] != 96 for row in cameras):
        raise AssertionError("external tool image dimensions changed")
    if {row["imageId"] for row in keypoints} != {row["imageId"] for row in images} or any(
            row["count"] <= 0 for row in keypoints):
        raise AssertionError("actual CPU feature extraction produced no keypoints for an input")
    if not all(digest(path) == value for path, value in preparation["sourceHashes"].items()):
        raise AssertionError("original fixture changed during external tool handoff")
    return dict(success=True, database=str(database), databaseSha256=digest(database),
                images=images, cameras=cameras, keypoints=keypoints, expectedTrainNames=sorted(expected),
                excludedSplitIds={key: preparation["splits"][key] for key in ("val", "test")},
                originalsUnchanged=True, sourceDataIdentity=preparation["sourceDataIdentity"],
                solveDataIdentity=preparation["solveDataIdentity"])


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("prepare")
    for name in ("backend-root", "source", "destination", "report"):
        command.add_argument("--" + name, required=True)
    command = commands.add_parser("inspect")
    for name in ("preparation", "database", "report"):
        command.add_argument("--" + name, required=True)
    options = parser.parse_args()
    try:
        result = prepare(options) if options.command == "prepare" else inspect(options)
        code = 0
    except Exception as error:
        result = dict(success=False, errorType=type(error).__name__, error=str(error))
        code = 1
    write_new(options.report, result)
    print(json.dumps(result, ensure_ascii=False, allow_nan=False))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
