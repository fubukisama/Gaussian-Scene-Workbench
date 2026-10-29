"""CPU-only composition of a verified continuous native-operation recording.

The full MP4 retains the 1600x936 application image at its original size and
speed, with an external 88px English footer. Localized GIFs are explicitly
selected excerpts; captions never pretend saved snapshots are live training.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import subprocess

LANGUAGES = ("en", "zh-CN", "ja")
TEXT = {
    "en": {
        "full": "Actual training snapshot replay · 1× · not live training",
        "excerpt": "Actual training snapshot replay · selected excerpts · 1×",
        "iteration": "iteration", "count": "Gaussians", "short_iteration": "iter", "short_count": "splats",
        "initialization": "Inspect initialization", "training": "Inspect saved training snapshot",
        "final": "Inspect final saved snapshot", "orbit": "Orbit camera", "pan": "Pan camera",
        "zoom": "Zoom camera", "export": "Native PLY export · original coordinates",
        "loading": "Loading and verifying the next saved snapshot",
        "export_done": "Native PLY export verified", "edit": "Model editing · source units",
        "move": "Move +1 on X · source units", "undo": "Undo restored model transform",
    },
    "zh-CN": {
        "full": "真实训练快照回放 · 原速 1× · 非实时训练",
        "excerpt": "真实训练快照回放 · 选段 · 原速 1×",
        "iteration": "迭代", "count": "个高斯", "short_iteration": "迭代", "short_count": "个高斯",
        "initialization": "查看随机初始化", "training": "查看已保存的训练快照",
        "final": "查看最终保存快照", "orbit": "旋转视角", "pan": "平移视角", "zoom": "缩放视角",
        "loading": "正在载入并核对下一阶段快照",
        "export": "原生 PLY 导出 · 原始坐标", "export_done": "原生 PLY 导出已验证",
        "edit": "模型编辑 · 源单位", "move": "沿 X 移动 +1 · 源单位", "undo": "撤销已恢复模型变换",
    },
    "ja": {
        "full": "実学習スナップショット再生 · 1倍速 · リアルタイム学習ではありません",
        "excerpt": "実学習スナップショット再生 · 抜粋 · 1倍速",
        "iteration": "反復", "count": "個のガウシアン", "short_iteration": "反復", "short_count": "個",
        "initialization": "ランダム初期化を表示", "training": "保存済み学習スナップショットを表示",
        "final": "最終保存結果を表示", "orbit": "視点回転", "pan": "視点移動", "zoom": "視点ズーム",
        "loading": "次の保存スナップショットを読み込み・確認中",
        "export": "ネイティブ PLY 出力 · 元座標", "export_done": "ネイティブ PLY 出力を確認",
        "edit": "モデル編集 · 元の単位", "move": "X 方向へ +1 移動 · 元の単位", "undo": "取り消しでモデル変換を復元",
    },
}


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 ** 2), b""):
            result.update(block)
    return result.hexdigest()


def d_path(value, *, exists=True):
    path = Path(value).absolute()
    require(path.drive.lower() == "d:", "All inputs and generated outputs must be on D:")
    for part in (path, *path.parents):
        require(not part.is_symlink() and not (part.exists() and getattr(part.lstat(), "st_file_attributes", 0) & 0x400),
                "Links/reparse paths are not accepted")
    return path.resolve(strict=exists)


def child(root, value):
    require(isinstance(value, str) and value and ":" not in value and "\\" not in value
            and not PurePosixPath(value).is_absolute()
            and all(part not in ("", ".", "..") for part in value.split("/")), "Invalid input relative path")
    path = d_path(root / value)
    path.relative_to(root)
    require(path.is_file(), "An ordinary input file is required")
    return path


def read(path):
    require(Path(path).stat().st_size <= 16 * 1024 ** 2, "JSON exceeds bounded size")
    value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    require(isinstance(value, dict), "JSON object required")
    return value


def write(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)


def validate_inputs(capture, training):
    evidence_path = child(capture, "recording-evidence.json")
    evidence = read(evidence_path)
    require(evidence.get("schema") == "gsw.readme_operation_capture/1" and evidence.get("status") == "passed",
            "Only an actually passed native recording may be composed")
    require(evidence.get("live_remote_optimization") is False and evidence.get("video_speed") == 1.0
            and evidence.get("frame_interpolation") is False and evidence.get("synthetic_ui") is False
            and evidence.get("ffmpeg_exit_code") == 0 and not evidence.get("capture_region_guard_errors"),
            "Capture speed, authenticity or owned-window guard failed")
    verification = evidence.get("video_verification", {})
    require(verification.get("decoded") is True and verification.get("dimensions_1600x936") is True
            and verification.get("distinct_one_second_samples", 0) >= 12
            and verification.get("chromatic_viewport_samples", 0) >= 8,
            "Actual nonblank native-video verification is incomplete")
    acceptance = evidence.get("acceptance", {})
    require(all(key in acceptance for key in ("3dgs_export", "2dgs_export", "transform_and_undo")),
            "Both actual exports and transform/undo evidence are required")
    editing = acceptance["transform_and_undo"]
    require(editing["restored"] == editing["before"] and editing["changed"] != editing["before"],
            "Actual transform/undo acceptance did not hold")
    raw_record = evidence["raw_video"]
    require(raw_record.get("path") == "raw-continuous-operation.mp4", "Unexpected recording file")
    raw = child(capture, raw_record["path"])
    require(raw.stat().st_size == raw_record["bytes"] and digest(raw) == raw_record["sha256"],
            "Continuous raw recording digest or size differs")
    manifest_path = child(training, "demo_manifest.json")
    manifest = read(manifest_path)
    require(manifest.get("schema") == "gsw.readme_training_demo/1"
            and digest(manifest_path) == evidence.get("training_manifest_sha256")
            and manifest.get("dataset_identity") == evidence.get("dataset_identity"),
            "Recording and actual training input identities differ")
    provenance = child(training, manifest["training_provenance_path"])
    require(digest(provenance) == manifest["training_provenance_sha256"]
            and evidence.get("training_provenance", {}).get("sha256") == digest(provenance),
            "Training provenance is not the one actually recorded")
    for name, expected in (("demo_manifest.json", digest(manifest_path)),
                           ("training-provenance.json", digest(provenance))):
        require(digest(child(capture, name)) == expected, "Captured training metadata copy differs")
    public = read(provenance)
    require(public.get("quality_or_accuracy_accepted") is False
            and public.get("physical_accuracy_accepted") is False
            and public.get("test_images_feedback_into_optimization") is False
            and public.get("truth_depth_normals_mesh_used") is False,
            "Training provenance has incompatible accuracy/input claims")
    snapshots = {row["path"]: row for row in manifest["snapshots"]}
    replayed = evidence.get("replayed_snapshots", [])
    require(len(replayed) == 6 and {row["backend"] for row in replayed} == {"3dgs", "2dgs"},
            "Both backends' three actual recorded stages are required")
    for row in replayed:
        original = snapshots[row["path"]]
        require(all(row[key] == original[key] for key in ("backend", "kind", "iteration", "sha256"))
                and digest(child(training, row["path"])) == row["sha256"], "Recorded model snapshot differs")
    return evidence, manifest, public, raw


def timed_events(evidence, duration):
    rows = []
    previous = -1.
    for event in evidence["events"]:
        value = event.get("video_seconds")
        if value is None:
            continue
        require(type(value) in (int, float) and math.isfinite(value) and value >= previous,
                "Actual event video timing is invalid or nonmonotone")
        previous = value
        if 0 <= value <= duration:
            rows.append(dict(event, video_seconds=float(value)))
    require(rows, "No actual video-timed events exist")
    return rows


def build_timeline(evidence, manifest, duration):
    events = timed_events(evidence, duration)
    lookup = {row["path"]: row for row in manifest["snapshots"]}
    initial = next(row for row in manifest["snapshots"] if row["backend"] == "3dgs" and row["iteration"] == 0)
    stages = [(0., initial)]
    actions = []
    export_start = None
    editing_start = None
    import_start = None
    for event in events:
        t = event["video_seconds"]
        action = event["action"]
        if action == "actual_training_snapshot_loaded":
            row = lookup[event["path"]]
            require(all(event[key] == row[key] for key in ("sha256", "backend", "kind", "iteration")),
                    "A loaded-stage event does not match its actual training snapshot")
            stages.append((t, row))
            if import_start is not None:
                actions.append((import_start, t, "loading", 25))
                import_start = None
        elif action == "actual_pointer_drag":
            length = float(event["paced_seconds"])
            require(0 < length <= 10, "Invalid observed pointer duration")
            actions.append((max(0., t - length), t, "orbit" if event["button"] == "left" else "pan", 30))
        elif action == "actual_wheel_zoom":
            actions.append((max(0., t - .4 * len(event["deltas"])), t, "zoom", 30))
        elif action == "actual_export_dialog_observed":
            export_start = t
        elif action == "actual_native_export_verified":
            if export_start is not None:
                actions.append((export_start, t, "export", 40))
                export_start = None
            actions.append((t, min(duration, t + 1.), "export_done", 35))
        elif action == "editing_lock_observed":
            if event.get("reason") == "snapshot replay/import and subsequent auto-framing":
                import_start = t
            if event.get("locked") is False:
                editing_start = t
        elif action == "actual_numeric_transform_verified":
            if editing_start is not None:
                actions.append((editing_start, t, "edit", 45))
            actions.append((t, min(duration, t + 1.5), "move", 45))
        elif action == "actual_undo_verified":
            actions.append((t, min(duration, t + 2.), "undo", 50))
    points = {0., duration, *(t for t, _ in stages)}
    for start, end, _, _ in actions:
        points.update((start, end))
    boundaries = sorted(t for t in points if 0 <= t <= duration)
    segments = []
    for start, end in zip(boundaries, boundaries[1:]):
        if end - start < .001:
            continue
        middle = (start + end) * .5
        snapshot = next(row for t, row in reversed(stages) if t <= middle)
        active = [row for row in actions if row[0] <= middle < row[1]]
        action = max(active, key=lambda row: row[3])[2] if active else snapshot["kind"]
        item = {"start": start, "end": end, "snapshot": snapshot, "action": action}
        if segments and segments[-1]["snapshot"]["path"] == snapshot["path"] and segments[-1]["action"] == action:
            segments[-1]["end"] = end
        else:
            segments.append(item)
    return segments, events


def excerpt_ranges(events, duration):
    """Only recorded intervals are cut; no invented transitions or UI frames."""
    ranges = []
    for event in events:
        t, action = event["video_seconds"], event["action"]
        if action == "capture_started":
            ranges.append((t, t + .9, "initial_saved_model"))
        elif action == "actual_training_snapshot_loaded":
            ranges.append((t, t + .9, "loaded_" + event["backend"] + "_" + event["kind"]))
        elif action == "actual_pointer_drag" and event.get("button") == "left":
            ranges.append((t - float(event["paced_seconds"]), t + .1, "actual_orbit_input"))
        elif action == "actual_export_dialog_observed":
            ranges.append((t, t + .8, "actual_export_dialog"))
        elif action == "actual_numeric_transform_verified":
            ranges.append((t - 1.2, t + 1., "actual_numeric_transform"))
        elif action == "actual_undo_verified":
            ranges.append((t, t + 1.2, "actual_undo"))
    merged = []
    for start, end, reason in sorted(ranges):
        start, end = max(0., start), min(duration, end)
        if end <= start:
            continue
        if merged and start <= merged[-1]["source_end"] + .02:
            merged[-1]["source_end"] = max(end, merged[-1]["source_end"])
            merged[-1]["reasons"].append(reason)
        else:
            merged.append({"source_start": start, "source_end": end, "reasons": [reason]})
    position = 0.
    for item in merged:
        item["output_start"] = position
        position += item["source_end"] - item["source_start"]
        item["output_end"] = position
    require(merged and position <= 30, "Actual selected preview intervals are absent or exceed 30 seconds")
    return merged


def excerpt_timeline(timeline, ranges):
    result = []
    for selected in ranges:
        for segment in timeline:
            start = max(selected["source_start"], segment["start"])
            end = min(selected["source_end"], segment["end"])
            if end > start:
                offset = selected["output_start"] - selected["source_start"]
                result.append(dict(segment, start=start + offset, end=end + offset))
    return result


def label(segment, language, *, preview=False):
    words = TEXT[language]
    if segment["action"] == "loading":
        # The actual loaded-stage event follows native import, project readback
        # and viewport observation. Do not label transition frames with the
        # previous model's count or claim the next snapshot is already verified.
        return words["loading"]
    row = segment["snapshot"]
    iteration = words["short_iteration" if preview else "iteration"]
    count = words["short_count" if preview else "count"]
    return (f"{row['backend'].upper()} · {iteration} {row['iteration']:,} · "
            f"{row['gaussian_count']:,} {count}  |  {words[segment['action']]}")


def font_collection():
    from PIL import ImageFont
    base = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts"
    files = {"en": base / "segoeui.ttf", "zh-CN": base / "msyh.ttc", "ja": base / "YuGothR.ttc"}
    expected = {"en": "segoeui", "zh-CN": "microsoftyahei", "ja": "yugothic"}
    records = {}
    for language, path in files.items():
        require(path.is_file(), "Required existing system font missing: " + path.name)
        face = ImageFont.truetype(str(path), 20, index=0)
        family, style = face.getname()
        require(expected[language] in family.lower().replace(" ", ""), "Unexpected resolved language font family")
        records[language] = {"file": path.name, "family": family, "style": style, "sha256": digest(path)}
    return files, records


def text_runs(text, language):
    pieces = []
    for character in text:
        face = "en" if language == "en" or ord(character) < 256 else language
        if pieces and pieces[-1][0] == face:
            pieces[-1] = (face, pieces[-1][1] + character)
        else:
            pieces.append((face, character))
    return pieces


def draw_line(draw, text, language, fonts, maximum, baseline, size, color, margin):
    from PIL import ImageFont
    for current_size in range(size, max(10, size - 5) - 1, -1):
        rendered = [(ImageFont.truetype(str(fonts[face]), current_size, index=0), value)
                    for face, value in text_runs(text, language)]
        width = sum(draw.textlength(value, font=font) for font, value in rendered)
        if width <= maximum:
            x = margin
            for font, value in rendered:
                draw.text((x, baseline), value, font=font, fill=color, anchor="ls")
                x += draw.textlength(value, font=font)
            return current_size
    raise ValueError("External caption does not fit; it must not be cropped")


def caption_images(root, timeline, language, fonts, *, preview=False):
    from PIL import Image, ImageDraw
    directory = root / ("preview-" + language if preview else "continuous-" + language)
    directory.mkdir(parents=True)
    width, height = (640, 74) if preview else (1600, 88)
    lines = ["ffconcat version 1.0"]
    settings = []
    previous_image = None
    for index, segment in enumerate(timeline):
        image = Image.new("RGB", (width, height), "#111827")
        draw = ImageDraw.Draw(image)
        draw.line((0, 0, width - 1, 0), fill="#64748b", width=1)
        margin = 12 if preview else 24
        first = TEXT[language]["excerpt" if preview else "full"]
        second = label(segment, language, preview=preview)
        size1 = draw_line(draw, first, language, fonts, width - margin * 2,
                          25 if preview else 32, 14 if preview else 25, "#cbd5e1", margin)
        size2 = draw_line(draw, second, language, fonts, width - margin * 2,
                          58 if preview else 71, 16 if preview else 28, "#ffffff", margin)
        path = directory / f"{index:04d}.png"
        image.save(path)
        escaped = path.as_posix().replace("'", "'\\''")
        lines.extend(["file '" + escaped + "'", f"duration {segment['end'] - segment['start']:.6f}"])
        previous_image = escaped
        settings.append({"index": index, "first_line": first, "second_line": second,
                         "first_size": size1, "second_size": size2})
    require(previous_image is not None, "No captions generated")
    lines.append("file '" + previous_image + "'")
    concat = directory / "captions.ffconcat"
    concat.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return concat, settings


def timestamp(value):
    milliseconds = round(value * 1000)
    hours, remainder = divmod(milliseconds, 3600000)
    minutes, remainder = divmod(remainder, 60000)
    seconds, milliseconds = divmod(remainder, 1000)
    return f"{hours:02}:{minutes:02}:{seconds:02},{milliseconds:03}"


def subtitles(path, timeline, language):
    rows = []
    for index, segment in enumerate(timeline, 1):
        rows.extend((str(index), f"{timestamp(segment['start'])} --> {timestamp(segment['end'])}",
                     TEXT[language]["full"], label(segment, language), ""))
    # Deterministic published bytes on Windows and Linux, without a spurious
    # final blank cue. Blank separators between actual SRT cues are retained.
    path.write_text("\n".join(rows).rstrip() + "\n", encoding="utf-8", newline="\n")


def run(command, log, *, timeout=240):
    with log.open("xb") as stream:
        result = subprocess.run([str(value) for value in command], stdin=subprocess.DEVNULL,
            stdout=stream, stderr=subprocess.STDOUT, timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    require(result.returncode == 0, "CPU media command failed; see " + log.name)
    return log.read_text(encoding="utf-8", errors="replace")


def probe(ffmpeg, source, log):
    text = run([ffmpeg, "-hide_banner", "-nostdin", "-i", source, "-map", "0:v:0",
                "-an", "-threads", "2", "-f", "null", "-"], log)
    duration = re.search(r"Duration:\s*(\d+):(\d+):(\d+\.\d+)", text)
    dimensions = re.search(r"Stream[^\n]*Video:[^\n]*?\b(\d{2,5})x(\d{2,5})\b", text)
    frames = re.findall(r"frame=\s*(\d+)", text)
    require(duration and dimensions and frames, "Actual video duration/dimensions/frame count were not observed")
    hours, minutes, seconds = map(float, duration.groups())
    return {"duration_seconds": hours * 3600 + minutes * 60 + seconds,
            "width": int(dimensions[1]), "height": int(dimensions[2]), "decoded_frames": int(frames[-1])}


def source_commit(record):
    keys = ("binarySourceCommit", "applicationSourceCommit", "application_source_commit", "nativeApplicationCommit",
            "nativeApplicationSourceCommit", "applicationCommit", "sourceCommit", "source_commit")
    return next((record[key] for key in keys if isinstance(record.get(key), str)
                 and re.fullmatch(r"[0-9a-f]{40}", record[key])), None)


def inspect_gif(path, planned_duration):
    from PIL import Image
    with Image.open(path) as image:
        require(image.format == "GIF" and image.size == (640, 448) and 1 < image.n_frames <= 400,
                "Actual preview GIF dimensions/frame count differ")
        frames = image.n_frames
        duration = 0
        for index in range(frames):
            image.seek(index)
            duration += int(image.info.get("duration", 0))
        seconds = duration / 1000.
        require(abs(seconds - planned_duration) <= .5,
                "Actual selected-excerpt GIF duration differs from the source intervals")
        return {"path": path.name, "dimensions": [640, 448], "frames": frames,
                "duration_seconds": seconds, "planned_source_seconds": planned_duration}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-root", type=Path, required=True)
    parser.add_argument("--training-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    args = parser.parse_args()
    capture, training = d_path(args.capture_root), d_path(args.training_root)
    output, ffmpeg = d_path(args.output, exists=False), d_path(args.ffmpeg)
    require(not output.exists(), "Composition output must be fresh")
    for original in (capture, training):
        try:
            output.relative_to(original)
        except ValueError:
            pass
        else:
            raise ValueError("Composition output may not modify an input tree")
    evidence, manifest, provenance, raw = validate_inputs(capture, training)
    output.mkdir(parents=True)
    raw_before = digest(raw)
    observed = probe(ffmpeg, raw, output / "raw-decode.log")
    require((observed["width"], observed["height"]) == (1600, 936)
            and 60 <= observed["duration_seconds"] <= 90
            and abs(observed["duration_seconds"] - evidence["video_verification"]["duration_seconds"]) <= .1,
            "Actual raw-video dimensions/duration differ from capture acceptance")
    timeline, events = build_timeline(evidence, manifest, observed["duration_seconds"])
    ranges = excerpt_ranges(events, observed["duration_seconds"])
    preview_timeline = excerpt_timeline(timeline, ranges)
    fonts, font_records = font_collection()
    captions = output / "external-caption-assets"
    captions.mkdir()
    continuous, full_caption_settings = caption_images(captions, timeline, "en", fonts)
    full_path = output / "gsw-native-operation.en.mp4"
    full_filter = "[0:v]pad=1600:1024:0:0:color=0x111827[base];[base][1:v]overlay=0:936:eof_action=repeat:shortest=1:format=auto[out]"
    full_command = [ffmpeg, "-hide_banner", "-nostdin", "-i", raw, "-f", "concat", "-safe", "0", "-i", continuous,
        "-filter_complex_threads", "2", "-filter_complex", full_filter, "-map", "[out]", "-an",
        "-c:v", "libx264", "-threads", "2", "-preset", "fast", "-crf", "18", "-pix_fmt", "yuv420p",
        "-fps_mode", "passthrough", "-movflags", "+faststart", full_path]
    run(full_command, output / "continuous-compose.log")
    full_observed = probe(ffmpeg, full_path, output / "continuous-decode.log")
    require((full_observed["width"], full_observed["height"]) == (1600, 1024)
            and full_observed["decoded_frames"] == observed["decoded_frames"]
            and abs(full_observed["duration_seconds"] - observed["duration_seconds"]) <= .1,
            "Composed continuous video did not retain actual dimensions/frame count/time")
    generated = [full_path]
    gif_observations = []
    caption_records = {"continuous-en": full_caption_settings}
    for language in LANGUAGES:
        srt = output / f"gsw-native-operation.{language}.srt"
        subtitles(srt, timeline, language)
        generated.append(srt)
        preview_captions, settings = caption_images(captions, preview_timeline, language, fonts, preview=True)
        caption_records["preview-" + language] = settings
        count = len(ranges)
        split = "[0:v]split=" + str(count) + "".join(f"[source{i}]" for i in range(count)) + ";"
        trims = "".join(f"[source{i}]trim=start={row['source_start']:.6f}:end={row['source_end']:.6f},setpts=PTS-STARTPTS[clip{i}];"
                        for i, row in enumerate(ranges))
        joined = "".join(f"[clip{i}]" for i in range(count)) + f"concat=n={count}:v=1:a=0[sequence];"
        preview_filter = (split + trims + joined
            + "[sequence]fps=8,scale=640:374:flags=lanczos,pad=640:448:0:0:color=0x111827[base];"
            + "[base][1:v]overlay=0:374:eof_action=repeat:shortest=0:format=auto,split[colors][pixels];"
            + "[colors]palettegen=stats_mode=diff[palette];[pixels][palette]paletteuse=dither=sierra2_4a[out]")
        gif = output / f"gsw-native-preview.{language}.gif"
        command = [ffmpeg, "-hide_banner", "-nostdin", "-i", raw, "-f", "concat", "-safe", "0", "-i", preview_captions,
            "-filter_complex_threads", "2", "-filter_complex", preview_filter, "-map", "[out]", "-an", "-loop", "0", gif]
        run(command, output / f"preview-{language}-compose.log")
        gif_observations.append(inspect_gif(gif, ranges[-1]["output_end"]))
        generated.append(gif)
    # Retain the already-sanitized training provenance verbatim, not the raw
    # capture record which contains temporary PID/HWND and local input paths.
    copied_provenance = output / "training-provenance.json"
    copied_provenance.write_bytes(child(training, manifest["training_provenance_path"]).read_bytes())
    generated.append(copied_provenance)
    require(digest(raw) == raw_before, "Original continuous recording changed during composition")
    package = evidence.get("package", {})
    receipt = {"schema": "gsw.readme_demo_composition/1", "status": "composed_not_visually_accepted",
        "visual_review_required": True, "source_script_sha256": digest(Path(__file__)),
        "inputs": {"raw_video_sha256": raw_before, "capture_evidence_sha256": digest(capture / "recording-evidence.json"),
                   "training_manifest_sha256": digest(training / "demo_manifest.json"),
                   "training_provenance_sha256": digest(copied_provenance), "dataset_identity": manifest["dataset_identity"]},
        "source_identity": {"training_source_commit": provenance["training_source_commit"],
            "native_recording_executable_sha256": package.get("executableSha256"),
            "native_recording_source_commit": source_commit(package),
            "recording_driver_source_commit": evidence.get("runner", {}).get("GITHUB_SHA"),
            "training_and_recording_application_are_separate_identities": True},
        "continuous_video": {"file": full_path.name, "raw_observation": observed, "composed_observation": full_observed,
            "ui_rectangle": [0, 0, 1600, 936], "external_footer_rectangle": [0, 936, 1600, 88],
            "ui_crop": False, "ui_scale": False, "speed": 1., "frame_interpolation": False,
            "cuts": False, "audio_added": False, "filter": full_filter, "fps_mode": "passthrough",
            "lossy_reencoding": "H.264 CRF18 yuv420p"},
        "preview_gifs": {"dimensions": [640, 448], "original_ui_downsampled_to": [640, 374],
            "external_footer_height": 74, "sampled_fps": 8, "speed_within_each_excerpt": 1.,
            "selected_excerpts_not_continuous_capture": True, "source_ranges": ranges,
            "localized_external_captions_not_native_ui_language_changes": True,
            "filter": preview_filter, "actual_observations": gif_observations},
        "chapter_timeline": [{"start": row["start"], "end": row["end"], "backend": row["snapshot"]["backend"],
            "iteration": row["snapshot"]["iteration"], "gaussian_count": row["snapshot"]["gaussian_count"],
            "operation": row["action"]} for row in timeline],
        "fonts": font_records, "Latin_runs_use_Segoe_UI": True,
        "srt_note": "UTF-8 language-specific text; subtitle player controls its own rendering font.",
        "caption_layouts": caption_records, "no_live_training_or_quality_benchmark_claim": True,
        "viewer_auto_framing_applied": evidence.get("viewer_auto_framing_applied"),
        "matched_camera_quality_comparison": evidence.get("matched_camera_quality_comparison"),
        "capture_ffmpeg_sha256": evidence.get("ffmpeg", {}).get("sha256"),
        "ffmpeg_sha256": digest(ffmpeg), "original_raw_unchanged": True,
        "files": [{"path": path.name, "bytes": path.stat().st_size, "sha256": digest(path)} for path in generated]}
    write(output / "composition-receipt.json", receipt)
    print(json.dumps({"status": receipt["status"], "output": str(output), "files": len(generated),
                      "visual_review_required": True}, ensure_ascii=False))


if __name__ == "__main__":
    main()
