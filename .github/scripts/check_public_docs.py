"""Fast, dependency-free checks for GSW's public documentation entry points.

Markdown source soft wrapping is valid. Only the plain-text LICENSE is required
to keep each prose paragraph on one physical line, so GitHub can wrap it once.
"""
from __future__ import annotations

import argparse
from html.parser import HTMLParser
import hashlib
import json
import math
from pathlib import Path
import posixpath
import re
import subprocess
from urllib.parse import unquote, urlsplit
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PRIMARY = (
    "README.md", "README.zh-CN.md", "README.ja.md", "README_RELEASE.md",
    "native/README.md", "native/LICENSE.md", "docs/README.md",
    "docs/LICENSING.md", "docs/images/README.md", "docs/media/README.md",
)
LANGUAGE_PAGES = {"README.md": "en", "README.zh-CN.md": "zh-CN", "README.ja.md": "ja"}
OVERVIEW_BADGE_TARGETS = {
    "README.md": ("download-the-preview", "before-you-start"),
    "README.zh-CN.md": ("下载预览版", "使用前确认"),
    "README.ja.md": ("プレビュー版の入手", "利用前の確認"),
}
MULTILINGUAL = {"README_RELEASE.md", "docs/README.md", "docs/LICENSING.md", "native/README.md", "docs/images/README.md", "docs/media/README.md", "docs/TRAINING_PERFORMANCE.md"}
DEMO_READMES = {"README.md": "en", "README.zh-CN.md": "zh-CN", "README.ja.md": "ja"}


class Html(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []
        self.errors = []
        self.details = 0
        self.divs = 0
        self.languages = set()
        self.anchor_stack = []
        self.linked_images = []
        self.ids = set()
        self.unfolded_showcase_images = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if attrs.get("id"):
            self.ids.add(attrs["id"])
        if tag == "a":
            self.anchor_stack.append(attrs.get("href", ""))
        if tag == "img":
            self.linked_images.append((attrs.get("src", ""), self.anchor_stack[-1] if self.anchor_stack else ""))
            if not self.details and attrs.get("src", "").startswith("docs/images/"):
                self.unfolded_showcase_images.append(attrs["src"])
        if attrs.get("lang"):
            self.languages.add(attrs["lang"])
        if tag == "div":
            self.divs += 1
        if tag == "details":
            self.details += 1
        if tag == "summary" and self.details == 0:
            self.errors.append("summary appears outside details")
        if tag == "img" and not attrs.get("alt", "").strip():
            self.errors.append("image has no alt text")
        for key in ("href", "src"):
            if attrs.get(key):
                self.links.append(attrs[key])

    def handle_endtag(self, tag):
        if tag == "a" and self.anchor_stack:
            self.anchor_stack.pop()
        if tag == "div":
            self.divs -= 1
            if self.divs < 0:
                self.errors.append("unmatched div closing tag")
        if tag == "details":
            self.details -= 1
            if self.details < 0:
                self.errors.append("unmatched details closing tag")


def without_fences(text, errors):
    result = []
    fence = None
    for number, line in enumerate(text.splitlines(), 1):
        match = re.match(r"^\s{0,3}(`{3,}|~{3,})(.*)$", line)
        if match:
            token, rest = match.groups()
            if fence is None:
                fence = (token[0], len(token), number)
                result.append("")
                continue
            if token[0] == fence[0] and len(token) >= fence[1] and not rest.strip():
                fence = None
                result.append("")
                continue
        result.append("" if fence else line)
    if fence:
        errors.append(f"line {fence[2]}: unclosed code fence")
    return result


def plain_license_errors(text):
    errors = []
    lines = text.splitlines()
    index = 0
    def heading(at):
        return at + 1 < len(lines) and bool(lines[at].strip()) and bool(re.fullmatch(r"[=-]+", lines[at + 1]))
    while index < len(lines):
        if not lines[index].strip():
            index += 1
            continue
        if heading(index):
            index += 2
            continue
        start = index
        while index < len(lines) and lines[index].strip() and not heading(index):
            index += 1
        count = index - start
        if count > 1:
            errors.append(f"line {start+1}: prose paragraph has {count} hard-wrapped lines; join the paragraph and let the viewer wrap it")
    return errors


def tracked_files():
    result=subprocess.run(["git","ls-files","-z"],cwd=ROOT,check=True,capture_output=True,timeout=10)
    return {path for path in result.stdout.decode("utf-8").split("\0") if path}


def link_destination(target, html_attribute=False):
    target=target.strip()
    if target.startswith("<") and ">" in target:
        return target[1:target.index(">")]
    if not html_attribute:
        target=re.split(r"(?<!\\)\s+",target,maxsplit=1)[0]
        return target.replace("\\ "," ")
    return target


def check_readme_media(tracked):
    """Bind published PNG bytes to the documented source; not visual acceptance."""
    relative = "docs/images/readme/manifest.json"
    try:
        manifest = json.loads((ROOT / relative).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        return [f"README media manifest is unavailable: {error}"], 0
    errors = []
    if (manifest.get("schema") != "gsw.readme_media/1" or
            not re.fullmatch(r"[0-9a-f]{40}", manifest.get("sourceCommit", "")) or
            manifest.get("rawUnedited") is not True or
            manifest.get("syntheticInputs") is not True or
            any(manifest.get(key) is not False for key in (
                "videoRecording", "trainingExecutionDemonstrated",
                "currentNativeDeliveryAcceptance", "reconstructionQualityEvidence"))):
        errors.append("README media source/claim boundary is invalid")
    images = manifest.get("images", [])
    expected = {(locale, purpose) for locale in ("en_US", "zh_CN", "ja_JP")
                for purpose in ("import", "initialization", "training-3dgs", "training-2dgs", "export")}
    actual = {(entry.get("locale"), entry.get("purpose")) for entry in images}
    if len(images) != len(expected) or actual != expected:
        errors.append("README media must cover all five views in all three languages")
    seen = set()
    for entry in images:
        path = entry.get("path", "")
        selected = (ROOT / path).resolve()
        if (path in seen or path not in tracked or not path.startswith("docs/images/readme/") or
                not path.endswith(".png") or not selected.is_relative_to(ROOT) or not selected.is_file()):
            errors.append(f"README media path is missing, duplicate or unpublished: {path}")
            continue
        seen.add(path)
        data = selected.read_bytes()
        if len(data) != entry.get("bytes") or hashlib.sha256(data).hexdigest() != entry.get("sha256"):
            errors.append(f"README media file binding mismatch: {path}")
        if (len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or
                int.from_bytes(data[16:20], "big") != entry.get("width") or
                int.from_bytes(data[20:24], "big") != entry.get("height")):
            errors.append(f"README media PNG dimensions/header mismatch: {path}")
    return errors, len(images)


def file_sha256(path):
    """Hash large published recordings without loading them into memory."""
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 ** 2), b""):
            result.update(block)
    return result.hexdigest()


def demo_require(condition, message):
    if not condition:
        raise ValueError(message)


def demo_json(path):
    demo_require(path.stat().st_size <= 2 * 1024 ** 2, "demo JSON exceeds the bounded metadata size")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    demo_require(isinstance(value, dict), "demo metadata must be a JSON object")
    return value


def demo_file(name, tracked):
    # Receipt entries are basenames, not paths supplied to an extraction tool.
    demo_require(isinstance(name, str) and re.fullmatch(r"[A-Za-z0-9_.-]+", name),
                 "demo path must be a safe file basename")
    relative = "docs/media/" + name
    path = ROOT / relative
    demo_require(relative in tracked and path.is_file() and not path.is_symlink()
                 and path.resolve().is_relative_to(ROOT.resolve())
                 and path.resolve().parent == (ROOT / "docs/media").resolve(),
                 "demo path is missing, escaping or unpublished: " + relative)
    return path


def demo_document_targets(relative):
    clean = "\n".join(without_fences((ROOT / relative).read_text(encoding="utf-8-sig"), []))
    clean = re.sub(r"<!--.*?-->", "", clean, flags=re.S)
    clean = re.sub(r"(`+).*?\1", "", clean, flags=re.S)
    parser = Html()
    parser.feed(clean)
    # Starting at ]( also finds both destinations in [![preview](GIF)](MP4).
    targets = [(value, False) for value in re.findall(r"\]\((<[^>\n]+>|[^)\n]+)\)", clean)]
    targets.extend((value, True) for value in parser.links)
    result = set()
    for value, html in targets:
        url = urlsplit(link_destination(value, html))
        if not url.scheme and not url.netloc and url.path:
            result.add(posixpath.normpath(posixpath.join(posixpath.dirname(relative), unquote(url.path))))
    return result


def demo_number(value):
    return type(value) in (int, float) and math.isfinite(value)


def demo_video_contract(receipt):
    """Check declared timing/claim boundaries, not pixels or playback quality."""
    demo_require(receipt.get("schema") == "gsw.readme_demo_composition/1"
                 and receipt.get("status") == "composed_not_visually_accepted"
                 and receipt.get("visual_review_required") is True
                 and receipt.get("no_live_training_or_quality_benchmark_claim") is True
                 and receipt.get("original_raw_unchanged") is True,
                 "demo receipt must preserve the unaccepted visual/quality claim boundary")
    video = receipt.get("continuous_video")
    preview = receipt.get("preview_gifs")
    demo_require(isinstance(video, dict) and isinstance(preview, dict), "demo video contracts are missing")
    demo_require(video.get("file") == "gsw-native-operation.en.mp4"
                 and demo_number(video.get("speed")) and video["speed"] == 1
                 and all(video.get(key) is False for key in ("ui_crop", "ui_scale", "frame_interpolation", "cuts", "audio_added"))
                 and video.get("ui_rectangle") == [0, 0, 1600, 936]
                 and video.get("external_footer_rectangle") == [0, 936, 1600, 88],
                 "continuous demo must retain the unscaled, uncropped UI at original speed without cuts/interpolation")
    raw, composed = video.get("raw_observation"), video.get("composed_observation")
    demo_require(isinstance(raw, dict) and isinstance(composed, dict), "demo duration observations are missing")
    duration = composed.get("duration_seconds")
    demo_require(demo_number(duration) and duration > 0 and demo_number(raw.get("duration_seconds"))
                 and abs(duration - raw["duration_seconds"]) <= .1
                 and (raw.get("width"), raw.get("height")) == (1600, 936)
                 and (composed.get("width"), composed.get("height")) == (1600, 1024)
                 and type(raw.get("decoded_frames")) is int and raw["decoded_frames"] > 1
                 and composed.get("decoded_frames") == raw["decoded_frames"],
                 "continuous demo duration/frame/dimension declarations disagree")
    demo_require(preview.get("dimensions") == [640, 448]
                 and demo_number(preview.get("speed_within_each_excerpt")) and preview["speed_within_each_excerpt"] == 1
                 and preview.get("selected_excerpts_not_continuous_capture") is True
                 and preview.get("localized_external_captions_not_native_ui_language_changes") is True,
                 "GIF previews must declare original-speed excerpts and external localized captions")
    ranges = preview.get("source_ranges")
    demo_require(isinstance(ranges, list) and ranges, "GIF source excerpt ranges are missing")
    position = previous_source_end = 0.0
    for row in ranges:
        demo_require(isinstance(row, dict) and all(demo_number(row.get(key)) for key in
                     ("source_start", "source_end", "output_start", "output_end")), "invalid GIF source excerpt timing")
        start, end = row["source_start"], row["source_end"]
        demo_require(previous_source_end <= start < end <= duration + .001
                     and abs(row["output_start"] - position) <= .001
                     and abs((row["output_end"] - row["output_start"]) - (end - start)) <= .001,
                     "GIF excerpt range changes speed, overlaps or escapes the source duration")
        position, previous_source_end = row["output_end"], end
    demo_require(0 < position <= 30.001, "GIF excerpt duration is outside the documented limit")
    timeline = receipt.get("chapter_timeline")
    demo_require(isinstance(timeline, list) and timeline, "demo chapter timeline is missing")
    intervals = []
    previous = 0
    for row in timeline:
        demo_require(isinstance(row, dict) and demo_number(row.get("start")) and demo_number(row.get("end")),
                     "invalid demo chapter timing")
        start, end = round(row["start"] * 1000), round(row["end"] * 1000)
        demo_require(previous <= start < end <= round(duration * 1000) + 1,
                     "demo chapter timing overlaps or escapes the source duration")
        intervals.append((start, end))
        previous = end
    return duration, intervals


def demo_srt_intervals(path, duration):
    demo_require(path.stat().st_size <= 2 * 1024 ** 2, "SRT exceeds the bounded text size")
    text = path.read_text(encoding="utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")
    demo_require(text.strip() and "\ufffd" not in text and "\x00" not in text, "SRT must contain nonempty UTF-8 text")
    intervals = []
    stamp = r"(\d{2,}):([0-5]\d):([0-5]\d),(\d{3})"
    for index, block in enumerate(re.split(r"\n\s*\n", text.strip()), 1):
        lines = block.splitlines()
        demo_require(len(lines) >= 3 and lines[0] == str(index) and "\n".join(lines[2:]).strip(),
                     "SRT cues need consecutive numbering and nonempty captions")
        match = re.fullmatch(stamp + r" --> " + stamp, lines[1])
        demo_require(match, "SRT cue has an invalid timestamp")
        values = [int(value) for value in match.groups()]
        start, end = [((values[offset] * 60 + values[offset+1]) * 60 + values[offset+2]) * 1000 + values[offset+3]
                      for offset in (0, 4)]
        demo_require((not intervals or intervals[-1][1] <= start) and start < end <= round(duration * 1000) + 1,
                     "SRT cue timing overlaps or escapes the video duration")
        intervals.append((start, end))
    return intervals


def check_readme_demo(tracked):
    """Bind published demo bytes and navigation; never grant visual/quality acceptance."""
    errors, count = [], 0
    try:
        targets = {readme: demo_document_targets(readme) for readme in DEMO_READMES}
        receipt_path = ROOT / "docs/media/composition-receipt.json"
        if not receipt_path.exists():
            if any(target.startswith("docs/media/") and target.lower().endswith((".mp4", ".gif"))
                   for values in targets.values() for target in values):
                errors.append("README demo MP4/GIF references require a published composition-receipt.json")
            return errors, count
        receipt = demo_json(demo_file("composition-receipt.json", tracked))
        duration, expected_intervals = demo_video_contract(receipt)
        expected = {"gsw-native-operation.en.mp4", "training-provenance.json"}
        expected.update(f"gsw-native-operation.{language}.srt" for language in DEMO_READMES.values())
        expected.update(f"gsw-native-preview.{language}.gif" for language in DEMO_READMES.values())
        rows = receipt.get("files")
        demo_require(isinstance(rows, list), "demo receipt file inventory must be a list")
        count = len(rows)
        demo_require(count == 8 and all(isinstance(row, dict) and isinstance(row.get("path"), str) for row in rows)
                     and len({row["path"] for row in rows}) == count
                     and {row["path"] for row in rows} == expected,
                     "demo file path inventory must contain exactly the eight expected unique MP4/GIF/SRT/provenance files")
        files = {}
        for row in rows:
            name = row["path"]
            path = demo_file(name, tracked)
            files[name] = path
            if (type(row.get("bytes")) is not int or row["bytes"] <= 0 or path.stat().st_size != row["bytes"]
                    or not isinstance(row.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", row["sha256"])
                    or file_sha256(path) != row["sha256"]):
                errors.append("demo file binding mismatch: " + name)
            with path.open("rb") as stream:
                header = stream.read(24)
            if name.endswith(".mp4"):
                demo_require(len(header) >= 16 and header[4:8] == b"ftyp"
                             and 16 <= int.from_bytes(header[:4], "big") <= path.stat().st_size,
                             "demo MP4 type signature is invalid")
            elif name.endswith(".gif"):
                demo_require(len(header) >= 13 and header[:6] in (b"GIF87a", b"GIF89a")
                             and [int.from_bytes(header[6:8], "little"), int.from_bytes(header[8:10], "little")] == [640, 448],
                             "demo GIF type signature/dimensions are invalid: " + name)
            elif name.endswith(".srt"):
                demo_require(demo_srt_intervals(path, duration) == expected_intervals,
                             "SRT timestamps differ from the shared chapter timeline: " + name)
        for readme, language in DEMO_READMES.items():
            required = {"docs/media/gsw-native-operation.en.mp4", "docs/media/README.md",
                        f"docs/media/gsw-native-preview.{language}.gif", f"docs/media/gsw-native-operation.{language}.srt"}
            missing = required - targets[readme]
            if missing:
                errors.append(readme + " lacks its localized demo/provenance links: " + ", ".join(sorted(missing)))
        guide = demo_file("README.md", tracked).read_text(encoding="utf-8-sig")
        bundle_hashes = re.findall(r"ZIP SHA256:\s*`([0-9a-f]{64})`", guide)
        bundle = demo_file("readme-training-demo.zip", tracked)
        demo_require(len(bundle_hashes) == 1 and file_sha256(bundle) == bundle_hashes[0],
                     "training ZIP hash differs from the independent provenance-guide binding")
        with zipfile.ZipFile(bundle) as archive:
            def member(name):
                selected = [row for row in archive.infolist() if row.filename == name]
                demo_require(len(selected) == 1 and selected[0].file_size <= 2 * 1024 ** 2,
                             "training ZIP metadata member is missing, duplicate or oversized: " + name)
                return archive.read(selected[0])
            manifest_bytes = member("demo_manifest.json")
            manifest = json.loads(manifest_bytes.decode("utf-8-sig"))
            demo_require(isinstance(manifest, dict) and manifest.get("schema") == "gsw.readme_training_demo/1"
                         and manifest.get("training_provenance_path") == "training_provenance.json",
                         "training ZIP manifest schema/provenance path is invalid")
            provenance_bytes = member("training_provenance.json")
        provenance = demo_json(files["training-provenance.json"])
        inputs, identity = receipt.get("inputs"), receipt.get("source_identity")
        demo_require(isinstance(inputs, dict) and isinstance(identity, dict), "demo source identity bindings are missing")
        provenance_hash = hashlib.sha256(provenance_bytes).hexdigest()
        demo_require(inputs.get("training_manifest_sha256") == hashlib.sha256(manifest_bytes).hexdigest()
                     and inputs.get("training_provenance_sha256") == manifest.get("training_provenance_sha256") == provenance_hash
                     and file_sha256(files["training-provenance.json"]) == provenance_hash
                     and isinstance(inputs.get("dataset_identity"), str) and inputs["dataset_identity"]
                     and inputs["dataset_identity"] == manifest.get("dataset_identity") == provenance.get("dataset_identity")
                     and identity.get("training_source_commit") == provenance.get("training_source_commit")
                     and re.fullmatch(r"[0-9a-f]{40}", str(identity.get("training_source_commit", "")))
                     and identity.get("training_and_recording_application_are_separate_identities") is True,
                     "training ZIP manifest/provenance/source identity differs from the composition receipt")
        demo_require(provenance.get("schema") == "gsw.readme_training_public_provenance/1"
                     and all(provenance.get(key) is False for key in (
                         "quality_or_accuracy_accepted", "physical_accuracy_accepted", "full_optimizer_resume_demonstrated",
                         "test_images_feedback_into_optimization", "truth_depth_normals_mesh_used")),
                     "training provenance must not claim quality/physical accuracy/full-state resume acceptance")
    except (OSError, ValueError, TypeError, OverflowError, zipfile.BadZipFile) as error:
        errors.append("demo publication check: " + str(error))
    return errors, count


def check_document(relative, tracked=None):
    if tracked is None:tracked=tracked_files()
    path = ROOT / relative
    errors = []
    text = path.read_text(encoding="utf-8-sig")
    if "\ufffd" in text:
        errors.append("replacement character in UTF-8 text")
    lines = without_fences(text, errors)
    clean = "\n".join(lines)
    parser = Html(); parser.feed(clean)
    errors.extend(parser.errors)
    if parser.details:
        errors.append("unclosed details block")
    if parser.divs:
        errors.append("unclosed language/container div")
    if relative in LANGUAGE_PAGES and not re.match(r'\s*<div lang="'+re.escape(LANGUAGE_PAGES[relative])+r'">', text):
        errors.append("localized overview has no matching document-language container")
    if relative in OVERVIEW_BADGE_TARGETS:
        release_guide = (ROOT / "README_RELEASE.md").read_text(encoding="utf-8-sig")
        for badge, fragment in zip(("native_preview", "platform"), OVERVIEW_BADGE_TARGETS[relative]):
            matches = [(src, href) for src, href in parser.linked_images if urlsplit(src).path.startswith("/badge/" + badge + "-") and urlsplit(src).netloc == "img.shields.io"]
            expected = "README_RELEASE.md#" + fragment
            if len(matches) != 1 or matches[0][1] != expected:
                errors.append(f"{badge} badge must link to its localized guide section: {expected}")
            headings = re.findall(r"^#{1,6}\s+(.+?)\s*$", release_guide, re.M)
            if fragment not in {heading.lower().replace(" ", "-") for heading in headings}:
                errors.append(f"{badge} badge target section does not exist: {expected}")
    if relative in LANGUAGE_PAGES:
        # Overview headings use plain text. Check its local navigation without
        # imposing a product-copy vocabulary or a fixed section count.
        anchors = set(parser.ids)
        counts = {}
        for heading in re.findall(r"^#{1,6}\s+(.+?)\s*$", clean, re.M):
            heading = re.sub(r"<[^>]+>|[*`~]", "", heading).lower()
            slug = re.sub(r"[^\w -]", "", heading).replace(" ", "-")
            count = counts.get(slug, 0)
            anchors.add(slug if not count else f"{slug}-{count}")
            counts[slug] = count + 1
        navigation = re.findall(r"(?<!!)\[[^\]\n]+\]\(\s*#([^\s)]+)\s*\)", clean)
        for fragment in navigation:
            if unquote(fragment) not in anchors:
                errors.append(f"overview navigation points to a missing heading: #{fragment}")
        # A showcase should not disappear into the folded details again.
        details_depth = 0
        unfolded_markdown_image = False
        for line in lines:
            details_depth += len(re.findall(r"<details\b", line))
            if not details_depth and re.search(r"!\[[^\]]*\]\(docs/images/", line):
                unfolded_markdown_image = True
            details_depth -= len(re.findall(r"</details>", line))
        if not unfolded_markdown_image and not parser.unfolded_showcase_images:
            errors.append("overview must show a provenance-documented viewport image outside folded details")
    if relative in MULTILINGUAL and not {"en", "zh-CN", "ja"}.issubset(parser.languages):
        errors.append("multilingual entry lacks explicit English/Chinese/Japanese language markers")
    if re.search(r'<font\b|style\s*=[^>]*font-family', clean, re.I):
        errors.append("do not rely on GitHub-filtered font overrides; use semantic language markers")
    for number, line in enumerate(lines, 1):
        no_code = re.sub(r"`[^`]*`", "", line)
        if re.search(r"!\[\s*\]\(", no_code):
            errors.append(f"line {number}: image has no alt text")
        if re.search(r"[A-Za-z0-9]\[[^\]\n]+\]\([^\n)]+\)", no_code):
            errors.append(f"line {number}: English text is joined directly to a Markdown link; add a space")
        if re.match(r"^日本語[:：]", line) and number > 1 and re.match(r"^中文[:：]", lines[number-2]):
            errors.append(f"line {number}: Chinese and Japanese paragraphs need a blank separator")
    if relative in MULTILINGUAL:
        language_stack=[]
        expected_language={"English":"en","中文":"zh-CN","简体中文":"zh-CN","日本語":"ja"}
        for number,line in enumerate(lines,1):
            for tag in re.finditer(r"<(/?)div\b([^>]*)>",line):
                if tag[1]:
                    if language_stack:language_stack.pop()
                else:
                    language=re.search(r'lang="([^"]+)"',tag[2])
                    language_stack.append(language[1] if language else (language_stack[-1] if language_stack else None))
            heading=re.match(r"^#{1,6}\s+(English|中文|简体中文|日本語)\s*$",line)
            if heading and (not language_stack or language_stack[-1]!=expected_language[heading[1]]):
                errors.append(f"line {number}: language section inherits the wrong language")
    def cells(line):
        parts = re.split(r"(?<!\\)\|", line.strip())
        if parts and not parts[0].strip():parts=parts[1:]
        if parts and not parts[-1].strip():parts=parts[:-1]
        return parts
    for index,line in enumerate(lines):
        columns=cells(line)
        if len(columns)<2 or not all(re.fullmatch(r"\s*:?-{3,}:?\s*",cell) for cell in columns):continue
        if index==0 or len(cells(lines[index-1]))!=len(columns):
            errors.append(f"line {index+1}: table header/separator column mismatch")
        if index>=2 and lines[index-2].strip():
            errors.append(f"line {index}: add a blank line before the table")
        row=index+1
        while row<len(lines) and lines[row].strip() and "|" in lines[row]:
            if len(cells(lines[row]))!=len(columns):errors.append(f"line {row+1}: table row column count differs from header")
            row+=1
    links = [(target,False) for target in re.findall(r"!?\[[^\]\n]*\]\(([^\n)]+)\)", clean)] + [(target,True) for target in parser.links]
    for target,is_html in links:
        target=link_destination(target,is_html)
        url = urlsplit(target)
        if url.scheme or url.netloc or not url.path:
            continue
        selected = (path.parent / unquote(url.path)).resolve()
        if not selected.is_relative_to(ROOT) or not selected.exists():
            errors.append(f"missing/escaping local link: {target}")
            continue
        public_target=posixpath.normpath(posixpath.join(posixpath.dirname(relative),unquote(url.path)))
        published=(public_target in tracked or public_target=="." or any(item.startswith(public_target.rstrip("/")+"/") for item in tracked))
        if not published:errors.append(f"local link target is not tracked/published: {target}")
    return errors, len(links)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    paths = sorted(set(PRIMARY) | {p.relative_to(ROOT).as_posix() for p in (ROOT / "docs").glob("*.md")})
    findings = []
    references = 0
    tracked=tracked_files()
    for relative in paths:
        errors, count = check_document(relative,tracked)
        references += count
        findings.extend({"path": relative, "message": error} for error in errors)
    findings.extend({"path": "LICENSE", "message": error} for error in plain_license_errors((ROOT / "LICENSE").read_text(encoding="utf-8")))
    media_errors, media_count = check_readme_media(tracked)
    findings.extend({"path": "docs/images/readme/manifest.json", "message": error} for error in media_errors)
    demo_errors, demo_count = check_readme_demo(tracked)
    findings.extend({"path": "docs/media/composition-receipt.json", "message": error} for error in demo_errors)
    report = {"checkedMarkdownFiles": len(paths), "checkedLinkReferences": references,
              "plainTextLicenseChecked": True, "publishedTargetInventoryChecked":True, "standardGplTextReformatted": False,
              "linkedOverviewBadgesChecked": True,
              "readmeMediaFilesChecked": media_count,
              "readmeDemoFilesChecked": demo_count,
              "demoBindingIsNotVisualOrQualityAcceptance": True,
              "findings": findings, "passed": not findings}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open("x", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return bool(findings)


if __name__ == "__main__":
    raise SystemExit(main())
