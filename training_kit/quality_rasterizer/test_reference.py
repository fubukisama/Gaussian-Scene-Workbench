"""Public numerical seam tests; these do not certify CUDA execution."""
import unittest

from training_kit.quality_rasterizer.reference import aggregate_screen_gradients, composite_pixel


class QualityReferenceTests(unittest.TestCase):
    def test_opposing_pixel_gradients_cancel_only_in_signed_statistic(self):
        result = aggregate_screen_gradients([(1., -2.), (-1., 2.)])
        self.assertEqual(result, {"signed": [0., 0.], "absolute": [2., 4.]})

    def test_alpha_and_inverse_depth_moment_are_distinct(self):
        result = composite_pixel([.5, .25], [.5, .25], [(1., 0., 0.), (0., 1., 0.)])
        self.assertEqual(result['rgb'], [.5, .125, 0.])
        self.assertEqual(result['alpha'], .625)
        self.assertEqual(result['inverse_depth_moment'], .28125)
        self.assertEqual(result['contributor_pixels'], [1, 1])

    def test_unaccepted_alpha_is_not_a_contributing_pixel(self):
        result = composite_pixel([.001, .5, .999], [1., .5, .25], [(1., 0., 0.)] * 3)
        self.assertEqual(result['contributor_pixels'], [0, 1, 1])
        result = composite_pixel([.99, .99, .99], [1.] * 3, [(1., 0., 0.)] * 3)
        self.assertEqual(result['contributor_pixels'], [1, 1, 0])

    def test_depth_moment_over_alpha_has_quotient_not_moment_gradient(self):
        def objective(alpha):
            result = composite_pixel(alpha, [.5, .25], [(0., 0., 0.)] * 2)
            return result['inverse_depth_moment'] / result['alpha']
        # Independent worked example: quotient=.45, d/da0=.16, d/da1=-.16.
        self.assertAlmostEqual(objective([.5, .25]), .45)
        eps = 1e-6
        gradients = [(objective([.5+eps, .25])-objective([.5-eps, .25]))/(2*eps),
                     (objective([.5, .25+eps])-objective([.5, .25-eps]))/(2*eps)]
        self.assertAlmostEqual(gradients[0], .16, places=7)
        self.assertAlmostEqual(gradients[1], -.16, places=7)


if __name__ == '__main__':
    unittest.main()
