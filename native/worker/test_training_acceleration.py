"""Renderer selection is observable policy; no mock renderer or CUDA import."""
import copy
import unittest

from native.worker.training_acceleration import select_acceleration


def verified_report():
    return {"ready": True, "engine": "fastergs", "adapterVersion": 1,
            "upstreamCommit": "c60ea269b7cf3ebda527d640b6fdd31462f91b41",
            "checks": {"render": True, "backward": True, "train": True,
                       "sh0to3": True, "antialiasing": True, "exposureBackward": True},
            "sameEngineAdamStateRoundtrip": True}


class TrainingAccelerationTests(unittest.TestCase):
    def test_only_named_backends_and_engines_are_accepted(self):
        for backend, engine, options in (("mesh", "fastergs", {}),
                                        ("3dgs", "typo", {}),
                                        ("3dgs", "stock", None)):
            with self.subTest(backend=backend, engine=engine):
                with self.assertRaises(ValueError):
                    select_acceleration(backend, engine, options, verified_report())

    def test_verified_compatible_new_run_and_same_engine_resume_are_explicit(self):
        options = {"densify_until_iter": 0, "optimizer_type": "default", "sh_degree": 3,
                   "antialiasing": True, "exposure_compensation": True}
        for checkpoint in (None, "fastergs"):
            with self.subTest(checkpoint=checkpoint):
                result = select_acceleration("3dgs", "fastergs", options,
                                             verified_report(), checkpoint_engine=checkpoint)
                self.assertEqual(result["effective"], "fastergs")
                self.assertEqual(result["reasons"], [])
        bad_pin = verified_report()
        bad_pin["upstreamCommit"] = "different"
        self.assertEqual(select_acceleration("3dgs", "fastergs", options,
                                            bad_pin)["effective"], "stock")

    def test_resume_may_not_silently_switch_renderer_or_lose_full_state(self):
        options = {"densify_until_iter": 0, "optimizer_type": "default", "resume": True}
        for engine in (None, "stock", "2dgs"):
            with self.subTest(engine=engine):
                result = select_acceleration("3dgs", "fastergs", options,
                    verified_report(), checkpoint_engine=engine)
                self.assertEqual(result["effective"], "stock")
                self.assertIn("fastergs_resume_engine_not_compatible", result["reasons"])
        report = verified_report()
        report["sameEngineAdamStateRoundtrip"] = False
        result = select_acceleration("3dgs", "fastergs", options, report,
                                     checkpoint_engine="fastergs")
        self.assertEqual(result["effective"], "stock")
        self.assertIn("fastergs_optimizer_resume_not_verified", result["reasons"])

    def test_unsupported_training_features_explain_stock_fallback(self):
        unsupported = (
            ({"densify_until_iter": 15000}, "fastergs_requires_fixed_density"),
            ({"optimizer_type": "sparse_adam"}, "fastergs_sparse_adam_not_supported"),
            ({"requires_depth": True}, "fastergs_depth_not_supported"),
            ({"requires_normals": True}, "fastergs_normals_not_supported"),
            ({"requires_screen_radii": True}, "fastergs_screen_radii_not_supported"),
            ({"compute_cov3D_python": True}, "fastergs_precomputed_covariance_not_supported"),
            ({"convert_SHs_python": True}, "fastergs_precomputed_color_not_supported"),
            ({"override_color": [1, 0, 0]}, "fastergs_precomputed_color_not_supported"),
            ({"debug": True}, "fastergs_debug_not_supported"),
            ({"sh_degree": 4}, "fastergs_sh_degree_not_supported"),
            ({"custom_activations": True}, "fastergs_custom_activations_not_supported"),
        )
        for additions, reason in unsupported:
            with self.subTest(options=additions):
                options = {"densify_until_iter": 0, "optimizer_type": "default", "sh_degree": 3}
                options.update(additions)
                before = copy.deepcopy(options)
                result = select_acceleration("3dgs", "fastergs", options, verified_report())
                self.assertEqual(result["effective"], "stock")
                self.assertIn(reason, result["reasons"])
                self.assertEqual(options, before)

    def test_import_success_without_kernel_and_training_proof_is_not_ready(self):
        options = {"densify_until_iter": 0, "optimizer_type": "default"}
        for report in (None, {"ready": True}, {"ready": True, "imported": True}):
            with self.subTest(report=report):
                result = select_acceleration("3dgs", "fastergs", options, report)
                self.assertEqual(result["effective"], "stock")
                self.assertIn("fastergs_runtime_not_verified", result["reasons"])

    def test_stock_is_default_for_both_training_backends(self):
        for backend in ("3dgs", "2dgs"):
            with self.subTest(backend=backend):
                result = select_acceleration(backend, "stock", {})
                self.assertEqual(result["effective"], "stock")
                self.assertEqual(result["reasons"], [])

    def test_surfel_training_never_selects_3d_fastgs(self):
        options = {"densify_until_iter": 0, "optimizer_type": "default"}
        before = copy.deepcopy(options)
        result = select_acceleration("2dgs", "fastergs", options)
        self.assertEqual(result["effective"], "stock")
        self.assertIn("fastergs_surfel_not_supported", result["reasons"])
        self.assertEqual(options, before)


if __name__ == "__main__":
    unittest.main()
