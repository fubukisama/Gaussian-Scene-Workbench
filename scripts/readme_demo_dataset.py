#!/usr/bin/env python3
"""Create an original, CPU-only multi-view fixture for a GSW README demo.

This is a deliberately standalone demo-data generator, not a reconstruction
algorithm or accuracy benchmark. It reads no pictures, models, scenes, user
preferences, or checkpoints. NumPy and Pillow are its only dependencies.

Example (run by the demo owner, never implicitly by a test):
    python scripts/readme_demo_dataset.py --output D:/CodexData/GSW/readme-demo/orbit-01

All output must be in a new D-drive directory. The renderer emits RGB PNGs
only: no surface samples, point clouds, meshes, depth, masks, or normal maps.
The independent random-initializer recipe is only a coarse manually chosen
volume and seed; GSW's existing initializer materializes it in training preparation.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import stat
import sys
import time


GENERATOR_VERSION = "gsw.readme_orbit_dataset/1"
SCENE_NAME = "Orbit Study"
# A manual coarse work volume, not computed from primitive bounds or surfaces.
DEFAULT_INITIALIZATION_BOUNDS = [[-1.50, -1.50, -0.18], [1.50, 1.50, 2.15]]


def _sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _json_hash(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=True, sort_keys=True,
                                    separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def _write_json(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def _is_reparse(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def _fresh_output(value):
    path = Path(value)
    if not path.is_absolute() or path.drive.upper() != "D:":
        raise ValueError("An explicit absolute D-drive output path is required.")
    if path.exists() or path.is_symlink():
        raise ValueError("Output already exists; select a new directory. No overwrite is supported.")
    for ancestor in path.parents:
        if ancestor.exists() and _is_reparse(ancestor):
            raise ValueError("Output ancestors must not be symbolic links or junctions.")
    path.mkdir(parents=True, exist_ok=False)
    return path.resolve()


def _normalize(value):
    import numpy as np
    array = np.asarray(value, dtype=np.float64)
    return array / np.maximum(np.linalg.norm(array, axis=-1, keepdims=True), 1e-15)


def _basis(axis):
    """Return a proper right-handed rotation with local Z along axis."""
    import numpy as np
    z = _normalize(axis)
    helper = [0., 0., 1.] if abs(z[2]) < .95 else [0., 1., 0.]
    x = _normalize(np.cross(helper, z))
    y = np.cross(z, x)
    return np.column_stack((x, y, z))


def _scene():
    """Original asymmetric tabletop sculpture; no loaded geometry or assets."""
    import numpy as np
    # All coordinate values are arbitrary synthetic units, never meters.
    return [
        {"kind": "cylinder", "center": np.array([0., 0., -.075]), "radius": 1.34,
         "half_height": .075, "color": [.43, .26, .125], "material": "walnut", "roughness": .75},
        {"kind": "torus", "center": np.array([-.08, .06, 1.00]), "major": .57, "minor": .135,
         "rotation": _basis([.12, 1., .17]), "color": [.025, .37, .46],
         "material": "blue_ceramic", "roughness": .28},
        {"kind": "sphere", "center": np.array([-.70, -.32, .34]), "radius": .34,
         "color": [.90, .16, .075], "material": "coral", "roughness": .42},
        {"kind": "sphere", "center": np.array([.015, -.04, .99]), "radius": .205,
         "color": [.95, .59, .055], "material": "gold", "roughness": .26},
        {"kind": "cylinder", "center": np.array([.65, .20, .28]), "radius": .245,
         "half_height": .28, "color": [.73, .77, .65], "material": "ivory_ceramic", "roughness": .42},
        {"kind": "sphere", "center": np.array([.65, .20, .70]), "radius": .165,
         "color": [.93, .36, .075], "material": "coral", "roughness": .36},
        {"kind": "sphere", "center": np.array([.46, -.62, .115]), "radius": .115,
         "color": [.025, .45, .35], "material": "blue_ceramic", "roughness": .26},
        {"kind": "sphere", "center": np.array([-.31, .78, .155]), "radius": .155,
         "color": [.91, .67, .19], "material": "gold", "roughness": .34},
    ]


def _sphere_interval(origins, directions, center, radius):
    import numpy as np
    offset = origins - center
    b = np.sum(offset * directions, axis=1)
    c = np.sum(offset * offset, axis=1) - radius * radius
    discriminant = b * b - c
    root = np.sqrt(np.maximum(discriminant, 0.))
    return -b - root, -b + root, discriminant >= 0.


def _intersect(obj, origins, directions, far=None):
    """RGB-rendering intersections only; nothing from here initializes GSW."""
    import numpy as np
    count = len(directions)
    distance = np.full(count, np.inf)
    local = origins - obj["center"]
    if obj["kind"] == "sphere":
        near, end, valid = _sphere_interval(origins, directions, obj["center"], obj["radius"])
        candidate = np.where(near > 1e-5, near, end)
        good = valid & (candidate > 1e-5)
        distance[good] = candidate[good]
        return distance
    if obj["kind"] == "cylinder":
        a = np.sum(directions[:, :2] ** 2, axis=1)
        b = np.sum(local[:, :2] * directions[:, :2], axis=1)
        c = np.sum(local[:, :2] ** 2, axis=1) - obj["radius"] ** 2
        discriminant = b * b - a * c
        root = np.sqrt(np.maximum(discriminant, 0.))
        denominator = np.where(a > 1e-14, a, 1.)
        for candidate in ((-b - root) / denominator, (-b + root) / denominator):
            z = local[:, 2] + candidate * directions[:, 2]
            good = ((a > 1e-14) & (discriminant >= 0.) & (candidate > 1e-5)
                    & (np.abs(z) <= obj["half_height"]) & (candidate < distance))
            distance[good] = candidate[good]
        zdir = directions[:, 2]
        denominator = np.where(np.abs(zdir) > 1e-14, zdir, 1.)
        for cap in (-obj["half_height"], obj["half_height"]):
            candidate = (cap - local[:, 2]) / denominator
            xy = local[:, :2] + candidate[:, None] * directions[:, :2]
            good = ((np.abs(zdir) > 1e-14) & (candidate > 1e-5)
                    & (np.sum(xy * xy, axis=1) <= obj["radius"] ** 2) & (candidate < distance))
            distance[good] = candidate[good]
        return distance
    # A torus has an exact signed distance. Restrict deterministic sphere
    # tracing to its bounding sphere so most background rays do no SDF work.
    near, end, valid = _sphere_interval(origins, directions, obj["center"], obj["major"] + obj["minor"])
    begin = np.maximum(near, 1e-5)
    if far is not None:
        end = np.minimum(end, far)
    active = np.flatnonzero(valid & (end >= begin))
    if not len(active):
        return distance
    ray_origins = local @ obj["rotation"]
    ray_directions = directions @ obj["rotation"]
    t = begin.copy()
    for unused in range(112):
        if not len(active):
            break
        position = ray_origins[active] + t[active, None] * ray_directions[active]
        radial = np.sqrt(np.sum(position[:, :2] ** 2, axis=1)) - obj["major"]
        sdf = np.sqrt(radial * radial + position[:, 2] ** 2) - obj["minor"]
        hit = np.abs(sdf) < 7e-5
        distance[active[hit]] = t[active[hit]]
        remaining = active[~hit]
        t[remaining] += np.maximum(np.abs(sdf[~hit]) * .92, 3e-5)
        active = remaining[t[remaining] <= end[remaining]]
    return distance


def _surface(obj, points):
    import numpy as np
    local = points - obj["center"]
    if obj["kind"] == "sphere":
        normal = _normalize(local)
    elif obj["kind"] == "cylinder":
        cap = np.abs(np.abs(local[:, 2]) - obj["half_height"]) < 1e-5
        normal = _normalize(np.column_stack((local[:, :2], np.zeros(len(points)))))
        normal[cap] = np.column_stack((np.zeros((int(cap.sum()), 2)), np.sign(local[cap, 2])))
    else:
        local = local @ obj["rotation"]
        rho = np.maximum(np.linalg.norm(local[:, :2], axis=1), 1e-12)
        radial = rho - obj["major"]
        normal = _normalize(np.column_stack((local[:, :2] * (radial / rho)[:, None], local[:, 2])))
        normal = normal @ obj["rotation"].T
    base = np.tile(np.asarray(obj["color"], dtype=np.float64), (len(points), 1))
    if obj["material"] == "walnut":
        radius = np.linalg.norm(local[:, :2], axis=1)
        grain = .91 + .075 * np.sin(radius * 83. + .8 * np.sin(local[:, 0] * 19.))
        base *= grain[:, None]
        rim = (np.abs(local[:, 2]) < obj["half_height"] - 1e-5)
        base[rim] *= .66
    elif obj["kind"] == "torus":
        angle = np.arctan2(local[:, 1], local[:, 0])
        # Original in-world glaze pattern; stable across every camera.
        stripe = np.cos(angle * 13. + local[:, 2] * 19.) > .82
        base[stripe] = [.065, .64, .63]
    elif obj["material"] == "ivory_ceramic":
        band = np.sin(local[:, 2] * 78.) > .58
        base[band] *= [.74, .81, .83]
    elif obj["material"] == "coral":
        pattern = .96 + .035 * np.sin(local[:, 0] * 31. + local[:, 2] * 11.)
        base *= pattern[:, None]
    return normal, base


def _trace(objects, origins, directions):
    import numpy as np
    nearest = np.full(len(directions), np.inf)
    labels = np.full(len(directions), -1, dtype=np.int16)
    for index, obj in enumerate(objects):
        distance = _intersect(obj, origins, directions, far=nearest)
        closer = distance < nearest
        nearest[closer] = distance[closer]
        labels[closer] = index
    return nearest, labels


def _render(camera, objects, supersample=1):
    import numpy as np
    from PIL import Image
    width, height = camera["width"], camera["height"]
    pose, k = np.asarray(camera["c2w_opencv"]), np.asarray(camera["K"])
    scale = supersample
    u, v = np.meshgrid((np.arange(width * scale) + .5) / scale,
                       (np.arange(height * scale) + .5) / scale)
    rays = np.stack(((u - k[0, 2]) / k[0, 0], (v - k[1, 2]) / k[1, 1], np.ones_like(u)), axis=-1)
    directions = _normalize(rays.reshape(-1, 3) @ pose[:3, :3].T)
    origins = np.broadcast_to(pose[:3, 3], directions.shape)
    # Chunking bounds CPU RAM even when 2x anti-aliasing is requested.
    pixels = np.empty((len(directions), 3), dtype=np.float64)
    key = _normalize([-.65, -.85, 1.6])
    fill = _normalize([.8, .5, .65])
    for begin in range(0, len(directions), 32768):
        end = min(begin + 32768, len(directions))
        ray = directions[begin:end]
        origin = origins[begin:end]
        distance, labels = _trace(objects, origin, ray)
        sky_factor = np.clip(ray[:, 2] * .32 + .60, 0., 1.)
        colors = np.column_stack((.012 + .012 * sky_factor, .020 + .020 * sky_factor,
                                  .034 + .032 * sky_factor))
        for index, obj in enumerate(objects):
            selected = np.flatnonzero(labels == index)
            if not len(selected):
                continue
            hit = origin[selected] + distance[selected, None] * ray[selected]
            normal, base = _surface(obj, hit)
            light = np.broadcast_to(key, hit.shape)
            shadow_origin = hit + normal * .0008
            blocker, unused = _trace(objects, shadow_origin, light)
            # Ambient + a world-fixed key/fill. These are renderer-only
            # intersections, not exported supervision or an initializer.
            visible = ~np.isfinite(blocker)
            diffuse = (.23 + .88 * np.maximum(normal @ key, 0.) * visible
                       + .22 * np.maximum(normal @ fill, 0.))
            halfway = _normalize(key - ray[selected])
            exponent = 18. + (1. - obj["roughness"]) * 118.
            specular = np.maximum(np.sum(normal * halfway, axis=1), 0.) ** exponent
            specular *= visible * (.08 + (1. - obj["roughness"]) * .32)
            colors[selected] = base * diffuse[:, None] + specular[:, None] * [1., .91, .77]
        pixels[begin:end] = colors
    pixels = np.clip(pixels.reshape(height * scale, width * scale, 3), 0., 1.)
    # Standard sRGB encoding, not a hidden per-camera exposure adjustment.
    pixels = np.where(pixels <= .0031308, pixels * 12.92, 1.055 * np.power(pixels, 1. / 2.4) - .055)
    image = Image.fromarray(np.rint(pixels * 255.).astype(np.uint8), "RGB")
    if scale != 1:
        image = image.resize((width, height), Image.Resampling.LANCZOS)
    return image


def _cameras(view_count, resolution):
    import numpy as np
    result = []
    focal = resolution / (2. * math.tan(math.radians(48.) / 2.))
    target = np.asarray([0., 0., .76])
    for index in range(view_count):
        angle = math.radians(13.) + index * 2. * math.pi / view_count
        elevation = math.radians(18. + 13. * (.5 + .5 * math.sin(index * 2.399963229728653)))
        radius = 3.65
        position = target + radius * np.asarray([math.cos(angle) * math.cos(elevation),
                                                 math.sin(angle) * math.cos(elevation), math.sin(elevation)])
        forward = _normalize(target - position)
        right = _normalize(np.cross(forward, [0., 0., 1.]))
        down = np.cross(forward, right)
        pose = np.eye(4)
        pose[:3, :3] = np.column_stack((right, down, forward))
        pose[:3, 3] = position
        # Every fifth orbit sample is held out, alternating validation/test.
        split = ("val" if (index // 5) % 2 == 0 else "test") if index % 5 == 4 else "train"
        result.append({"view_id": "orbit-%03d" % index,
                       "image_path": "images/%s/orbit-%03d.png" % (split, index),
                       "width": resolution, "height": resolution,
                       "K": [[focal, 0., resolution / 2.], [0., focal, resolution / 2.], [0., 0., 1.]],
                       "c2w_opencv": pose.tolist(), "pixel_coordinates": "pixel_border",
                       "projection_model": "PINHOLE", "distortion": [], "undistorted": True,
                       "split": split})
    return result


def _initializer_recipe(bounds, count, seed):
    """Independent manual recipe: deliberately receives no renderer/scene."""
    if len(bounds) != 6 or any(not math.isfinite(value) for value in bounds):
        raise ValueError("Initialization bounds require six finite numbers.")
    low, high = list(bounds[:3]), list(bounds[3:])
    if any(low[axis] >= high[axis] for axis in range(3)):
        raise ValueError("Initialization bounds must have positive extent in all axes.")
    return {"kind": "random", "frame": "source", "priors": [], "bounds": [low, high],
            "count": count, "seed": seed,
            "provenance": {"kind": "manual_coarse_volume", "generated_by": "GSW existing random initializer",
                           "sampling": "uniform_volume_not_surface", "color_source": "constant_neutral_gray_not_image_or_truth",
                           "bounds_source": "explicit_operator_chosen_demo_work_volume",
                           "geometry_or_depth_consumed": False, "full_optimizer_resume": False}}


def _blender_manifest(cameras, split):
    frames = []
    for camera in cameras:
        if camera["split"] != split:
            continue
        pose = [list(row) for row in camera["c2w_opencv"]]
        for row in pose[:3]:
            row[1], row[2] = -row[1], -row[2]
        frames.append({"view_id": camera["view_id"], "file_path": camera["image_path"],
                       "transform_matrix": pose})
    first = cameras[0]
    return {"w": first["width"], "h": first["height"], "K": first["K"],
            "pixel_coordinates": "pixel_border", "camera_model": "PINHOLE",
            "scale": {"status": "arbitrary", "source_unit": "unknown", "meters_per_unit": None, "evidence": []},
            "frames": frames}


def generate(args):
    import numpy as np
    import PIL
    started = time.monotonic()
    recipe = _initializer_recipe(args.initialization_bounds, args.initial_points, args.initial_seed)
    cameras = _cameras(args.views, args.resolution)
    output = _fresh_output(args.output)
    # This marker deliberately remains after failure; rerun into a new output.
    _write_json(output / "generation-started.json", {"schema": GENERATOR_VERSION, "status": "incomplete"})
    objects = _scene()
    for camera in cameras:
        path = output / camera["image_path"]
        path.parent.mkdir(parents=True, exist_ok=True)
        _render(camera, objects, args.supersample).save(path, format="PNG", compress_level=6)
        camera["sha256"] = _sha256(path)
        print(json.dumps({"stage": "render", "view_id": camera["view_id"], "split": camera["split"],
                          "completed": len([view for view in cameras if "sha256" in view]), "total": len(cameras)}), flush=True)
    image_hashes = [camera["sha256"] for camera in cameras]
    if len(set(image_hashes)) != len(image_hashes):
        raise ValueError("Duplicate RGB content found; the demo splits cannot be treated as disjoint content.")
    _write_json(output / "random_initializer_recipe.json", recipe)
    splits = {split: [camera["view_id"] for camera in cameras if camera["split"] == split]
              for split in ("train", "val", "test")}
    source_hash = _sha256(Path(__file__).resolve())
    manifest = {"schema": "gsw.camera_dataset/1", "pose_policy": "locked",
                "source": {"kind": "original_procedural_readme_demo", "generator": GENERATOR_VERSION,
                           "scene_name": SCENE_NAME, "generator_sha256": source_hash,
                           "world_axes": "right-handed z-up", "camera_axes": "opencv", "handedness": "right",
                           "claim": "synthetic_appearance_demo_not_geometry_accuracy_benchmark",
                           "files": [{"path": "random_initializer_recipe.json",
                                      "sha256": _sha256(output / "random_initializer_recipe.json")}]},
                "scale": {"status": "arbitrary", "source_unit": "unknown", "meters_per_unit": None, "evidence": []},
                "normalization": {"mode": "none"}, "views": cameras, "splits": splits,
                "initialization": recipe,
                "demo_provenance": {"initialization_recipe_path": "random_initializer_recipe.json",
                                    "initialization_independent_of_render_geometry": True,
                                    "truth_depth_normals_mesh_exported": False, "third_party_assets": False}}
    _write_json(output / "camera_dataset.json", manifest)
    for split in ("train", "val", "test"):
        _write_json(output / ("transforms_%s.json" % split), _blender_manifest(cameras, split))
    conventions = {
        "schema": "gsw.readme_demo_conventions/1", "world": "right-handed, +Z up, arbitrary units",
        "native_camera_manifest": "camera_dataset.json", "native_camera_pose": "row-major C2W; +X right, +Y down, +Z forward",
        "blender_camera_pose": "row-major C2W; +X right, +Y up, -Z forward; world axes unchanged",
        "pixels": "pixel-border intrinsics; pixel centers (column+0.5,row+0.5)",
        "intrinsics": "square pixels, no skew, principal point (width/2,height/2), no distortion",
        "image_encoding": "8-bit RGB sRGB PNG; one fixed exposure; opaque world background",
        "rendering": "deterministic NumPy CPU ray tracer; analytic spheres/capped cylinders and sphere-traced torus",
        "units": "unknown/arbitrary, not metric; no real-world dimensional or accuracy claim",
        "split_policy": "fixed by index before rendering; every fifth view held out, alternating val/test",
        "split_counts": {split: len(ids) for split, ids in splits.items()},
        "initializer": "independent manual coarse uniform-volume recipe; no oracle points, images, mesh, depth or normals",
        "native_import": {"adapter": "generic_json", "posePolicy": "locked", "sourcePath": str(output / "camera_dataset.json")},
        "alternative_import": "blender_transforms adapter can read this directory; supply random_initializer_recipe.json as initialization options",
        "quality_claim": "demonstration input only, not a quality comparison or physical-geometry benchmark"}
    _write_json(output / "conventions.json", conventions)
    files = [{"path": path.relative_to(output).as_posix(), "byte_size": path.stat().st_size, "sha256": _sha256(path)}
             for path in sorted(output.rglob("*")) if path.is_file()]
    provenance = {"schema": GENERATOR_VERSION, "status": "complete", "scene_name": SCENE_NAME,
                  "generator_path": str(Path(__file__).resolve()), "generator_sha256": source_hash,
                  "parameters": {"views": args.views, "resolution": args.resolution, "supersample": args.supersample,
                                 "initial_points": args.initial_points, "initial_seed": args.initial_seed,
                                 "initialization_bounds": recipe["bounds"]},
                  "runtime": {"python": platform.python_version(), "numpy": np.__version__, "pillow": PIL.__version__},
                  "renderer": "CPU only; no Blender process, user scene, MCP, Gaussian renderer or GPU",
                  "inputs": [], "original_procedural_content": True, "geometry_exports": [],
                  "depth_normal_or_surface_initialization": False, "split_ids": splits,
                  "camera_set_sha256": _json_hash(cameras), "image_set_sha256": _json_hash(image_hashes),
                  "files": files, "elapsed_seconds": time.monotonic() - started}
    _write_json(output / "dataset_provenance.json", provenance)
    _write_json(output / "generation-complete.json", {"schema": GENERATOR_VERSION, "status": "complete",
                "provenance_sha256": _sha256(output / "dataset_provenance.json"),
                "manifest_sha256": _sha256(output / "camera_dataset.json"),
                "split_counts": conventions["split_counts"]})
    return {"output": str(output), "manifest": str(output / "camera_dataset.json"),
            "split_counts": conventions["split_counts"], "provenance": str(output / "dataset_provenance.json")}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", required=True, help="New absolute D-drive directory; never overwritten")
    parser.add_argument("--views", type=int, default=30, choices=range(24, 33), metavar="24..32")
    parser.add_argument("--resolution", type=int, default=320, choices=(256, 320, 384))
    parser.add_argument("--supersample", type=int, default=1, choices=(1, 2), help="CPU anti-aliasing factor")
    parser.add_argument("--initial-points", type=int, default=12000)
    parser.add_argument("--initial-seed", type=int, default=73129)
    parser.add_argument("--initialization-bounds", type=float, nargs=6,
                        default=DEFAULT_INITIALIZATION_BOUNDS[0] + DEFAULT_INITIALIZATION_BOUNDS[1],
                        metavar=("XMIN", "YMIN", "ZMIN", "XMAX", "YMAX", "ZMAX"))
    args = parser.parse_args(argv)
    if not 1000 <= args.initial_points <= 100000 or not 0 <= args.initial_seed < 2 ** 32:
        parser.error("initial-points must be 1000..100000 and initial-seed must be 0..2^32-1")
    try:
        result = generate(args)
    except (ImportError, OSError, ValueError) as error:
        print(json.dumps({"status": "failed", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2
    print(json.dumps({"status": "complete", **result}, ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
