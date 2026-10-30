"""Remote regression checks for overview, changelog and demo-publication guards."""
from pathlib import Path
import hashlib
import json
import re
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import check_public_docs as docs


class OverviewChecks(unittest.TestCase):
    def setUp(self):
        self.target = docs.ROOT / "README.md"
        self.original_read = Path.read_text
        self.source = self.target.read_text(encoding="utf-8-sig")
        self.tracked = docs.tracked_files()

    def findings(self, source):
        original_read, target = self.original_read, self.target
        def read(path, *args, **kwargs):
            return source if path == target else original_read(path, *args, **kwargs)
        with patch.object(Path, "read_text", read):
            return docs.check_document("README.md", self.tracked)[0]

    def test_current_overview(self):
        self.assertEqual(self.findings(self.source), [])

    def test_missing_navigation_target(self):
        self.assertIn("## Why GSW", self.source)
        source = self.source.replace("## Why GSW", "## Renamed without updating navigation", 1)
        self.assertTrue(any("missing heading: #why-gsw" in error for error in self.findings(source)))

    def test_folded_showcase_is_not_an_unfolded_example(self):
        pattern = r'!\[[^\]]*\]\(docs/images/[^)]+\)|<img\b[^>]*src="docs/images/[^>]+>'
        def fold(match):
            return "<details>\n<summary>Example</summary>\n\n" + match[0] + "\n\n</details>"
        source, count = re.subn(pattern, fold, self.source)
        self.assertGreater(count, 0)
        self.assertTrue(any("outside folded details" in error for error in self.findings(source)))

    def test_gallery_can_keep_some_images_folded(self):
        pattern = r'<img\b[^>]*src="docs/images/[^>]+>'
        source, count = re.subn(pattern,
            lambda match: "<details>\n<summary>Example</summary>\n" + match[0] + "\n</details>",
            self.source, count=1)
        self.assertEqual(count, 1)
        self.assertEqual(self.findings(source), [])

    def test_image_alt_is_required(self):
        source, count = re.subn(r"!\[[^\]]+\]\((docs/images/[^)]+)\)", r"![](\1)", self.source, count=1)
        self.assertEqual(count, 1)
        self.assertTrue(any("image has no alt text" in error for error in self.findings(source)))

    def test_badge_destination_remains_actionable(self):
        source = self.source.replace("README_RELEASE.md#download-the-preview", "README_RELEASE.md#missing", 1)
        self.assertTrue(any("native_preview badge must link" in error for error in self.findings(source)))

    def test_actual_media_bindings(self):
        self.assertEqual(docs.check_readme_media(self.tracked), ([], 15))

    def test_media_pixel_file_change_is_rejected(self):
        manifest_path = docs.ROOT / "docs/images/readme/manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        target = docs.ROOT / manifest["images"][0]["path"]
        original_read = Path.read_bytes
        def read(path):
            return b"changed image" if path == target else original_read(path)
        with patch.object(Path, "read_bytes", read):
            errors, count = docs.check_readme_media(self.tracked)
        self.assertEqual(count, 15)
        self.assertTrue(any("file binding mismatch" in error for error in errors))


class ChangelogChecks(unittest.TestCase):
    def setUp(self):
        workspace = tempfile.TemporaryDirectory(prefix=".docs-changelog-fixture-", dir=docs.ROOT)
        self.addCleanup(workspace.cleanup)
        self.root = Path(workspace.name)
        self.tracked = set(docs.CHANGELOG_PAGES)
        self.sources = {}
        for relative, language in docs.CHANGELOG_PAGES.items():
            self.sources[relative] = (
                f'<div lang="{language}">\n\n# History\n\n'
                '[English](CHANGELOG.md) · [简体中文](CHANGELOG.zh-CN.md) · [日本語](CHANGELOG.ja.md)\n\n'
                '## 2026-10-30\n\n- Changed example. '
                '[Source](https://github.com/example/workbench/commit/' + '1' * 40 + ')\n\n'
                '## 2026-10-29\n\n- Fixed example. '
                '[Source](https://github.com/example/workbench/commit/' + '2' * 40 + ')\n\n</div>\n'
            )
            self.write(relative, self.sources[relative])
        localized_history = {language: relative for relative, language in docs.CHANGELOG_PAGES.items()}
        for overview, language in docs.LANGUAGE_PAGES.items():
            self.tracked.add(overview)
            self.sources[overview] = '<a href="' + localized_history[language] + '">History</a>\n'
            self.write(overview, self.sources[overview])

    def write(self, relative, text):
        (self.root / relative).write_text(text, encoding="utf-8")

    def findings(self):
        with patch.object(docs, "ROOT", self.root):
            return docs.check_changelogs(self.tracked)[0]

    def test_aligned_three_language_history(self):
        self.assertEqual(self.findings(), [])
        self.assertTrue(set(docs.CHANGELOG_PAGES).issubset(docs.PRIMARY))

    def test_missing_or_unpublished_locale_is_rejected(self):
        relative = "CHANGELOG.ja.md"
        (self.root / relative).unlink()
        self.assertTrue(any("missing or unpublished" in item["message"] for item in self.findings()))
        self.write(relative, self.sources[relative])
        self.tracked.remove(relative)
        self.assertTrue(any("missing or unpublished" in item["message"] for item in self.findings()))

    def test_locale_marker_must_match_the_document(self):
        relative = "CHANGELOG.zh-CN.md"
        self.write(relative, self.sources[relative].replace('lang="zh-CN"', 'lang="en"'))
        self.assertTrue(any("document-language container" in item["message"] for item in self.findings()))

    def test_language_navigation_cannot_disappear_or_exist_only_in_an_example(self):
        relative = "CHANGELOG.md"
        navigation = "[日本語](CHANGELOG.ja.md)"
        for replacement in ("日本語", "`" + navigation + "`", "<!-- " + navigation + " -->",
                            "\n```markdown\n" + navigation + "\n```\n"):
            with self.subTest(replacement=replacement):
                self.write(relative, self.sources[relative].replace(navigation, replacement))
                self.assertTrue(any("language navigation is incomplete" in item["message"] for item in self.findings()))

    def test_each_locale_requires_matching_date_coverage(self):
        relative = "CHANGELOG.ja.md"
        self.write(relative, self.sources[relative].replace("## 2026-10-29", "## 2026-10-28"))
        self.assertTrue(any("date/source-reference coverage" in item["message"] for item in self.findings()))

    def test_dates_cannot_be_empty_duplicated_or_oldest_first(self):
        relative = "CHANGELOG.md"
        for source in (
            self.sources[relative].replace("## 2026-10-30", "## Updated").replace("## 2026-10-29", "## Earlier"),
            self.sources[relative].replace("## 2026-10-29", "## 2026-10-30"),
            self.sources[relative].replace("## 2026-10-30", "## 2026-10-28"),
        ):
            with self.subTest(source=source):
                self.write(relative, source)
                self.assertTrue(any("nonempty, unique and newest first" in item["message"] for item in self.findings()))

    def test_source_references_are_compared_per_date_not_just_as_a_global_set(self):
        relative = "CHANGELOG.zh-CN.md"
        source = self.sources[relative].replace("1" * 40, "3" * 40).replace("2" * 40, "1" * 40).replace("3" * 40, "2" * 40)
        self.write(relative, source)
        self.assertTrue(any("date/source-reference coverage" in item["message"] for item in self.findings()))

    def test_all_locales_cannot_agree_on_history_without_source_references(self):
        for relative in docs.CHANGELOG_PAGES:
            self.write(relative, self.sources[relative].replace("1" * 40, "short").replace("2" * 40, "short"))
        self.assertTrue(any("requires at least one full source-commit reference" in item["message"]
                            for item in self.findings()))

    def test_each_overview_requires_a_real_localized_history_entry_point(self):
        for overview in docs.LANGUAGE_PAGES:
            original = self.sources[overview]
            for replacement in ("History", "`" + original.strip() + "`", "<!-- " + original.strip() + " -->",
                                "```html\n" + original + "```\n"):
                with self.subTest(overview=overview, replacement=replacement):
                    self.write(overview, replacement)
                    self.assertTrue(any(item["path"] == overview and "visible link to its localized changelog" in item["message"]
                                        for item in self.findings()))
            self.write(overview, original)


class DemoMediaChecks(unittest.TestCase):
    """Byte-level publication fixtures, not video decoding or visual acceptance."""

    def setUp(self):
        workspace = tempfile.TemporaryDirectory(prefix=".docs-demo-fixture-", dir=docs.ROOT)
        self.addCleanup(workspace.cleanup)
        self.root = Path(workspace.name)
        self.media = self.root / "docs/media"
        self.media.mkdir(parents=True)
        self.tracked = set()
        self.receipt_path = self.media / "composition-receipt.json"
        provenance = {
            "schema": "gsw.readme_training_public_provenance/1", "dataset_identity": "fixture-dataset",
            "training_source_commit": "1" * 40, "quality_or_accuracy_accepted": False,
            "physical_accuracy_accepted": False, "full_optimizer_resume_demonstrated": False,
            "test_images_feedback_into_optimization": False, "truth_depth_normals_mesh_used": False,
        }
        provenance_bytes = self.json_bytes(provenance)
        manifest = {
            "schema": "gsw.readme_training_demo/1", "dataset_identity": "fixture-dataset",
            "training_provenance_path": "training_provenance.json",
            "training_provenance_sha256": hashlib.sha256(provenance_bytes).hexdigest(),
        }
        manifest_bytes = self.json_bytes(manifest)
        bundle = self.media / "readme-training-demo.zip"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.writestr("demo_manifest.json", manifest_bytes)
            archive.writestr("training_provenance.json", provenance_bytes)
        self.tracked.add("docs/media/readme-training-demo.zip")
        self.write("docs/media/README.md", (
            "# Demo provenance\n\nZIP SHA256: `" + hashlib.sha256(bundle.read_bytes()).hexdigest() + "`.\n"
        ).encode("utf-8"))
        self.write("docs/media/training-provenance.json", provenance_bytes)
        # These intentionally exercise signatures only; they are not playable media.
        self.write("docs/media/gsw-native-operation.en.mp4", b"\x00\x00\x00\x18ftypisom\x00\x00\x02\x00isomiso2")
        gif_header = b"GIF89a\x80\x02\xc0\x01\x00\x00\x00;"
        for language, readme in (("en", "README.md"), ("zh-CN", "README.zh-CN.md"), ("ja", "README.ja.md")):
            self.write(f"docs/media/gsw-native-preview.{language}.gif", gif_header)
            self.write(f"docs/media/gsw-native-operation.{language}.srt",
                       f"1\n00:00:00,000 --> 00:00:01,000\nSnapshot replay ({language})\n".encode("utf-8"))
            self.write(readme, (
                f"![Preview](docs/media/gsw-native-preview.{language}.gif)\n\n"
                "[Video](docs/media/gsw-native-operation.en.mp4)\n\n"
                f"[Subtitles](docs/media/gsw-native-operation.{language}.srt)\n\n"
                "[Provenance](docs/media/README.md)\n"
            ).encode("utf-8"))
        observation = {"duration_seconds": 1.0, "width": 1600, "height": 936, "decoded_frames": 30}
        self.receipt = {
            "schema": "gsw.readme_demo_composition/1", "status": "composed_not_visually_accepted",
            "visual_review_required": True, "no_live_training_or_quality_benchmark_claim": True,
            "original_raw_unchanged": True,
            "inputs": {"training_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
                       "training_provenance_sha256": hashlib.sha256(provenance_bytes).hexdigest(),
                       "dataset_identity": "fixture-dataset"},
            "source_identity": {"training_source_commit": "1" * 40,
                                "training_and_recording_application_are_separate_identities": True},
            "continuous_video": {
                "file": "gsw-native-operation.en.mp4", "raw_observation": observation,
                "composed_observation": dict(observation, height=1024), "speed": 1.0,
                "ui_rectangle": [0, 0, 1600, 936], "external_footer_rectangle": [0, 936, 1600, 88],
                "ui_crop": False, "ui_scale": False, "frame_interpolation": False,
                "cuts": False, "audio_added": False,
            },
            "preview_gifs": {
                "dimensions": [640, 448], "speed_within_each_excerpt": 1.0,
                "selected_excerpts_not_continuous_capture": True,
                "localized_external_captions_not_native_ui_language_changes": True,
                "source_ranges": [{"source_start": 0.0, "source_end": 1.0,
                                   "output_start": 0.0, "output_end": 1.0}],
            },
            "chapter_timeline": [{"start": 0.0, "end": 1.0}],
            "files": [],
        }
        for name in sorted(path.name for path in self.media.iterdir() if path.suffix in (".mp4", ".gif", ".srt")
                           or path.name == "training-provenance.json"):
            self.refresh_binding(name)
        self.save_receipt()

    @staticmethod
    def json_bytes(value):
        return json.dumps(value, ensure_ascii=False, sort_keys=True).encode("utf-8")

    def write(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        self.tracked.add(relative)

    def refresh_binding(self, name):
        data = (self.media / name).read_bytes()
        self.receipt["files"] = [row for row in self.receipt["files"] if row["path"] != name]
        self.receipt["files"].append({"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})

    def save_receipt(self):
        self.write("docs/media/composition-receipt.json", self.json_bytes(self.receipt))

    def findings(self):
        with patch.object(docs, "ROOT", self.root):
            return docs.check_readme_demo(self.tracked)

    def test_bound_three_language_demo_is_not_visual_acceptance(self):
        self.assertEqual(self.findings(), ([], 8))

    def test_published_mp4_gif_and_srt_tampering_is_rejected(self):
        for name in ("gsw-native-operation.en.mp4", "gsw-native-preview.zh-CN.gif", "gsw-native-operation.ja.srt"):
            with self.subTest(name=name):
                path = self.media / name
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                self.assertTrue(any("file binding mismatch" in error for error in self.findings()[0]))
                path.write_bytes(original)

    def test_recording_receipt_is_required_when_an_overview_publishes_media(self):
        self.receipt_path.unlink()
        self.tracked.remove("docs/media/composition-receipt.json")
        self.assertTrue(any("references require" in error for error in self.findings()[0]))

    def test_no_recording_references_can_precede_the_optional_receipt(self):
        self.receipt_path.unlink()
        for readme in ("README.md", "README.zh-CN.md", "README.ja.md"):
            self.write(readme, b"# Existing overview without demo video\n")
        self.assertEqual(self.findings(), ([], 0))

    def test_inactive_examples_do_not_require_a_recording_receipt(self):
        self.receipt_path.unlink()
        for readme in ("README.md", "README.zh-CN.md", "README.ja.md"):
            self.write(readme, (
                "`[Example](docs/media/not-published.mp4)`\n\n"
                "```markdown\n![Example](docs/media/not-published.gif)\n```\n\n"
                "<!-- [Future video](docs/media/not-published.mp4) -->\n"
            ).encode("utf-8"))
        self.assertEqual(self.findings(), ([], 0))

    def test_a_nested_preview_link_can_target_the_continuous_video(self):
        self.write("README.md", (
            "[![Preview](docs/media/gsw-native-preview.en.gif)](docs/media/gsw-native-operation.en.mp4)\n\n"
            '[Subtitles](<docs/media/gsw-native-operation.en.srt> "English captions")\n\n'
            "[Provenance](docs/media/README.md)\n"
        ).encode("utf-8"))
        self.assertEqual(self.findings(), ([], 8))

    def test_each_overview_requires_its_own_gif_and_srt_even_when_other_languages_exist(self):
        path = self.root / "README.zh-CN.md"
        original = path.read_text(encoding="utf-8")
        for target in ("gsw-native-preview.zh-CN.gif", "gsw-native-operation.zh-CN.srt"):
            with self.subTest(target=target):
                path.write_text(original.replace(target, target.replace("zh-CN", "en")), encoding="utf-8")
                self.assertTrue(any("README.zh-CN.md lacks its localized" in error for error in self.findings()[0]))
        path.write_text(original, encoding="utf-8")

    def test_overview_video_and_provenance_links_cannot_silently_disappear(self):
        path = self.root / "README.ja.md"
        original = path.read_text(encoding="utf-8")
        for target in ("gsw-native-operation.en.mp4", "README.md"):
            with self.subTest(target=target):
                path.write_text(original.replace("docs/media/" + target, "docs/media/unrelated.txt"), encoding="utf-8")
                self.assertTrue(any("README.ja.md lacks its localized" in error for error in self.findings()[0]))
        path.write_text(original, encoding="utf-8")

    def test_media_inventory_rejects_traversal_absolute_paths_duplicates_and_extra_files(self):
        original = self.json_bytes(self.receipt)
        for value in ("../outside.mp4", "/outside.mp4", "D:/outside.mp4", self.receipt["files"][1]["path"]):
            with self.subTest(path=value):
                self.receipt["files"][0]["path"] = value
                self.save_receipt()
                self.assertTrue(any("file path inventory" in error for error in self.findings()[0]))
                self.receipt = json.loads(original)
        self.receipt["files"].append(dict(self.receipt["files"][0]))
        self.save_receipt()
        self.assertTrue(any("file path inventory" in error for error in self.findings()[0]))

    def test_existing_but_unpublished_receipt_or_media_is_rejected(self):
        for relative in ("docs/media/composition-receipt.json", "docs/media/gsw-native-operation.en.mp4"):
            with self.subTest(path=relative):
                self.tracked.remove(relative)
                self.assertTrue(any("unpublished" in error for error in self.findings()[0]))
                self.tracked.add(relative)

    def test_a_new_hash_does_not_turn_a_wrong_file_type_into_media(self):
        for name in ("gsw-native-operation.en.mp4", "gsw-native-preview.en.gif"):
            with self.subTest(path=name):
                path = self.media / name
                original = path.read_bytes()
                path.write_bytes(b"not the advertised media type")
                self.refresh_binding(name)
                self.save_receipt()
                self.assertTrue(any("type signature" in error for error in self.findings()[0]))
                path.write_bytes(original)
                self.refresh_binding(name)
                self.save_receipt()

    def test_rebound_srt_still_requires_utf8_numbering_valid_time_and_shared_timeline(self):
        name = "gsw-native-operation.en.srt"
        for content, expected in (
            (b"\xff\xfe", "codec"),
            (b"2\n00:00:00,000 --> 00:00:01,000\nReplay\n", "consecutive numbering"),
            (b"1\n00:60:00,000 --> 00:60:01,000\nReplay\n", "invalid timestamp"),
            (b"1\n00:00:00,900 --> 00:00:00,800\nReplay\n", "timing overlaps"),
            (b"1\n00:00:00,000 --> 00:00:02,000\nReplay\n", "escapes the video duration"),
            (b"1\n00:00:00,000 --> 00:00:00,500\nReplay\n", "shared chapter timeline"),
            (b"1\n00:00:00,000 --> 00:00:00,700\nReplay\n\n2\n00:00:00,600 --> 00:00:01,000\nReplay\n", "timing overlaps"),
        ):
            with self.subTest(content=content):
                self.write("docs/media/" + name, content)
                self.refresh_binding(name)
                self.save_receipt()
                self.assertTrue(any(expected in error for error in self.findings()[0]))

    def test_receipt_cannot_promote_visual_or_quality_acceptance_or_change_recording_speed(self):
        original = self.json_bytes(self.receipt)
        changes = (
            (None, "status", "passed"), (None, "visual_review_required", False),
            (None, "no_live_training_or_quality_benchmark_claim", False),
            (None, "original_raw_unchanged", False),
            ("continuous_video", "speed", 2.0), ("continuous_video", "speed", True),
            ("continuous_video", "cuts", True), ("continuous_video", "ui_crop", True),
            ("continuous_video", "ui_scale", True), ("continuous_video", "frame_interpolation", True),
            ("preview_gifs", "selected_excerpts_not_continuous_capture", False),
            ("preview_gifs", "localized_external_captions_not_native_ui_language_changes", False),
        )
        for section, key, value in changes:
            with self.subTest(section=section, key=key, value=value):
                target = self.receipt if section is None else self.receipt[section]
                target[key] = value
                self.save_receipt()
                self.assertTrue(self.findings()[0])
                self.receipt = json.loads(original)

    def test_excerpt_timing_cannot_overlap_escape_or_accelerate_the_source(self):
        original = self.json_bytes(self.receipt)
        for key, value in (("source_start", -0.1), ("source_end", 1.1), ("output_start", .1), ("output_end", .5)):
            with self.subTest(key=key):
                self.receipt["preview_gifs"]["source_ranges"][0][key] = value
                self.save_receipt()
                self.assertTrue(any("excerpt range" in error for error in self.findings()[0]))
                self.receipt = json.loads(original)

    def test_nonfinite_or_extreme_timings_report_findings_instead_of_crashing(self):
        for value in (float("nan"), float("inf"), 1e308, 10 ** 500):
            with self.subTest(value=value):
                self.receipt["continuous_video"]["raw_observation"]["duration_seconds"] = value
                self.receipt["continuous_video"]["composed_observation"]["duration_seconds"] = value
                self.save_receipt()
                self.assertTrue(self.findings()[0])

    def test_training_zip_has_an_independent_hash_binding(self):
        path = self.media / "readme-training-demo.zip"
        path.write_bytes(path.read_bytes() + b"changed archive")
        self.assertTrue(any("training ZIP hash" in error for error in self.findings()[0]))

    def test_rebound_training_zip_must_still_match_the_recorded_manifest(self):
        bundle = self.media / "readme-training-demo.zip"
        with zipfile.ZipFile(bundle) as archive:
            manifest = json.loads(archive.read("demo_manifest.json"))
            provenance = archive.read("training_provenance.json")
        manifest["dataset_identity"] = "different-dataset"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.writestr("demo_manifest.json", self.json_bytes(manifest))
            archive.writestr("training_provenance.json", provenance)
        self.write("docs/media/README.md", (
            "ZIP SHA256: `" + hashlib.sha256(bundle.read_bytes()).hexdigest() + "`.\n"
        ).encode("utf-8"))
        self.assertTrue(any("manifest/provenance/source identity differs" in error for error in self.findings()[0]))

    def test_even_consistently_rehashed_provenance_cannot_claim_accuracy_or_full_resume(self):
        provenance = json.loads((self.media / "training-provenance.json").read_bytes())
        provenance["physical_accuracy_accepted"] = True
        provenance_bytes = self.json_bytes(provenance)
        provenance_hash = hashlib.sha256(provenance_bytes).hexdigest()
        self.write("docs/media/training-provenance.json", provenance_bytes)
        self.refresh_binding("training-provenance.json")
        manifest_bytes = self.json_bytes({
            "schema": "gsw.readme_training_demo/1", "dataset_identity": "fixture-dataset",
            "training_provenance_path": "training_provenance.json", "training_provenance_sha256": provenance_hash,
        })
        bundle = self.media / "readme-training-demo.zip"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.writestr("demo_manifest.json", manifest_bytes)
            archive.writestr("training_provenance.json", provenance_bytes)
        self.write("docs/media/README.md", (
            "ZIP SHA256: `" + hashlib.sha256(bundle.read_bytes()).hexdigest() + "`.\n"
        ).encode("utf-8"))
        self.receipt["inputs"]["training_manifest_sha256"] = hashlib.sha256(manifest_bytes).hexdigest()
        self.receipt["inputs"]["training_provenance_sha256"] = provenance_hash
        self.save_receipt()
        self.assertTrue(any("must not claim quality/physical accuracy/full-state resume" in error for error in self.findings()[0]))


if __name__ == "__main__":
    unittest.main()
