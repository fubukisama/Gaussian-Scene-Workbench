# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Measure explicit RGB8 reference/render pairs on CPU into a fresh JSON report.

No rendering, training, registration, resizing, model access or automatic
held-out claim. Progress is JSONL on stderr; stdout is the final report. Send
``cancel`` on stdin or Ctrl+C to cancel this owned helper. Exit 0=success,
2=invalid input/output/dependency, 3=cancelled. Existing files are never replaced.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from native.worker.image_quality_measurement import MAX_REPORT_BYTES, measure_image_quality


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def _constant(value):
    raise ValueError("Nonstandard JSON number: " + value)


def _read(path):
    with Path(path).open("rb") as handle:
        contents = handle.read(MAX_REPORT_BYTES + 1)
    if len(contents) > MAX_REPORT_BYTES:
        raise ValueError("Measurement manifest exceeds 16 MiB")
    return json.loads(contents.decode("utf-8-sig"), object_pairs_hook=_object, parse_constant=_constant)


class _StdinCancellation:
    """Poll the owned input pipe without blocking computation or process exit."""
    def __init__(self, cancellation):
        self._cancellation = cancellation
        self._descriptor = None
        self._pending = b""
        try:
            descriptor = sys.stdin.fileno()
            if os.name == "nt":
                import ctypes
                from ctypes import wintypes
                import msvcrt
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.GetFileType.argtypes = [wintypes.HANDLE]
                kernel.GetFileType.restype = wintypes.DWORD
                kernel.PeekNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                                                wintypes.LPDWORD, wintypes.LPDWORD, wintypes.LPDWORD]
                kernel.PeekNamedPipe.restype = wintypes.BOOL
                handle = wintypes.HANDLE(msvcrt.get_osfhandle(descriptor))
                if kernel.GetFileType(handle) != 3:  # FILE_TYPE_PIPE; console uses Ctrl+C instead.
                    return
                def available():
                    count = wintypes.DWORD()
                    if not kernel.PeekNamedPipe(handle, None, 0, None, ctypes.byref(count), None):
                        raise ctypes.WinError(ctypes.get_last_error())
                    return count.value
            else:
                import select
                def available():
                    return 256 if select.select([descriptor], [], [], 0)[0] else 0
            self._available = available
            self._descriptor = descriptor
        except (OSError, ValueError, AttributeError):
            pass  # No usable stdin; SIGINT and the desktop's owned-process cancel still work.

    def is_set(self):
        if self._cancellation.is_set():
            return True
        if self._descriptor is not None:
            try:
                # Bound each poll, including malformed/unterminated input. There
                # is no other stdin reader and no pending synchronous pipe I/O.
                for _ in range(16):
                    count = self._available()
                    if not count:
                        break
                    block = os.read(self._descriptor, min(count, 256))
                    if not block:
                        self._descriptor = None
                        break
                    self._pending += block
                    while b"\n" in self._pending:
                        line, self._pending = self._pending.split(b"\n", 1)
                        if line.strip().lower() == b"cancel":
                            self._cancellation.set()
                            return True
                    if len(self._pending) > 4096:
                        self._descriptor = None
                        break
            except (OSError, ValueError):
                self._descriptor = None
        return self._cancellation.is_set()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--output", required=True, help="New JSON file; existing destinations are refused")
    parser.add_argument("--base-dir", help="Original manifest directory for relative image paths")
    args = parser.parse_args(argv)
    cancellation = threading.Event()
    input_cancellation = _StdinCancellation(cancellation)
    previous_signal = signal.getsignal(signal.SIGINT)
    signal.signal(signal.SIGINT, lambda signum, frame: cancellation.set())
    try:
        destination = Path(args.output)
        if destination.exists() or destination.is_symlink():
            raise ValueError("Measurement output must be a new file")
        manifest = _read(args.manifest)
        def progress(value):
            sys.stderr.write(json.dumps(value, separators=(",", ":"), ensure_ascii=True) + "\n")
            sys.stderr.flush()
        report = measure_image_quality(manifest, args.base_dir or Path(args.manifest).resolve().parent,
                                       progress=progress, cancelled=input_cancellation.is_set)
        contents = json.dumps(report, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                              allow_nan=False) + "\n"
        if input_cancellation.is_set():
            raise InterruptedError("image_quality_measurement_cancelled")
        with destination.open("x", encoding="utf-8", newline="\n") as handle:
            handle.write(contents)
            handle.flush()
            os.fsync(handle.fileno())
        sys.stdout.write(contents)
        sys.stdout.flush()
        return 0
    except (InterruptedError, KeyboardInterrupt):
        sys.stderr.write('{"event":"cancelled"}\n')
        return 3
    except (OSError, ValueError, TypeError, RecursionError, ImportError) as error:
        sys.stderr.write("Image quality measurement failed: " + str(error) + "\n")
        return 2
    finally:
        signal.signal(signal.SIGINT, previous_signal)


if __name__ == "__main__":
    raise SystemExit(main())
