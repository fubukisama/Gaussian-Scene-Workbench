"""Public quality-plan contract, independent of GUI, Torch and runtime options."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from native.worker.training_quality_plan import QualityPlanError, resolve_quality_plan, geometry_rate_overrides


class QualityPlanTests(unittest.TestCase):
    def test_absent_plan_preserves_standard_training_without_other_overrides(self):
        job = {"backend": "3dgs", "resolution": 1, "iterations": 15000,
               "runtimeOptions": {"version": 2, "execution": {"engine": "stock"}},
               "posePolicy": "locked", "antialiasing": True}
        original = copy.deepcopy(job)
        result = resolve_quality_plan(None, backend=job["backend"])
        self.assertEqual(result["effective"]["densification"], {"strategy": "standard", "parameters": {}})
        self.assertEqual(result["effective"]["filter"], {"mode": "legacy_ewa"})
        self.assertEqual(result["training_overrides"], {})
        self.assertEqual(job, original)

    def test_equivalent_partial_requests_have_one_stable_identity_and_are_not_mutated(self):
        request = {"densification": {"parameters": {}, "strategy": "standard"}, "version": 1}
        original = copy.deepcopy(request)
        explicit = resolve_quality_plan(request)
        self.assertEqual(explicit["plan_hash"], resolve_quality_plan(None)["plan_hash"])
        self.assertEqual(request, original)
        self.assertFalse(explicit["quality_accepted"])

    def test_declared_geometry_multiplier_changes_only_position_and_scale_rates(self):
        request = {"version": 1, "geometry_lr_multiplier": .3}
        options = {"position_lr_init": .2, "position_lr_final": .02, "scaling_lr": .5,
                   "position_lr_delay_mult": .01, "feature_lr": .003, "densify_grad_threshold": .0002}
        before = dict(options)
        result = resolve_quality_plan(request)
        self.assertEqual(geometry_rate_overrides(result, options), {
            "position_lr_init": .06, "position_lr_final": .006, "scaling_lr": .15})
        self.assertEqual(options, before)
        self.assertEqual(geometry_rate_overrides(resolve_quality_plan(None), options), {})

    def test_invalid_or_unavailable_quality_requests_are_not_silently_replaced(self):
        requests = [[], {"version": True}, {"version": 2}, {"version": 1, "resolution": 8},
                    {"version": 1, "densification": {"strategy": "unknown"}},
                    {"version": 1, "densification": {"strategy": "mcmc"}}]
        requests.extend({"version": 1, "geometry_lr_multiplier": value}
                        for value in (True, 0, -1, "0.3", float("nan"), float("inf"), 10 ** 400))
        for request in requests:
            with self.subTest(request=str(request)), self.assertRaises(QualityPlanError):
                resolve_quality_plan(request)

    def test_two_dgs_uses_its_own_baseline_filter_and_same_geometry_rate_contract(self):
        result = resolve_quality_plan({"version": 1, "geometry_lr_multiplier": .3}, backend="2dgs")
        self.assertEqual(result["effective"]["filter"], {"mode": "surfel_default"})
        self.assertNotEqual(result["plan_hash"], resolve_quality_plan({"version": 1, "geometry_lr_multiplier": .3})["plan_hash"])
        with self.assertRaisesRegex(QualityPlanError, "quality_filter_unsupported_backend"):
            resolve_quality_plan({"version": 1, "filter": {"mode": "legacy_ewa"}}, backend="2dgs")

    def test_explicit_backend_baseline_is_equivalent_to_default_filter(self):
        result = resolve_quality_plan({"version": 1, "filter": {"mode": "legacy_ewa"}})
        self.assertEqual(result["plan_hash"], resolve_quality_plan()["plan_hash"])

    def test_worker_admission_uses_the_same_plan_and_preserves_other_settings(self):
        from native.worker.gsw_worker import load_configuration
        base = Path(__file__).resolve().parents[2] / "native/dist/quality-plan-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            path = Path(directory) / "job.json"
            for backend in ("3dgs", "2dgs"):
                config = {"repositoryRoot": "repo", "datasetPath": "input", "outputRoot": "output",
                          "outputScene": "owned", "backend": backend, "quality": "quick",
                          "nativeCheckpoint": True, "posePolicy": "locked",
                          "trainOptions": {"iterations": 15000, "resolution": 1},
                          "qualityPlan": {"version": 1, "geometry_lr_multiplier": .3}}
                path.write_text(json.dumps(config), encoding="utf-8")
                admitted = load_configuration(path)
                self.assertEqual(admitted["qualityPlan"], resolve_quality_plan(config["qualityPlan"], backend=backend)["requested"])
                for key in ("quality", "posePolicy", "trainOptions", "nativeCheckpoint"):
                    self.assertEqual(admitted[key], config[key])
                config["qualityPlan"]["densification"] = {"strategy": "abs_gs"}
                path.write_text(json.dumps(config), encoding="utf-8")
                if backend == '2dgs':
                    with self.assertRaisesRegex(QualityPlanError, "quality_strategy_unsupported_backend"):
                        load_configuration(path)
                else:
                    self.assertEqual(load_configuration(path)['qualityPlan']['densification']['strategy'], 'abs_gs')

    def test_methods_are_explicit_experimental_requests_not_quality_certificates(self):
        for strategy in ('abs_gs', 'pixel_gs'):
            plan = resolve_quality_plan({'version': 1, 'densification': {'strategy': strategy}})
            self.assertEqual(plan['effective']['densification']['strategy'], strategy)
            self.assertFalse(plan['quality_accepted'])
            with self.assertRaisesRegex(QualityPlanError, 'unsupported_backend'):
                resolve_quality_plan(plan['requested'], backend='2dgs')
        mip = resolve_quality_plan({'version': 1, 'filter': {'mode': 'full_mip'}})
        self.assertEqual(mip['effective']['filter']['pixel_variance'], .2)
        with self.assertRaisesRegex(QualityPlanError, 'combination_unvalidated'):
            resolve_quality_plan({'version': 1, 'densification': {'strategy': 'abs_gs'},
                                  'filter': {'mode': 'full_mip'}})

    def test_supervision_identity_is_shared_and_invalid_parameters_fail(self):
        request = {'version': 1, 'supervision': {'mask': {'manifest': 'D:/owned/masks.json',
            'foreground_weight': 1., 'background_alpha_weight': .1}}}
        for backend in ('3dgs', '2dgs'):
            plan = resolve_quality_plan(request, backend=backend)
            self.assertEqual(plan['effective']['supervision']['mask']['manifest'], 'D:/owned/masks.json')
        request['supervision']['mask']['foreground_weight'] = -1
        with self.assertRaisesRegex(QualityPlanError, 'supervision'):
            resolve_quality_plan(request)


if __name__ == "__main__":
    unittest.main()
