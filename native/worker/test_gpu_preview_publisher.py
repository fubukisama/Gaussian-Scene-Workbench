import json
import ctypes
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

from native.worker import gpu_preview_publisher


class DriverFunction:
    def __init__(self, name): self.name = name
    def __call__(self, *args):
        if self.name == "cuMemGetAllocationGranularity":
            ctypes.cast(args[0], ctypes.POINTER(ctypes.c_size_t))[0] = 2 * 1024**2
        elif self.name in ("cuMemAddressReserve", "cuMemCreate", "cuMemExportToShareableHandle"):
            ctypes.cast(args[0], ctypes.POINTER(ctypes.c_ulonglong))[0] = 0x100000
        return 0


class DriverDll:
    def __getattr__(self, name):
        return DriverFunction(name)


class GpuPreviewPublisherTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "win32", "Shared preview uses Win32 control objects")
    def test_fallback_health_requires_a_real_consumer_slot_release(self):
        try:
            import torch
        except ImportError:
            self.skipTest("Run with the configured training Python for tensor observation tests")
        now = [1.]
        cuda = SimpleNamespace(is_available=lambda: True, current_device=lambda: 0,
                               current_stream=lambda device: SimpleNamespace(cuda_stream=0),
                               mem_get_info=lambda device: (8 * 1024**3, 12 * 1024**3),
                               get_device_name=lambda device: "CPU fixture CUDA boundary",
                               Event=lambda **kwargs: SimpleNamespace(record=lambda stream: None,
                                                                      query=lambda: True, synchronize=lambda: None))
        class TorchBoundary:
            def __getattr__(self, name): return getattr(torch, name)
        torch_boundary = TorchBoundary()
        torch_boundary.cuda = cuda
        model = SimpleNamespace(get_xyz=torch.zeros(2, 3), get_features_dc=torch.zeros(2, 1, 3),
                                get_opacity=torch.zeros(2, 1), get_scaling=torch.zeros(2, 3),
                                get_rotation=torch.zeros(2, 4))
        real_windll = ctypes.WinDLL
        def windll(name, *args, **kwargs):
            return DriverDll() if name == "nvcuda.dll" else real_windll(name, *args, **kwargs)
        with mock.patch.object(ctypes, "WinDLL", side_effect=windll), \
             mock.patch("native.worker.gpu_preview_publisher.time.monotonic", side_effect=lambda: now[0]):
            publisher = gpu_preview_publisher.GpuPreviewPublisher(torch_boundary)
            handle = None
            try:
                self.assertFalse(publisher.has_active_consumer())
                self.assertTrue(publisher.publish(model, 1))
                now[0] = 1.1
                self.assertTrue(publisher.publish(model, 2))
                self.assertFalse(publisher.has_active_consumer())
                # This is the actual renderer/producer boundary: signal the
                # session-named event advertised in the public descriptor.
                kernel = ctypes.WinDLL("kernel32.dll", use_last_error=True)
                kernel.OpenEventW.restype = ctypes.wintypes.HANDLE
                kernel.OpenEventW.argtypes = [ctypes.wintypes.DWORD, ctypes.wintypes.BOOL,
                                              ctypes.wintypes.LPCWSTR]
                kernel.SetEvent.argtypes = [ctypes.wintypes.HANDLE]
                kernel.CloseHandle.argtypes = [ctypes.wintypes.HANDLE]
                handle = kernel.OpenEventW(2, False, publisher.descriptor()["releaseEvent0"])
                self.assertTrue(handle)
                self.assertTrue(kernel.SetEvent(handle))
                now[0] = 1.2
                self.assertTrue(publisher.publish(model, 3))
                self.assertTrue(publisher.has_active_consumer())
                now[0] = 3.3
                self.assertFalse(publisher.has_active_consumer())
            finally:
                if handle: kernel.CloseHandle(handle)
                publisher.close(detach_timeout=0)

    @unittest.skipUnless(sys.platform == "win32", "Shared preview uses Win32 control objects")
    def test_live_low_vram_gate_prevents_reading_and_packing_model_tensors(self):
        free = [8 * 1024**3]
        cuda = SimpleNamespace(is_available=lambda: True, current_device=lambda: 0,
                               current_stream=lambda device: SimpleNamespace(cuda_stream=0),
                               mem_get_info=lambda device: (free[0], 12 * 1024**3))
        torch_boundary = SimpleNamespace(cuda=cuda)

        class Model:
            reads = 0
            @property
            def get_xyz(self):
                self.reads += 1
                raise AssertionError("Low-headroom previews must not inspect training tensors")

        real_windll = ctypes.WinDLL
        def windll(name, *args, **kwargs):
            return DriverDll() if name == "nvcuda.dll" else real_windll(name, *args, **kwargs)

        with mock.patch.object(ctypes, "WinDLL", side_effect=windll):
            publisher = gpu_preview_publisher.GpuPreviewPublisher(torch_boundary)
            model = Model()
            try:
                free[0] = 128 * 1024**2
                self.assertFalse(publisher.publish(model, 3860))
                self.assertEqual(model.reads, 0)
            finally:
                publisher.close(detach_timeout=0)

    def test_geometry_is_bounded_aligned_and_matches_vertex_stride(self):
        geometry = gpu_preview_publisher.compute_preview_geometry(
            free_bytes=6 * 1024**3,
            requested_capacity=1_500_000,
            granularity=2 * 1024**2,
        )

        self.assertIsNotNone(geometry)
        self.assertEqual(geometry["slot_bytes"] % (2 * 1024**2), 0)
        self.assertEqual(
            geometry["allocation_bytes"], geometry["slot_bytes"] * 2
        )
        self.assertLessEqual(geometry["capacity"], 1_500_000)
        self.assertLessEqual(
            geometry["capacity"] * gpu_preview_publisher.VERTEX_STRIDE_BYTES,
            geometry["slot_bytes"],
        )

    def test_geometry_disables_preview_when_vram_reserve_is_unsafe(self):
        self.assertIsNone(
            gpu_preview_publisher.compute_preview_geometry(
                free_bytes=200 * 1024**2,
                requested_capacity=1_500_000,
                granularity=2 * 1024**2,
            )
        )

    def test_control_block_matches_native_little_endian_abi(self):
        block = gpu_preview_publisher.build_control_block(
            sequence=8,
            state=gpu_preview_publisher.CONTROL_READY,
            capacity=1000,
            slot_bytes=56000,
            allocation_bytes=112000,
            slot_snapshots=(
                (17, 900, 80, 123456789, -2.0, 1.0, 4.0, 8.0),
                (18, 950, 81, 123456999, -1.5, 1.5, 4.5, 9.0),
            ),
        )

        self.assertEqual(len(block), gpu_preview_publisher.CONTROL_BYTES)
        self.assertEqual(block[:8], b"GSWGPU2\0")
        self.assertEqual(struct.unpack_from("<I", block, 8)[0], 2)
        self.assertEqual(struct.unpack_from("<I", block, 12)[0], 156)
        self.assertEqual(struct.unpack_from("<I", block, 16)[0], 8)
        self.assertEqual(struct.unpack_from("<Q", block, 104)[0], 18)
        self.assertEqual(struct.unpack_from("<Q", block, 112)[0], 950)
        self.assertEqual(struct.unpack_from("<4f", block, 136), (-1.5, 1.5, 4.5, 9.0))
        self.assertEqual(struct.unpack_from("<I", block, 152)[0], 8)

    def test_ready_descriptor_uses_hex_handle_and_session_scoped_names(self):
        descriptor = gpu_preview_publisher.ready_descriptor(
            session_id="7b29e168-5e64-447d-b262-d3ebbe947c78",
            producer_pid=4242,
            memory_handle=0x41C,
            allocation_bytes=112000,
            slot_bytes=56000,
            capacity=1000,
            device="NVIDIA GPU",
        )
        encoded = json.dumps(descriptor)

        self.assertIn('"memoryHandle": "0x41c"', encoded)
        self.assertEqual(descriptor["slotCount"], 2)
        self.assertEqual(descriptor["strideBytes"], 56)
        self.assertEqual(descriptor["memoryHandleType"], "opaque_win32_kmt")
        self.assertTrue(descriptor["controlMapping"].startswith("Local\\GSW-GPU-"))
        self.assertNotIn("/", descriptor["frameEvent"])


if __name__ == "__main__":
    unittest.main()
