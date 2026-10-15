# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Bounded CPU image-pair measurements; never render, train, resize or register.

Inputs are encoded RGB8 samples, normalized by 255 without a color transform.
PSNR uses pooled valid-channel MSE. SSIM uses an 11x11 Gaussian (sigma 1.5),
valid convolution, population covariance, RGB channel mean and only windows
whose entire mask is valid. These are not training-loss SSIM boundary semantics.
"""
from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path


MAX_REPORT_BYTES = 16 * 1024 * 1024
MAX_IMAGE_PIXELS = 64 * 1024 * 1024
MAX_FILE_BYTES = 512 * 1024 * 1024
TILE_WIDTH, TILE_HEIGHT = 256, 64
PROTOCOL = {"version": 1, "input": "encoded_rgb8_div_255_float64", "color_transform": "none",
            "icc_conversion": "none", "colorimetry": "not_assumed_from_code_values",
            "resize": "none", "registration": "none", "crop": "none", "exif_orientation": "identity_only",
            "mask": "L8_nonzero_valid_same_shape", "psnr": "pooled_valid_channel_mse_peak_1",
            "ssim": "gaussian_11_sigma_1.5_valid_population_rgb_mean_all_valid_mask_windows",
            "ssim_constants": [0.0001, 0.0009], "ssim_numerical_bounds": [-1, 1],
            "ssim_fp64_roundoff_tolerance": 1e-9,
            "input_binding": "hash_and_decode_same_owned_byte_snapshot",
            "ssim_aggregation": "valid_window_weighted_mean", "failed_views": "fail_entire_measurement"}


def _json_bytes(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("utf-8")


def _identity(value):
    return "sha256:" + hashlib.sha256(_json_bytes(value)).hexdigest()


def _hash(path, cancelled=None, copy_to=None):
    digest = hashlib.sha256()
    copied = 0
    with path.open("rb") as handle:
        while True:
            _check_cancel(cancelled)
            block = handle.read(min(1024 * 1024, MAX_FILE_BYTES - copied + 1))
            if not block:
                break
            copied += len(block)
            if copied > MAX_FILE_BYTES:
                raise ValueError("Measurement input exceeds the bounded read/copy byte budget")
            if copy_to is not None and copy_to.write(block) != len(block):
                raise OSError("Incomplete owned image snapshot write")
            digest.update(block)
    return digest.hexdigest()


def _psnr(mse):
    return ({"value": None, "special": "positive_infinity", "reason": "zero_mse"}
            if mse == 0 else -10.0 * math.log10(mse))


def _check_cancel(cancelled):
    if cancelled is not None and cancelled():
        raise InterruptedError("image_quality_measurement_cancelled")


def _settings_json(value, depth=0):
    if depth > 16:
        raise ValueError("Measurement settings exceed the JSON nesting budget")
    if type(value) is dict:
        if any(type(key) is not str for key in value):
            raise ValueError("Measurement settings require string object keys")
        for child in value.values():
            _settings_json(child, depth + 1)
    elif type(value) is list:
        for child in value:
            _settings_json(child, depth + 1)
    elif type(value) in (int, float):
        try:
            finite = math.isfinite(value)
        except OverflowError:
            finite = False
        if not finite:
            raise ValueError("Measurement settings require finite JSON numbers")
    elif value is not None and type(value) not in (str, bool):
        raise ValueError("Measurement settings require JSON values")


def _validate_manifest(manifest):
    if (type(manifest) is not dict or set(manifest) - {"version", "views", "split", "settings"}
            or type(manifest.get("version")) is not int or manifest["version"] != 1
            or type(manifest.get("views")) is not list or not 1 <= len(manifest["views"]) <= 4096):
        raise ValueError("Measurement manifest requires version 1 and 1..4096 explicit views")
    if manifest.get("split", "unverified") not in ("unverified", "training_fit"):
        raise ValueError("Paired images do not prove held-out provenance; split must be unverified or training_fit")
    if "settings" in manifest and type(manifest["settings"]) is not dict:
        raise ValueError("Measurement settings must be a JSON object")
    _settings_json(manifest.get("settings", {}))
    seen = set()
    for view in manifest["views"]:
        if (type(view) is not dict or set(view) - {"id", "reference", "render", "mask"}
                or not {"id", "reference", "render"}.issubset(view)
                or type(view["id"]) is not str or not view["id"].strip() or "\0" in view["id"] or len(view["id"]) > 1024
                or view["id"] in seen):
            raise ValueError("Each view requires a unique nonempty id and explicit reference/render paths")
        seen.add(view["id"])
        for key in ("reference", "render") + (("mask",) if "mask" in view else ()):
            if type(view[key]) is not str or not view[key].strip() or "\0" in view[key]:
                raise ValueError("Measurement file paths must be nonempty strings")
    try:
        encoded = _json_bytes(manifest)
    except (TypeError, ValueError, RecursionError) as error:
        raise ValueError("Measurement manifest requires finite JSON values") from error
    if len(encoded) > MAX_REPORT_BYTES:
        raise ValueError("Measurement manifest exceeds 16 MiB")
    return json.loads(encoded)


def _checked_image(stack, snapshot, mask=False):
    import warnings
    from PIL import Image
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            snapshot.seek(0)
            image = stack.enter_context(Image.open(snapshot))
    except (Image.DecompressionBombError, Image.DecompressionBombWarning) as error:
        raise ValueError("Image decoding exceeds the bounded pixel budget") from error
    if image.format not in ("PNG", "JPEG") or image.mode != ("L" if mask else "RGB"):
        raise ValueError("Only explicit PNG/JPEG RGB8 images and grayscale8 masks are supported")
    width, height = image.size
    if min(width, height) < 11 or max(width, height) > 32768 or width * height > MAX_IMAGE_PIXELS:
        raise ValueError("Image dimensions exceed the measurement budget or 11x11 SSIM minimum")
    if getattr(image, "n_frames", 1) != 1 or "transparency" in image.info:
        raise ValueError("Image measurement does not silently discard animation or alpha")
    if image.getexif().get(274, 1) != 1:
        raise ValueError("Nonidentity EXIF orientation requires explicit preprocessing; no implicit transpose is performed")
    if image.format == "PNG":
        position = snapshot.tell()
        snapshot.seek(0)
        header = snapshot.read(29)
        snapshot.seek(position)
        if (len(header) < 29 or header[:8] != b"\x89PNG\r\n\x1a\n" or
                header[24] != 8 or header[25] != (0 if mask else 2)):
            raise ValueError("PNG requires actual eight-bit RGB/grayscale samples without alpha or a palette")
    elif getattr(image, "bits", 8) != 8:
        raise ValueError("JPEG measurement requires eight-bit samples")
    return image


def _rgb_tile(image, box, np):
    return np.asarray(image.crop(box), dtype=np.float64) / 255.0


def _ssim_windows(reference, rendered, mask, np):
    coefficients = np.asarray([math.exp(-((index - 5) ** 2) / 4.5) for index in range(11)],
                              dtype=np.float64)
    coefficients /= coefficients.sum()
    height, width, _ = reference.shape
    def smooth(value):
        horizontal = np.zeros((height, width - 10, 3), dtype=np.float64)
        for index, weight in enumerate(coefficients):
            horizontal += weight * value[:, index:index + width - 10]
        output = np.zeros((height - 10, width - 10, 3), dtype=np.float64)
        for index, weight in enumerate(coefficients):
            output += weight * horizontal[index:index + height - 10]
        return output
    mu_x, mu_y = smooth(reference), smooth(rendered)
    variance_x = np.maximum(smooth(reference * reference) - mu_x * mu_x, 0)
    variance_y = np.maximum(smooth(rendered * rendered) - mu_y * mu_y, 0)
    covariance = smooth(reference * rendered) - mu_x * mu_y
    scores = ((2 * mu_x * mu_y + .0001) * (2 * covariance + .0009) /
              ((mu_x * mu_x + mu_y * mu_y + .0001) * (variance_x + variance_y + .0009)))
    if not np.isfinite(scores).all() or scores.min() < -1 - 1e-9 or scores.max() > 1 + 1e-9:
        raise ValueError("SSIM produced nonfinite or materially out-of-range values")
    # Population SSIM has this mathematical range; bound only recorded FP roundoff.
    scores = np.clip(scores, -1.0, 1.0).mean(axis=2)
    if mask is None:
        return float(scores.sum(dtype=np.float64)), int(scores.size)
    integral = np.pad(mask.astype(np.int64), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    valid = (integral[11:, 11:] - integral[:-11, 11:] - integral[11:, :-11] + integral[:-11, :-11]) == 121
    return float(scores[valid].sum(dtype=np.float64)), int(valid.sum())


def _measure_pair(reference_snapshot, render_snapshot, mask_snapshot, cancelled):
    import numpy as np
    from contextlib import ExitStack
    with ExitStack() as stack:
        reference = _checked_image(stack, reference_snapshot)
        rendered = _checked_image(stack, render_snapshot)
        mask = _checked_image(stack, mask_snapshot, mask=True) if mask_snapshot is not None else None
        width, height = reference.size
        if reference.mode != "RGB" or rendered.mode != "RGB":
            raise ValueError("Image measurement requires explicit RGB8 inputs")
        if reference.size != rendered.size or (mask is not None and mask.size != reference.size):
            raise ValueError("Image measurement never resizes mismatched images or masks")
        if mask is not None and mask.mode != "L":
            raise ValueError("Image masks require explicit grayscale8 storage")
        if min(width, height) < 11 or width * height > MAX_IMAGE_PIXELS:
            raise ValueError("Image dimensions exceed the measurement budget or 11x11 SSIM minimum")
        squared_error, valid_pixels = 0.0, 0
        for top in range(0, height, TILE_HEIGHT):
            for left in range(0, width, TILE_WIDTH):
                _check_cancel(cancelled)
                box = (left, top, min(width, left + TILE_WIDTH), min(height, top + TILE_HEIGHT))
                x, y = _rgb_tile(reference, box, np), _rgb_tile(rendered, box, np)
                difference = (x - y) ** 2
                if mask is None:
                    squared_error += float(difference.sum(dtype=np.float64))
                    valid_pixels += int(difference.shape[0] * difference.shape[1])
                else:
                    valid = np.asarray(mask.crop(box)) > 0
                    squared_error += float(difference[valid].sum(dtype=np.float64))
                    valid_pixels += int(valid.sum())
        ssim_sum, windows = 0.0, 0
        for top in range(0, height - 10, TILE_HEIGHT):
            for left in range(0, width - 10, TILE_WIDTH):
                _check_cancel(cancelled)
                box = (left, top, min(width, left + TILE_WIDTH + 10), min(height, top + TILE_HEIGHT + 10))
                valid = np.asarray(mask.crop(box)) > 0 if mask is not None else None
                value, count = _ssim_windows(_rgb_tile(reference, box, np), _rgb_tile(rendered, box, np), valid, np)
                ssim_sum += value
                windows += count
        if not valid_pixels or not windows:
            raise ValueError("Image mask has no valid pixels or no complete valid 11x11 SSIM windows")
    return width, height, squared_error, valid_pixels, ssim_sum, windows


def _measure_snapshots(reference, rendered, mask, cancelled):
    """Hash the bytes copied to owned disk spools and decode only those bytes."""
    from contextlib import ExitStack
    import tempfile
    paths = {path for path in (reference, rendered, mask) if path is not None}
    # Native workers supply a workspace-owned TEMP on the data drive. An
    # explicit environment path avoids a previously cached system temp choice.
    temporary_directory = os.environ.get("TMPDIR") or os.environ.get("TEMP") or os.environ.get("TMP")
    with ExitStack() as owned:
        snapshots, hashes = {}, {}
        for path in paths:
            snapshot = owned.enter_context(tempfile.TemporaryFile(mode="w+b", dir=temporary_directory))
            hashes[path] = _hash(path, cancelled, copy_to=snapshot)
            snapshot.flush()
            snapshot.seek(0)
            snapshots[path] = snapshot
        measured = _measure_pair(snapshots[reference], snapshots[rendered],
                                 snapshots[mask] if mask is not None else None, cancelled)
        # A visible original-file change still fails the complete measurement.
        # Even an A->B->A change cannot substitute B into the private decoder.
        if any(_hash(path, cancelled) != hashes[path] for path in paths):
            raise ValueError("Measurement input changed while it was being decoded or measured")
        return measured, hashes


def measure_image_quality(manifest, base_dir=None, progress=None, cancelled=None):
    """Measure explicitly paired RGB8 files; unknown split stays unverified.

    ``progress`` receives one immutable-style JSON dict per completed view;
    ``cancelled`` is an optional callable checked between bounded CPU tiles.
    Originals are read-only. Owned, bounded temporary disk snapshots bind hashes
    to the exact decoded bytes and are removed on completion/error/cancellation.
    This interface publishes no output file; the CLI owns the new JSON output.
    """
    if (progress is not None and not callable(progress)) or (cancelled is not None and not callable(cancelled)):
        raise ValueError("Measurement progress and cancellation hooks must be callable")
    _check_cancel(cancelled)
    manifest = _validate_manifest(manifest)
    import numpy as np
    import PIL
    from PIL import features
    protocol = deepcopy(PROTOCOL)
    protocol["runtime"] = {"numpy": np.__version__, "pillow": PIL.__version__,
        "libjpeg_build": features.version_codec("jpg"), "zlib_build": features.version_codec("zlib"),
        "libjpeg_turbo_build": (features.version_feature("libjpeg_turbo")
                                 if "libjpeg_turbo" in features.get_supported_features() else None)}
    base = Path(base_dir or ".").resolve()
    if not base.is_dir():
        raise ValueError("Measurement base directory is unavailable")
    seen_references, seen_reference_files = set(), set()
    views, sum_error, total_pixels, sum_ssim, total_windows = [], 0.0, 0, 0.0, 0
    for view in manifest["views"]:
        _check_cancel(cancelled)
        reference, rendered = (base / view["reference"]).resolve(), (base / view["render"]).resolve()
        mask = (base / view["mask"]).resolve() if view.get("mask") else None
        key = os.path.normcase(str(reference))
        if key in seen_references:
            raise ValueError("Duplicate reference views are not silently weighted more than once")
        seen_references.add(key)
        paths = {path for path in (reference, rendered, mask) if path is not None}
        for path in paths:
            if not path.is_file() or path.stat().st_size > MAX_FILE_BYTES:
                raise ValueError("Measurement input is missing, not a file, or exceeds the 512 MiB file budget")
        info = reference.stat()
        if type(info.st_dev) is int and type(info.st_ino) is int and info.st_dev >= 0 and info.st_ino > 0:
            file_identity = (info.st_dev, info.st_ino)
            if file_identity in seen_reference_files:
                raise ValueError("Duplicate reference views include a hardlink/file identity alias")
            seen_reference_files.add(file_identity)
        measured, hashes = _measure_snapshots(reference, rendered, mask, cancelled)
        width, height, error, pixels, ssim, windows = measured
        mse = error / (pixels * 3)
        views.append({"id": view["id"], "width": width, "height": height,
                      "reference": str(reference), "render": str(rendered), "mask": str(mask) if mask else None,
                      "reference_sha256": hashes[reference], "render_sha256": hashes[rendered],
                      "mask_sha256": hashes[mask] if mask else None,
                      "valid_pixels": pixels, "valid_channels": pixels * 3, "ssim_windows": windows,
                      "metrics": {"mse": mse, "psnr": _psnr(mse), "ssim": ssim / windows}})
        sum_error += error
        total_pixels += pixels
        sum_ssim += ssim
        total_windows += windows
        if progress is not None:
            progress({"event": "progress", "completed": len(views), "total": len(manifest["views"]), "view_id": view["id"]})
    references = [[view["id"], view["reference_sha256"]] for view in views]
    report = {"version": 1, "split": manifest.get("split", "unverified"),
              "identity": {"dataset_id": _identity(sorted(references)), "viewset_id": _identity(references),
                  "resolution_id": _identity([[view["id"], view["width"], view["height"]] for view in views]),
                  "mask_id": _identity([[view["id"], view["mask_sha256"]] for view in views]),
                  "metric_protocol_id": _identity(protocol)},
              "metrics": {"psnr": _psnr(sum_error / (total_pixels * 3)), "ssim": sum_ssim / total_windows},
              "settings": deepcopy(manifest.get("settings", {})),
              "evidence": {"version": 1, "producer": "gsw-image-quality-v1", "held_out_verified": False,
                  "protocol": protocol, "aggregate": {"views": len(views), "valid_pixels": total_pixels,
                      "valid_channels": total_pixels * 3, "ssim_windows": total_windows}, "views": views}}
    if len(_json_bytes(report)) + 1 > MAX_REPORT_BYTES:
        raise ValueError("Image measurement report exceeds 16 MiB")
    return report
