import http.client
import errno
import base64
import json
import os
import subprocess
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import server  # noqa: E402


class OwnedAttachmentHandler(server.Handler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/owned-attachment":
            name = parse_qs(parsed.query).get("name", ["attachment.ply"])[0]
            return self.serve_file(server.OUTPUT_DIR / "attachment.ply", "application/octet-stream", name)
        return super().do_GET()


class BackendPathSecurityTests(unittest.TestCase):
    def setUp(self):
        fixture_parent = Path(os.environ.get("GSW_SMOKE_OUTPUT_DIR") or tempfile.gettempdir())
        fixture_parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="gsw-path-security-", dir=str(fixture_parent))
        self.root = Path(self.temporary.name)
        self.original_paths = {}
        for name, relative in (
            ("STATIC_DIR", "static"),
            ("OUTPUT_DIR", "output"),
            ("DATASETS_DIR", "datasets"),
            ("PREVIEW_DIR", "previews"),
            ("PSNR_REPORTS_DIR", "reports/psnr"),
        ):
            self.original_paths[name] = getattr(server, name)
            setattr(server, name, self.root / relative)
            getattr(server, name).mkdir(parents=True, exist_ok=True)
        self.httpd = server.ThreadingHTTPServer(("127.0.0.1", 0), OwnedAttachmentHandler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)
        for name, original in self.original_paths.items():
            setattr(server, name, original)
        self.temporary.cleanup()

    def request(self, path, method="GET", data=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.httpd.server_address[1], timeout=5)
        try:
            body = None if data is None else json.dumps(data).encode("utf-8")
            headers = {} if body is None else {"Content-Type": "application/json"}
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def write_fixture(self, relative, contents):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def make_link(self, relative, target, directory=False):
        link = self.root / relative
        link.parent.mkdir(parents=True, exist_ok=True)
        try:
            link.symlink_to(target, target_is_directory=directory)
        except OSError as exc:
            if exc.errno in (errno.EPERM, errno.EACCES, errno.ENOSYS) or getattr(exc, "winerror", None) == 1314:
                if directory and os.name == "nt":
                    # Owned fixture paths only; a junction needs no symlink privilege.
                    for path in (link, target):
                        Path(path).absolute().relative_to(self.root.absolute())
                        if any(character in str(path) for character in '"&|<>^%\r\n'):
                            raise ValueError("Invalid owned fixture path")
                    result = subprocess.run(
                        [os.environ.get("ComSpec", "cmd.exe"), "/d", "/c", "mklink", "/J", str(link), str(target)],
                        capture_output=True, text=True,
                    )
                    if result.returncode == 0:
                        return link
                    failure = (result.stdout + result.stderr).lower()
                    if not any(reason in failure for reason in ("not supported", "privilege", "access is denied")):
                        self.fail("Unable to prepare the owned junction fixture: " + failure)
                self.skipTest("Platform does not permit the owned symbolic-link or junction fixture")
            raise
        return link

    def test_static_download_preserves_bytes_and_rejects_sibling_traversal(self):
        self.write_fixture("static/index.html", b"legal static page")
        self.write_fixture("static/assets/app.js", b"legal nested script")
        outside = b"private sibling fixture"
        self.write_fixture("static_sibling/private.txt", outside)

        self.assertEqual(self.request("/")[2], b"legal static page")
        self.assertEqual(self.request("/assets/app.js")[2], b"legal nested script")
        status, _, body = self.request("/../static_sibling/private.txt")
        self.assertEqual(status, 404)
        self.assertNotEqual(body, outside)

    def test_mesh_chunk_downloads_reject_parent_keys_for_both_backends(self):
        legal = b"legal mesh chunk bytes"
        outside = b"private file outside chunk cache"
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-scene_42.a"
            directory = "output/{}/train/ours_1".format(scene)
            self.write_fixture(directory + "/fuse_post.ply", b"source mesh fixture")
            textured = directory + "/fuse_post_texture/colmap_texturer/output"
            self.write_fixture(textured + "/mesh.ply", b"source textured mesh fixture")
            for endpoint, parent, cache, key in (
                ("/api/mesh/chunk_file", directory, ".mesh_chunks", "fuse_post_1_20_250000"),
                ("/api/mesh/texture/chunk_file", textured, ".textured_mesh_chunks", "mesh_textured_1_20_250000"),
            ):
                self.write_fixture("{}/{}/{}/chunk_00000.bin".format(parent, cache, key), legal)
                self.write_fixture(parent + "/chunk_00000.bin", outside)
                for requested_key in (key, "..", "../..", "..\\..", "C:escape"):
                    with self.subTest(backend=backend, endpoint=endpoint, key=requested_key):
                        query = urlencode({"scene": scene, "iteration": 1, "key": requested_key, "index": 0})
                        status, _, body = self.request(endpoint + "?" + query)
                        if requested_key == key:
                            self.assertEqual(status, 200)
                            self.assertEqual(body, legal)
                        else:
                            self.assertIn(status, (400, 404, 500))
                            self.assertNotEqual(body, outside)

    def test_model_file_link_cannot_download_outside_managed_outputs(self):
        outside = b"private file outside managed outputs"
        private = self.write_fixture("outside/private.ply", outside)
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-scene_42.a"
            relative = "output/{}/point_cloud/iteration_1/point_cloud.ply".format(scene)
            self.make_link(relative, private)
            query = urlencode({"scene": scene, "iteration": 1})
            status, _, body = self.request("/api/splat/ply?" + query)
            self.assertIn(status, (400, 404, 500))
            self.assertNotEqual(body, outside)

    def test_model_directory_link_cannot_download_outside_managed_outputs(self):
        outside = b"private linked model outside managed outputs"
        private = self.write_fixture("outside/model/point_cloud/iteration_1/point_cloud.ply", outside)
        self.make_link("output/linked-scene_42.a", private.parents[2], directory=True)
        query = urlencode({"scene": "linked-scene_42.a", "iteration": 1})
        status, _, body = self.request("/api/splat/ply?" + query)
        self.assertIn(status, (400, 404, 500))
        self.assertNotEqual(body, outside)

    def test_report_link_cannot_download_from_same_prefix_sibling_scene(self):
        outside = b"private report in a sibling scene"
        private = self.write_fixture("reports/psnr/scene_42.a_sibling/run-real/psnr_results.json", outside)
        self.make_link("reports/psnr/scene_42.a/run-42.a", private.parent, directory=True)
        query = urlencode({"scene": "scene_42.a", "run": "run-42.a", "name": "psnr_results.json"})
        status, _, body = self.request("/api/assets/psnr-file?" + query)
        self.assertIn(status, (400, 404, 500))
        self.assertNotEqual(body, outside)

    def test_external_source_import_is_allowed_but_default_dataset_links_cannot_escape(self):
        image = base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/X2sAAAAASUVORK5CYII="
        )
        source = self.write_fixture("external-source/input.png", image)
        files = [{"path": str(source), "name": "input.png"}]
        status, _, body = self.request("/api/import-paths", "POST", {"scene": "legal-import_42.a", "files": files})
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body.decode("utf-8"))["image_count"], 1)

        for linked_part in ("", "/images", "/source/originals"):
            scene = "linked-import_42.a" + str(len(linked_part))
            outside = self.write_fixture("outside/import-{}-target/marker.txt".format(len(linked_part)), b"owned external target")
            self.make_link("datasets/" + scene + linked_part, outside.parent, directory=True)
            status, _, body = self.request("/api/import-paths", "POST", {"scene": scene, "files": files})
            with self.subTest(linked_part=linked_part):
                self.assertEqual(status, 400)
                self.assertIn("error", json.loads(body.decode("utf-8")))

    def test_nested_model_directory_link_cannot_download_outside_managed_outputs(self):
        outside = b"private linked point cloud outside managed outputs"
        private = self.write_fixture("outside/iteration_1/point_cloud.ply", outside)
        self.make_link("output/scene_42.a/point_cloud/iteration_1", private.parent, directory=True)
        query = urlencode({"scene": "scene_42.a", "iteration": 1})
        status, _, body = self.request("/api/splat/ply?" + query)
        self.assertEqual(status, 404)
        self.assertNotEqual(body, outside)

    def test_static_directory_link_cannot_download_outside_static_root(self):
        outside = b"private file behind a static directory link"
        private = self.write_fixture("outside/static-assets/private.txt", outside)
        self.make_link("static/linked-assets", private.parent, directory=True)
        status, _, body = self.request("/linked-assets/private.txt")
        self.assertEqual(status, 404)
        self.assertNotEqual(body, outside)

    def test_point_payload_cannot_read_through_nested_model_links(self):
        points = (b"ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\n"
                  b"property float y\nproperty float z\nend_header\n777 888 999\n1 2 3\n")
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-point-read"
            private = self.write_fixture("outside/" + scene + "/iteration_1/point_cloud.ply", points)
            self.write_fixture("output/" + scene + "/training_backend.json",
                               json.dumps({"backend": backend}).encode("utf-8"))
            self.make_link("output/" + scene + "/point_cloud", private.parent.parent, directory=True)
            for endpoint in ("/api/points", "/api/splat/ply"):
                with self.subTest(backend=backend, endpoint=endpoint):
                    status, _, body = self.request(endpoint + "?" + urlencode({"scene": scene, "iteration": 1}))
                    self.assertEqual(status, 404)
                    self.assertNotEqual(body, points)

    def test_crop_cannot_write_through_nested_output_links(self):
        points = (b"ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\n"
                  b"property float y\nproperty float z\nend_header\n0 0 0\n1 2 3\n")
        for backend in ("3dgs", "2dgs"):
            source = backend + "-crop-source"
            output = backend + "-crop-output"
            self.write_fixture("output/" + source + "/point_cloud/iteration_1/point_cloud.ply", points)
            self.write_fixture("output/" + source + "/training_backend.json",
                               json.dumps({"backend": backend}).encode("utf-8"))
            marker = self.write_fixture("outside/" + output + "/iteration_1/point_cloud.ply", b"original marker")
            self.make_link("output/" + output + "/point_cloud", marker.parent.parent, directory=True)
            status, _, _ = self.request("/api/save", "POST", {
                "scene": source, "iteration": 1, "output_scene": output,
                "overwrite": True, "delete_indices": [1],
            })
            with self.subTest(backend=backend):
                self.assertEqual(status, 400)
                self.assertEqual(marker.read_bytes(), b"original marker")
            legal_output = backend + "-legal-crop"
            status, _, body = self.request("/api/save", "POST", {
                "scene": source, "iteration": 1, "output_scene": legal_output, "delete_indices": [1],
            })
            self.assertEqual(status, 200, body)
            self.assertEqual(json.loads(body)["kept"], 1)
            self.assertEqual(self.request("/api/points?" + urlencode({"scene": legal_output, "iteration": 1}))[0], 200)

    def binary_mesh_fixture(self, textured=False):
        header = (b"ply\nformat binary_little_endian 1.0\nelement vertex 3\nproperty float x\n"
                  b"property float y\nproperty float z\nelement face 1\nproperty list uchar uint vertex_indices\n")
        if textured:
            header += b"property list uchar float texcoord\n"
        body = struct.pack("<9fB3I", 0, 0, 0, 1, 0, 0, 0, 1, 0, 3, 0, 1, 2)
        if textured:
            body += struct.pack("<B6f", 6, 0, 0, 1, 0, 0, 1)
        return header + b"end_header\n" + body

    def test_mesh_chunk_generation_cannot_write_through_cache_links(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-cache-output"
            directory = "output/" + scene + "/train/ours_1"
            self.write_fixture(directory + "/fuse_post.ply", self.binary_mesh_fixture())
            marker = self.write_fixture("outside/" + scene + "/marker.txt", b"original marker")
            self.make_link(directory + "/.mesh_chunks", marker.parent, directory=True)
            status, _, _ = self.request("/api/mesh/chunks?" + urlencode({"scene": scene, "iteration": 1, "max_faces": 1}))
            with self.subTest(backend=backend):
                self.assertIn(status, (404, 500))
                self.assertEqual([path.name for path in marker.parent.iterdir()], ["marker.txt"])
                self.assertEqual(marker.read_bytes(), b"original marker")

    def test_textured_chunk_generation_cannot_write_through_cache_links(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-texture-cache"
            directory = "output/" + scene + "/train/ours_1/fuse_post_texture/colmap_texturer/output"
            self.write_fixture(directory + "/mesh.ply", self.binary_mesh_fixture(textured=True))
            self.write_fixture(directory + "/texture.png", b"owned texture fixture")
            marker = self.write_fixture("outside/" + scene + "/marker.txt", b"original marker")
            self.make_link(directory + "/.textured_mesh_chunks", marker.parent, directory=True)
            status, _, _ = self.request("/api/mesh/texture/chunks?" + urlencode({"scene": scene, "iteration": 1, "max_faces": 1}))
            with self.subTest(backend=backend):
                self.assertIn(status, (404, 500))
                self.assertEqual([path.name for path in marker.parent.iterdir()], ["marker.txt"])
                self.assertEqual(marker.read_bytes(), b"original marker")

    def test_mesh_preview_cannot_write_through_cache_links(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-preview-cache"
            directory = "output/" + scene + "/train/ours_1"
            self.write_fixture(directory + "/fuse_post.ply", self.binary_mesh_fixture())
            marker = self.write_fixture("outside/" + scene + "/marker.txt", b"original marker")
            self.make_link(directory + "/.mesh_preview", marker.parent, directory=True)
            status, _, _ = self.request("/api/mesh/preview_file?" + urlencode({"scene": scene, "iteration": 1, "max_faces": 1}))
            with self.subTest(backend=backend):
                self.assertIn(status, (404, 500))
                self.assertEqual([path.name for path in marker.parent.iterdir()], ["marker.txt"])

    def test_mesh_generation_views_cannot_follow_nested_train_links(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-train-link"
            private = self.write_fixture("outside/" + scene + "/ours_1/fuse_post.ply", self.binary_mesh_fixture())
            self.make_link("output/" + scene + "/train", private.parent.parent, directory=True)
            for endpoint in ("/api/mesh/chunks", "/api/mesh/preview_file", "/api/mesh/file"):
                with self.subTest(backend=backend, endpoint=endpoint):
                    status, _, _ = self.request(endpoint + "?" + urlencode({"scene": scene, "iteration": 1, "max_faces": 1}))
                    self.assertIn(status, (404, 500))
                    self.assertEqual([path.name for path in private.parent.iterdir()], ["fuse_post.ply"])

    def test_textured_mesh_view_cannot_follow_nested_texturer_output_links(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-texturer-link"
            private = self.write_fixture("outside/" + scene + "/mesh.ply", self.binary_mesh_fixture(textured=True))
            self.write_fixture("outside/" + scene + "/texture.png", b"owned texture fixture")
            self.make_link("output/" + scene + "/train/ours_1/fuse_post_texture/colmap_texturer/output",
                           private.parent, directory=True)
            status, _, _ = self.request("/api/mesh/texture/chunks?" + urlencode({"scene": scene, "iteration": 1, "max_faces": 1}))
            with self.subTest(backend=backend):
                self.assertIn(status, (404, 500))
                self.assertEqual(sorted(path.name for path in private.parent.iterdir()), ["mesh.ply", "texture.png"])

    def test_psnr_asset_listing_cannot_read_through_report_scene_links(self):
        private = self.write_fixture("outside/psnr-list/run-1/psnr_results.json",
                                     b'{"backend":"private-marker","average_psnr":987.65}')
        self.make_link("reports/psnr/report-list", private.parent.parent, directory=True)
        status, _, body = self.request("/api/assets?" + urlencode({"scene": "report-list", "iteration": 1}))
        self.assertIn(status, (404, 500))
        self.assertNotIn(b"private-marker", body)

    def test_psnr_start_rejects_escaping_report_output_before_starting_a_worker(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-psnr-link"
            marker = self.write_fixture("outside/" + scene + "/marker.txt", b"original marker")
            self.make_link("reports/psnr/" + scene, marker.parent, directory=True)
            # No model exists in this owned fixture. The old implementation's
            # worker therefore fails before any renderer or training process.
            status, _, _ = self.request("/api/psnr/start", "POST", {
                "scene": scene, "iteration": 1, "backend": backend, "count": 1,
            })
            with self.subTest(backend=backend):
                self.assertEqual(status, 400)
                self.assertEqual([path.name for path in marker.parent.iterdir()], ["marker.txt"])

    def test_crop_cannot_copy_or_overwrite_escaping_sidecar_file_links(self):
        points = (b"ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\n"
                  b"property float y\nproperty float z\nend_header\n0 0 0\n1 2 3\n")
        for linked_side in ("source", "destination"):
            source = "sidecar-source-" + linked_side
            output = "sidecar-output-" + linked_side
            self.write_fixture("output/" + source + "/point_cloud/iteration_1/point_cloud.ply", points)
            marker = self.write_fixture("outside/" + linked_side + "/input.ply", b"private sidecar marker")
            if linked_side == "source":
                self.make_link("output/" + source + "/input.ply", marker)
            else:
                self.write_fixture("output/" + source + "/input.ply", b"legitimate input cloud")
                self.make_link("output/" + output + "/input.ply", marker)
            status, _, _ = self.request("/api/save", "POST", {
                "scene": source, "iteration": 1, "output_scene": output,
                "overwrite": True, "delete_indices": [1],
            })
            with self.subTest(linked_side=linked_side):
                self.assertEqual(status, 400)
                self.assertEqual(marker.read_bytes(), b"private sidecar marker")

    def test_legal_mesh_generation_views_keep_geometry_and_cache_downloads(self):
        for backend in ("3dgs", "2dgs"):
            for mode, filename in (("bounded", "fuse_post.ply"), ("unbounded", "fuse_unbounded_post.ply"),
                                   ("sugar", "sugar_mesh_post.ply"), ("gs2mesh", "gs2mesh_post.ply")):
                scene = backend + "-legal-" + mode
                directory = "output/" + scene + "/train/ours_1"
                original = self.binary_mesh_fixture()
                mesh = self.write_fixture(directory + "/" + filename, original)
                query = {"scene": scene, "iteration": 1, "mode": mode, "max_faces": 1}
                status, _, body = self.request("/api/mesh/chunks?" + urlencode(query))
                self.assertEqual(status, 200, body)
                manifest = json.loads(body)
                self.assertEqual((manifest["vertex_count"], manifest["face_count"], manifest["chunk_count"]), (3, 1, 1))
                status, _, chunk = self.request(manifest["chunks"][0]["url"])
                self.assertEqual(status, 200, chunk)
                header_size = struct.unpack("<I", chunk[:4])[0]
                self.assertEqual(json.loads(chunk[4:4 + header_size])["face_count"], 1)
                self.assertEqual(self.request("/api/mesh/preview_file?" + urlencode(query))[0], 200)
                self.assertEqual(mesh.read_bytes(), original)
            scene = backend + "-legal-texture"
            directory = "output/" + scene + "/train/ours_1/fuse_post_texture/colmap_texturer/output"
            original = self.binary_mesh_fixture(textured=True)
            mesh = self.write_fixture(directory + "/mesh.ply", original)
            self.write_fixture(directory + "/texture.png", b"controlled texture fixture")
            status, _, body = self.request("/api/mesh/texture/chunks?" + urlencode({"scene": scene, "iteration": 1, "max_faces": 1}))
            self.assertEqual(status, 200, body)
            manifest = json.loads(body)
            self.assertEqual((manifest["vertex_count"], manifest["face_count"], manifest["chunk_count"]), (3, 1, 1))
            status, _, chunk = self.request(manifest["chunks"][0]["url"])
            self.assertEqual(status, 200, chunk)
            header_size = struct.unpack("<I", chunk[:4])[0]
            self.assertTrue(json.loads(chunk[4:4 + header_size])["has_uvs"])
            self.assertEqual(mesh.read_bytes(), original)

    def test_attachment_names_cannot_inject_response_headers(self):
        expected = b"original attachment bytes"
        self.write_fixture("output/attachment.ply", expected)
        for name in ("model.ply", 'model\r\nX-Injected: true\r\n".ply', 'model"; name="other.ply'):
            with self.subTest(name=name):
                status, headers, body = self.request("/owned-attachment?" + urlencode({"name": name}))
                self.assertEqual(status, 200)
                self.assertEqual(body, expected)
                self.assertNotIn("x-injected", {key.lower() for key in headers})
                disposition = headers["Content-Disposition"]
                self.assertNotIn("\r", disposition)
                self.assertNotIn("\n", disposition)
                self.assertEqual(disposition.count('"'), 2)

    def test_viewing_experiment_does_not_execute_model_configuration(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-untrusted-config"
            marker = self.root / (backend + "-config-executed.txt")
            source = self.root / "datasets" / "owned-config-source"
            expression = "Namespace(source_path=(__import__('pathlib').Path({!r}).write_text('executed'), {!r})[1])".format(
                str(marker), str(source)
            )
            self.write_fixture("output/{}/cfg_args".format(scene), expression.encode("utf-8"))
            self.write_fixture("output/{}/point_cloud/iteration_1/point_cloud.ply".format(scene),
                               b"ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n")
            self.write_fixture("output/{}/training_backend.json".format(scene), json.dumps({"backend": backend}).encode("utf-8"))
            status, _, body = self.request("/api/experiments?" + urlencode({"scene": scene}))
            with self.subTest(backend=backend):
                self.assertEqual(status, 200, body)
                self.assertFalse(marker.exists(), "Viewing model parameters must never execute configuration expressions")
                self.assertEqual(json.loads(body.decode("utf-8"))["cfg_args"], {})

    def test_viewing_experiment_preserves_literal_configuration_for_both_backends(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-literal-config"
            source = str(self.root / "datasets" / "源 モデル with spaces")
            values = {"source_path": source, "iterations": 30000, "white_background": True,
                      "resolution": -1, "depth_ratio": 0.25, "optional": None,
                      "save_iterations": [7, 30000], "crop_box": (-1.0, 2.0),
                      "metadata": {"label": "原始 パラメータ", "active": False}}
            expression = "Namespace(" + ", ".join(key + "=" + repr(value) for key, value in values.items()) + ")"
            self.write_fixture("output/{}/cfg_args".format(scene), expression.encode("utf-8"))
            self.write_fixture("output/{}/point_cloud/iteration_1/point_cloud.ply".format(scene),
                               b"ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n")
            self.write_fixture("output/{}/training_backend.json".format(scene), json.dumps({"backend": backend}).encode("utf-8"))
            status, _, body = self.request("/api/experiments?" + urlencode({"scene": scene}))
            with self.subTest(backend=backend):
                self.assertEqual(status, 200, body)
                expected = dict(values, crop_box=[-1.0, 2.0])
                self.assertEqual(json.loads(body.decode("utf-8"))["cfg_args"], expected)

    def test_legal_model_mesh_and_report_downloads_preserve_original_bytes(self):
        for backend in ("3dgs", "2dgs"):
            scene = backend + "-scene_42.a"
            model = "output/" + scene
            gaussian = b"ply\noriginal Gaussian attributes\x00\xff"
            mesh = b"ply\noriginal mesh attributes\x01\xfe"
            config = json.dumps({"backend": backend}).encode("utf-8")
            report = b'{"psnr": 32.5}'
            self.write_fixture(model + "/point_cloud/iteration_1/point_cloud.ply", gaussian)
            self.write_fixture(model + "/train/ours_1/fuse_post.ply", mesh)
            self.write_fixture(model + "/training_backend.json", config)
            self.write_fixture("reports/psnr/{}/run-42.a/psnr_results.json".format(scene), report)
            for path, expected in (
                ("/api/splat/ply?" + urlencode({"scene": scene, "iteration": 1}), gaussian),
                ("/api/mesh/file?" + urlencode({"scene": scene, "iteration": 1}), mesh),
                ("/api/assets/file?" + urlencode({"scene": scene, "name": "training_backend.json"}), config),
                ("/api/assets/psnr-file?" + urlencode({"scene": scene, "run": "run-42.a", "name": "psnr_results.json"}), report),
            ):
                with self.subTest(backend=backend, path=path):
                    status, _, body = self.request(path)
                    self.assertEqual(status, 200)
                    self.assertEqual(body, expected)


if __name__ == "__main__":
    unittest.main()
