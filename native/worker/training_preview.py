"""Bounded, atomic observation snapshots, independent of resumable checkpoints.

Only the training thread reads tensors. A single writer owns the detached CPU
array; slow disks skip intermediate updates rather than queueing unbounded work.
"""
import os
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


class TrainingPreviewPublisher:
    def __init__(self, output, emit, interval=3.0, max_points=500_000):
        self.directory = Path(output) / ".gsw" / "previews" / uuid.uuid4().hex
        self.emit = emit
        self.interval = max(0.1, float(interval))
        self.max_points = max(1, int(max_points))
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="gsw-preview")
        self.future = None
        self.last_time = float("-inf")
        self.sequence = 0
        self.paths = []

    def due(self):
        return (self.future is None or self.future.done()) and time.monotonic() - self.last_time >= self.interval

    def publish_gaussians(self, model, iteration, initial=False):
        if not self.due():
            return False
        # Raw log-scale/logit/quaternion attributes match the source PLY contract.
        # No optimizer state or full SH arrays are copied for an observation frame.
        import torch
        with torch.no_grad():
            count = int(model.get_xyz.shape[0])
            stride = max(1, (count + self.max_points - 1) // self.max_points)
            fields = (model.get_xyz[::stride], model._features_dc[::stride, 0, :],
                      model._opacity[::stride], model._scaling[::stride], model._rotation[::stride])
            rows = torch.cat(fields, dim=1).detach().to(device="cpu", dtype=torch.float32).numpy()
        return self.publish_rows(rows, iteration, count, initial)

    def publish_rows(self, rows, iteration, source_count=None, initial=False):
        if not self.due():
            return False
        import numpy as np
        rows = np.asarray(rows, dtype="<f4")
        if rows.ndim != 2 or rows.shape[1] != 14 or rows.shape[0] == 0:
            return False
        source_count = int(source_count if source_count is not None else len(rows))
        stride = max(1, (len(rows) + self.max_points - 1) // self.max_points)
        rows = rows[::stride].copy(order="C")
        self.last_time = time.monotonic()
        self.sequence += 1
        path = self.directory / f"preview_{self.sequence:06d}.ply"
        self.future = self.executor.submit(self._write, path, rows, int(iteration), source_count, initial)
        return True

    def _write(self, path, rows, iteration, source_count, initial):
        temporary = path.with_suffix(".tmp")
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            properties = ("x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                          "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3")
            header = "ply\nformat binary_little_endian 1.0\ncomment gsw observation preview, not a checkpoint\n"
            header += f"element vertex {len(rows)}\n"
            header += "".join(f"property float {name}\n" for name in properties) + "end_header\n"
            with temporary.open("wb") as stream:
                stream.write(header.encode("ascii"))
                stream.write(rows.tobytes())
            os.replace(temporary, path)
            self.emit("[gsw-training-preview]", {
                "iteration": iteration, "point_cloud_path": str(path.resolve()),
                "preview_kind": "gaussian_initial" if initial else "gaussian_live",
                "gaussian_count": source_count, "preview_count": len(rows),
            })
            self.paths.append(path)
            while len(self.paths) > 4:
                obsolete = self.paths.pop(0)
                try:
                    obsolete.unlink()
                except OSError:
                    pass  # A viewer may still be reading it on Windows.
        except Exception as error:
            # Preview I/O must never abort optimization or overwrite a checkpoint.
            self.emit("[gsw-preview-warning]", str(error))
        finally:
            # Some supported external training environments still use Python
            # 3.7, where pathlib.unlink has no missing_ok keyword.
            try:
                temporary.unlink()
            except OSError:
                pass

    def close(self):
        self.executor.shutdown(wait=True)
