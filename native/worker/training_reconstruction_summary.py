"""Read-only desktop dataset provenance; never imports CUDA or starts workers.

The admission gate delegates to the generation backend's shared reconstruction
assessment. The extra camera count is calibration metadata, not photo count.
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

_CAMERA_MODELS = {
    0: ("SIMPLE_PINHOLE", 3), 1: ("PINHOLE", 4), 2: ("SIMPLE_RADIAL", 4),
    3: ("RADIAL", 5), 4: ("OPENCV", 8), 5: ("OPENCV_FISHEYE", 8),
    6: ("FULL_OPENCV", 12), 7: ("FOV", 5), 8: ("SIMPLE_RADIAL_FISHEYE", 4),
    9: ("RADIAL_FISHEYE", 5), 10: ("THIN_PRISM_FISHEYE", 12)}


def _read(stream, layout):
    size = struct.calcsize(layout)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("Truncated COLMAP camera metadata")
    return struct.unpack(layout, data)


def _camera_metadata(model, extension, dataset):
    cameras = {}
    camera_path = model / ("cameras." + extension)
    if extension == "bin":
        end = camera_path.stat().st_size
        with camera_path.open("rb") as stream:
            count, = _read(stream, "<Q")
            if count > end // 24:
                raise ValueError("Camera count exceeds payload")
            for _ in range(count):
                identifier, model_id, width, height = _read(stream, "<iiQQ")
                name, parameter_count = _CAMERA_MODELS[model_id]
                parameters = _read(stream, "<" + "d" * parameter_count)
                if identifier in cameras or width <= 0 or height <= 0 or not all(math.isfinite(x) for x in parameters):
                    raise ValueError("Invalid camera calibration")
                cameras[identifier] = name
            if stream.tell() != end:
                raise ValueError("Unexpected camera calibration payload")
    else:
        model_parameters = dict(_CAMERA_MODELS.values())
        with camera_path.open(encoding="utf-8") as stream:
            for line in stream:
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                fields = line.split()
                identifier, name = int(fields[0]), fields[1]
                if len(fields) != 4 + model_parameters[name] or identifier in cameras or int(fields[2]) <= 0 or int(fields[3]) <= 0 or not all(math.isfinite(float(x)) for x in fields[4:]):
                    raise ValueError("Invalid text camera calibration")
                cameras[identifier] = name
    image_path = model / ("images." + extension)
    missing_images = 0
    registered_ids = set()
    if extension == "bin":
        end = image_path.stat().st_size
        with image_path.open("rb") as stream:
            count, = _read(stream, "<Q")
            if count > end // 73:
                raise ValueError("Image count exceeds payload")
            for _ in range(count):
                record = _read(stream, "<idddddddi")
                if (record[-1] not in cameras or record[0] in registered_ids or
                        not all(math.isfinite(value) for value in record[1:8]) or
                        not .99 < sum(value * value for value in record[1:5]) < 1.01):
                    raise ValueError("Image references an unknown calibration")
                registered_ids.add(record[0])
                name = bytearray()
                while True:
                    value = stream.read(1)
                    if not value or len(name) > 65536:
                        raise ValueError("Invalid image filename")
                    if value == b"\0":
                        break
                    name.extend(value)
                filename = name.decode("utf-8")
                missing_images += not (dataset / "images" / filename).is_file()
                observations, = _read(stream, "<Q")
                if stream.tell() + observations * 24 > end:
                    raise ValueError("Image observation count exceeds payload")
                stream.seek(observations * 24, 1)
            if stream.tell() != end:
                raise ValueError("Unexpected image metadata payload")
    else:
        with image_path.open(encoding="utf-8") as stream:
            for line in stream:
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                fields = line.strip().split(maxsplit=9)
                if (len(fields) != 10 or int(fields[8]) not in cameras or int(fields[0]) in registered_ids or
                        not all(math.isfinite(float(value)) for value in fields[1:8]) or
                        not .99 < sum(float(value)**2 for value in fields[1:5]) < 1.01):
                    raise ValueError("Invalid image calibration reference")
                registered_ids.add(int(fields[0]))
                missing_images += not (dataset / "images" / fields[9]).is_file()
                if next(stream, None) is None:
                    raise ValueError("Truncated image observations")
    return len(cameras), len(registered_ids), all(name in ("PINHOLE", "SIMPLE_PINHOLE") for name in cameras.values()), missing_images


def _has_colmap(model):
    return (all((model / (name + ".bin")).exists() for name in ("cameras", "images", "points3D")) or
            all((model / (name + ".txt")).exists() for name in ("cameras", "images", "points3D")) or
            ((model / "points3D.ply").exists() and any(
                all((model / (name + "." + extension)).exists() for name in ("cameras", "images"))
                for extension in ("bin", "txt"))))


def _transforms_summary(dataset, input_images):
    report = dict(sourceKind="transforms", format="transforms_json",
                  registeredImages=None, cameraCount=None, frameCount=None,
                  sparsePoints=None, validPoints=None, inputImages=input_images,
                  cameraFile=str(dataset / "transforms_train.json"),
                  imageFile=str(dataset / "transforms_test.json"), pointFile="",
                  usable=False, minimumViabilityPassed=False, reasons=[])
    try:
        count = 0
        for filename in ("transforms_train.json", "transforms_test.json"):
            path = dataset / filename
            if path.stat().st_size > 64 * 1024 * 1024:
                raise ValueError("Transforms file exceeds inspection budget")
            data = json.loads(path.read_text(encoding="utf-8"))
            fov = float(data["camera_angle_x"])
            if not math.isfinite(fov) or not 0 < fov < math.pi or not isinstance(data["frames"], list):
                raise ValueError("Invalid transform camera parameters")
            if filename == "transforms_train.json" and not data["frames"]:
                raise ValueError("No training camera frames")
            for frame in data["frames"]:
                matrix = frame["transform_matrix"]
                if len(matrix) != 4 or any(len(row) != 4 for row in matrix) or not all(math.isfinite(float(x)) for row in matrix for x in row):
                    raise ValueError("Invalid camera transform")
                determinant = sum(matrix[0][column] * (
                    matrix[1][(column + 1) % 3] * matrix[2][(column + 2) % 3] -
                    matrix[1][(column + 2) % 3] * matrix[2][(column + 1) % 3]) for column in range(3))
                if abs(determinant) < 1e-12 or matrix[3] != [0, 0, 0, 1]:
                    raise ValueError("Noninvertible camera transform")
                if not (dataset / (frame["file_path"] + ".png")).is_file():
                    raise ValueError("Referenced transform image is missing")
                count += 1
        report.update(frameCount=count, usable=True)
    except (OSError, ValueError, TypeError, KeyError, IndexError, OverflowError) as error:
        report.update(reasons=["invalid_camera_metadata"], detail=str(error))
    return report


def summarize_reconstruction(dataset, run_colmap=False):
    from crop_editor import reconstruction_quality
    dataset = Path(dataset).resolve()
    model = dataset / "sparse" / "0"
    quality = reconstruction_quality.assess_dataset(dataset)
    source_kind = "colmap" if _has_colmap(model) else "none"
    transforms = source_kind == "none" and (dataset / "transforms_train.json").exists()
    if source_kind == "none" and not transforms and _has_colmap(dataset / ".alignment_cache" / "sparse" / "0"):
        model = dataset / ".alignment_cache" / "sparse" / "0"
        quality = reconstruction_quality.assess_model(model, quality["inputImages"])
        source_kind = "alignment_cache"
    quality.pop("imageNames", None)
    binary = all((model / name).is_file() for name in ("cameras.bin", "images.bin"))
    extension = "bin" if binary else "txt"
    count = None
    registered = None
    camera_valid, missing_images = False, 0
    try:
        if source_kind != "none":
            count, registered, camera_valid, missing_images = _camera_metadata(model, extension, dataset)
    except (OSError, ValueError, UnicodeError, struct.error, KeyError, IndexError):
        count = None
    report = dict(quality, version=1, ready=True, dataset=str(dataset),
                  sourceKind=source_kind, format="colmap_binary" if binary else "colmap_text",
                  cameraCount=count, cameraFile=str(model / ("cameras." + extension)),
                  imageFile=str(model / ("images." + extension)),
                  pointFile=str(next((model / name for name in ("points3D.bin", "points3D.txt", "points3D.ply")
                                     if (model / name).is_file()), model / ("points3D." + extension))), blocked=False)
    report["minimumViabilityPassed"] = quality["usable"]
    if registered is not None:
        report["registeredImages"] = registered
    if source_kind != "none" and (not camera_valid or missing_images):
        report["usable"] = False
        report["reasons"] = list(report["reasons"]) + ["invalid_camera_metadata"]
    if transforms:
        report.update(_transforms_summary(dataset, quality["inputImages"]))
        source_kind = "transforms"
        # Both upstream scene adapters select COLMAP whenever sparse exists,
        # even when an otherwise valid transforms file is present.
        if (dataset / "sparse").exists():
            report.update(usable=False, reasons=["invalid_camera_metadata"])
    elif source_kind == "none":
        report.update(format="none", cameraFile="", imageFile="", pointFile="")
    image_directory = next((path for path in (dataset / "input", dataset / "images", dataset)
                            if reconstruction_quality.image_count(path)), dataset / "images")
    report["colmapInputDirectory"] = str(image_directory)
    report["trainingImagesDirectory"] = str(dataset if transforms else dataset / "images")
    if reconstruction_quality.image_count(image_directory) == 0:
        report.update(decision="no_images", effectiveRunColmap=False, blocked=True)
    elif image_directory.name == "input" and not (dataset / "images").exists():
        report.update(decision="undistortion_required", effectiveRunColmap=True)
    elif source_kind == "none":
        report.update(decision="missing_reconstruction", effectiveRunColmap=True)
    elif not quality["usable"] and source_kind == "alignment_cache" and not run_colmap:
        report.update(decision="invalid_cache", effectiveRunColmap=False, blocked=True)
    elif source_kind != "transforms" and not quality["usable"]:
        report.update(decision="repair_required", effectiveRunColmap=True)
    elif run_colmap:
        report.update(decision="user_rerun", effectiveRunColmap=True)
    elif not report["usable"]:
        report.update(decision="invalid_source", effectiveRunColmap=False, blocked=True)
    else:
        report.update(decision="reuse_cache" if source_kind == "alignment_cache" else
                      "reuse_transforms" if source_kind == "transforms" else "reuse", effectiveRunColmap=False)
    return report


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
        sys.stderr.reconfigure(encoding="utf-8", line_buffering=True)
    except (AttributeError, ValueError):
        pass
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend-root", required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--run-colmap", action="store_true")
    arguments = parser.parse_args()
    sys.path.insert(0, str(Path(arguments.backend_root).resolve()))
    try:
        report = summarize_reconstruction(arguments.dataset, arguments.run_colmap)
    except (OSError, ValueError, TypeError, struct.error) as error:
        report = dict(version=1, ready=False, diagnostic=str(error))
    print(json.dumps(report, ensure_ascii=False, separators=(",", ":")))
    return 0 if report.get("ready") else 1


if __name__ == "__main__":
    sys.exit(main())
