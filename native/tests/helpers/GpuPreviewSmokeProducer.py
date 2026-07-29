"""Hardware smoke producer for CUDA VMM -> OpenGL shared previews.

This helper is intentionally outside the packaged worker.  It publishes two
deterministic frames, then waits for the native probe to release both slots.
"""

import argparse
import json
import sys
import time
from pathlib import Path

import torch


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY_ROOT))

from native.worker.gpu_preview_publisher import (  # noqa: E402
    GpuPreviewPublisher,
    WAIT_OBJECT_0,
    emit_descriptor,
)


class DeterministicGaussians:
    def __init__(self, count):
        self.get_xyz = torch.empty((count, 3), device="cuda", dtype=torch.float32)
        self.get_xyz[:, 0] = torch.linspace(
            -2.0, 2.0, count, device="cuda", dtype=torch.float32
        )
        self.get_xyz[:, 1] = 2.0
        self.get_xyz[:, 2] = 3.0
        self.get_features_dc = torch.zeros(
            (count, 1, 3), device="cuda", dtype=torch.float32
        )
        self.get_opacity = torch.full(
            (count, 1), 0.8, device="cuda", dtype=torch.float32
        )
        self.get_scaling = torch.full(
            (count, 3), 0.01, device="cuda", dtype=torch.float32
        )
        self.get_rotation = torch.zeros(
            (count, 4), device="cuda", dtype=torch.float32
        )
        self.get_rotation[:, 0] = 1.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--descriptor-file", type=Path)
    args = parser.parse_args()

    publisher = GpuPreviewPublisher(torch, requested_capacity=4096, fps=60.0)
    descriptor = publisher.descriptor()
    emit_descriptor(descriptor)
    if args.descriptor_file is not None:
        args.descriptor_file.write_text(
            "[gsw-training-gpu-preview] "
            + json.dumps(descriptor, ensure_ascii=False, separators=(",", ":"))
            + "\n",
            encoding="utf-8",
        )
    gaussians = DeterministicGaussians(64)
    try:
        for iteration in (11, 12):
            if not publisher.publish(gaussians, iteration):
                raise RuntimeError("smoke frame was not enqueued")
            publisher._complete_pending(True)
            time.sleep(publisher.minimum_interval)

        released = [False, False]
        deadline = time.monotonic() + max(args.timeout, 1.0)
        while time.monotonic() < deadline and not all(released):
            for slot, handle in enumerate(publisher.release_events):
                if not released[slot]:
                    released[slot] = (
                        publisher.kernel32.WaitForSingleObject(handle, 0)
                        == WAIT_OBJECT_0
                    )
            time.sleep(0.005)
        if not all(released):
            raise RuntimeError("native smoke probe did not release both slots")
        print("GPU_PREVIEW_PRODUCER_OK", flush=True)
    finally:
        publisher.close(detach_timeout=0.0)


if __name__ == "__main__":
    main()
