"""Independent generated source-adapter fixtures for P0 acceptance.

No user dataset discovery, scene constants or live application access. All
dimensions, units, origins and initialization assumptions belong to the caller.
"""
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image

from native.backend import camera_dataset


def adapter_fixture(root, case):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=False)
    size = float(case["size"])
    origin = np.asarray(case.get("origin", [0., 0., 0.]), dtype=np.float64)
    adapter = case["adapter"]
    views = []
    for index, split in enumerate(("train", "train", "val", "test")):
        width, height = ((1728, 24) if index == 0 else ((128, 96) if index % 2 == 0 else (160, 112)))
        relative = "images/{}/camera-{}/same.png".format(split, index)
        target = root / relative
        target.parent.mkdir(parents=True)
        y, x = np.indices((height, width))
        pixels = np.stack(((x * 2 + index * 17) % 256, (y * 3 + index * 13) % 256,
                           ((x + y) * 2 + 40 + index) % 256), axis=-1).astype(np.uint8)
        Image.fromarray(pixels).save(target)
        pose = np.eye(4)
        pose[:3, 3] = origin + [(-.2 if index == 0 else .2) * size, 0., -1.5 * size]
        views.append({"image_path": relative, "width": width, "height": height,
            "K": [[width * (.9 + index * .03), 0., width * (.43 + index * .01)],
                  [0., height * (1.2 + index * .02), height * (.47 - index * .01)], [0., 0., 1.]],
            "c2w_opencv": pose.tolist(), "split": split})
    bounds = [(origin + [-.4 * size, -.3 * size, -.2 * size]).tolist(),
              (origin + [.4 * size, .3 * size, .2 * size]).tolist()]
    scale = {"status": "source_declared", "source_unit": case["unit"], "meters_per_unit": case["mpu"],
             "evidence": [{"kind": "generated_fixture", "description": "Synthetic coordinate definition, not physical accuracy"}]}
    initialization = {"kind": "random", "frame": "source", "bounds": bounds,
                      "count": 256, "seed": 19, "priors": []}
    options = {"pose_policy": "locked", "scale": scale, "initialization": initialization}
    if adapter == "blender_transforms":
        for split in ("train", "val", "test"):
            frames = []
            for view in views:
                if view["split"] != split:
                    continue
                pose = np.asarray(view["c2w_opencv"], dtype=np.float64)
                pose[:3, 1:3] *= -1.
                k = view["K"]
                frames.append({"file_path": view["image_path"][:-4], "transform_matrix": pose.tolist(),
                               "fl_x": k[0][0], "fl_y": k[1][1], "cx": k[0][2], "cy": k[1][2],
                               "w": view["width"], "h": view["height"]})
            (root / ("transforms_" + split + ".json")).write_text(json.dumps({"frames": frames, "scale": scale}), encoding="utf-8")
        source = root
    elif adapter in ("colmap_text", "colmap_binary"):
        source = root / "sparse" / "0"
        source.mkdir(parents=True)
        options["split_by_path"] = {view["image_path"]: view["split"] for view in views}
        if adapter == "colmap_text":
            cameras, images = [], []
            for identifier, view in enumerate(views, 1):
                k = view["K"]
                cameras.append("{} PINHOLE {} {} {} {} {} {}\n".format(identifier, view["width"], view["height"],
                    k[0][0], k[1][1], k[0][2], k[1][2]))
                translation = -np.asarray(view["c2w_opencv"], dtype=np.float64)[:3, 3]
                images.append("{} 1 0 0 0 {} {} {} {} {}\n\n".format(identifier, *translation, identifier,
                    view["image_path"][len("images/"):]))
            (source / "cameras.txt").write_text("".join(cameras), encoding="utf-8")
            (source / "images.txt").write_text("".join(images), encoding="utf-8")
        else:
            with (source / "cameras.bin").open("wb") as cameras, (source / "images.bin").open("wb") as images:
                cameras.write(struct.pack("<Q", len(views)))
                images.write(struct.pack("<Q", len(views)))
                for identifier, view in enumerate(views, 1):
                    k = view["K"]
                    cameras.write(struct.pack("<iiQQ4d", identifier, 1, view["width"], view["height"],
                                              k[0][0], k[1][1], k[0][2], k[1][2]))
                    translation = -np.asarray(view["c2w_opencv"], dtype=np.float64)[:3, 3]
                    images.write(struct.pack("<i4d3di", identifier, 1., 0., 0., 0., *translation, identifier))
                    images.write(view["image_path"][len("images/"):].encode("utf-8") + b"\0")
                    images.write(struct.pack("<Q", 0))
    elif adapter == "research_json":
        if case["unit"] != "m" or case["mpu"] != 1.:
            raise ValueError("The documented research *_m fields must explicitly use metres")
        cameras = []
        for view in views:
            cv_pose = np.asarray(view["c2w_opencv"], dtype=np.float64)
            blender_pose = cv_pose.copy()
            blender_pose[:3, 1:3] *= -1.
            cameras.append({"file_name": view["image_path"], "width": view["width"], "height": view["height"],
                            "K": view["K"], "split": view["split"],
                            "camera_to_world_blender_m": blender_pose.tolist(),
                            "world_to_camera_opencv_m": np.linalg.inv(cv_pose).tolist()})
        source = root / "cameras_metric.json"
        source.write_text(json.dumps({"conventions": {"units": "m", "world": "right-handed z-up",
                                                       "pixels": "pixel_border"}, "cameras": cameras}), encoding="utf-8")
    else:
        raise ValueError("unsupported acceptance adapter: " + adapter)
    dataset = camera_dataset.load_camera_dataset(source, adapter=adapter, options=options)
    camera_dataset.write_camera_dataset(dataset, root / "camera_dataset.json")
    # Reopen the exact normalized record, proving adapter axes/units are not
    # applied for a second time before it reaches the real GPU harness.
    reopened = camera_dataset.load_camera_dataset(root / "camera_dataset.json")
    if reopened != dataset:
        raise AssertionError("adapter fixture failed normalized-manifest roundtrip")
    return dataset
