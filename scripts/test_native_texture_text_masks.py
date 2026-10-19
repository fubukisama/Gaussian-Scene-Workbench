"""CPU-only regressions for the public screenshot-analysis boundary.

These independent bitmap fixtures test discovery, not native rendering. Real
Windows framebuffer capture remains the texture-text acceptance gate.
"""
import unittest

import numpy as np

from test_native_texture_text import projection_text_mask


def bitmap_control(theme, scale=1, caption=True):
    width, height = 960 * scale, 540 * scale
    background, foreground = ((255, 80) if theme == "light" else (12, 180))
    image = np.full((height, width, 3), background, dtype=np.uint8)
    image[20 * scale:60 * scale, width - 100 * scale:width - 60 * scale] = (210, 30, 30)
    # A long thin world axis must not become the navigation's dense row gate.
    image[130 * scale:132 * scale, width - 250 * scale:width - 5 * scale] = (20, 180, 20)
    expected = np.zeros((height, width), dtype=bool)
    if caption:
        glyph = np.array([[1, 1, 1, 1, 0], [1, 0, 0, 0, 1],
                          [1, 0, 0, 0, 1], [1, 1, 1, 1, 0],
                          [1, 0, 0, 0, 0], [1, 0, 0, 0, 0],
                          [1, 0, 0, 0, 0]], dtype=bool)
        glyph = np.kron(glyph, np.ones((2 * scale, 2 * scale), dtype=bool))
        label = np.concatenate((glyph, np.zeros((len(glyph), 3 * scale), dtype=bool), glyph), axis=1)
        x, y = width - 120 * scale, 90 * scale
        expected[y:y + label.shape[0], x:x + label.shape[1]] = label
        image[expected] = foreground
    else:
        # A neutral square represents non-text mini-button artwork. It cannot
        # satisfy the missing caption simply by producing more than 50 pixels.
        image[90 * scale:106 * scale, width - 80 * scale:width - 64 * scale] = foreground
    return image, expected


class ProjectionCaptionMaskTests(unittest.TestCase):
    def test_neutral_caption_is_discovered_in_both_themes_and_dpi_scales(self):
        for theme in ("light", "dark"):
            for scale in (1, 2):
                with self.subTest(theme=theme, scale=scale):
                    image, expected = bitmap_control(theme, scale)
                    mask = projection_text_mask(image, scale, theme)
                    self.assertGreaterEqual(int(mask.sum()), 50 * scale * scale)
                    self.assertTrue(np.array_equal(mask, expected))

    def test_navigation_artwork_is_not_accepted_as_missing_projection_text(self):
        for theme in ("light", "dark"):
            with self.subTest(theme=theme):
                image, _ = bitmap_control(theme, caption=False)
                with self.assertRaisesRegex(AssertionError, "projection-state caption"):
                    projection_text_mask(image, 1, theme)


if __name__ == "__main__":
    unittest.main()
