"""Public pixel, mask/depth and finite staging-budget regressions."""
import unittest


class ImagePipelineConfigurationTests(unittest.TestCase):
    def test_environment_configuration_is_explicit_and_finite(self):
        from native.worker.training_image_pipeline import ImagePipelineConfig
        config = ImagePipelineConfig.from_environment({
            "GSW_TRAINING_IMAGE_PIPELINE": "bounded",
            "GSW_TRAINING_IMAGE_GPU_SLOTS": "2",
            "GSW_TRAINING_IMAGE_PINNED_MIB": "64",
            "GSW_TRAINING_IMAGE_GPU_MIB": "512"})
        self.assertTrue(config.enabled)
        self.assertEqual(config.slots, 2)
        self.assertEqual(config.pinned_bytes, 67108864)
        self.assertEqual(config.device_bytes, 536870912)
        self.assertFalse(ImagePipelineConfig.from_environment({}).enabled)
        self.assertFalse(ImagePipelineConfig.from_environment({"GSW_TRAINING_IMAGE_PIPELINE": "legacy"}).enabled)
        with self.assertRaises(ValueError):
            ImagePipelineConfig.from_environment({"GSW_TRAINING_IMAGE_GPU_SLOTS": "3"})
        with self.assertRaises(ValueError):
            ImagePipelineConfig.from_environment({"GSW_TRAINING_IMAGE_PINNED_MIB": "-1"})

    def test_direct_configuration_rejects_non_integral_and_non_finite_budgets(self):
        from native.worker.training_image_pipeline import ImagePipelineConfig
        for settings in ({"slots": True}, {"slots": 2.0}, {"pinned_bytes": .5},
                         {"device_bytes": float("inf")}, {"device_bytes": float("nan")},
                         {"device_bytes": True}, {"enabled": "bounded"}):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                ImagePipelineConfig(**settings)


class TrainingImagePipelineTests(unittest.TestCase):
    def setUp(self):
        try:
            import torch
            from PIL import Image
        except ImportError:
            self.skipTest("Run with a training runtime for camera image tensor tests")
        from native.worker.training_image_pipeline import HostCameraImage, TrainingImagePipeline
        from native.worker.training_image_pipeline import ImagePipelineConfig
        self.torch = torch
        self.Image = Image
        self.HostCameraImage = HostCameraImage
        self.Pipeline = TrainingImagePipeline
        self.Config = ImagePipelineConfig

    def _two_dgs_modules(self):
        """Load the real external Camera/decode seam, not the full Scene."""
        import importlib.util
        import os
        from pathlib import Path
        import sys
        from types import ModuleType
        source = Path(os.environ.get("TWO_DGS_DIR", "D:/Apps/GSW/runtime/2dgs"))
        if not (source / "scene/cameras.py").is_file():
            self.skipTest("The optional 2DGS runtime source is not installed")
        old_path = list(sys.path)
        saved = {key: value for key, value in sys.modules.items()
                 if key in ("utils", "scene") or key.startswith(("utils.", "scene."))}
        for key in saved:
            sys.modules.pop(key)
        def restore():
            sys.path[:] = old_path
            for key in list(sys.modules):
                if key in ("utils", "scene") or key.startswith(("utils.", "scene.")):
                    sys.modules.pop(key)
            sys.modules.update(saved)
        self.addCleanup(restore)
        sys.path.insert(0, str(source))
        # A namespace avoids importing Scene and its CUDA rasterizer merely to
        # inspect its real camera/decode interface. No camera implementation is
        # substituted: both source modules below are executed unchanged.
        package = ModuleType("scene")
        package.__path__ = [str(source / "scene")]
        sys.modules["scene"] = package
        camera_spec = importlib.util.spec_from_file_location("scene.cameras", source / "scene/cameras.py")
        cameras = importlib.util.module_from_spec(camera_spec)
        sys.modules["scene.cameras"] = cameras
        camera_spec.loader.exec_module(cameras)
        decode_spec = importlib.util.spec_from_file_location("utils.camera_utils", source / "utils/camera_utils.py")
        decode = importlib.util.module_from_spec(decode_spec)
        sys.modules["utils.camera_utils"] = decode
        decode_spec.loader.exec_module(decode)
        return source, cameras, decode

    def test_two_dgs_native_decode_is_compact_reversible_and_does_not_edit_sources(self):
        import hashlib
        import os
        from unittest.mock import patch
        from native.worker.training_image_pipeline import install_two_dgs_camera_adapter
        source, cameras, decode = self._two_dgs_modules()
        paths = (source / "scene/cameras.py", source / "utils/camera_utils.py",
                 source / "utils/general_utils.py")
        hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths]
        image = self.Image.new("RGB", (2, 1))
        image.putdata([(0, 127, 255), (255, 128, 1)])
        original_decode = decode.PILtoTorch
        legacy = original_decode(image, (2, 1))
        with patch.dict(os.environ, {"GSW_TRAINING_IMAGE_PIPELINE": "bounded"}):
            restore = install_two_dgs_camera_adapter(self.torch, cameras, decode)
            try:
                compact = decode.PILtoTorch(image, (2, 1))
                self.assertEqual(compact.dtype, self.torch.uint8)
                self.assertEqual(compact.numel() * compact.element_size(), 6)
                self.assertTrue(self.torch.equal(compact.float().div(255.0), legacy))
            finally:
                restore()
        self.assertIs(decode.PILtoTorch, original_decode)
        self.assertEqual(hashes, [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths])

    def test_two_dgs_real_camera_keeps_rgb_alpha_and_geometry_with_compact_host_storage(self):
        if not self.torch.cuda.is_available():
            self.skipTest("The external public Camera constructor requires CUDA transforms")
        import hashlib
        import os
        import numpy as np
        from types import SimpleNamespace
        from unittest.mock import patch
        from native.worker.training_image_pipeline import install_two_dgs_camera_adapter
        source, cameras, decode = self._two_dgs_modules()
        paths = (source / "scene/cameras.py", source / "utils/camera_utils.py")
        hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths]
        image = self.Image.new("RGBA", (2, 1))
        image.putdata([(0, 127, 255, 0), (255, 128, 1, 128)])
        info = SimpleNamespace(image=image, uid=19, R=np.eye(3), T=np.array([.5, -.3, .2]),
                               FovX=.8, FovY=.6, image_name="literal-rgba")
        args = SimpleNamespace(resolution=1, data_device="cpu")
        with patch.dict(os.environ, {"GSW_TRAINING_IMAGE_PIPELINE": "legacy"}):
            legacy = decode.loadCam(args, 2, info, 1)
        with patch.dict(os.environ, {"GSW_TRAINING_IMAGE_PIPELINE": "bounded"}):
            restore = install_two_dgs_camera_adapter(self.torch, cameras, decode)
            try:
                native = decode.loadCam(args, 2, info, 1)
                with self.Pipeline(self.torch, device="cpu") as pipeline:
                    prepared = pipeline.prepare(native)
                    self.assertEqual(pipeline.stats()["active_host_bytes"], 8)
                    self.assertTrue(self.torch.equal(prepared.image, legacy.original_image))
                    self.assertTrue(self.torch.equal(prepared.alpha_mask, legacy.gt_alpha_mask))
                    self.assertEqual(float(prepared.image[2, 0, 0]), 1.0)
                    self.assertEqual(float(prepared.alpha_mask[0, 0, 0]), 0.0)
                self.assertEqual((native.image_width, native.image_height), (2, 1))
                self.assertEqual((native.uid, native.colmap_id, native.image_name), (2, 19, "literal-rgba"))
                for name in ("world_view_transform", "projection_matrix", "full_proj_transform", "camera_center"):
                    self.assertTrue(self.torch.equal(getattr(native, name), getattr(legacy, name)))
            finally:
                restore()
        self.assertEqual(hashes, [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths])

    def test_eight_bit_host_cache_preserves_literal_pixels(self):
        image = self.Image.new("RGB", (2, 1))
        image.putdata([(0, 127, 255), (255, 128, 1)])
        original = image.tobytes()
        payload = self.HostCameraImage.from_pil(image, (2, 1), self.torch)
        self.assertEqual(payload.rgb.dtype, self.torch.uint8)
        self.assertEqual(payload.rgb.numel() * payload.rgb.element_size(), 6)
        self.assertIsNone(payload.alpha_mask)
        ready = self.Pipeline(self.torch, device="cpu").prepare(payload)
        expected = self.torch.tensor([[[0.0, 1.0]],
                                      [[0.4980392156862745, 0.5019607843137255]],
                                      [[1.0, 0.00392156862745098]]], dtype=self.torch.float32)
        self.assertTrue(self.torch.equal(ready.image, expected))
        self.assertEqual(image.tobytes(), original)

    def test_total_host_bytes_counts_real_shared_cache_storage_without_materializing_float_targets(self):
        from types import SimpleNamespace
        from native.worker.training_image_pipeline import total_host_bytes
        rgba = self.torch.zeros((4, 2, 2), dtype=self.torch.uint8)
        payload = self.HostCameraImage(rgba[:3], rgba[3:4])
        camera = SimpleNamespace(_gsw_image_payload=payload)
        depth = self.torch.ones((1, 2, 2), dtype=self.torch.float32)
        legacy = SimpleNamespace(original_image=rgba[:3], alpha_mask=rgba[3:4],
                                 invdepthmap=depth, depth_mask=None)
        self.assertEqual(total_host_bytes([camera, camera]), 16)
        self.assertEqual(total_host_bytes([camera, legacy]), 32)
        self.assertEqual(total_host_bytes([]), 0)

    def test_rgba_depth_and_depth_mask_keep_their_independent_values(self):
        image = self.Image.new("RGBA", (2, 1))
        image.putdata([(10, 20, 30, 0), (255, 0, 0, 128)])
        payload = self.HostCameraImage.from_pil(image, (2, 1), self.torch)
        depth = self.torch.tensor([[[1.25, -0.125]]], dtype=self.torch.float32)
        depth_mask = self.torch.tensor([[[1.0, 0.0]]], dtype=self.torch.float32)
        payload = self.HostCameraImage(payload.rgb, payload.alpha_mask, depth, depth_mask)
        ready = self.Pipeline(self.torch, device="cpu").prepare(payload)
        self.assertEqual(ready.alpha_mask.dtype, self.torch.float32)
        expected_alpha = self.torch.tensor([[[0.0, 0.5019607843137255]]])
        self.assertTrue(self.torch.equal(ready.alpha_mask, expected_alpha))
        self.assertTrue(self.torch.equal(ready.invdepthmap, depth))
        self.assertTrue(self.torch.equal(ready.depth_mask, depth_mask))

    def test_exposure_half_preserves_real_alpha_and_opaque_semantics(self):
        image = self.Image.new("RGBA", (4, 1))
        image.putdata([(10, 20, 30, 128)] * 4)
        for half, expected in (("left", [0.0, 0.0, 0.5019607843137255, 0.5019607843137255]),
                               ("right", [0.5019607843137255, 0.5019607843137255, 0.0, 0.0])):
            with self.subTest(half=half):
                payload = self.HostCameraImage.from_pil(image, (4, 1), self.torch, masked_half=half)
                ready = self.Pipeline(self.torch, device="cpu").prepare(payload)
                self.assertTrue(self.torch.equal(ready.alpha_mask,
                    self.torch.tensor([[expected]], dtype=self.torch.float32)))
        opaque = self.Image.new("RGB", (4, 1), (255, 0, 0))
        payload = self.HostCameraImage.from_pil(opaque, (4, 1), self.torch, masked_half="left")
        ready = self.Pipeline(self.torch, device="cpu").prepare(payload)
        self.assertTrue(self.torch.equal(ready.alpha_mask, self.torch.tensor([[[0.0, 0.0, 1.0, 1.0]]])))

    def test_high_bit_depth_uses_float_fallback_not_lossy_uint8(self):
        import numpy as np
        image = self.Image.fromarray(np.array([[1, 32768]], dtype=np.uint16))
        self.assertIsNone(self.HostCameraImage.from_pil(image, (2, 1), self.torch))
        rgb = self.torch.tensor([[[0.1234567, 0.7654321]]] * 3, dtype=self.torch.float32)
        payload = self.HostCameraImage(rgb)
        ready = self.Pipeline(self.torch, device="cpu").prepare(payload)
        self.assertTrue(self.torch.equal(ready.image, rgb))
        self.assertEqual(payload.rgb.dtype, self.torch.float32)

    def test_owned_slots_are_bounded_and_do_not_change_sampler_state(self):
        import random
        payloads = [self.HostCameraImage.from_pil(self.Image.new("RGB", (2, 1), (index, 40, 90)),
                                                (2, 1), self.torch) for index in range(6)]
        pipeline = self.Pipeline(self.torch, device="cpu", config=self.Config(slots=2, pinned_bytes=0))
        before = random.getstate()
        for index, payload in enumerate(payloads):
            next_payload = payloads[index + 1] if index + 1 < len(payloads) else None
            ready = pipeline.prepare(payload, next_camera=next_payload)
            self.assertEqual(float(ready.image[0, 0, 0]), float(self.torch.tensor(index / 255.0)))
            self.assertLessEqual(pipeline.stats()["resident_slots"], 2)
        self.assertEqual(random.getstate(), before)
        self.assertEqual(pipeline.stats()["pinned_bytes"], 0)
        pipeline.close()
        self.assertEqual(pipeline.stats()["resident_slots"], 0)

    def test_one_slot_keeps_current_pixels_and_reports_skipped_lookahead(self):
        first = self.HostCameraImage(self.torch.zeros((3, 1, 2), dtype=self.torch.uint8))
        second = self.HostCameraImage(self.torch.full((3, 1, 2), 255, dtype=self.torch.uint8))
        with self.Pipeline(self.torch, device="cpu",
                           config=self.Config(slots=1, pinned_bytes=0)) as pipeline:
            ready = pipeline.prepare(first, next_camera=second)
            self.assertTrue(self.torch.equal(ready.image, self.torch.zeros((3, 1, 2))))
            self.assertEqual(pipeline.stats()["resident_slots"], 1)
            self.assertEqual(pipeline.stats()["transfers"], 1)
            self.assertEqual(pipeline.stats()["prefetch_skips"], 1)
            ready = pipeline.prepare(second)
            self.assertTrue(self.torch.equal(ready.image, self.torch.ones((3, 1, 2))))

    def test_lookahead_pipeline_preserves_python_numpy_and_torch_rng_states(self):
        import random
        import numpy as np
        from native.worker.training_camera_lookahead import predict_next_camera
        cameras = [self.HostCameraImage(self.torch.arange(12, dtype=self.torch.uint8)
                   .reshape(3, 2, 2)),
                   self.HostCameraImage(self.torch.full((3, 2, 2), 255, dtype=self.torch.uint8))]
        python_state, numpy_state, torch_state = random.getstate(), np.random.get_state(), self.torch.get_rng_state()
        with self.Pipeline(self.torch, device="cpu", config=self.Config(pinned_bytes=0)) as pipeline:
            predicted = predict_next_camera(cameras, [1])
            prepared = pipeline.prepare(cameras[0], next_camera=predicted)
            cached = pipeline.prepare(predicted)
            self.assertTrue(self.torch.equal(prepared.image, cameras[0].rgb.float().div(255.0)))
            self.assertTrue(self.torch.equal(cached.image, self.torch.ones((3, 2, 2))))
            self.assertEqual(pipeline.stats()["cache_hits"], 1)
        self.assertEqual(random.getstate(), python_state)
        observed_numpy = np.random.get_state()
        self.assertEqual(observed_numpy[0], numpy_state[0])
        self.assertTrue(np.array_equal(observed_numpy[1], numpy_state[1]))
        self.assertEqual(observed_numpy[2:], numpy_state[2:])
        self.assertTrue(self.torch.equal(self.torch.get_rng_state(), torch_state))

    def test_device_budget_refuses_before_image_allocation(self):
        if not self.torch.cuda.is_available():
            self.skipTest("CUDA is required for the controlled allocation gate")
        payload = self.HostCameraImage.from_pil(self.Image.new("RGB", (2, 1), (10, 20, 30)),
                                              (2, 1), self.torch)
        pipeline = self.Pipeline(self.torch, device="cuda",
                                 config=self.Config(pinned_bytes=0, device_bytes=16))
        with self.assertRaises(MemoryError):
            pipeline.prepare(payload)
        self.assertEqual(pipeline.stats()["resident_slots"], 0)
        self.assertEqual(pipeline.stats()["device_bytes"], 0)
        pipeline.close()

    def test_invalid_targets_are_rejected_without_mutating_live_payloads(self):
        payload = self.HostCameraImage(self.torch.tensor([[[float("nan")]]] * 3))
        pipeline = self.Pipeline(self.torch, device="cpu")
        with self.assertRaises(ValueError):
            pipeline.prepare(payload)
        self.assertEqual(pipeline.stats()["resident_slots"], 0)
        self.assertTrue(self.torch.isnan(payload.rgb).all())

    def test_cuda_prefetch_has_bounded_pins_and_consumer_stream_pixels(self):
        if not self.torch.cuda.is_available():
            self.skipTest("CUDA is required for the controlled stream test")
        pixels = self.torch.arange(256, dtype=self.torch.uint8).reshape(1, 16, 16).repeat(3, 1, 1)
        first = self.HostCameraImage(pixels)
        second = self.HostCameraImage(pixels.flip(-1))
        pipeline = self.Pipeline(self.torch, device="cuda",
                                 config=self.Config(slots=2, pinned_bytes=4096, device_bytes=65536))
        consumer = self.torch.cuda.Stream()
        with self.torch.cuda.stream(consumer):
            prepared = pipeline.prepare(first, next_camera=second)
            observed = prepared.image.square()
        consumer.synchronize()
        reference = pixels.float().div(255.0)
        self.assertTrue(self.torch.equal(prepared.image.cpu(), reference))
        self.assertTrue(self.torch.equal(observed.cpu(), reference.square()))
        self.assertGreater(pipeline.stats()["pinned_bytes"], 0)
        self.assertLessEqual(pipeline.stats()["pinned_bytes"], 4096)
        self.assertLessEqual(pipeline.stats()["resident_slots"], 2)
        cached = pipeline.prepare(second)
        self.assertTrue(self.torch.equal(cached.image.cpu(), second.rgb.float().div(255.0)))
        self.assertGreaterEqual(pipeline.stats()["cache_hits"], 1)
        pipeline.close()

    def test_cuda_optional_prefetch_oom_keeps_current_image_and_next_step_retries(self):
        if not self.torch.cuda.is_available():
            self.skipTest("CUDA is required for controlled H2D allocator failure")
        from unittest.mock import patch
        first = self.HostCameraImage(self.torch.zeros((3, 16, 16), dtype=self.torch.uint8))
        second = self.HostCameraImage(self.torch.full((3, 16, 16), 255, dtype=self.torch.uint8),
                                      self.torch.full((1, 16, 16), 128, dtype=self.torch.uint8))
        original_to = self.torch.Tensor.to
        torch = self.torch
        def unavailable_second_allocation(tensor, *args, **kwargs):
            device = args[0] if args else kwargs.get("device")
            if (isinstance(device, torch.device) and device.type == "cuda"
                    and tensor.device.type == "cpu" and tuple(tensor.shape) == (1, 16, 16)
                    and tensor.dtype == torch.uint8 and int(tensor[0, 0, 0]) == 128):
                raise torch.cuda.OutOfMemoryError("controlled test allocator failure; no memory exhausted")
            return original_to(tensor, *args, **kwargs)
        with self.Pipeline(self.torch, device="cuda",
                           config=self.Config(slots=2, pinned_bytes=4096, device_bytes=65536)) as pipeline:
            with patch.object(self.torch.Tensor, "to", unavailable_second_allocation):
                ready = pipeline.prepare(first, next_camera=second)
            self.assertTrue(self.torch.equal(ready.image.cpu(), self.torch.zeros((3, 16, 16))))
            self.assertEqual(pipeline.stats()["resident_slots"], 1)
            self.assertEqual(pipeline.stats()["prefetch_skips"], 1)
            ready = pipeline.prepare(second)
            self.assertTrue(self.torch.equal(ready.image.cpu(), self.torch.ones((3, 16, 16))))
            self.assertTrue(self.torch.equal(ready.alpha_mask.cpu(), second.alpha_mask.float().div(255.0)))
        # An unavailable current image is not a successful step. Only optional
        # lookahead may degrade to the ordinary current-step transfer path.
        with self.Pipeline(self.torch, device="cuda",
                           config=self.Config(slots=2, pinned_bytes=4096, device_bytes=65536)) as pipeline:
            with patch.object(self.torch.Tensor, "to", unavailable_second_allocation):
                with self.assertRaises(self.torch.cuda.OutOfMemoryError):
                    pipeline.prepare(second)
            self.assertEqual(pipeline.stats()["resident_slots"], 0)
            self.assertEqual(pipeline.stats()["prefetch_skips"], 0)
            ready = pipeline.prepare(second)
            self.assertTrue(self.torch.equal(ready.image.cpu(), self.torch.ones((3, 16, 16))))

    def test_cuda_two_image_budget_falls_back_without_rejecting_a_valid_current_step(self):
        if not self.torch.cuda.is_available():
            self.skipTest("CUDA is required for the controlled staging-budget gate")
        first = self.HostCameraImage(self.torch.zeros((3, 16, 16), dtype=self.torch.uint8))
        second = self.HostCameraImage(self.torch.full((3, 16, 16), 255, dtype=self.torch.uint8))
        with self.Pipeline(self.torch, device="cuda",
                           config=self.Config(slots=2, pinned_bytes=4096, device_bytes=4000)) as pipeline:
            ready = pipeline.prepare(first, next_camera=second)
            self.assertTrue(self.torch.equal(ready.image.cpu(), self.torch.zeros((3, 16, 16))))
            self.assertEqual(pipeline.stats()["resident_slots"], 1)
            self.assertEqual(pipeline.stats()["prefetch_skips"], 1)
            del ready
            ready = pipeline.prepare(second)
            self.assertTrue(self.torch.equal(ready.image.cpu(), self.torch.ones((3, 16, 16))))
            self.assertLessEqual(pipeline.stats()["device_bytes"], 4000)

    def test_legacy_mode_preserves_float_contract_without_new_residency(self):
        from types import SimpleNamespace
        image = self.torch.tensor([[[0.125, 0.75]]] * 3)
        mask = self.torch.ones(1, 1, 2)
        camera = SimpleNamespace(original_image=image, alpha_mask=mask,
                                 invdepthmap=None, depth_mask=None)
        pipeline = self.Pipeline(self.torch, device="cpu", config=self.Config(enabled=False))
        prepared = pipeline.prepare(camera)
        self.assertTrue(self.torch.equal(prepared.image, image))
        self.assertTrue(self.torch.equal(prepared.alpha_mask, mask))
        self.assertEqual(prepared.image.dtype, self.torch.float32)
        self.assertEqual(pipeline.stats()["resident_slots"], 0)
        self.assertEqual(pipeline.stats()["pinned_bytes"], 0)
        pipeline.close()

    def test_float_adapter_only_compacts_explicit_reversible_eight_bit_targets(self):
        floats = self.torch.tensor([[[0.0, 0.5019607843137255]]] * 3)
        packed = self.HostCameraImage.from_tensors(floats, bit_depth=8, torch_module=self.torch)
        self.assertEqual(packed.rgb.dtype, self.torch.uint8)
        self.assertTrue(self.torch.equal(self.Pipeline(self.torch, "cpu").prepare(packed).image, floats))
        for bit_depth in (None, 16):
            preserved = self.HostCameraImage.from_tensors(floats, bit_depth=bit_depth, torch_module=self.torch)
            self.assertEqual(preserved.rgb.dtype, self.torch.float32)
        non_grid = self.torch.full((3, 1, 1), 0.1234567)
        preserved = self.HostCameraImage.from_tensors(non_grid, bit_depth=8, torch_module=self.torch)
        self.assertEqual(preserved.rgb.dtype, self.torch.float32)

    def test_three_dgs_camera_native_hook_keeps_pixels_depth_and_intrinsics(self):
        if not self.torch.cuda.is_available():
            self.skipTest("The existing public Camera constructor requires CUDA transforms")
        import importlib.util
        import os
        from pathlib import Path
        import sys
        import numpy as np
        from unittest.mock import patch
        source = Path(__file__).resolve().parents[2] / "gaussian-splatting"
        old_path = list(sys.path)
        utils_modules = {key: value for key, value in sys.modules.items()
                         if key == "utils" or key.startswith("utils.")}
        for key in utils_modules:
            sys.modules.pop(key)
        def restore():
            sys.path[:] = old_path
            for key in list(sys.modules):
                if key == "utils" or key.startswith("utils."):
                    sys.modules.pop(key)
            sys.modules.update(utils_modules)
        self.addCleanup(restore)
        sys.path.insert(0, str(source))
        spec = importlib.util.spec_from_file_location("native_image_test_camera", source / "scene/cameras.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        image = self.Image.new("RGBA", (2, 1))
        image.putdata([(0, 127, 255, 255), (255, 128, 1, 128)])
        kwargs = dict(resolution=(2, 1), colmap_id=1, R=np.eye(3), T=np.zeros(3),
                      FoVx=.8, FoVy=.6, depth_params={"scale": 2., "offset": .5, "med_scale": 2.},
                      image=image, invdepthmap=np.array([[-1., 2.]], dtype=np.float32),
                      image_name="literal", uid=0, data_device="cpu", train_test_exp=True,
                      is_test_dataset=True, is_test_view=True)
        with patch.dict(os.environ, {"GSW_TRAINING_IMAGE_PIPELINE": "legacy"}):
            legacy = module.Camera(**kwargs)
        with patch.dict(os.environ, {"GSW_TRAINING_IMAGE_PIPELINE": "bounded"}):
            native = module.Camera(**kwargs)
        pipeline = self.Pipeline(self.torch, device="cpu")
        prepared = pipeline.prepare(native)
        self.assertEqual(pipeline.stats()["active_host_bytes"], 24)
        self.assertTrue(self.torch.equal(prepared.image, legacy.original_image))
        self.assertTrue(self.torch.equal(prepared.alpha_mask, legacy.alpha_mask))
        self.assertTrue(self.torch.equal(prepared.invdepthmap, self.torch.tensor([[[.5, 4.5]]])))
        self.assertTrue(self.torch.equal(prepared.depth_mask, legacy.depth_mask))
        self.assertTrue(self.torch.equal(native.world_view_transform, legacy.world_view_transform))
        self.assertTrue(self.torch.equal(native.projection_matrix, legacy.projection_matrix))
        self.assertEqual((native.image_width, native.image_height), (2, 1))
        self.assertTrue(native.depth_reliable)


if __name__ == "__main__":
    unittest.main()
