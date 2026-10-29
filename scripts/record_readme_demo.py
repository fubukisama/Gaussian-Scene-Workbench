"""Remote-only real input/capture driver; no application instrumentation.

Input demo_manifest.json schema: gsw.readme_training_demo/1, dataset_identity,
training_provenance_path, training_provenance_sha256, and snapshots containing
backend (3dgs/2dgs), kind (initialization/training/final), iteration, path,
sha256, elapsed_seconds. Paths are relative to the extracted training root.
The training provenance is retained verbatim; this driver cannot independently
certify that an upstream training run occurred. Root's provenance review is a
separate gate. This driver proves actual native import/navigation/edit/export.

FFmpeg's gdigrab desktop-region input is documented at
https://ffmpeg.org/ffmpeg-devices.html#gdigrab . The fixed ROI is bound to the
owned main HWND client rectangle and includes separate owned modal dialogs.
UIA reads the actual application;
all interactions use Win32 SendInput, never QAction::trigger or Qt test events.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import threading
import time
import traceback


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def read_json(path: Path) -> dict:
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('JSON exceeds bounded read limit')
    value = json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(value, dict):
        raise ValueError('JSON object required')
    return value


def child(root: Path, value: object) -> Path:
    if (not isinstance(value, str) or not value or '\\' in value or ':' in value
            or PurePosixPath(value).is_absolute()
            or any(p in ('', '.', '..') for p in value.split('/'))):
        raise ValueError(f'Unsafe relative training path: {value!r}')
    path = (root / value).resolve(strict=True)
    path.relative_to(root.resolve())
    if not path.is_file() or path.is_symlink():
        raise ValueError('Regular input file required')
    return path


def ply_identity(path: Path) -> dict:
    with path.open('rb') as stream:
        lines = []
        for _ in range(1024):
            row = stream.readline(4096)
            if not row or len(row) == 4096:
                raise ValueError('Invalid PLY header')
            lines.append(row.decode('ascii').strip())
            if lines[-1] == 'end_header':
                break
        else:
            raise ValueError('PLY header exceeds limit')
        if lines[0] != 'ply':
            raise ValueError('PLY marker missing')
        vertex = next((int(x.split()[2]) for x in lines if x.startswith('element vertex ')), 0)
        if vertex < 1:
            raise ValueError('A nonempty PLY is required')
        properties = [x for x in lines if x.startswith('property ')]
        payload = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            payload.update(block)
    return {'vertex_count': vertex, 'properties': properties, 'payload_sha256': payload.hexdigest(),
            'sha256': digest(path), 'bytes': path.stat().st_size}


def validate_bundle(root: Path) -> tuple[dict, list[dict]]:
    manifest = read_json(root / 'demo_manifest.json')
    if manifest.get('schema') != 'gsw.readme_training_demo/1' or not manifest.get('dataset_identity'):
        raise ValueError('Training demo schema/dataset identity missing')
    provenance = child(root, manifest.get('training_provenance_path'))
    if digest(provenance) != manifest.get('training_provenance_sha256'):
        raise ValueError('Training provenance digest mismatch')
    snapshots = manifest.get('snapshots')
    if not isinstance(snapshots, list) or not 6 <= len(snapshots) <= 32:
        raise ValueError('Both backends require initialization, training and final snapshots')
    for item in snapshots:
        if (not isinstance(item, dict) or item.get('backend') not in ('3dgs', '2dgs')
                or item.get('kind') not in ('initialization', 'training', 'final')
                or type(item.get('iteration')) is not int or item['iteration'] < 0
                or not isinstance(item.get('elapsed_seconds'), (int, float))
                or not math.isfinite(item['elapsed_seconds']) or item['elapsed_seconds'] < 0
                or not re.fullmatch('[0-9a-f]{64}', item.get('sha256', ''))):
            raise ValueError('Invalid training snapshot metadata')
        path = child(root, item.get('path'))
        if digest(path) != item['sha256']:
            raise ValueError(f'Training snapshot digest mismatch: {item["path"]}')
        item['_path'] = path
        item['_ply'] = ply_identity(path)
    selected = []
    for backend in ('3dgs', '2dgs'):
        rows = sorted((x for x in snapshots if x['backend'] == backend), key=lambda x: x['iteration'])
        initial = next((x for x in rows if x['kind'] == 'initialization'), None)
        training = [x for x in rows if x['kind'] == 'training']
        final = next((x for x in reversed(rows) if x['kind'] == 'final'), None)
        if not initial or not training or not final:
            raise ValueError(f'{backend} capability matrix incomplete')
        middle = training[len(training) // 2]
        if not initial['iteration'] < middle['iteration'] < final['iteration']:
            raise ValueError('Replay iterations must strictly increase independently for each backend')
        if not initial['elapsed_seconds'] <= middle['elapsed_seconds'] <= final['elapsed_seconds']:
            raise ValueError('Training snapshot elapsed time is not monotone')
        selected.extend([initial, middle, final])
    return manifest, selected


def json_public(value):
    return {k: v for k, v in value.items() if not k.startswith('_')}


def qt_id_matches(actual: str, leaf: str) -> bool:
    """Qt UIA may expose an objectName with a dotted ancestor prefix."""
    return actual == leaf or actual.endswith('.' + leaf)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package-root', type=Path, required=True)
    parser.add_argument('--training-root', type=Path, required=True)
    parser.add_argument('--package-receipt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ffmpeg', type=Path, required=True)
    args = parser.parse_args()
    # Enforce this before importing any desktop library, inspecting windows,
    # changing settings or starting a process. Never spoof these variables.
    if (sys.platform != 'win32' or os.environ.get('GITHUB_ACTIONS') != 'true'
            or os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted'):
        parser.error('Only disposable GitHub-hosted Windows runners may use desktop input/capture')
    import winreg
    from pywinauto import Desktop
    from PIL import ImageStat

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    events = []
    evidence = {'schema': 'gsw.readme_operation_capture/1', 'status': 'running',
                'training_visualization': 'ACTUAL TRAINING SNAPSHOT REPLAY',
                'live_remote_optimization': False, 'video_speed': 1.0,
                'frame_interpolation': False, 'synthetic_ui': False,
                'viewer_auto_framing_applied': True,
                'matched_camera_quality_comparison': False,
                'replay_view_policy': 'Native Home auto-framing after each import; demonstration views are not camera-matched quality evidence',
                'input_method': 'Win32 SendInput', 'observation_method': 'Windows UI Automation',
                'runner': {k: os.environ.get(k) for k in ('GITHUB_REPOSITORY', 'GITHUB_RUN_ID', 'GITHUB_SHA', 'RUNNER_ENVIRONMENT')},
                'events': events, 'acceptance': {}}
    app = recorder = None
    video_started = None
    app_log = ffmpeg_log = None
    hwnd = 0
    guard_stop = threading.Event()
    guard_errors = []
    guard_thread = None
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindow.argtypes = [wintypes.HWND]
    user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
    user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
    user32.GetAncestor.restype = wintypes.HWND
    enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [enum_callback, wintypes.LPARAM]
    dwmapi = ctypes.WinDLL('dwmapi', use_last_error=True)
    dwmapi.DwmGetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
    kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    ulong_ptr = wintypes.WPARAM

    class Mouse(ctypes.Structure):
        _fields_ = [('dx', wintypes.LONG), ('dy', wintypes.LONG), ('data', wintypes.DWORD),
                    ('flags', wintypes.DWORD), ('time', wintypes.DWORD), ('extra', ulong_ptr)]

    class Key(ctypes.Structure):
        _fields_ = [('vk', wintypes.WORD), ('scan', wintypes.WORD), ('flags', wintypes.DWORD),
                    ('time', wintypes.DWORD), ('extra', ulong_ptr)]

    class Payload(ctypes.Union):
        _fields_ = [('mouse', Mouse), ('key', Key)]

    class Input(ctypes.Structure):
        _fields_ = [('type', wintypes.DWORD), ('payload', Payload)]

    user32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]
    user32.SendInput.restype = wintypes.UINT
    desktop = Desktop(backend='uia')

    def log(action, **detail):
        item = {'action': action, 'wall_seconds': round(time.monotonic() - started, 3), **detail}
        if video_started is not None:
            item['video_seconds'] = round(time.monotonic() - video_started, 3)
        events.append(item)
        (out / 'recording-evidence.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
        print(json.dumps(item), flush=True)

    def wait_for(predicate, label, timeout=15):
        deadline = time.monotonic() + timeout
        last_error = None
        while time.monotonic() < deadline:
            if guard_errors:
                raise RuntimeError(f'Owned capture region guard failed: {guard_errors[0]}')
            if app and app.poll() is not None:
                raise RuntimeError(f'Owned application exited during {label}: {app.returncode}')
            if recorder and recorder.poll() is not None:
                raise RuntimeError(f'FFmpeg exited during {label}: {recorder.returncode}')
            if video_started and time.monotonic() - video_started > 95:
                raise RuntimeError('Recording exceeded the bounded 95-second input budget')
            try:
                result = predicate()
                if result:
                    return result
            except Exception as error:
                last_error = repr(error)
            time.sleep(0.12)
        raise RuntimeError(f'Observation failed: {label}; last_error={last_error}')

    def owned(handle):
        process_id = wintypes.DWORD()
        user32.GetWindowThreadProcessId(handle, ctypes.byref(process_id))
        return bool(user32.IsWindow(handle)) and app is not None and process_id.value == app.pid

    def require_foreground():
        if guard_errors:
            raise RuntimeError(f'Owned capture region guard failed: {guard_errors[0]}')
        if not owned(user32.GetForegroundWindow()):
            raise RuntimeError('Foreground window is not owned by the exact launched native PID; input aborted')

    def send(packet):
        require_foreground()
        if user32.SendInput(1, ctypes.byref(packet), ctypes.sizeof(Input)) != 1:
            raise ctypes.WinError(ctypes.get_last_error())

    def key(vk, up=False):
        send(Input(1, Payload(key=Key(vk, 0, 2 if up else 0, 0, 0))))

    def press(vk):
        key(vk)
        time.sleep(0.06)
        key(vk, True)

    def chord(*keys):
        for vk in keys:
            key(vk)
        time.sleep(0.07)
        for vk in reversed(keys):
            key(vk, True)
        time.sleep(0.15)

    def text(value):
        # Unicode keyboard input: do not use clipboard, WM_SETTEXT, UIA SetValue
        # or direct edits of application controls.
        encoded = value.encode('utf-16-le')
        for offset in range(0, len(encoded), 2):
            code = int.from_bytes(encoded[offset:offset + 2], 'little')
            for flags in (4, 6):
                send(Input(1, Payload(key=Key(0, code, flags, 0, 0))))

    def move(x, y):
        width, height = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
        send(Input(0, Payload(mouse=Mouse(round(x * 65535 / (width - 1)),
                                        round(y * 65535 / (height - 1)), 0, 0x8001, 0, 0))))

    def mouse(flags, data=0):
        send(Input(0, Payload(mouse=Mouse(0, 0, data & 0xffffffff, flags, 0, 0))))

    def click(control):
        if control.element_info.process_id != app.pid or not control.is_visible() or not control.is_enabled():
            raise RuntimeError('Refusing to click an unowned, hidden or disabled control')
        rect = control.rectangle()
        move((rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2)
        mouse(2)
        time.sleep(0.07)
        mouse(4)
        time.sleep(0.15)

    def windows():
        return [w for w in desktop.windows(process=app.pid) if w.is_visible() and owned(w.handle)]

    def find(auto_id=None, title=None, control_type=None, scope=None, timeout=15):
        def locate():
            matches = []
            for window in ([scope] if scope else windows()):
                candidates = [window] + window.descendants()
                for item in candidates:
                    info = item.element_info
                    if (info.process_id != app.pid or not item.is_visible()
                            or (auto_id and not qt_id_matches(info.automation_id, auto_id))
                            or (title and info.name.replace('&', '') != title)
                            or (control_type and info.control_type != control_type)):
                        continue
                    matches.append(item)
            if len(matches) == 1:
                return matches[0]
            return None
        return wait_for(locate, f'UIA {auto_id or title}', timeout)

    def save_project():
        old = project.stat().st_mtime_ns
        chord(0x11, ord('S'))
        wait_for(lambda: project.stat().st_mtime_ns != old, 'ordinary Ctrl+S writes project')
        return read_json(project)

    def active_state(project_data):
        collection = project_data.get('sceneCollection', {})
        return next(x for x in collection.get('objects', []) if x.get('id') == collection.get('activeId'))

    def editing_lock_state():
        # Observe the real UIA Toggle pattern. Never infer the current state
        # from a setting, button icon, expected default or previous keystroke.
        control = find(title='Lock Editing Tools', scope=main_window)
        state = int(control.iface_toggle.CurrentToggleState)
        if state not in (0, 1):
            raise RuntimeError(f'Lock Editing Tools exposes an indeterminate UIA Toggle state: {state}')
        return control, state == 1

    def capture_region_guard(region):
        """Read-only Win32 guard; it never sends input or activates a window."""
        foreign_since = None
        while not guard_stop.is_set():
            try:
                foreground = user32.GetForegroundWindow()
                if not owned(foreground):
                    foreign_since = foreign_since or time.monotonic()
                    # Windows can briefly report HWND=0 during activation.
                    # A foreign foreground, however, invalidates this take.
                    if foreground or time.monotonic() - foreign_since > .15:
                        raise RuntimeError(f'Foreground HWND {int(foreground or 0)} is not the owned native PID')
                else:
                    foreign_since = None
                origin = wintypes.POINT(0, 0)
                size = wintypes.RECT()
                user32.ClientToScreen(hwnd, ctypes.byref(origin))
                user32.GetClientRect(hwnd, ctypes.byref(size))
                if [origin.x, origin.y, size.right, size.bottom] != region:
                    raise RuntimeError('Main client moved/resized outside the fixed continuous recording region')
                problems = []

                @enum_callback
                def inspect_window(handle, _):
                    if handle == hwnd:
                        return False  # EnumWindows is top-to-bottom Z order.
                    if not user32.IsWindowVisible(handle):
                        return True
                    cloaked = wintypes.DWORD()
                    if dwmapi.DwmGetWindowAttribute(handle, 14, ctypes.byref(cloaked), ctypes.sizeof(cloaked)) == 0 and cloaked.value:
                        return True
                    rect = wintypes.RECT()
                    if not user32.GetWindowRect(handle, ctypes.byref(rect)) or rect.right <= rect.left or rect.bottom <= rect.top:
                        return True
                    left, top, width, height = region
                    overlaps = rect.right > left and rect.left < left + width and rect.bottom > top and rect.top < top + height
                    if not overlaps:
                        return True
                    if not owned(handle):
                        problems.append(f'Foreign visible HWND {int(handle)} occludes the recording ROI')
                    elif rect.left < left or rect.top < top or rect.right > left + width or rect.bottom > top + height:
                        problems.append(f'Owned modal/popup HWND {int(handle)} extends beyond the recording ROI')
                    return True

                user32.EnumWindows(inspect_window, 0)
                if problems:
                    raise RuntimeError('; '.join(problems))
            except Exception as error:
                guard_errors.append({'message': str(error), 'wall_seconds': round(time.monotonic() - started, 3)})
                return
            guard_stop.wait(.05)

    def ensure_editing_lock(locked, reason):
        control, before = editing_lock_state()
        if before != locked:
            click(control)
            wait_for(lambda: editing_lock_state()[1] == locked,
                     f'observed editing lock becomes {locked}')
        _, after = editing_lock_state()
        if after != locked:
            raise RuntimeError('Actual editing lock did not reach the required state')
        log('editing_lock_observed', before=before, locked=after,
            input_applied=before != locked, reason=reason,
            observation='actual UIA Toggle.CurrentToggleState')

    def screenshot(label):
        image = desktop.window(handle=hwnd).capture_as_image()
        image.save(out / f'{label}.png')
        return image

    def save_uia_tree(label):
        rows = []
        for window in windows():
            for control in [window] + window.descendants()[:2500]:
                try:
                    info = control.element_info
                    rect = control.rectangle()
                    rows.append({'name': info.name, 'automation_id': info.automation_id,
                        'control_type': info.control_type, 'process_id': info.process_id,
                        'visible': control.is_visible(), 'enabled': control.is_enabled(),
                        'rectangle': [rect.left, rect.top, rect.right, rect.bottom]})
                except Exception:
                    continue
        (out / f'{label}-uia-tree.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')

    def prove_viewport(label):
        viewport = find(auto_id='nativeViewport', scope=main_window)
        rect = viewport.rectangle()
        image = viewport.capture_as_image().convert('RGB')
        image.save(out / f'{label}-viewport.png')
        # More than the grid and overlay need visual review; this bounded signal
        # catches blank/black captures, not reconstruction quality or accuracy.
        variance = sum(ImageStat.Stat(image).var)
        if rect.width() < 600 or rect.height() < 400 or variance < 15:
            raise RuntimeError('Native viewport is undersized or capture is blank')
        log('viewport_observed', stage=label, variance=variance, size=list(image.size))
        return rect

    def select_model(snapshot):
        tree = find(auto_id='projectTree', control_type='Tree', scope=main_window)
        row = find(title=snapshot['_path'].name, control_type='TreeItem', scope=tree)
        click(row)
        wait_for(lambda: row.is_selected(), 'actual project-tree model selection')
        log('model_selected', path=snapshot['path'])

    def verify_active_snapshot(project_data, snapshot, reason):
        state = active_state(project_data)
        source = Path(state['path'])
        if not source.is_absolute():
            source = project.parent / source
        if source.resolve() != snapshot['_path'] or state.get('visible') is not True:
            raise RuntimeError('Saved active native model is not the expected visible training snapshot')
        log('saved_active_model_verified', path=snapshot['path'], active_id=state['id'],
            visible=state['visible'], reason=reason)
        return state

    def import_snapshot(snapshot):
        ensure_editing_lock(True, 'snapshot replay/import and subsequent auto-framing')
        click(find(title='Import Model (PLY / SPZ)', control_type='Button', scope=main_window))
        dialog = find(title='Import Model (PLY / SPZ)', control_type='Window')
        filename = find(auto_id='fileNameEdit', scope=dialog)
        click(filename)
        chord(0x11, ord('A'))
        text(str(snapshot['_path']))
        press(0x0D)
        choice = find(auto_id='sceneImportChoiceDialog')
        click(find(auto_id='replaceSceneButton', scope=choice))
        wait_for(lambda: not any(qt_id_matches(w.element_info.automation_id, 'sceneImportChoiceDialog') for w in windows()),
                 'replace-current dialog closes')
        select_model(snapshot)
        verify_active_snapshot(save_project(), snapshot, 'actual import and tree selection')
        press(0x24)  # Home: actual public reset-camera shortcut.
        time.sleep(0.7)
        prove_viewport(f'{snapshot["backend"]}-{snapshot["kind"]}')
        log('actual_training_snapshot_loaded', **json_public(snapshot),
            label='ACTUAL TRAINING SNAPSHOT REPLAY; no remote optimization',
            viewer_auto_framing_applied=True, matched_camera_quality_comparison=False)
        time.sleep(1.2)

    def drag(rect, button, dx, dy, seconds):
        ensure_editing_lock(True, f'paced {button}-button camera navigation, never object manipulation')
        x, y = rect.left + int(rect.width() * 0.55), rect.top + int(rect.height() * 0.56)
        move(x, y)
        mouse(2 if button == 'left' else 0x20)
        steps = max(10, int(seconds * 30))
        begin = time.monotonic()
        for step in range(1, steps + 1):
            t = step / steps
            smooth = t * t * (3 - 2 * t)
            move(x + round(dx * smooth), y + round(dy * smooth))
            time.sleep(max(0, begin + step * seconds / steps - time.monotonic()))
        mouse(4 if button == 'left' else 0x40)
        log('actual_pointer_drag', button=button, dx=dx, dy=dy, paced_seconds=seconds)

    def export_ply(snapshot):
        target = out / f'{snapshot["backend"]}-native-ui-export.ply'
        chord(0x11, ord('E'))
        dialog = find(auto_id='modelExportDialog')
        path_control = find(auto_id='modelExportPath', scope=dialog)
        # Choose the first/original coordinate entry explicitly. No invented
        # units and no claim that a transformed preview is optimizer resume.
        coordinates = find(auto_id='modelExportCoordinates', scope=dialog)
        click(coordinates)
        press(0x24)
        press(0x0D)
        click(path_control)
        chord(0x11, ord('A'))
        text(str(target))
        screenshot(f'{snapshot["backend"]}-actual-export-dialog')
        log('actual_export_dialog_observed', output=target.name, coordinate_choice='first/original')
        time.sleep(1.0)
        click(find(auto_id='confirmModelExport', scope=dialog))
        wait_for(lambda: target.is_file() and target.stat().st_size > 128, 'real native export output')
        wait_for(lambda: not any(qt_id_matches(w.element_info.automation_id, 'modelExportDialog') for w in windows()),
                 'real export dialog closes')
        last = [None, time.monotonic()]
        def output_settled():
            identity = (target.stat().st_size, target.stat().st_mtime_ns)
            if identity != last[0]:
                last[:] = [identity, time.monotonic()]
            return time.monotonic() - last[1] >= 0.6
        wait_for(output_settled, 'native export completes and output settles')
        actual = ply_identity(target)
        source = snapshot['_ply']
        if any(actual[k] != source[k] for k in ('vertex_count', 'properties', 'payload_sha256')):
            raise RuntimeError('Original-coordinate native PLY export does not preserve source Gaussian payload')
        evidence['acceptance'][f'{snapshot["backend"]}_export'] = actual
        log('actual_native_export_verified', output=target.name, **actual)

    try:
        manifest, snapshots = validate_bundle(args.training_root.resolve())
        receipt = read_json(args.package_receipt)
        exe = args.package_root.resolve() / 'bin/Gaussian Scene Workbench.exe'
        if digest(exe) != receipt.get('executableSha256'):
            raise ValueError('Launched EXE digest differs from the verified native package receipt')
        evidence['package'] = receipt
        evidence['training_manifest_sha256'] = digest(args.training_root / 'demo_manifest.json')
        evidence['training_provenance'] = {'path': manifest['training_provenance_path'],
                                           'sha256': manifest['training_provenance_sha256']}
        evidence['dataset_identity'] = manifest['dataset_identity']
        ffmpeg_version = subprocess.run([str(args.ffmpeg), '-version'], capture_output=True,
                                        timeout=10, check=True)
        evidence['ffmpeg'] = {'sha256': digest(args.ffmpeg),
                             'version': ffmpeg_version.stdout.decode('utf-8', errors='replace').splitlines()[0]}
        evidence['replayed_snapshots'] = [json_public(x) for x in snapshots]
        (out / 'demo_manifest.json').write_bytes((args.training_root / 'demo_manifest.json').read_bytes())
        (out / 'training-provenance.json').write_bytes(child(args.training_root, manifest['training_provenance_path']).read_bytes())
        registry = r'Software\GaussianSceneWorkbench\Gaussian Scene Workbench'
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry):
                raise RuntimeError('GSW settings already exist; this is not a fresh disposable runner')
        except FileNotFoundError:
            pass
        for group, name, value, kind in [('ui', 'scaleMode', 'manual', winreg.REG_SZ),
                                        ('ui', 'manualScalePercent', 100, winreg.REG_DWORD)]:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, registry + '\\' + group) as handle:
                winreg.SetValueEx(handle, name, 0, kind, value)
        project = out / 'Actual Training Snapshot Replay.gsw.json'
        # Same public native project schema as generate_gaussian_visual_fixture.ps1.
        # It opens an actual input snapshot; no training state/progress is fabricated.
        project.write_text(json.dumps({'schemaVersion': 1, 'application': 'Gaussian Scene Workbench',
            'projectName': 'ACTUAL TRAINING SNAPSHOT REPLAY', 'rootPath': '.', 'datasetPath': '',
            'scenePath': str(snapshots[0]['_path'])}, indent=2), encoding='utf-8')
        env = os.environ.copy()
        env.update({'GSW_USE_NON_NATIVE_DIALOGS': '1', 'QT_SCALE_FACTOR': '1', 'QT_FONT_DPI': '96',
                    'QT_FORCE_STDERR_LOGGING': '1'})
        app_log = (out / 'native-process.log').open('wb')
        app = subprocess.Popen([str(exe), '--language', 'en_US', '--theme', 'dark', '--project', str(project)],
                               cwd=exe.parent, env=env, stdout=app_log, stderr=subprocess.STDOUT)
        buffer = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(buffer))
        if (not kernel32.QueryFullProcessImageNameW(wintypes.HANDLE(int(app._handle)), 0, buffer, ctypes.byref(length))
                or Path(buffer.value).resolve() != exe.resolve()):
            raise RuntimeError('Exact launched PID executable path could not be verified')
        main_window = find(auto_id='mainWindow', control_type='Window', timeout=30)
        hwnd = main_window.handle
        save_uia_tree('initial')
        user32.SetForegroundWindow(hwnd)
        wait_for(lambda: owned(user32.GetForegroundWindow()), 'owned foreground')
        rect = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        outer = main_window.rectangle()
        width = 1600 + outer.width() - rect.right
        height = 936 + outer.height() - rect.bottom
        if user32.GetSystemMetrics(0) < width + 16 or user32.GetSystemMetrics(1) < height + 16:
            raise RuntimeError('Real display is too small for an unscaled 1600x936 owned client capture')
        if not user32.SetWindowPos(hwnd, 0, 8, 8, width, height, 0x0040):
            raise ctypes.WinError(ctypes.get_last_error())
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        if (rect.right, rect.bottom) != (1600, 936):
            raise RuntimeError('Native client dimensions differ from 1600x936; no resize filter permitted')
        evidence['window'] = {'pid': app.pid, 'hwnd': int(hwnd), 'client_size': [1600, 936], 'exe': str(exe)}
        ensure_editing_lock(True, 'initial camera reset and all initial orbit/pan/zoom')
        press(0x24)
        log('viewer_auto_framing_applied', shortcut='Home', matched_camera_quality_comparison=False)
        time.sleep(1)
        select_model(snapshots[0])
        viewport_rect = prove_viewport('initial-3dgs')
        client_origin = wintypes.POINT(0, 0)
        if not user32.ClientToScreen(hwnd, ctypes.byref(client_origin)):
            raise ctypes.WinError(ctypes.get_last_error())
        evidence['viewport_client_rectangle'] = [viewport_rect.left - client_origin.x,
            viewport_rect.top - client_origin.y, viewport_rect.right - client_origin.x,
            viewport_rect.bottom - client_origin.y]
        recording_region = [client_origin.x, client_origin.y, 1600, 936]
        evidence['capture_method'] = 'one continuous FFmpeg gdigrab desktop ROI matching the owned main HWND client rectangle'
        evidence['capture_region_screen_xywh'] = recording_region
        evidence['capture_region_guard'] = {'interval_seconds': .05, 'exact_owned_foreground_pid': app.pid,
            'reject_foreign_occlusion': True, 'owned_dialogs_must_fit_roi': True,
            'separate_dialog_screenshots_inserted_into_video': False}
        guard_thread = threading.Thread(target=capture_region_guard, args=(recording_region,),
                                        name='owned-native-capture-roi-guard', daemon=True)
        guard_thread.start()
        ffmpeg_log = (out / 'ffmpeg-capture.log').open('wb')
        capture_command = [str(args.ffmpeg), '-hide_banner', '-y', '-f', 'gdigrab', '-framerate', '20',
                           '-draw_mouse', '1', '-offset_x', str(client_origin.x), '-offset_y', str(client_origin.y),
                           '-video_size', '1600x936', '-i', 'desktop', '-an', '-c:v', 'libx264',
                           '-preset', 'veryfast', '-crf', '18', '-pix_fmt', 'yuv420p', '-fps_mode', 'passthrough',
                           '-movflags', '+faststart', str(out / 'raw-continuous-operation.mp4')]
        evidence['ffmpeg_command'] = capture_command
        recorder = subprocess.Popen(capture_command, stdin=subprocess.PIPE, stdout=ffmpeg_log, stderr=subprocess.STDOUT)
        video_started = time.monotonic()
        time.sleep(1)
        if recorder.poll() is not None:
            raise RuntimeError('FFmpeg could not start the guarded native-client desktop-region capture')
        log('capture_started', snapshot=json_public(snapshots[0]), label='ACTUAL TRAINING SNAPSHOT REPLAY')
        # Ordinary public navigation, at real wall-clock speed.
        drag(viewport_rect, 'left', 140, -45, 3.0)
        drag(viewport_rect, 'middle', -45, 18, 1.4)
        ensure_editing_lock(True, 'paced wheel camera zoom')
        move(viewport_rect.left + viewport_rect.width() // 2, viewport_rect.top + viewport_rect.height() // 2)
        for delta in (120, 120, -120):
            mouse(0x800, delta)
            time.sleep(0.4)
        log('actual_wheel_zoom', deltas=[120, 120, -120])
        for snapshot in snapshots[1:]:
            import_snapshot(snapshot)
            if snapshot['kind'] == 'final':
                export_ply(snapshot)
        final = snapshots[-1]
        select_model(final)
        viewport_rect = prove_viewport('final-2dgs')
        drag(viewport_rect, 'left', -125, 30, 3.0)
        drag(viewport_rect, 'middle', 40, -12, 1.2)
        # Save/read the application-owned project before and after actual keys.
        ensure_editing_lock(False, 'only the explicit numeric model-edit demonstration')
        select_model(final)
        before = verify_active_snapshot(save_project(), final,
            'verified visible tree selection and saved active identity immediately before G')['transform']
        # Do not click an arbitrary viewport center or use F to repair a lost
        # selection. The saved active source + selected real tree row are the
        # gate. The public G QAction starts the transform and focuses the real
        # viewport itself (NativeViewport::beginModelTransform).
        for code in (ord('G'), ord('X'), ord('1'), 0x0D):
            press(code)
            time.sleep(0.3)
        changed = active_state(save_project())['transform']
        expected = list(before['translation'])
        expected[0] += 1
        if any(abs(a - b) > 1e-4 for a, b in zip(changed['translation'], expected)):
            raise RuntimeError('Actual G X 1 Enter did not persist exactly +1 source unit on X')
        if changed.get('rotation') != before.get('rotation') or changed.get('scale') != before.get('scale'):
            raise RuntimeError('Numeric move unexpectedly changed rotation or scale')
        screenshot('actual-numeric-transform')
        log('actual_numeric_transform_verified', keys='G X 1 Enter', before=before, after=changed,
            units='source units; not a physical accuracy claim')
        time.sleep(1.2)
        chord(0x11, ord('Z'))
        restored = active_state(save_project())['transform']
        if restored != before:
            raise RuntimeError('Actual Ctrl+Z did not restore the saved transform')
        evidence['acceptance']['transform_and_undo'] = {'before': before, 'changed': changed, 'restored': restored}
        log('actual_undo_verified')
        screenshot('actual-undo')
        ensure_editing_lock(True, 'return to camera-only observation after verified undo')
        while time.monotonic() - video_started < 64:
            require_foreground()
            time.sleep(0.25)
        elapsed = time.monotonic() - video_started
        if elapsed > 90:
            raise RuntimeError(f'Actual operation exceeded the 60-90 second target: {elapsed:.2f}s')
        log('capture_operations_complete')
        evidence['status'] = 'passed'
    except Exception as error:
        evidence['status'] = 'failed'
        evidence['error'] = str(error)
        (out / 'driver-error.log').write_text(traceback.format_exc(), encoding='utf-8')
        try:
            if app:
                save_uia_tree('failure')
            if hwnd and owned(hwnd):
                screenshot('failure-owned-window')
        except Exception:
            pass
    finally:
        if recorder:
            try:
                recorder.communicate(input=b'q\n', timeout=15)
            except subprocess.TimeoutExpired:
                recorder.terminate()
                recorder.wait(timeout=10)
            evidence['ffmpeg_exit_code'] = recorder.returncode
            if video_started is not None:
                evidence['capture_wall_seconds'] = round(time.monotonic() - video_started, 3)
                video_started = None
            if recorder.returncode:
                evidence['status'] = 'failed'
        guard_stop.set()
        if guard_thread:
            guard_thread.join(timeout=2)
        evidence['capture_region_guard_errors'] = guard_errors
        if guard_errors:
            evidence['status'] = 'failed'
        if ffmpeg_log:
            ffmpeg_log.close()
        if app:
            # Only this disposable process is stopped, never other sessions.
            app.terminate()
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait(timeout=5)
        if app_log:
            app_log.close()
        raw = out / 'raw-continuous-operation.mp4'
        if raw.is_file():
            evidence['raw_video'] = {'path': raw.name, 'bytes': raw.stat().st_size, 'sha256': digest(raw)}
            # Decode the actual recorded stream, not a screenshot sequence.
            # Sampling/scaling is QA-only and never alters the retained video.
            try:
                decoded = subprocess.run([str(args.ffmpeg), '-hide_banner', '-i', str(raw),
                    '-vf', 'fps=1,scale=160:94', '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'],
                    capture_output=True, timeout=45, check=True)
                (out / 'ffmpeg-decode-verification.log').write_bytes(decoded.stderr)
                frame_bytes = 160 * 94 * 3
                if len(decoded.stdout) % frame_bytes:
                    raise RuntimeError('Decoded sample has an incomplete video frame')
                frames = [decoded.stdout[i:i + frame_bytes] for i in range(0, len(decoded.stdout), frame_bytes)]
                unique = len({hashlib.sha256(x).hexdigest() for x in frames})
                duration_match = re.search(rb'Duration: (\d+):(\d+):(\d+\.\d+)', decoded.stderr)
                if not duration_match:
                    raise RuntimeError('Recorded video duration was not observable')
                hours, minutes, seconds = map(float, duration_match.groups())
                duration = hours * 3600 + minutes * 60 + seconds
                dimensions_ok = bool(re.search(rb'1600x936', decoded.stderr))
                # The original dataset is deliberately chromatic. Check the
                # recorded viewport interior, not window chrome or overlays,
                # for non-neutral pixels so a black OpenGL capture cannot pass
                # merely because a real file dialog moved on screen.
                viewport = evidence.get('viewport_client_rectangle')
                chromatic_samples = 0
                if viewport:
                    left, top, right, bottom = viewport
                    margin_x, margin_y = (right - left) * .18, (bottom - top) * .18
                    x0, x1 = int((left + margin_x) / 10), int((right - margin_x) / 10)
                    y0, y1 = int((top + margin_y) * 94 / 936), int((bottom - margin_y) * 94 / 936)
                    for frame in frames:
                        colorful = 0
                        for y in range(max(0, y0), min(94, y1)):
                            for x in range(max(0, x0), min(160, x1)):
                                rgb = frame[(y * 160 + x) * 3:(y * 160 + x) * 3 + 3]
                                colorful += max(rgb) - min(rgb) > 24 and max(rgb) > 45
                        chromatic_samples += colorful >= 20
                if evidence['status'] == 'passed' and (not 60 <= duration <= 90 or unique < 12 or not dimensions_ok
                                                       or chromatic_samples < 8):
                    raise RuntimeError(f'Video duration, dimensions or real motion gate failed: {duration}s, {unique} distinct sampled frames')
                evidence['video_verification'] = {'decoded': True, 'duration_seconds': duration,
                    'dimensions_1600x936': dimensions_ok, 'distinct_one_second_samples': unique,
                    'chromatic_viewport_samples': chromatic_samples,
                    'purpose': 'blank/static capture gate; not image quality or geometry acceptance'}
            except Exception as error:
                evidence['status'] = 'failed'
                evidence['video_verification_error'] = str(error)
        log('driver_finished', status=evidence['status'])
    return 0 if evidence['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
