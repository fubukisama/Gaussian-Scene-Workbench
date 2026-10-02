"""Read-only COLMAP admission checks shared by every generation backend.

This is a minimum viability gate, not a reconstruction accuracy certificate.
Counts/coverage are reported separately from training PSNR and Gaussian count.
The binary layout follows COLMAP's documented reconstruction format.
"""
import math
import json
import struct
from pathlib import Path

MIN_VIEWS = 3
MIN_POINTS = 100
MIN_COVERAGE = .5
PREFERRED_COVERAGE = .8
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}


class ReconstructionQualityError(RuntimeError):
    def __init__(self, report, code="reconstruction_quality"):
        self.report, self.code = report, code
        super().__init__("{}: registered {}/{}, sparse points {}, reasons {}".format(
            code, report.get("registeredImages", 0), report.get("inputImages", 0),
            report.get("sparsePoints", 0), ",".join(report.get("reasons", []))))


def image_count(path):
    path = Path(path)
    return sum(1 for item in path.iterdir() if item.is_file() and item.suffix.lower() in IMAGE_EXTENSIONS) if path.is_dir() else 0


def _read(stream, format):
    size = struct.calcsize(format)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("Truncated COLMAP model")
    return struct.unpack(format, data)


def _skip(stream, size, end):
    if size < 0 or stream.tell() + size > end:
        raise ValueError("COLMAP model count exceeds payload")
    stream.seek(size, 1)


def _binary_images(path):
    names, ids = [], set()
    end = path.stat().st_size
    with path.open("rb") as stream:
        count, = _read(stream, "<Q")
        if count > end // 73:
            raise ValueError("Unsafe COLMAP image count")
        for _ in range(count):
            record = _read(stream, "<idddddddi")
            if not all(math.isfinite(x) for x in record[1:8]) or not .99 < sum(x*x for x in record[1:5]) < 1.01:
                raise ValueError("Invalid COLMAP camera pose")
            if record[0] in ids:
                raise ValueError("Duplicate COLMAP image ID")
            ids.add(record[0])
            name = bytearray()
            while True:
                char = stream.read(1)
                if not char or len(name) > 65536:
                    raise ValueError("Invalid COLMAP image name")
                if char == b"\0":
                    break
                name.extend(char)
            names.append(name.decode("utf-8"))
            observations, = _read(stream, "<Q")
            _skip(stream, observations * 24, end)
        if stream.tell() != end:
            raise ValueError("Unexpected COLMAP image payload")
    return names, ids


def _binary_points(path, image_ids):
    valid, errors, track_sum = 0, 0., 0
    end = path.stat().st_size
    with path.open("rb") as stream:
        count, = _read(stream, "<Q")
        if count > end // 51:
            raise ValueError("Unsafe COLMAP point count")
        for _ in range(count):
            record = _read(stream, "<QdddBBBdQ")
            tracks = record[-1]
            # Track IDs must reference registered cameras, not unrelated models.
            if tracks > end // 8:
                raise ValueError("Unsafe COLMAP track count")
            seen = set()
            for __ in range(tracks):
                image_id, point2d_id = _read(stream, "<ii")
                if image_id not in image_ids or point2d_id < 0:
                    raise ValueError("Invalid COLMAP point track")
                seen.add(image_id)
            if all(math.isfinite(x) for x in record[1:4]) and math.isfinite(record[-2]) and 0 <= record[-2] <= 4 and len(seen) >= 2:
                valid += 1
                errors += record[-2]
                track_sum += tracks
        if stream.tell() != end:
            raise ValueError("Unexpected COLMAP point payload")
    return count, valid, errors / max(valid, 1), track_sum / max(valid, 1)


def _text_model(root):
    names, ids = [], set()
    # COLMAP image records alternate a pose line and a points2D line, which can
    # be empty. Do not strip empty lines before consuming each record.
    with (root / "images.txt").open(encoding="utf-8") as stream:
        for line in stream:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            fields = line.strip().split(maxsplit=9)
            if len(fields) != 10 or not all(math.isfinite(float(x)) for x in fields[1:8]):
                raise ValueError("Invalid COLMAP text camera")
            image_id = int(fields[0])
            if image_id in ids or not .99 < sum(float(x)**2 for x in fields[1:5]) < 1.01:
                raise ValueError("Invalid COLMAP text camera pose or duplicate ID")
            ids.add(image_id)
            names.append(fields[9])
            if next(stream, None) is None:
                raise ValueError("Truncated COLMAP image observations")
    count, valid, errors, track_sum = 0, 0, 0., 0
    with (root / "points3D.txt").open(encoding="utf-8") as stream:
        for line in stream:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            fields = line.split()
            if len(fields) < 8 or (len(fields) - 8) % 2:
                raise ValueError("Invalid COLMAP text point")
            xyz = [float(x) for x in fields[1:4]]
            error = float(fields[7])
            tracks = {int(x) for x in fields[8::2]}
            if not tracks <= ids or any(int(x) < 0 for x in fields[9::2]):
                raise ValueError("Invalid COLMAP text track")
            count += 1
            if all(math.isfinite(x) for x in xyz) and math.isfinite(error) and 0 <= error <= 4 and len(tracks) >= 2:
                valid += 1
                errors += error
                track_sum += (len(fields) - 8) // 2
    return names, count, valid, errors / max(valid, 1), track_sum / max(valid, 1)


def assess_model(root, input_images=0):
    root = Path(root)
    report = dict(version=1, modelPath=str(root), inputImages=int(input_images),
                  registeredImages=0, sparsePoints=0, validPoints=0,
                  coverage=0., meanReprojectionError=0., meanTrackLength=0.,
                  usable=False, preferred=False, reasons=[])
    try:
        if (root / "images.bin").is_file() and (root / "points3D.bin").is_file() and (root / "cameras.bin").is_file():
            names, ids = _binary_images(root / "images.bin")
            count, valid, error, track = _binary_points(root / "points3D.bin", ids)
        else:
            if not (root / "cameras.txt").is_file():
                raise ValueError("Missing COLMAP cameras")
            names, count, valid, error, track = _text_model(root)
        registered = len(names)
        total = max(input_images, registered)
        coverage = registered / max(total, 1)
        report.update(registeredImages=registered, inputImages=total,
                      sparsePoints=count, validPoints=valid, coverage=coverage,
                      meanReprojectionError=error, meanTrackLength=track,
                      imageNames=names)
        if registered < MIN_VIEWS:
            report["reasons"].append("too_few_views")
        if valid < MIN_POINTS:
            report["reasons"].append("too_few_points")
        if valid < count * .95:
            report["reasons"].append("invalid_points")
        if coverage < MIN_COVERAGE:
            report["reasons"].append("low_coverage")
        if input_images and registered > input_images:
            report["reasons"].append("input_mismatch")
        report["usable"] = not report["reasons"]
        report["preferred"] = report["usable"] and coverage >= PREFERRED_COVERAGE
    except (OSError, ValueError, UnicodeError, struct.error) as exc:
        report["reasons"] = ["invalid_model"]
        report["detail"] = str(exc)
    return report


def select_model(sparse_root, input_images):
    root = Path(sparse_root)
    candidates = [root] if (root / "images.bin").is_file() or (root / "images.txt").is_file() else sorted((x for x in root.iterdir() if x.is_dir()), key=lambda p: p.name) if root.exists() else []
    reports = [assess_model(candidate, input_images) for candidate in candidates]
    viable = [report for report in reports if report["usable"]]
    best = max(viable, key=lambda r: (r["registeredImages"], r["validPoints"], -r["meanReprojectionError"])) if viable else None
    return best, reports


def assess_dataset(dataset):
    dataset = Path(dataset)
    total = image_count(dataset / "input") or image_count(dataset / "images") or image_count(dataset)
    try:
        records = json.loads((dataset / "source" / "metadata_manifest.json").read_text(encoding="utf-8-sig")).get("files", [])
        expected = sum(1 if r.get("kind") == "image" else max(int(r.get("extractedFrames", 0)), 0) for r in records)
        total = max(total, expected)
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    return assess_model(dataset / "sparse" / "0", total)


def require_dataset(dataset):
    report = assess_dataset(dataset)
    if not report["usable"]:
        raise ReconstructionQualityError(report)
    return report
