"""Opt-in official 2DGS rasterizer -> TSDF -> native mesh validation test.

Uses a test-owned analytic surfel plane, cameras and files. This verifies the
real command integration; it is not a trained real-photo quality benchmark.
GSW_MESH_TEST_ROOT can point at an installed package.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class CudaMeshTests(unittest.TestCase):
    def test_official_bounded_tsdf_through_native_worker(self):
        import numpy as np
        from PIL import Image
        from plyfile import PlyData, PlyElement
        import torch
        self.assertTrue(torch.cuda.is_available())
        repository = Path(os.environ.get("GSW_MESH_TEST_ROOT", Path(__file__).resolve().parents[2]))
        with tempfile.TemporaryDirectory(prefix="native-cuda-mesh-") as temporary:
            root = Path(temporary)
            dataset = root / "dataset"
            (dataset / "images").mkdir(parents=True)
            frames = []
            for index, (x, y) in enumerate(((-1, -1), (1, -1), (1, 1), (-1, 1))):
                position = np.array([x, y, 2.0])
                backward = position / np.linalg.norm(position)
                right = np.cross([0, 1, 0], backward)
                right /= np.linalg.norm(right)
                up = np.cross(backward, right)
                matrix = np.eye(4)
                matrix[:3, :3] = np.stack([right, up, backward], axis=1)
                matrix[:3, 3] = position
                frames.append({"file_path": "images/view-{}".format(index), "transform_matrix": matrix.tolist()})
                Image.new("RGBA", (64, 64), (180, 120, 60, 255)).save(dataset / ("images/view-{}.png".format(index)))
            for split in ("train", "test"):
                (dataset / ("transforms_" + split + ".json")).write_text(json.dumps(
                    {"camera_angle_x": 0.7, "frames": frames if split == "train" else []}))
            coordinates = [(x, y, 0.) for x in np.linspace(-.8, .8, 17) for y in np.linspace(-.8, .8, 17)]
            names = ["x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2"]
            names += ["f_rest_" + str(i) for i in range(45)]
            names += ["opacity", "scale_0", "scale_1", "rot_0", "rot_1", "rot_2", "rot_3"]
            vertices = np.zeros(len(coordinates), dtype=[(name, "f4") for name in names])
            for index, name in enumerate(("x", "y", "z")):
                vertices[name] = np.asarray(coordinates)[:, index]
            vertices["opacity"] = 8
            vertices["scale_0"] = vertices["scale_1"] = math.log(.08)
            vertices["rot_0"] = 1
            source = root / "source"
            cloud = source / "point_cloud/iteration_7/point_cloud.ply"
            cloud.parent.mkdir(parents=True)
            PlyData([PlyElement.describe(vertices, "vertex")]).write(str(cloud))
            initial = np.zeros(len(coordinates), dtype=[(name, "f4") for name in ("x", "y", "z", "nx", "ny", "nz")] +
                               [(name, "u1") for name in ("red", "green", "blue")])
            for name in ("x", "y", "z"):
                initial[name] = vertices[name]
            PlyData([PlyElement.describe(initial, "vertex")]).write(str(dataset / "points3d.ply"))
            (source / "training_backend.json").write_text('{"backend":"2dgs"}')
            (source / "cfg_args").write_text("Namespace(sh_degree=3, source_path={!r}, model_path={!r}, images='images', resolution=1, white_background=False, data_device='cpu', eval=False)".format(str(dataset), str(source)))
            digest = hashlib.sha256(cloud.read_bytes()).hexdigest()
            config = {"task": "mesh", "repositoryRoot": str(repository), "modelDirectory": str(source),
                      "outputRoot": str(root / "output"), "runName": "mesh-cuda-owned", "iteration": 7,
                      "mode": "bounded", "meshOptions": {"mesh_res": 64, "num_cluster": 1}}
            path = root / "job.json"
            path.write_text(json.dumps(config))
            worker_python = os.environ.get("GSW_MESH_TEST_WORKER_PYTHON", sys.executable)
            worker_env = os.environ.copy()
            worker_directory = Path(worker_python).parent
            worker_env["PATH"] = os.pathsep.join([str(worker_directory), str(worker_directory / "Library/bin"),
                                                  worker_env.get("PATH", "")])
            result = subprocess.run([worker_python, "-B", str(repository / "native/worker/gsw_worker.py"),
                                     "--config", str(path)], input="", text=True,
                                    env=worker_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout[-12000:])
            record = json.loads((root / "output/mesh-cuda-owned/result.json").read_text())
            self.assertEqual(record["state"], "done")
            self.assertGreater(record["geometry"]["faces"], 0)
            self.assertEqual(hashlib.sha256(cloud.read_bytes()).hexdigest(), digest)
            print("Real CUDA TSDF PASS:", record["geometry"], flush=True)


if __name__ == "__main__":
    unittest.main()
