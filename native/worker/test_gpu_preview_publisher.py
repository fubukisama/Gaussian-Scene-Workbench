import json
import struct
import unittest

from native.worker import gpu_preview_publisher


class GpuPreviewPublisherTests(unittest.TestCase):
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
            slot_snapshots=((17, 900, 80, 123456789), (18, 950, 81, 123456999)),
        )

        self.assertEqual(len(block), gpu_preview_publisher.CONTROL_BYTES)
        self.assertEqual(block[:8], b"GSWGPU1\0")
        self.assertEqual(struct.unpack_from("<I", block, 8)[0], 1)
        self.assertEqual(struct.unpack_from("<I", block, 12)[0], 124)
        self.assertEqual(struct.unpack_from("<I", block, 16)[0], 8)
        self.assertEqual(struct.unpack_from("<Q", block, 88)[0], 18)
        self.assertEqual(struct.unpack_from("<Q", block, 96)[0], 950)
        self.assertEqual(struct.unpack_from("<I", block, 120)[0], 8)

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
