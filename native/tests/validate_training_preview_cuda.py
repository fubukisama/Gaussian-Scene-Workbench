"""Run with the external training Python, including older Python 3.7 stacks."""
import json
import sys
import tempfile
from pathlib import Path

import torch
from plyfile import PlyData

backend_root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
sys.path.insert(0, str(backend_root))
from native.worker.training_preview import TrainingPreviewPublisher


def main():
    class Model:
        pass

    model = Model()
    count = 512001
    model.get_xyz = torch.zeros((count, 3), device="cuda")
    model.get_xyz[:, 0] = torch.linspace(-10, 10, count, device="cuda")
    model._features_dc = torch.zeros((count, 1, 3), device="cuda")
    model._opacity = torch.ones((count, 1), device="cuda")
    model._scaling = torch.full((count, 3), -3.0, device="cuda")
    model._rotation = torch.zeros((count, 4), device="cuda")
    model._rotation[:, 0] = 1
    events = []
    with tempfile.TemporaryDirectory(prefix="gsw-cuda-observation-") as output:
        publisher = TrainingPreviewPublisher(output, lambda *event: events.append(event))
        for iteration in range(6):
            publisher.last_time = float("-inf")
            assert publisher.publish_gaussians(model, iteration, initial=iteration == 0)
            publisher.future.result()
        publisher.close()
        assert len(events) == 6 and all(event[0] == "[gsw-training-preview]" for event in events), events
        assert len(list(publisher.directory.glob("*.ply"))) == 4
        assert not list(publisher.directory.glob("*.tmp"))
        payload = events[-1][1]
        data = PlyData.read(payload["point_cloud_path"], mmap=False)["vertex"].data
        assert len(data) == 256001 and payload["gaussian_count"] == count
        assert len(data.dtype.names) == 14 and data["rot_0"][0] == 1
        assert data["scale_0"][0] == -3 and abs(data["x"][-1] - 10) < 1e-5
        print(json.dumps({"valid": True, "sourceCount": count, "previewCount": len(data),
                          "retainedFiles": 4, "python": sys.version.split()[0]}))


if __name__ == "__main__":
    main()
