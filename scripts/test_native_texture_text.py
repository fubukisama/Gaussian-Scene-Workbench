"""Compare overlay glyphs after a texture draw in independent native processes.

The native harness captures the public framebuffer. The untextured control is
an independent source of truth for static text, not an imitation of the OpenGL
glyph-atlas implementation. Geometry, basename, project-data text, font, window,
theme, and language stay equal; only the mesh's TextureFile comment differs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image


def write_fixture(directory: Path, textured: bool, texture_format: str) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    texture_name = f"atlas.{texture_format}"
    texture_comment = f"comment TextureFile {texture_name}\n" if textured else ""
    source = directory / "mesh.ply"
    source.write_text(
        "ply\nformat ascii 1.0\n" + texture_comment +
        "element vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "element face 1\nproperty list uchar int vertex_indices\n"
        "property list uchar float texcoord\nend_header\n"
        "20 20 20 0 0 1\n22 20 20 0 0 1\n22 22 20 0 0 1\n20 22 20 0 0 1\n"
        "4 0 1 2 3 8 0 0 1 0 1 1 0 1\n",
        encoding="ascii",
    )
    if textured:
        # Deliberately non-font-like colors make an incorrectly sampled model
        # texture easy to detect in the independent screenshot comparison.
        pixels = np.array(((220, 35, 25), (20, 175, 55),
                           (40, 70, 215), (225, 180, 30)), dtype=np.uint8).reshape(2, 2, 3)
        texture = Image.fromarray(pixels)
        if texture_format == "jpg":
            texture.save(directory / texture_name, quality=100, subsampling=0)
        else:
            texture.save(directory / texture_name)
    return source


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def copy_source_controls(source: Path, directory: Path) -> tuple[Path, Path, dict]:
    """Copy a real PLY payload unchanged; remove only its texture declaration.

    The textured fixture gets an unchanged copy of the original atlas and a
    relative declaration. Nothing is written beside the original mesh or atlas.
    Binary/ASCII payload bytes are preserved, rather than reparsing the model.
    """
    original_hash = file_sha256(source)
    with source.open("rb") as stream:
        header = []
        total = 0
        while True:
            line = stream.readline(64 * 1024)
            total += len(line)
            if not line or total > 1024 * 1024:
                raise AssertionError("Real fixture has an invalid or oversized PLY header")
            header.append(line)
            if line.strip() == b"end_header":
                break
        payload_offset = stream.tell()
    if header[0].strip() != b"ply":
        raise AssertionError("Real fixture must be a PLY mesh")
    declarations = [line for line in header if line.strip().startswith(b"comment TextureFile")]
    if len(declarations) != 1:
        raise AssertionError("Real texture-text fixture requires exactly one declared atlas")
    declared = declarations[0].strip()[len(b"comment TextureFile"):].strip().decode("utf-8")
    if len(declared) >= 2 and declared[0] in "\"'" and declared[-1] == declared[0]:
        declared = declared[1:-1]
    original_texture = (source.parent / declared).resolve()
    if not original_texture.is_file():
        raise AssertionError(f"Declared original texture is not available: {original_texture}")
    texture_hash = file_sha256(original_texture)
    counts = {}
    for line in header:
        match = re.fullmatch(rb"element (vertex|face) (\d+)", line.strip())
        if match:
            counts["source_vertices" if match[1] == b"vertex" else "source_faces"] = int(match[2])
    if counts.get("source_vertices", 0) <= 0 or counts.get("source_faces", 0) <= 0:
        raise AssertionError("Real fixture must contain mesh vertices and faces")

    destinations = []
    for textured, policy in ((False, "control"), (True, "textured")):
        target_directory = directory / policy
        target_directory.mkdir(parents=True, exist_ok=True)
        destination = target_directory / source.name
        if destination.resolve() == source.resolve():
            raise AssertionError("Validation copies must not overwrite the original mesh")
        atlas_name = "source-atlas" + original_texture.suffix
        with source.open("rb") as original, destination.open("wb") as copied:
            for line in header:
                if line in declarations:
                    if textured:
                        copied.write(f"comment TextureFile {atlas_name}\n".encode("utf-8"))
                else:
                    copied.write(line)
            original.seek(payload_offset)
            shutil.copyfileobj(original, copied, length=1024 * 1024)
        if textured:
            shutil.copy2(original_texture, target_directory / atlas_name)
            if file_sha256(target_directory / atlas_name) != texture_hash:
                raise AssertionError("Copied texture bytes differ from the original atlas")
        destinations.append(destination)
    if file_sha256(source) != original_hash or file_sha256(original_texture) != texture_hash:
        raise AssertionError("Original mesh or atlas changed during fixture preparation")
    (directory / "source-integrity.json").write_text(
        json.dumps({"mesh": str(source), "mesh_sha256": original_hash,
                    "atlas": str(original_texture), "atlas_sha256": texture_hash,
                    **counts}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return destinations[0], destinations[1], counts


def capture(executable: Path, output: Path, source: Path, *, textured: bool,
            language: str, theme: str, paging: str) -> dict:
    environment = os.environ.copy()
    environment["GSW_TEXTURE_TEXT_CAPTURE_PATH"] = str(output)
    environment["GSW_TEXTURE_TEXT_EXPECT_TEXTURE"] = "1" if textured else "0"
    resident_limit = "1" if paging == "paged" else "2147483647"
    environment["GSW_MESH_RESIDENT_VERTEX_LIMIT"] = resident_limit
    environment["GSW_MESH_RESIDENT_FACE_LIMIT"] = resident_limit
    environment["QT_FORCE_STDERR_LOGGING"] = "1"
    # Each process has its own Qt glyph cache and smoke-test settings. The
    # textured process cannot inherit glyph textures primed by its control.
    command = [str(executable), "--smoke-test-texture-text", "--smoke-scene",
               str(source), "--language", language, "--theme", theme]
    result = subprocess.run(command, env=environment, capture_output=True,
                            timeout=40, check=False)
    output.with_suffix(".stdout.log").write_bytes(result.stdout)
    output.with_suffix(".stderr.log").write_bytes(result.stderr)
    if result.returncode != 0:
        raise AssertionError(f"Native capture exited {result.returncode}: {output}")
    if not output.exists():
        raise AssertionError(f"Native capture did not produce its framebuffer: {output}")
    metadata = json.loads(Path(str(output) + ".json").read_text(encoding="utf-8"))
    if metadata["texture_available"] != textured:
        raise AssertionError("Capture did not exercise the requested texture state")
    if metadata["language"] != language or metadata["theme"] != theme:
        raise AssertionError("Capture language/theme differs from requested settings")
    return metadata


def first_title_mask(image: np.ndarray, scale: float, theme: str) -> np.ndarray:
    height, width, _ = image.shape
    # This broad screen region locates the first neutral-colored text line from
    # the control image. No private overlay rectangle, font advance, expected
    # string raster, or renderer color literal is reconstructed here.
    left, right = round(16 * scale), min(round(1000 * scale), width - round(260 * scale))
    top, bottom = round(10 * scale), min(round(60 * scale), height)
    crop = image[top:bottom, left:right].astype(np.int16)
    neutral = crop.max(axis=2) - crop.min(axis=2) <= 32
    ink = neutral & ((crop.max(axis=2) < 100) if theme == "light"
                     else (crop.min(axis=2) > 150))
    rows = np.flatnonzero(ink.sum(axis=1) >= max(3, round(3 * scale)))
    groups = np.split(rows, np.flatnonzero(np.diff(rows) > 1) + 1)
    line = next((group for group in groups if len(group) >= round(6 * scale)), None)
    if line is None:
        raise AssertionError("Control image has no meaningful static title text")
    mask = np.zeros((height, width), dtype=bool)
    mask[top + line[0]:top + line[-1] + 1, left:right] = ink[line[0]:line[-1] + 1]
    return mask


def mode_text_mask(image: np.ndarray, scale: float) -> np.ndarray:
    height, width, _ = image.shape
    left, right = max(0, width - round(250 * scale)), width - round(5 * scale)
    top, bottom = round(5 * scale), min(round(48 * scale), height)
    crop = image[top:bottom, left:right].astype(np.int16)
    # Discover the densely filled badge in the known-good control rather than
    # relying on its translated width, theme color, or private layout rectangle.
    # A thin blue world axis can cross this broad region: taking the bounding
    # box of every colored pixel would include white background as "glyphs".
    colored = crop.max(axis=2) - crop.min(axis=2) > 40
    row_counts = colored.sum(axis=1)
    dense_rows = np.flatnonzero(row_counts >= max(round(8 * scale), row_counts.max() * 0.20))
    row_groups = np.split(dense_rows, np.flatnonzero(np.diff(dense_rows) > 1) + 1)
    row_groups = [group for group in row_groups if len(group) >= round(8 * scale)]
    if not row_groups:
        raise AssertionError("Control image has no densely filled tool-state badge")
    rows = max(row_groups, key=lambda group: int(row_counts[group].sum()))
    column_counts = colored[rows[0]:rows[-1] + 1].sum(axis=0)
    dense_columns = np.flatnonzero(column_counts >= max(round(3 * scale), len(rows) * 0.25))
    column_groups = np.split(dense_columns, np.flatnonzero(np.diff(dense_columns) > 1) + 1)
    column_groups = [group for group in column_groups if len(group) >= round(10 * scale)]
    if not column_groups:
        raise AssertionError("Control image has no continuously filled tool-state badge")
    columns = max(column_groups, key=lambda group: int(column_counts[group].sum()))
    inset = max(2, round(3 * scale))
    x0, x1 = columns[0] + inset, columns[-1] - inset + 1
    y0, y1 = rows[0] + inset, rows[-1] - inset + 1
    interior = crop[y0:y1, x0:x1]
    ink = (interior.min(axis=2) > 170) & (interior.max(axis=2) - interior.min(axis=2) < 32)
    mask = np.zeros((height, width), dtype=bool)
    mask[top + y0:top + y1, left + x0:left + x1] = ink
    return mask


def compare_capture(control_path: Path, textured_path: Path, metadata: dict,
                    theme: str, *, verify_texture_sampling: bool = True) -> dict:
    control = np.asarray(Image.open(control_path).convert("RGB"))
    textured = np.asarray(Image.open(textured_path).convert("RGB"))
    if control.shape != textured.shape:
        raise AssertionError("Independent processes produced different framebuffer sizes")
    scale = control.shape[1] / metadata["logical_width"]
    difference = np.abs(control.astype(np.int16) - textured.astype(np.int16))
    checks = {}
    failed_regions = []
    if verify_texture_sampling:
        height, width, _ = control.shape
        # The fixture has deliberately colored texels and neutral untextured
        # material. A broad central model ROI proves that the actual mesh shader
        # sampled that texture, independently of texture-availability metadata.
        model_difference = difference[height // 6:height * 5 // 6,
                                      width // 6:width * 5 // 6].max(axis=2)
        changed = int(np.count_nonzero(model_difference > 32))
        checks["model_texture_sampling"] = {"changed_pixels": changed}
        print(f"model_texture_sampling: changed_pixels={changed}")
        if changed < round(1000 * scale * scale):
            failed_regions.append("model_texture_sampling")
    for name, mask, minimum in (
        ("title", first_title_mask(control, scale, theme), 500),
        ("tool_state", mode_text_mask(control, scale), 50),
    ):
        count = int(mask.sum())
        if count < round(minimum * scale * scale):
            raise AssertionError(f"Control {name} has insufficient glyph pixels: {count}")
        glyph_difference = difference[mask]
        overlap = float(np.mean(glyph_difference.max(axis=1) <= 32))
        mean_difference = float(glyph_difference.mean())
        checks[name] = {"glyph_pixels": count, "matching_fraction": overlap,
                        "mean_channel_difference": mean_difference}
        print(f"{name}: glyph_pixels={count}, matching_fraction={overlap:.6f}, "
              f"mean_channel_difference={mean_difference:.4f}")
        if overlap < 0.98 or mean_difference > 7.0:
            failed_regions.append(name)
    (control_path.parent / "comparison.json").write_text(
        json.dumps(checks, indent=2) + "\n", encoding="utf-8")
    if failed_regions:
        raise AssertionError("Texture-text comparison failed in: " + ", ".join(failed_regions))
    return checks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--language", choices=("zh_CN", "en_US", "ja_JP"), default="zh_CN")
    parser.add_argument("--theme", choices=("light", "dark"), default="light")
    parser.add_argument("--paging", choices=("resident", "paged"), default="resident")
    parser.add_argument("--texture-format", choices=("ppm", "png", "jpg"), default="ppm")
    parser.add_argument("--source", type=Path,
                        help="Use independent copies of a real single-atlas PLY instead of the tiny quad")
    args = parser.parse_args()
    suffix = ("" if args.paging == "resident" else "-paged") + (
        "" if args.texture_format == "ppm" else f"-{args.texture_format}")
    directory = args.output_directory.resolve() / f"{args.language}-{args.theme}{suffix}"
    directory.mkdir(parents=True, exist_ok=True)
    if args.source:
        control_source, textured_source, expected_counts = copy_source_controls(args.source.resolve(), directory)
    else:
        control_source = write_fixture(directory / "control", False, args.texture_format)
        textured_source = write_fixture(directory / "textured", True, args.texture_format)
        expected_counts = {"source_vertices": 4, "source_faces": 1}
    control_path, textured_path = directory / "control.png", directory / "textured.png"
    control_meta = capture(args.executable.resolve(), control_path, control_source,
                           textured=False, language=args.language, theme=args.theme, paging=args.paging)
    textured_meta = capture(args.executable.resolve(), textured_path, textured_source,
                            textured=True, language=args.language, theme=args.theme, paging=args.paging)
    for key in ("logical_width", "logical_height", "frame_width", "frame_height", "device_pixel_ratio"):
        if control_meta[key] != textured_meta[key]:
            raise AssertionError(f"Independent capture dimensions differ: {key}")
    for key, expected in expected_counts.items():
        if control_meta[key] != expected or textured_meta[key] != expected:
            raise AssertionError(f"Independent captures did not preserve source {key}: {expected}")
    compare_capture(control_path, textured_path, control_meta, args.theme,
                    verify_texture_sampling=not bool(args.source))
    print(f"PASS: model texture preserves static overlay text ({args.language}, {args.theme}, {args.paging})")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
