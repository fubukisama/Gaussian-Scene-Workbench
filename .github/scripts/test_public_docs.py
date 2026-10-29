"""Remote regression checks for actual overview navigation and showcase guards."""
from pathlib import Path
import unittest
from unittest.mock import patch

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
        image = next(line for line in self.source.splitlines()
                     if line.startswith("![") and "docs/images/" in line)
        source = self.source.replace(image, "<details>\n<summary>Example</summary>\n\n" + image + "\n\n</details>", 1)
        self.assertTrue(any("outside folded details" in error for error in self.findings(source)))

    def test_image_alt_is_required(self):
        import re
        source, count = re.subn(r"!\[[^\]]+\]\((docs/images/[^)]+)\)", r"![](\1)", self.source, count=1)
        self.assertEqual(count, 1)
        self.assertTrue(any("image has no alt text" in error for error in self.findings(source)))

    def test_badge_destination_remains_actionable(self):
        source = self.source.replace("README_RELEASE.md#download-the-preview", "README_RELEASE.md#missing", 1)
        self.assertTrue(any("native_preview badge must link" in error for error in self.findings(source)))


if __name__ == "__main__":
    unittest.main()
