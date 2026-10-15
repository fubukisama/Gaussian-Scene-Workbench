"""Real paired-image measurements through the public CPU producer seam."""
import math
from pathlib import Path
import tempfile
import unittest


class ImageQualityMeasurementTests(unittest.TestCase):
    def setUp(self):
        from PIL import Image
        self.Image = Image
        self.directory = tempfile.TemporaryDirectory(prefix="gsw-measure-quality-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def image(self, name, color, size=(16, 16), mode="RGB"):
        path = self.root / name
        self.Image.new(mode, size, color).save(path)
        return name

    def test_same_file_pair_remains_valid_without_claiming_independent_quality(self):
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("same.png", 127)
        report = measure_image_quality({"version": 1, "views": [
            {"id": "same", "reference": reference, "render": reference}]}, self.root)
        self.assertEqual(report["metrics"]["psnr"]["special"], "positive_infinity")
        self.assertAlmostEqual(report["metrics"]["ssim"], 1.0, places=12)
        self.assertEqual(report["split"], "unverified")
        self.assertFalse(report["evidence"]["held_out_verified"])

    def test_black_white_pair_has_measured_zero_db_psnr_and_known_constant_ssim(self):
        from native.worker.image_quality_measurement import measure_image_quality
        manifest = {"version": 1, "views": [{"id": "front", "reference": self.image("gt.png", (0, 0, 0)),
                     "render": self.image("render.png", (255, 255, 255))}]}
        result = measure_image_quality(manifest, self.root)
        self.assertEqual(result["split"], "unverified")
        self.assertEqual(result["metrics"]["psnr"], 0.0)
        self.assertAlmostEqual(result["metrics"]["ssim"], 1.0 / 10001.0, places=12)
        view = result["evidence"]["views"][0]
        self.assertEqual(view["metrics"]["mse"], 1.0)
        self.assertEqual((view["width"], view["height"], view["valid_pixels"], view["ssim_windows"]),
                         (16, 16, 256, 36))
        self.assertFalse(result["evidence"]["held_out_verified"])

    def test_invalid_pairing_or_unproven_heldout_is_rejected_without_guessing(self):
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("gt.png", (0, 0, 0))
        rendered = self.image("render.png", (1, 1, 1))
        valid = {"id": "one", "reference": reference, "render": rendered}
        cases = [{"version": True, "views": [valid]}, {"version": 1, "views": []},
                 {"version": 1, "views": [valid, dict(valid)]},
                 {"version": 1, "views": [valid], "split": "held_out"},
                 {"version": 1, "views": [valid], "resize": True},
                 {"version": 1, "views": [{"id": "", "reference": reference, "render": rendered}]}]
        for manifest in cases:
            with self.subTest(manifest=manifest), self.assertRaises(ValueError):
                measure_image_quality(manifest, self.root)

    def test_size_alpha_orientation_and_empty_mask_are_hard_errors(self):
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("gt.png", (0, 0, 0))
        small = self.image("small.png", (0, 0, 0), size=(15, 16))
        rgba = self.image("rgba.png", (0, 0, 0, 255), mode="RGBA")
        orientation = self.root / "rotated.jpg"
        exif = self.Image.Exif()
        exif[274] = 6
        self.Image.new("RGB", (16, 16)).save(orientation, exif=exif)
        for rendered in (small, rgba, orientation.name):
            with self.subTest(rendered=rendered), self.assertRaises(ValueError):
                measure_image_quality({"version": 1, "views": [{"id": "one", "reference": reference,
                                      "render": rendered}]}, self.root)
        empty = self.image("empty.png", 0, mode="L")
        with self.assertRaises(ValueError):
            measure_image_quality({"version": 1, "views": [{"id": "one", "reference": reference,
                                  "render": reference, "mask": empty}]}, self.root)

    def test_gaussian_impulse_has_independently_derived_psnr_and_population_ssim(self):
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("gt.png", (0, 0, 0), size=(11, 11))
        image = self.Image.new("RGB", (11, 11), (0, 0, 0))
        image.putpixel((5, 5), (255, 255, 255))
        image.save(self.root / "impulse.png")
        result = measure_image_quality({"version": 1, "views": [{"id": "impulse", "reference": reference,
                                                               "render": "impulse.png"}]}, self.root)
        # One white RGB pixel among 121 pixels: MSE=1/121. For SSIM the
        # Gaussian center mass is .07076223776394698; mu=p, var=p-p*p.
        self.assertAlmostEqual(result["metrics"]["psnr"], 20.827853703164504, places=11)
        self.assertAlmostEqual(result["metrics"]["ssim"], .0002643743162066147, places=14)

    def test_mask_excludes_pixels_and_incomplete_ssim_windows_without_zero_filling(self):
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("gt.png", (0, 0, 0))
        image = self.Image.new("RGB", (16, 16), (255, 255, 255))
        image.paste((0, 0, 0), (0, 0, 11, 11))
        image.save(self.root / "render.png")
        mask = self.Image.new("L", (16, 16), 0)
        mask.paste(128, (0, 0, 11, 11))
        mask.save(self.root / "mask.png")
        result = measure_image_quality({"version": 1, "views": [{"id": "masked", "reference": reference,
            "render": "render.png", "mask": "mask.png"}]}, self.root)
        self.assertEqual(result["metrics"]["psnr"]["reason"], "zero_mse")
        self.assertEqual(result["metrics"]["ssim"], 1.0)
        self.assertEqual(result["evidence"]["aggregate"]["valid_pixels"], 121)
        self.assertEqual(result["evidence"]["aggregate"]["ssim_windows"], 1)

    def test_aggregation_pools_valid_pixels_and_weights_complete_ssim_windows(self):
        from native.worker.image_quality_measurement import measure_image_quality
        manifest = {"version": 1, "views": [
            {"id": "small", "reference": self.image("small-gt.png", (0, 0, 0), (11, 11)),
             "render": self.image("small-render.png", (255, 255, 255), (11, 11))},
            {"id": "large", "reference": self.image("large-gt.png", (0, 0, 0), (22, 22)),
             "render": self.image("large-render.png", (0, 0, 0), (22, 22))}]}
        result = measure_image_quality(manifest, self.root)
        self.assertAlmostEqual(result["metrics"]["psnr"], 6.989700043360188, places=12)
        self.assertAlmostEqual(result["metrics"]["ssim"], (144 + 1 / 10001) / 145, places=12)
        self.assertEqual(result["evidence"]["aggregate"]["ssim_windows"], 145)

    def test_candidate_pixels_do_not_change_comparison_identity_but_gt_and_mask_do(self):
        from native.worker.image_quality_measurement import measure_image_quality
        from native.worker.training_quality import compare_quality
        reference = self.image("gt.png", (0, 0, 0))
        manifest = {"version": 1, "views": [{"id": "one", "reference": reference,
                                            "render": self.image("first.png", (1, 1, 1))}]}
        first = measure_image_quality(manifest, self.root)
        manifest["views"][0]["render"] = self.image("second.png", (2, 2, 2))
        second = measure_image_quality(manifest, self.root)
        self.assertEqual(first["identity"], second["identity"])
        result = compare_quality(first, second, {"version": 1, "required_domains": ["image"],
                                                "metrics": {"psnr": {"max_regression": 10.}}})
        self.assertEqual(result["status"], "unverified")
        self.assertEqual(result["metrics"]["psnr"]["threshold_status"], "pass")
        manifest["views"][0]["mask"] = self.image("mask.png", 255, mode="L")
        masked = measure_image_quality(manifest, self.root)
        self.assertNotEqual(masked["identity"]["mask_id"], second["identity"]["mask_id"])
        self.image("gt.png", (3, 3, 3))
        changed = measure_image_quality(manifest, self.root)
        self.assertNotEqual(changed["identity"]["dataset_id"], masked["identity"]["dataset_id"])

    def test_cancellation_and_progress_do_not_mutate_inputs_or_sampler_state(self):
        import hashlib
        import random
        import numpy as np
        from native.worker.image_quality_measurement import measure_image_quality
        reference, rendered = self.image("gt.png", (0, 0, 0)), self.image("render.png", (1, 1, 1))
        manifest = {"version": 1, "split": "training_fit", "views": [{"id": "one", "reference": reference,
                                                                 "render": rendered}]}
        before = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in self.root.iterdir()}
        python_rng, numpy_rng = random.getstate(), np.random.get_state()
        progress = []
        measure_image_quality(manifest, self.root, progress=progress.append)
        self.assertEqual(progress, [{"event": "progress", "completed": 1, "total": 1, "view_id": "one"}])
        with self.assertRaises(InterruptedError):
            measure_image_quality(manifest, self.root, cancelled=lambda: True)
        self.assertEqual(random.getstate(), python_rng)
        after_numpy = np.random.get_state()
        self.assertEqual(after_numpy[0], numpy_rng[0])
        self.assertTrue(np.array_equal(after_numpy[1], numpy_rng[1]))
        self.assertEqual(after_numpy[2:], numpy_rng[2:])
        self.assertEqual({path: hashlib.sha256(path.read_bytes()).hexdigest() for path in before}, before)

    def test_textured_patch_matches_independent_scalar_weighted_population_reference(self):
        from native.worker.image_quality_measurement import measure_image_quality
        # Golden calculated independently using the paper's normalized 2-D
        # Gaussian and weighted deviations about each mean (not separable
        # second moments as in production), over 6 valid windows and RGB.
        import numpy as np
        y, x, channel = np.indices((12, 13, 3))
        reference = ((y * 17 + x * 29 + channel * 47) % 256).astype(np.uint8)
        rendered = ((reference.astype(np.int16) + 19) % 256).astype(np.uint8)
        self.Image.fromarray(reference, "RGB").save(self.root / "textured-gt.png")
        self.Image.fromarray(rendered, "RGB").save(self.root / "textured-render.png")
        result = measure_image_quality({"version": 1, "views": [{"id": "textured",
            "reference": "textured-gt.png", "render": "textured-render.png"}]}, self.root)
        self.assertAlmostEqual(result["metrics"]["ssim"], .6215652761358995, places=12)
        self.assertAlmostEqual(result["metrics"]["psnr"], 11.565341737628296, places=11)

    def test_cpu_tiles_cover_every_pixel_and_valid_window_across_both_tile_edges(self):
        from native.worker.image_quality_measurement import measure_image_quality
        result = measure_image_quality({"version": 1, "views": [{"id": "wide",
            "reference": self.image("wide-gt.png", (0, 0, 0), (270, 85)),
            "render": self.image("wide-render.png", (1, 1, 1), (270, 85))}]}, self.root)
        self.assertEqual(result["evidence"]["aggregate"]["valid_pixels"], 22950)
        self.assertEqual(result["evidence"]["aggregate"]["ssim_windows"], 19500)
        self.assertAlmostEqual(result["metrics"]["psnr"], 48.1308036086791, places=11)
        self.assertAlmostEqual(result["metrics"]["ssim"], .0001 / (.0001 + (1 / 255) ** 2), places=12)

    def test_true_rgb16_png_and_images_below_ssim_size_are_not_silently_converted(self):
        import struct
        import zlib
        from native.worker.image_quality_measurement import measure_image_quality
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
        header = struct.pack(">IIBBBBB", 16, 16, 16, 2, 0, 0, 0)
        pixels = b"".join(b"\0" + b"\0\x01" * (16 * 3) for _ in range(16))
        (self.root / "rgb16.png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))
        for name in ("rgb16.png", self.image("tiny.png", (0, 0, 0), (10, 10))):
            with self.subTest(name=name), self.assertRaises(ValueError):
                measure_image_quality({"version": 1, "views": [{"id": "invalid", "reference": name,
                                      "render": name}]}, self.root)

    def test_protocol_identity_records_the_actual_cpu_decoder_and_numeric_runtime(self):
        import numpy as np
        import PIL
        from PIL import features
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("gt.png", (10, 20, 30))
        result = measure_image_quality({"version": 1, "views": [{"id": "one", "reference": reference,
                                                               "render": reference}]}, self.root)
        runtime = result["evidence"]["protocol"]["runtime"]
        self.assertEqual(runtime["numpy"], np.__version__)
        self.assertEqual(runtime["pillow"], PIL.__version__)
        self.assertEqual(runtime["libjpeg_build"], features.version_codec("jpg"))

    def test_input_change_during_real_decoding_is_not_attested_as_the_original_content(self):
        from unittest.mock import patch
        from native.worker.image_quality_measurement import measure_image_quality
        reference, rendered = self.image("gt.png", (0, 0, 0)), self.image("render.png", (1, 1, 1))
        original_crop = self.Image.Image.crop
        changed = [False]
        def crop_then_external_writer(image, box):
            result = original_crop(image, box)
            if not changed[0]:
                changed[0] = True
                self.Image.new("RGB", (16, 16), (2, 2, 2)).save(self.root / rendered)
            return result
        with patch.object(self.Image.Image, "crop", crop_then_external_writer):
            with self.assertRaisesRegex(ValueError, "changed"):
                measure_image_quality({"version": 1, "views": [{"id": "one", "reference": reference,
                                                              "render": rendered}]}, self.root)

    def test_missing_inputs_duplicate_reference_and_oversized_dimensions_are_explicit_errors(self):
        import struct
        import zlib
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("gt.png", (0, 0, 0))
        for views in ([{"id": "one", "reference": reference, "render": "missing.png"}],
                      [{"id": "one", "reference": reference, "render": reference},
                       {"id": "two", "reference": reference, "render": reference}]):
            with self.assertRaises(ValueError):
                measure_image_quality({"version": 1, "views": views}, self.root)
        data = bytearray((self.root / reference).read_bytes())
        data[16:24] = struct.pack(">II", 32769, 11)
        data[29:33] = struct.pack(">I", zlib.crc32(data[12:29]) & 0xffffffff)
        (self.root / "oversized.png").write_bytes(data)
        with self.assertRaises(ValueError):
            measure_image_quality({"version": 1, "views": [{"id": "large", "reference": "oversized.png",
                                                          "render": "oversized.png"}]}, self.root)

    def test_produced_settings_stay_within_the_comparison_json_contract(self):
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("gt.png", (0, 0, 0))
        deep = {}
        for _ in range(18):
            deep = {"nested": deep}
        for settings in ({1: "coerced-key"}, {"bad": float("nan")}, deep):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                measure_image_quality({"version": 1, "views": [{"id": "one", "reference": reference,
                                      "render": reference}], "settings": settings}, self.root)

    def test_hardlink_reference_alias_is_a_duplicate_but_distinct_equal_files_are_not(self):
        import os
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.image("gt.png", (0, 0, 0))
        alias = self.root / "alias.png"
        try:
            os.link(self.root / reference, alias)
        except OSError:
            self.skipTest("This filesystem does not support test-owned hardlinks")
        if not alias.stat().st_ino:
            self.skipTest("This filesystem does not expose a stable file identity")
        views = [{"id": "one", "reference": reference, "render": reference},
                 {"id": "two", "reference": alias.name, "render": alias.name}]
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            measure_image_quality({"version": 1, "views": views}, self.root)
        views[1]["reference"] = self.image("independent.png", (0, 0, 0))
        result = measure_image_quality({"version": 1, "views": views}, self.root)
        self.assertEqual(result["evidence"]["aggregate"]["views"], 2)

    def test_aba_path_swap_cannot_measure_b_while_attesting_restored_a_bytes(self):
        import hashlib
        import os
        from unittest.mock import patch
        from native.worker.image_quality_measurement import measure_image_quality
        reference = self.root / self.image("gt-a.png", (0, 0, 0))
        rendered = self.image("render-a.png", (0, 0, 0))
        alternate = self.root / self.image("gt-b.png", (255, 255, 255))
        original_bytes, alternate_bytes = reference.read_bytes(), alternate.read_bytes()
        real_open = self.Image.open
        swapped = [False]
        def open_b_then_restore_a(file, *args, **kwargs):
            if isinstance(file, (str, os.PathLike)) and Path(file) == reference and not swapped[0]:
                swapped[0] = True
                reference.write_bytes(alternate_bytes)
                try:
                    image = real_open(file, *args, **kwargs)
                    image.load()
                finally:
                    reference.write_bytes(original_bytes)
                return image
            return real_open(file, *args, **kwargs)
        with patch.object(self.Image, "open", open_b_then_restore_a):
            result = measure_image_quality({"version": 1, "views": [{"id": "one",
                "reference": str(reference), "render": rendered}]}, self.root)
        self.assertEqual(reference.read_bytes(), original_bytes)
        self.assertEqual(result["evidence"]["views"][0]["reference_sha256"], hashlib.sha256(original_bytes).hexdigest())
        self.assertEqual(result["metrics"]["psnr"],
                         {"value": None, "special": "positive_infinity", "reason": "zero_mse"})


class ImageQualityMeasurementCliTests(unittest.TestCase):
    setUp = ImageQualityMeasurementTests.setUp
    image = ImageQualityMeasurementTests.image

    def _run_with_open_stdin(self, cancel=False):
        import json
        import subprocess
        import sys
        import threading
        reference, rendered = self.image("gt.png", (0, 0, 0)), self.image("render.png", (1, 1, 1))
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps({"version": 1, "views": [{"id": "front", "reference": reference,
                                                               "render": rendered}]}), encoding="utf-8")
        output = self.root / "measurement.json"
        script = Path(__file__).resolve().parents[2] / "scripts/measure_image_quality.py"
        process = subprocess.Popen([sys.executable, str(script), "--manifest", str(manifest),
                                    "--output", str(output)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        captured = {}
        def drain(name, stream):
            captured[name] = stream.read()
        readers = [threading.Thread(target=drain, args=(name, stream), daemon=True)
                   for name, stream in (("stdout", process.stdout), ("stderr", process.stderr))]
        for reader in readers:
            reader.start()
        try:
            if cancel:
                process.stdin.write(b"cancel\n")
                process.stdin.flush()
            # QProcess leaves its input channel open. Completion/cancellation
            # must not depend on EOF from the desktop owner.
            try:
                code = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.fail("Measurement helper did not exit while its stdin pipe remained open")
            for reader in readers:
                reader.join(timeout=5)
                self.assertFalse(reader.is_alive(), "Measurement output pipe was not closed")
            self.assertFalse(process.stdin.closed)
            return code, captured, output
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stdin.close()
            for reader in readers:
                reader.join(timeout=5)
            process.stdout.close()
            process.stderr.close()

    def test_cli_completes_and_publishes_report_without_input_eof(self):
        import json
        code, captured, output = self._run_with_open_stdin()
        self.assertEqual(code, 0, captured["stderr"])
        self.assertEqual(output.read_bytes(), captured["stdout"])
        self.assertAlmostEqual(json.loads(captured["stdout"])["metrics"]["psnr"], 48.1308036086791, places=10)

    def test_cli_cancel_is_observed_without_input_eof_or_published_report(self):
        code, captured, output = self._run_with_open_stdin(cancel=True)
        self.assertEqual(code, 3, captured["stderr"])
        self.assertEqual(captured["stdout"], b"")
        self.assertIn(b'"event":"cancelled"', captured["stderr"])
        self.assertFalse(output.exists())

    def test_cli_measures_real_files_progress_and_never_overwrites_inputs(self):
        import hashlib
        import json
        import subprocess
        import sys
        reference, rendered = self.image("gt.png", (0, 0, 0)), self.image("render.png", (1, 1, 1))
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps({"version": 1, "views": [{"id": "front", "reference": reference,
                                                               "render": rendered}]}), encoding="utf-8")
        inputs = {path: hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in (manifest, self.root / reference, self.root / rendered)}
        script = Path(__file__).resolve().parents[2] / "scripts/measure_image_quality.py"
        output = self.root / "measurement.json"
        command = [sys.executable, str(script), "--manifest", str(manifest), "--output", str(output)]
        result = subprocess.run(command, capture_output=True, text=True,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertAlmostEqual(report["metrics"]["psnr"], 48.1308036086791, places=10)
        self.assertEqual(output.read_bytes(), result.stdout.replace("\r\n", "\n").encode("utf-8"))
        self.assertEqual(json.loads(result.stderr.strip())["completed"], 1)
        blocked = subprocess.run(command, capture_output=True, text=True,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(blocked.returncode, 2)
        self.assertEqual({path: hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}, inputs)

    def test_cli_base_directory_duplicate_keys_and_owned_cancel_have_distinct_results(self):
        import json
        import subprocess
        import sys
        reference, rendered = self.image("gt.png", (0, 0, 0)), self.image("render.png", (1, 1, 1))
        snapshot = self.root / "snapshot"
        snapshot.mkdir()
        manifest = snapshot / "manifest.json"
        manifest.write_text(json.dumps({"version": 1, "views": [{"id": "front", "reference": reference,
                                                               "render": rendered}]}), encoding="utf-8")
        script = Path(__file__).resolve().parents[2] / "scripts/measure_image_quality.py"
        output = snapshot / "report.json"
        command = [sys.executable, str(script), "--manifest", str(manifest), "--base-dir", str(self.root),
                   "--output", str(output)]
        cancelled = subprocess.run(command, input="cancel\n", capture_output=True, text=True,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(cancelled.returncode, 3, cancelled.stderr)
        self.assertFalse(output.exists())
        completed = subprocess.run(command, input="", capture_output=True, text=True,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["evidence"]["views"][0]["reference"],
                         str((self.root / reference).resolve()))
        invalid_output = snapshot / "invalid-report.json"
        manifest.write_text('{"version":1,"version":1,"views":[]}', encoding="utf-8")
        invalid = subprocess.run(command[:-1] + [str(invalid_output)], input="", capture_output=True, text=True,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(invalid.returncode, 2)
        self.assertFalse(invalid_output.exists())


if __name__ == "__main__":
    unittest.main()
