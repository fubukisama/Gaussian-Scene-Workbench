import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import server  # noqa: E402


class SceneWorkspaceTests(unittest.TestCase):
    def test_scene_listing_exposes_real_model_path(self):
        original_output = server.OUTPUT_DIR
        try:
            with tempfile.TemporaryDirectory() as tmp:
                server.OUTPUT_DIR = Path(tmp) / "output"
                model = server.OUTPUT_DIR / "existing_scene"
                point_cloud = model / "point_cloud" / "iteration_30000" / "point_cloud.ply"
                point_cloud.parent.mkdir(parents=True)
                point_cloud.write_bytes(b"ply\n")
                (model / "training_backend.json").write_text(
                    json.dumps({"backend": "2dgs"}),
                    encoding="utf-8",
                )

                scenes = server.list_scenes()

                self.assertEqual(len(scenes), 1)
                self.assertEqual(scenes[0]["name"], "existing_scene")
                self.assertEqual(scenes[0]["latest_iteration"], 30000)
                self.assertEqual(scenes[0]["backend"], "2dgs")
                self.assertEqual(Path(scenes[0]["path"]), model.resolve())
        finally:
            server.OUTPUT_DIR = original_output


if __name__ == "__main__":
    unittest.main()
