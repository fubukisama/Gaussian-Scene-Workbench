"""Zero-copy CUDA VMM publisher for the native training viewport.

The producer owns two slots in one exportable CUDA allocation.  The Qt
renderer imports the same Win32 allocation into OpenGL; only a small named
control mapping and named events cross the CPU boundary.
"""

import ctypes
import json
import math
import mmap
import os
import struct
import sys
import time
import uuid
from ctypes import wintypes


PROTOCOL_VERSION = 2
CONTROL_MAGIC = b"GSWGPU2\0"
CONTROL_BYTES = 4096
CONTROL_HEADER_BYTES = 156
CONTROL_TRAILING_SEQUENCE_OFFSET = 152
CONTROL_INITIALIZING = 0
CONTROL_READY = 1
CONTROL_CLOSING = 2
CONTROL_FAILED = 3
SLOT_COUNT = 2
VERTEX_FLOATS = 14
VERTEX_STRIDE_BYTES = VERTEX_FLOATS * 4
DEFAULT_CAPACITY = 1_500_000
DEFAULT_MAX_ALLOCATION_BYTES = 192 * 1024**2
MIN_ALLOCATION_BYTES = 32 * 1024**2
MIN_FREE_VRAM_BYTES = 256 * 1024**2
SH_C0 = 0.28209479177387814

WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
INFINITE = 0xFFFFFFFF
HANDLE_TYPE_OPAQUE_WIN32 = "opaque_win32"
HANDLE_TYPE_OPAQUE_WIN32_KMT = "opaque_win32_kmt"
CUDA_HANDLE_TYPES = {
    HANDLE_TYPE_OPAQUE_WIN32: 2,
    HANDLE_TYPE_OPAQUE_WIN32_KMT: 4,
}


class SecurityAttributes(ctypes.Structure):
    _fields_ = [
        ("nLength", wintypes.DWORD),
        ("lpSecurityDescriptor", wintypes.LPVOID),
        ("bInheritHandle", wintypes.BOOL),
    ]


class MemLocation(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("id", ctypes.c_int)]


class AllocationFlags(ctypes.Structure):
    _fields_ = [
        ("compressionType", ctypes.c_ubyte),
        ("gpuDirectRDMACapable", ctypes.c_ubyte),
        ("usage", ctypes.c_ushort),
        ("reserved", ctypes.c_ubyte * 4),
    ]


class MemAllocationProp(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("requestedHandleTypes", ctypes.c_int),
        ("location", MemLocation),
        ("win32HandleMetaData", ctypes.c_void_p),
        ("allocFlags", AllocationFlags),
    ]


class MemAccessDesc(ctypes.Structure):
    _fields_ = [("location", MemLocation), ("flags", ctypes.c_ulonglong)]


def _align_up(value, alignment):
    return ((int(value) + int(alignment) - 1) // int(alignment)) * int(alignment)


def _align_down(value, alignment):
    return (int(value) // int(alignment)) * int(alignment)


def compute_preview_geometry(
    free_bytes,
    requested_capacity=DEFAULT_CAPACITY,
    granularity=2 * 1024**2,
    max_allocation_bytes=DEFAULT_MAX_ALLOCATION_BYTES,
):
    """Return a bounded two-slot layout, or None when the VRAM reserve is unsafe."""
    free_bytes = max(int(free_bytes), 0)
    requested_capacity = max(int(requested_capacity), 0)
    granularity = max(int(granularity), 1)
    if free_bytes < MIN_FREE_VRAM_BYTES or requested_capacity <= 0:
        return None
    allocation_budget = min(int(max_allocation_bytes), int(free_bytes * 0.08))
    allocation_budget = _align_down(allocation_budget, granularity * SLOT_COUNT)
    if allocation_budget < MIN_ALLOCATION_BYTES:
        return None
    requested_slot_bytes = _align_up(
        requested_capacity * VERTEX_STRIDE_BYTES, granularity
    )
    slot_bytes = min(
        requested_slot_bytes,
        _align_down(allocation_budget // SLOT_COUNT, granularity),
    )
    if slot_bytes < granularity:
        return None
    capacity = min(requested_capacity, slot_bytes // VERTEX_STRIDE_BYTES)
    if capacity <= 0:
        return None
    return {
        "capacity": int(capacity),
        "slot_bytes": int(slot_bytes),
        "allocation_bytes": int(slot_bytes * SLOT_COUNT),
    }


def _object_names(session_id):
    token = "".join(character for character in session_id if character.isalnum())
    root = "Local\\GSW-GPU-{}".format(token)
    return {
        "control": root + "-control",
        "frame": root + "-frame",
        "release": (root + "-release-0", root + "-release-1"),
    }


def ready_descriptor(
    session_id,
    producer_pid,
    memory_handle,
    allocation_bytes,
    slot_bytes,
    capacity,
    device,
    memory_handle_type=HANDLE_TYPE_OPAQUE_WIN32_KMT,
    device_luid="",
    device_node_mask=0,
):
    names = _object_names(session_id)
    descriptor = {
        "version": PROTOCOL_VERSION,
        "type": "gpu_preview",
        "state": "ready",
        "sessionId": str(session_id),
        "producerPid": int(producer_pid),
        "memoryHandle": "0x{:x}".format(int(memory_handle)),
        "memoryHandleType": str(memory_handle_type),
        "allocationBytes": int(allocation_bytes),
        "slotBytes": int(slot_bytes),
        "slotCount": SLOT_COUNT,
        "strideBytes": VERTEX_STRIDE_BYTES,
        "capacity": int(capacity),
        "controlMapping": names["control"],
        "frameEvent": names["frame"],
        "releaseEvent0": names["release"][0],
        "releaseEvent1": names["release"][1],
        "device": str(device),
    }
    if device_luid:
        descriptor["deviceLuid"] = str(device_luid)
        descriptor["deviceNodeMask"] = int(device_node_mask)
    return descriptor


def state_descriptor(session_id, state, error=""):
    descriptor = {
        "version": PROTOCOL_VERSION,
        "type": "gpu_preview",
        "state": str(state),
        "sessionId": str(session_id),
    }
    if error:
        descriptor["error"] = str(error)[:1000]
    return descriptor


def emit_descriptor(descriptor):
    stream = getattr(sys, "__stdout__", None) or sys.stdout
    stream.write(
        "[gsw-training-gpu-preview] "
        + json.dumps(descriptor, ensure_ascii=False, separators=(",", ":"))
        + "\n"
    )
    stream.flush()


def build_control_block(
    sequence,
    state,
    capacity,
    slot_bytes,
    allocation_bytes,
    slot_snapshots,
):
    snapshots = list(slot_snapshots)
    if len(snapshots) != SLOT_COUNT:
        raise ValueError("GPU preview control requires exactly two slots")
    block = bytearray(CONTROL_BYTES)
    struct.pack_into(
        "<8sIIIIIIQQQ",
        block,
        0,
        CONTROL_MAGIC,
        PROTOCOL_VERSION,
        CONTROL_HEADER_BYTES,
        int(sequence),
        int(state),
        SLOT_COUNT,
        VERTEX_STRIDE_BYTES,
        int(capacity),
        int(slot_bytes),
        int(allocation_bytes),
    )
    for index, snapshot in enumerate(snapshots):
        if len(snapshot) != 8:
            raise ValueError("GPU preview slot snapshot must contain metadata and bounds")
        struct.pack_into(
            "<QQQQffff",
            block,
            56 + index * 48,
            *map(int, snapshot[:4]),
            *map(float, snapshot[4:]),
        )
    struct.pack_into("<I", block, CONTROL_TRAILING_SEQUENCE_OFFSET, int(sequence))
    return bytes(block)


class CudaDriverApi:
    """Small ctypes binding that uses the CUDA context already owned by PyTorch."""

    def __init__(self):
        if sys.platform != "win32":
            raise RuntimeError("CUDA/OpenGL shared preview currently requires Windows")
        self.driver = ctypes.WinDLL("nvcuda.dll")
        self._bind_functions()

    @staticmethod
    def _bind(dll, name, restype, argtypes):
        function = getattr(dll, name)
        function.restype = restype
        function.argtypes = argtypes
        return function

    def _bind_functions(self):
        u64_pointer = ctypes.POINTER(ctypes.c_ulonglong)
        size_pointer = ctypes.POINTER(ctypes.c_size_t)
        self.cu_init = self._bind(
            self.driver, "cuInit", ctypes.c_int, [ctypes.c_uint]
        )
        self.cu_granularity = self._bind(
            self.driver,
            "cuMemGetAllocationGranularity",
            ctypes.c_int,
            [size_pointer, ctypes.POINTER(MemAllocationProp), ctypes.c_int],
        )
        self.cu_reserve = self._bind(
            self.driver,
            "cuMemAddressReserve",
            ctypes.c_int,
            [
                u64_pointer,
                ctypes.c_size_t,
                ctypes.c_size_t,
                ctypes.c_ulonglong,
                ctypes.c_ulonglong,
            ],
        )
        self.cu_create = self._bind(
            self.driver,
            "cuMemCreate",
            ctypes.c_int,
            [
                u64_pointer,
                ctypes.c_size_t,
                ctypes.POINTER(MemAllocationProp),
                ctypes.c_ulonglong,
            ],
        )
        self.cu_map = self._bind(
            self.driver,
            "cuMemMap",
            ctypes.c_int,
            [
                ctypes.c_ulonglong,
                ctypes.c_size_t,
                ctypes.c_size_t,
                ctypes.c_ulonglong,
                ctypes.c_ulonglong,
            ],
        )
        self.cu_set_access = self._bind(
            self.driver,
            "cuMemSetAccess",
            ctypes.c_int,
            [
                ctypes.c_ulonglong,
                ctypes.c_size_t,
                ctypes.POINTER(MemAccessDesc),
                ctypes.c_size_t,
            ],
        )
        self.cu_export = self._bind(
            self.driver,
            "cuMemExportToShareableHandle",
            ctypes.c_int,
            [
                ctypes.POINTER(ctypes.c_void_p),
                ctypes.c_ulonglong,
                ctypes.c_int,
                ctypes.c_ulonglong,
            ],
        )
        self.cu_copy = self._bind(
            self.driver,
            "cuMemcpyDtoDAsync_v2",
            ctypes.c_int,
            [
                ctypes.c_ulonglong,
                ctypes.c_ulonglong,
                ctypes.c_size_t,
                ctypes.c_void_p,
            ],
        )
        self.cu_unmap = self._bind(
            self.driver,
            "cuMemUnmap",
            ctypes.c_int,
            [ctypes.c_ulonglong, ctypes.c_size_t],
        )
        self.cu_release = self._bind(
            self.driver,
            "cuMemRelease",
            ctypes.c_int,
            [ctypes.c_ulonglong],
        )
        self.cu_free_address = self._bind(
            self.driver,
            "cuMemAddressFree",
            ctypes.c_int,
            [ctypes.c_ulonglong, ctypes.c_size_t],
        )
        try:
            self.cu_device_get_luid = self._bind(
                self.driver,
                "cuDeviceGetLuid",
                ctypes.c_int,
                [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint), ctypes.c_int],
            )
        except AttributeError:
            self.cu_device_get_luid = None

    @staticmethod
    def check(result, operation):
        if result != 0:
            raise RuntimeError("{} failed with CUDA error {}".format(operation, result))

    def device_luid(self, device):
        if self.cu_device_get_luid is None:
            return "", 0
        luid = ctypes.create_string_buffer(8)
        node_mask = ctypes.c_uint()
        result = self.cu_device_get_luid(luid, ctypes.byref(node_mask), int(device))
        if result != 0:
            return "", 0
        return bytes(luid.raw).hex(), int(node_mask.value)


class GpuPreviewPublisher:
    """Owns exportable CUDA storage and publishes non-blocking snapshots."""

    def __init__(
        self,
        torch_module,
        requested_capacity=DEFAULT_CAPACITY,
        fps=15.0,
        memory_handle_type=HANDLE_TYPE_OPAQUE_WIN32_KMT,
    ):
        if sys.platform != "win32" or not torch_module.cuda.is_available():
            raise RuntimeError("CUDA/OpenGL shared preview is unavailable")
        self.torch = torch_module
        if memory_handle_type not in CUDA_HANDLE_TYPES:
            raise ValueError("Unsupported GPU preview memory handle type")
        self.memory_handle_type = memory_handle_type
        self.cuda_handle_type = CUDA_HANDLE_TYPES[memory_handle_type]
        self.session_id = str(uuid.uuid4())
        self.names = _object_names(self.session_id)
        self.api = CudaDriverApi()
        self.kernel32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)
        self._bind_kernel32()
        self.device = int(torch_module.cuda.current_device())
        # Force creation of the PyTorch primary context before CUDA driver VMM calls.
        torch_module.cuda.current_stream(self.device)
        self.api.check(self.api.cu_init(0), "cuInit")
        self.security = SecurityAttributes(
            ctypes.sizeof(SecurityAttributes), None, wintypes.BOOL(False)
        )
        self.prop = MemAllocationProp()
        self.prop.type = 1  # CU_MEM_ALLOCATION_TYPE_PINNED
        self.prop.requestedHandleTypes = self.cuda_handle_type
        self.prop.location = MemLocation(1, self.device)
        self.prop.win32HandleMetaData = (
            ctypes.addressof(self.security)
            if self.memory_handle_type == HANDLE_TYPE_OPAQUE_WIN32
            else None
        )
        granularity = ctypes.c_size_t()
        self.api.check(
            self.api.cu_granularity(ctypes.byref(granularity), ctypes.byref(self.prop), 0),
            "cuMemGetAllocationGranularity",
        )
        free_bytes, _ = self._memory_info()
        geometry = compute_preview_geometry(
            free_bytes, requested_capacity, max(int(granularity.value), 1)
        )
        if geometry is None:
            raise RuntimeError(
                "Insufficient free VRAM for the protected shared-preview reserve"
            )
        self.capacity = geometry["capacity"]
        self.slot_bytes = geometry["slot_bytes"]
        self.allocation_bytes = geometry["allocation_bytes"]
        self.address = ctypes.c_ulonglong()
        self.allocation = ctypes.c_ulonglong()
        self.exported_handle = ctypes.c_void_p()
        self._reserved = False
        self._created = False
        self._mapped = False
        self.control = None
        self.frame_event = None
        self.release_events = []
        self.pending = [None, None]
        self.slot_snapshots = [
            [0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0],
            [0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0],
        ]
        self.sequence = 0
        self.next_slot = 0
        self.generation = 0
        self.last_enqueue = 0.0
        self.minimum_interval = 1.0 / max(min(float(fps), 60.0), 1.0)
        self._sample_cache = None
        self._bounds_cache = None
        self._bounds_point_count = -1
        self._bounds_updated_at = 0.0
        self._closed = False
        try:
            self._allocate()
            self._create_control_objects()
        except Exception:
            self._destroy()
            raise

    def _bind_kernel32(self):
        self.kernel32.CreateEventW.restype = wintypes.HANDLE
        self.kernel32.CreateEventW.argtypes = [
            ctypes.POINTER(SecurityAttributes),
            wintypes.BOOL,
            wintypes.BOOL,
            wintypes.LPCWSTR,
        ]
        self.kernel32.SetEvent.restype = wintypes.BOOL
        self.kernel32.SetEvent.argtypes = [wintypes.HANDLE]
        self.kernel32.WaitForSingleObject.restype = wintypes.DWORD
        self.kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

    def _memory_info(self):
        try:
            return self.torch.cuda.mem_get_info(self.device)
        except (AttributeError, TypeError):
            properties = self.torch.cuda.get_device_properties(self.device)
            used = self.torch.cuda.memory_reserved(self.device)
            return max(int(properties.total_memory) - int(used), 0), int(
                properties.total_memory
            )

    def _allocate(self):
        self.api.check(
            self.api.cu_reserve(
                ctypes.byref(self.address), self.allocation_bytes, 0, 0, 0
            ),
            "cuMemAddressReserve",
        )
        self._reserved = True
        self.api.check(
            self.api.cu_create(
                ctypes.byref(self.allocation),
                self.allocation_bytes,
                ctypes.byref(self.prop),
                0,
            ),
            "cuMemCreate",
        )
        self._created = True
        self.api.check(
            self.api.cu_map(
                self.address.value,
                self.allocation_bytes,
                0,
                self.allocation.value,
                0,
            ),
            "cuMemMap",
        )
        self._mapped = True
        access = MemAccessDesc(MemLocation(1, self.device), 3)
        self.api.check(
            self.api.cu_set_access(
                self.address.value,
                self.allocation_bytes,
                ctypes.byref(access),
                1,
            ),
            "cuMemSetAccess",
        )
        self.api.check(
            self.api.cu_export(
                ctypes.byref(self.exported_handle),
                self.allocation.value,
                self.cuda_handle_type,
                0,
            ),
            "cuMemExportToShareableHandle",
        )

    def _create_event(self, name, initially_signaled):
        handle = self.kernel32.CreateEventW(
            None, wintypes.BOOL(False), wintypes.BOOL(initially_signaled), name
        )
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        return handle

    def _create_control_objects(self):
        self.control = mmap.mmap(
            -1, CONTROL_BYTES, tagname=self.names["control"], access=mmap.ACCESS_WRITE
        )
        self.frame_event = self._create_event(self.names["frame"], False)
        self.release_events = [
            self._create_event(name, True) for name in self.names["release"]
        ]
        self._write_control(CONTROL_READY)

    def descriptor(self):
        luid, node_mask = self.api.device_luid(self.device)
        return ready_descriptor(
            self.session_id,
            os.getpid(),
            int(self.exported_handle.value),
            self.allocation_bytes,
            self.slot_bytes,
            self.capacity,
            self.torch.cuda.get_device_name(self.device),
            self.memory_handle_type,
            luid,
            node_mask,
        )

    def _write_control(self, state):
        next_sequence = self.sequence + 2
        stable = build_control_block(
            next_sequence,
            state,
            self.capacity,
            self.slot_bytes,
            self.allocation_bytes,
            self.slot_snapshots,
        )
        odd = struct.pack("<I", next_sequence - 1)
        even = struct.pack("<I", next_sequence)
        self.control[16:20] = odd
        self.control[
            CONTROL_TRAILING_SEQUENCE_OFFSET:CONTROL_HEADER_BYTES
        ] = odd
        self.control[0:16] = stable[0:16]
        self.control[20:CONTROL_TRAILING_SEQUENCE_OFFSET] = stable[
            20:CONTROL_TRAILING_SEQUENCE_OFFSET
        ]
        self.control[
            CONTROL_TRAILING_SEQUENCE_OFFSET:CONTROL_HEADER_BYTES
        ] = even
        self.control[16:20] = even
        self.sequence = next_sequence

    def _complete_pending(self, wait=False):
        completed = False
        for slot, pending in enumerate(self.pending):
            if pending is None:
                continue
            event, packed, point_count, iteration, bounds = pending
            if wait:
                event.synchronize()
                ready = True
            else:
                ready = bool(event.query())
            if not ready:
                continue
            self.generation += 1
            self.slot_snapshots[slot] = [
                self.generation,
                int(point_count),
                int(iteration),
                time.monotonic_ns(),
                *bounds,
            ]
            self.pending[slot] = None
            # Keep the tensor alive until the recorded CUDA event completes.
            del packed
            self._write_control(CONTROL_READY)
            if not self.kernel32.SetEvent(self.frame_event):
                raise ctypes.WinError(ctypes.get_last_error())
            completed = True
        return completed

    def _acquire_slot(self):
        for offset in range(SLOT_COUNT):
            slot = (self.next_slot + offset) % SLOT_COUNT
            if self.pending[slot] is not None:
                continue
            result = self.kernel32.WaitForSingleObject(self.release_events[slot], 0)
            if result == WAIT_OBJECT_0:
                self.next_slot = (slot + 1) % SLOT_COUNT
                return slot
            if result != WAIT_TIMEOUT:
                raise ctypes.WinError(ctypes.get_last_error())
        return None

    def _sample_indices(self, point_count, device):
        key = (int(point_count), int(self.capacity), str(device))
        if self._sample_cache is None or self._sample_cache[0] != key:
            indices = (
                self.torch.arange(self.capacity, device=device, dtype=self.torch.long)
                * int(point_count)
                // int(self.capacity)
            )
            self._sample_cache = (key, indices)
        return self._sample_cache[1]

    def _pack(self, gaussians):
        xyz = gaussians.get_xyz.detach()
        point_count = int(xyz.shape[0])
        indices = None
        if point_count > self.capacity:
            indices = self._sample_indices(point_count, xyz.device)

        def select(tensor):
            tensor = tensor.detach()
            return tensor if indices is None else tensor.index_select(0, indices)

        xyz = select(xyz)
        features_dc = select(gaussians.get_features_dc)[:, 0, :]
        rgb = self.torch.clamp(features_dc * SH_C0 + 0.5, 0.0, 1.0)
        opacity = select(gaussians.get_opacity)
        scale = select(gaussians.get_scaling)
        rotation = select(gaussians.get_rotation)
        packed = self.torch.cat((xyz, rgb, opacity, scale, rotation), dim=1)
        packed = packed.to(dtype=self.torch.float32).contiguous()
        if packed.shape[1] != VERTEX_FLOATS:
            raise RuntimeError("Packed GPU preview vertex ABI is invalid")
        return packed

    def _scene_bounds(self, xyz, now):
        point_count = int(xyz.shape[0])
        refresh = (
            self._bounds_cache is None
            or self._bounds_point_count != point_count
            or now - self._bounds_updated_at >= 1.0
        )
        if not refresh:
            return self._bounds_cache
        if point_count <= 0:
            bounds = (0.0, 0.0, 0.0, 1.0)
        else:
            lower = self.torch.min(xyz, dim=0).values
            upper = self.torch.max(xyz, dim=0).values
            center = (lower + upper) * 0.5
            half_extent = (upper - lower) * 0.5
            radius = (half_extent * half_extent).sum().sqrt().reshape(1)
            values = self.torch.cat((center, radius)).detach().cpu().tolist()
            bounds = tuple(float(value) for value in values)
            if not all(math.isfinite(value) for value in bounds):
                raise RuntimeError("GPU preview scene bounds are not finite")
            bounds = bounds[:3] + (max(bounds[3], 1.0e-4),)
        self._bounds_cache = bounds
        self._bounds_point_count = point_count
        self._bounds_updated_at = now
        return bounds

    def publish(self, gaussians, iteration):
        if self._closed:
            return False
        self._complete_pending(False)
        now = time.monotonic()
        if now - self.last_enqueue < self.minimum_interval:
            return False
        slot = self._acquire_slot()
        if slot is None:
            return False
        try:
            packed = self._pack(gaussians)
            bounds = self._scene_bounds(packed[:, :3], now)
            byte_count = int(packed.numel() * packed.element_size())
            if byte_count > self.slot_bytes:
                raise RuntimeError("Packed GPU preview exceeds its slot")
            stream = self.torch.cuda.current_stream(self.device)
            destination = self.address.value + slot * self.slot_bytes
            self.api.check(
                self.api.cu_copy(
                    destination,
                    packed.data_ptr(),
                    byte_count,
                    ctypes.c_void_p(stream.cuda_stream),
                ),
                "cuMemcpyDtoDAsync(shared preview)",
            )
            event = self.torch.cuda.Event(enable_timing=False, blocking=False)
            event.record(stream)
            self.pending[slot] = (
                event,
                packed,
                int(packed.shape[0]),
                int(iteration),
                bounds,
            )
            self.last_enqueue = now
            return True
        except Exception:
            self.kernel32.SetEvent(self.release_events[slot])
            raise

    def close(self, detach_timeout=1.5):
        if self._closed:
            return
        try:
            self._complete_pending(True)
            self._write_control(CONTROL_CLOSING)
            self.kernel32.SetEvent(self.frame_event)
            deadline = time.monotonic() + max(float(detach_timeout), 0.0)
            for handle in self.release_events:
                remaining = max(deadline - time.monotonic(), 0.0)
                self.kernel32.WaitForSingleObject(handle, int(remaining * 1000))
        finally:
            self._closed = True
            self._destroy()

    def _destroy(self):
        for handle in self.release_events:
            if handle:
                self.kernel32.CloseHandle(handle)
        self.release_events = []
        if self.frame_event:
            self.kernel32.CloseHandle(self.frame_event)
            self.frame_event = None
        if self.control is not None:
            self.control.close()
            self.control = None
        if (
            self.memory_handle_type == HANDLE_TYPE_OPAQUE_WIN32
            and self.exported_handle.value
        ):
            self.kernel32.CloseHandle(self.exported_handle)
        self.exported_handle = ctypes.c_void_p()
        if self._mapped:
            self.api.check(
                self.api.cu_unmap(self.address.value, self.allocation_bytes),
                "cuMemUnmap",
            )
            self._mapped = False
        if self._created:
            self.api.check(self.api.cu_release(self.allocation.value), "cuMemRelease")
            self._created = False
        if self._reserved:
            self.api.check(
                self.api.cu_free_address(self.address.value, self.allocation_bytes),
                "cuMemAddressFree",
            )
            self._reserved = False


def create_publisher(torch_module, requested_capacity=DEFAULT_CAPACITY, fps=15.0):
    """Create the publisher without making preview support fatal to training."""
    session_id = str(uuid.uuid4())
    try:
        publisher = GpuPreviewPublisher(torch_module, requested_capacity, fps)
        return publisher, None
    except Exception as exc:  # preview capability is optional; training must continue
        return None, state_descriptor(session_id, "failed", str(exc))
