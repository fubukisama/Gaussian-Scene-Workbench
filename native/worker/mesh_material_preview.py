"""Lossless geometry / diffuse-atlas adapter for owned generated OBJ assets.

This is a preview artifact, not an edit of the mesh, an optimizer checkpoint,
or a general-purpose OBJ importer. Geometry and UV tables are spooled to disk
so conversion memory is independent of mesh size. Only opaque diffuse triangle
materials are accepted; unsupported effects are never silently discarded.
"""

import math
import ntpath
import os
import struct
import tempfile
from collections import OrderedDict
from pathlib import Path


MAX_ATLAS_SIZE = 8192
MAX_IMAGE_BYTES = 256 * 1024 * 1024
GUTTER = 2
_VERTEX = struct.Struct("<ddd")
_UV = struct.Struct("<dd")
_FACE = struct.Struct("<7i")
_PLY_FACE = struct.Struct("<B3iB6f")
_MAX_LINE = 1024 * 1024
_UV_CACHE_BLOCK = 65536
_UV_CACHE_BLOCKS = 128


def _check_cancel(cancel):
    if cancel is not None and cancel.is_set():
        raise InterruptedError("Material preview preparation cancelled")


def _inside(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _owned_file(path, root):
    candidate = Path(path).resolve()
    if not _inside(candidate, root) or not candidate.is_file():
        raise ValueError("Material asset is missing or outside the current job: " + str(path))
    return candidate


def _reference(base, name, root):
    name = name.strip()
    if len(name) >= 2 and name[0] == name[-1] and name[0] in "\"'":
        name = name[1:-1]
    # ntpath also rejects Windows drive/UNC paths when tests run on POSIX.
    if not name or "\x00" in name or ":" in name or ntpath.isabs(name) or Path(name).is_absolute():
        raise ValueError("Material references must be relative local paths: " + name)
    return _owned_file(base / name.replace("\\", "/"), root)


def _numbers(values, expected, label):
    if len(values) != expected:
        raise ValueError("Invalid " + label)
    result = tuple(float(value) for value in values)
    if not all(math.isfinite(value) for value in result):
        raise ValueError("Non-finite " + label)
    return result


def _lines(path, cancel):
    # Bound individual records too: a malicious line cannot allocate a GB.
    with path.open("r", encoding="utf-8-sig", errors="strict") as stream:
        number = 0
        while True:
            line = stream.readline(_MAX_LINE + 1)
            if not line:
                return
            number += 1
            if len(line) > _MAX_LINE:
                raise ValueError("OBJ/MTL record exceeds the preview limit")
            if number % 4096 == 1:
                _check_cancel(cancel)
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            if line.endswith("\\"):
                raise ValueError("OBJ/MTL continuation records are unsupported")
            parts = line.split(None, 1)
            yield parts[0], parts[1].strip() if len(parts) > 1 else ""


def _load_materials(path, root, materials, signatures, cancel):
    signatures[path] = path.stat()
    current = None
    for keyword, value in _lines(path, cancel):
        key = keyword.lower()
        if key == "newmtl":
            if not value or value in materials:
                raise ValueError("Missing or duplicate OBJ material name")
            current = {"texture": None}
            materials[value] = current
            continue
        if current is None:
            raise ValueError("MTL property appears before newmtl")
        if key == "map_kd":
            if value.startswith("-"):
                raise ValueError("MTL diffuse map options are unsupported")
            if current["texture"] is not None:
                raise ValueError("Duplicate MTL diffuse map")
            current["texture"] = _reference(path.parent, value, root)
        elif key == "kd":
            if _numbers(value.split(), 3, "MTL diffuse color") != (1.0, 1.0, 1.0):
                raise ValueError("Non-white MTL diffuse multipliers are unsupported")
        elif key in {"d", "tr"}:
            opacity = _numbers(value.split(), 1, "MTL opacity")[0]
            if opacity != (1.0 if key == "d" else 0.0):
                raise ValueError("Transparent MTL materials are unsupported")
        elif key in {"ka", "ks", "ke", "tf"}:
            color = _numbers(value.split(), 3, "MTL material color")
            if key == "ka":
                if color not in {(0.0, 0.0, 0.0), (1.0, 1.0, 1.0)}:
                    raise ValueError("Colored MTL ambient lighting is unsupported")
            elif key == "tf":
                if color != (1.0, 1.0, 1.0):
                    raise ValueError("MTL transmission filters are unsupported")
            elif color != (0.0, 0.0, 0.0):
                raise ValueError("Non-diffuse MTL effects are unsupported")
        elif key == "illum":
            if value not in {"0", "1", "2"}:
                raise ValueError("Non-diffuse MTL illumination is unsupported")
        elif key in {"ns", "ni"}:
            _numbers(value.split(), 1, "MTL numeric property")
        else:
            raise ValueError("Unsupported MTL property: " + keyword)


def _index(value, count, label):
    index = int(value)
    if index == 0:
        raise ValueError("OBJ indices cannot be zero")
    if index < 0:
        index = count + index
        if index < 0:
            raise ValueError("OBJ negative " + label + " index is out of range")
        return index
    if index > 2147483647:
        raise ValueError("OBJ index exceeds the native int32 format")
    return index - 1


def _parse_obj(obj, root, work, signatures, cancel):
    materials = {}
    material_ids = {}
    used_names = []
    loaded_libraries = set()
    active = None
    counts = {"vertices": 0, "uvs": 0, "normals": 0, "faces": 0}
    signatures[obj] = obj.stat()
    with (work / "vertices.bin").open("wb") as vertices, (work / "uvs.bin").open("wb") as uvs, (work / "faces.bin").open("wb") as faces:
        for key, value in _lines(obj, cancel):
            values = value.split()
            if key == "v":
                xyz = _numbers(values, 3, "OBJ vertex (only Cartesian XYZ is supported)")
                vertices.write(_VERTEX.pack(*xyz))
                counts["vertices"] += 1
                if counts["vertices"] > 2147483647:
                    raise ValueError("OBJ vertices exceed the native int32 format")
            elif key == "vt":
                uv = _numbers(values, 2, "OBJ texture coordinate")
                if not all(0.0 <= component <= 1.0 for component in uv):
                    raise ValueError("Tiled or out-of-range OBJ UV coordinates are unsupported")
                uvs.write(_UV.pack(*uv))
                counts["uvs"] += 1
            elif key == "vn":
                _numbers(values, 3, "OBJ normal")
                counts["normals"] += 1
            elif key == "mtllib":
                library = _reference(obj.parent, value, root)
                if library not in loaded_libraries:
                    _load_materials(library, root, materials, signatures, cancel)
                    loaded_libraries.add(library)
            elif key == "usemtl":
                if not value:
                    raise ValueError("OBJ usemtl is empty")
                active = value
            elif key == "f":
                if len(values) != 3:
                    raise ValueError("Material previews require triangular OBJ faces")
                if active is None:
                    raise ValueError("OBJ faces have no diffuse material")
                if active not in material_ids:
                    material_ids[active] = len(used_names)
                    used_names.append(active)
                vertex_indices, uv_indices = [], []
                for token in values:
                    reference = token.split("/")
                    if len(reference) not in {2, 3} or not reference[0] or not reference[1]:
                        raise ValueError("Every OBJ face corner must have a vertex and UV index")
                    vertex_indices.append(_index(reference[0], counts["vertices"], "vertex"))
                    uv_indices.append(_index(reference[1], counts["uvs"], "UV"))
                    if len(reference) == 3:
                        if not reference[2]:
                            raise ValueError("OBJ corner normal index is empty")
                        normal = _index(reference[2], counts["normals"], "normal")
                        if normal >= counts["normals"]:
                            raise ValueError("OBJ normal index is out of range")
                faces.write(_FACE.pack(*(vertex_indices + uv_indices + [material_ids[active]])))
                counts["faces"] += 1
            elif key not in {"o", "g", "s"}:
                raise ValueError("Unsupported OBJ record: " + key)
    if not counts["vertices"] or not counts["faces"] or not counts["uvs"]:
        raise ValueError("Generated OBJ has no textured triangle geometry")
    # A declared but unused material cannot invent texture pages in the atlas.
    used_materials = []
    for name in used_names:
        if name not in materials or materials[name]["texture"] is None:
            raise ValueError("OBJ material is missing its diffuse texture: " + name)
        used_materials.append(materials[name])
    return counts, used_materials


def _layout(pages):
    if len(pages) == 1:
        width, height = pages[0][1]
        return (width, height), {pages[0][0]: (0, 0, width, height)}
    boxes = sorted(pages, key=lambda page: (-page[1][1], -page[1][0], str(page[0])))
    minimum = max(size[0] + 2 * GUTTER for _, size in boxes)
    if minimum > MAX_ATLAS_SIZE:
        raise ValueError("Full-resolution texture pages exceed the 8192 px atlas limit")
    candidates = {minimum, MAX_ATLAS_SIZE}
    candidates.update(2 ** power for power in range(1, 14) if minimum <= 2 ** power <= MAX_ATLAS_SIZE)
    best = None
    for row_limit in sorted(candidates):
        x, y, row_height, used_width = 0, 0, 0, 0
        placements = {}
        for path, (width, height) in boxes:
            box_width, box_height = width + 2 * GUTTER, height + 2 * GUTTER
            if x + box_width > row_limit:
                x, y, row_height = 0, y + row_height, 0
            if y + box_height > MAX_ATLAS_SIZE:
                break
            placements[path] = (x + GUTTER, y + GUTTER, width, height)
            x += box_width
            used_width = max(used_width, x)
            row_height = max(row_height, box_height)
        if len(placements) == len(pages):
            height = y + row_height
            score = (used_width * height, max(used_width, height), used_width)
            if best is None or score < best[0]:
                best = (score, (used_width, height), placements)
    if best is None:
        raise ValueError("Full-resolution texture pages exceed the 8192 px atlas limit")
    return best[1], best[2]


def _make_atlas(materials, signatures, work, cancel):
    from PIL import Image

    paths = list(dict.fromkeys(material["texture"] for material in materials))
    pages = []
    for path in paths:
        _check_cancel(cancel)
        signatures[path] = path.stat()
        with Image.open(str(path)) as image:
            width, height = image.size
            if width <= 0 or height <= 0 or width > MAX_ATLAS_SIZE or height > MAX_ATLAS_SIZE:
                raise ValueError("Diffuse texture exceeds the 8192 px preview limit")
            if getattr(image, "n_frames", 1) != 1:
                raise ValueError("Animated material textures are unsupported")
            # EXIF transformations are not OBJ UV transforms. Reject rather than
            # auto-rotating pixels differently from the native image loader.
            if image.getexif().get(274, 1) != 1:
                raise ValueError("EXIF-oriented material textures are unsupported")
            if image.mode not in {"RGB", "RGBA", "L", "LA", "P", "1"}:
                raise ValueError("HDR / non-byte material textures are unsupported")
            pages.append((path, (width, height)))
    size, placements = _layout(pages)
    largest = max(width * height for _, (width, height) in pages)
    # Atlas plus original decode, RGBA conversion and worst-case padding copy.
    if 4 * (size[0] * size[1] + largest * 3) > MAX_IMAGE_BYTES:
        raise ValueError("Full-resolution textures exceed the 256 MiB preview image budget")
    atlas = Image.new("RGBA", size, (0, 0, 0, 255))
    try:
        for path, _ in pages:
            _check_cancel(cancel)
            with Image.open(str(path)) as source:
                image = source.convert("RGBA")
                try:
                    if image.getchannel("A").getextrema() != (255, 255):
                        raise ValueError("Diffuse image transparency is unsupported")
                    x, y, width, height = placements[path]
                    atlas.paste(image, (x, y))
                    if len(pages) > 1:
                        # Extend edge pixels into each page's private gutter so
                        # bilinear filtering never samples the adjacent material.
                        edges = [((0, 0, width, 1), (width, GUTTER), (x, y - GUTTER)),
                                 ((0, height - 1, width, height), (width, GUTTER), (x, y + height)),
                                 ((0, 0, 1, height), (GUTTER, height), (x - GUTTER, y)),
                                 ((width - 1, 0, width, height), (GUTTER, height), (x + width, y))]
                        for bounds, dimensions, offset in edges:
                            strip = image.crop(bounds).resize(dimensions, resample=Image.NEAREST)
                            atlas.paste(strip, offset)
                            strip.close()
                        for px, py, ox, oy in ((0, 0, -GUTTER, -GUTTER), (width - 1, 0, width, -GUTTER),
                                              (0, height - 1, -GUTTER, height), (width - 1, height - 1, width, height)):
                            atlas.paste(image.getpixel((px, py)), (x + ox, y + oy, x + ox + GUTTER, y + oy + GUTTER))
                finally:
                    image.close()
        _check_cancel(cancel)
        atlas.save(str(work / "atlas.png"), format="PNG")
    finally:
        atlas.close()
    return size, placements, len(pages)


class _UvReader:
    """An 8 MiB LRU avoids one disk seek per corner on large generated OBJs."""
    def __init__(self, stream):
        self.stream = stream
        self.blocks = OrderedDict()

    def read(self, index):
        offset = index * _UV.size
        block_index, within = divmod(offset, _UV_CACHE_BLOCK)
        block = self.blocks.get(block_index)
        if block is None:
            self.stream.seek(block_index * _UV_CACHE_BLOCK)
            block = self.stream.read(_UV_CACHE_BLOCK)
            self.blocks[block_index] = block
            if len(self.blocks) > _UV_CACHE_BLOCKS:
                self.blocks.popitem(last=False)
        else:
            self.blocks.move_to_end(block_index)
        return _UV.unpack_from(block, within)


def _write_ply(counts, materials, size, placements, work, cancel):
    header = ("ply\nformat binary_little_endian 1.0\ncomment TextureFile atlas.png\n"
              "element vertex {vertices}\nproperty double x\nproperty double y\nproperty double z\n"
              "element face {faces}\nproperty list uchar int vertex_indices\n"
              "property list uchar float texcoord\nend_header\n").format(**counts)
    with (work / "preview.ply").open("wb") as output, (work / "vertices.bin").open("rb") as vertices, (work / "uvs.bin").open("rb") as uvs, (work / "faces.bin").open("rb") as faces:
        uv_reader = _UvReader(uvs)
        output.write(header.encode("ascii"))
        while True:
            _check_cancel(cancel)
            block = vertices.read(1024 * 1024)
            if not block:
                break
            output.write(block)
        for face_number in range(counts["faces"]):
            if face_number % 4096 == 0:
                _check_cancel(cancel)
            indices = _FACE.unpack(faces.read(_FACE.size))
            if any(index < 0 or index >= counts["vertices"] for index in indices[:3]):
                raise ValueError("OBJ vertex index is out of range")
            coordinates = []
            x, y, width, height = placements[materials[indices[6]]["texture"]]
            for index in indices[3:6]:
                if index < 0 or index >= counts["uvs"]:
                    raise ValueError("OBJ UV index is out of range")
                u, v = uv_reader.read(index)
                # PNG row zero is the top; OpenGL UV origin is bottom-left. The
                # native renderer flips image rows during upload, not the UVs.
                coordinates.extend(((x + u * width) / size[0], (size[1] - y - height + v * height) / size[1]))
            output.write(_PLY_FACE.pack(3, *indices[:3], 6, *coordinates))


def prepare_material_preview(files, output_directory, asset_root, cancel=None):
    """Publish a validated, full-resolution diffuse PLY/PNG preview atomically.

    ``output_directory`` must be a new directory inside this job's asset_root.
    Original source assets are read-only. A failed conversion leaves no PLY or
    atlas; completed source geometry and texture packages remain untouched.
    """
    root = Path(asset_root).resolve()
    _check_cancel(cancel)
    obj_value = files.get("obj")
    # The backend's source_obj may be an external SuGaR provenance location. It
    # is never read. Raw OpenMVS assets are preferred only inside this owned job.
    raw = files.get("openmvs_obj")
    # OpenMVS's exporter writes legacy Tr=1 for opaque materials. The owned
    # collector normalizes that known provenance to d=1 in the copied bundle;
    # preferring the raw OBJ here would undo the explicit normalization.
    if raw and files.get("material_normalization") != "openmvs_opaque_Tr":
        candidate = Path(raw).resolve()
        if _inside(candidate, root) and candidate.is_file():
            obj_value = str(candidate)
    if not obj_value:
        raise ValueError("The texture result has no owned OBJ mesh")
    obj = _owned_file(obj_value, root)
    directory = Path(output_directory).resolve()
    if directory == root or not _inside(directory, root):
        raise ValueError("Material preview directory must be inside the current job")
    directory.parent.mkdir(parents=True, exist_ok=True)
    directory.mkdir()  # exclusive ownership: never overwrite an earlier preview
    signatures = {}
    published = False
    try:
        with tempfile.TemporaryDirectory(prefix=".prepare-", dir=str(directory)) as temporary:
            work = Path(temporary)
            counts, materials = _parse_obj(obj, root, work, signatures, cancel)
            size, placements, pages = _make_atlas(materials, signatures, work, cancel)
            _write_ply(counts, materials, size, placements, work, cancel)
            for path, before in signatures.items():
                after = path.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise ValueError("A material source changed during preview preparation")
            _check_cancel(cancel)
            # The complete PLY is the publication gate, and appears last. The
            # caller receives paths only after both complete assets are present.
            os.replace(str(work / "atlas.png"), str(directory / "atlas.png"))
            os.replace(str(work / "preview.ply"), str(directory / "preview.ply"))
            published = True
        return {"ply": str(directory / "preview.ply"), "atlas": str(directory / "atlas.png"),
                "vertices": counts["vertices"], "faces": counts["faces"],
                "materials": len(materials), "texturePages": pages, "sourceObj": str(obj)}
    finally:
        if not published:
            for name in ("atlas.png", "preview.ply"):
                try:
                    (directory / name).unlink()
                except FileNotFoundError:
                    pass
            directory.rmdir()
