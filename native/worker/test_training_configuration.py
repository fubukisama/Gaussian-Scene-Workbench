"""Public runtime configuration and migration contract; no GPU or jobs."""
import unittest


class RuntimeConfigurationTests(unittest.TestCase):
    def test_legacy_recipe_wire_keeps_historical_requested_shape(self):
        from native.worker.training_configuration import legacy_performance_options
        self.assertEqual(legacy_performance_options({'enabled': False}), {
            'version': 1, 'enabled': False, 'usage_fraction': .9,
            'minimum_spare_mib': 512, 'gpu_slots': 2, 'pinned_mib': 256,
            'gpu_mib': 512, 'profile_interval': 30, 'engine': 'stock',
            'growth_mode': 'pause', 'loss_mode': 'auto', 'camera_prefetch': True,
            'capacity_storage': True, 'surfel_auxiliary': True})

    def test_prefetch_dependency_is_reported_without_rewriting_the_request(self):
        from native.worker.training_configuration import normalize_runtime_options
        options = {'version': 2, 'resourceGuard': {'enabled': False},
                   'execution': {'image_staging': False, 'camera_prefetch': True}}
        report = normalize_runtime_options(options)
        self.assertTrue(report['requested']['execution']['camera_prefetch'])
        self.assertFalse(report['effective']['execution']['camera_prefetch'])
        self.assertEqual(report['reasons'], [{'feature': 'execution.camera_prefetch',
                                             'code': 'requires_image_staging'}])
        self.assertTrue(options['execution']['camera_prefetch'])
        self.assertEqual(report['effective']['execution']['loss_mode'], 'auto')
        self.assertTrue(report['effective']['execution']['capacity_storage'])

    def test_resume_guard_only_override_preserves_checkpoint_execution(self):
        from native.worker.training_configuration import normalize_runtime_options
        checkpoint = normalize_runtime_options(legacy_performance={
            'enabled': False, 'engine': 'fastergs', 'profile_interval': 77})['requested']
        report = normalize_runtime_options({'version': 2, 'resourceGuard': {'enabled': True}},
                                           resume=True, checkpoint_runtime=checkpoint)
        self.assertTrue(report['requested']['resourceGuard']['enabled'])
        self.assertEqual(report['requested']['execution'], checkpoint['execution'])
        self.assertEqual(report['requested']['monitoring'], {'profile_interval': 77})
        unknown = normalize_runtime_options(resume=True)
        self.assertFalse(unknown['requested']['execution']['image_staging'])
        self.assertEqual(unknown['requested']['execution']['loss_mode'], 'stock')
        self.assertEqual(unknown['migration']['source'], 'legacy_resume_without_policy')

    def test_invalid_unknown_or_conflicting_configuration_is_rejected(self):
        from native.worker.training_configuration import normalize_runtime_options
        for value in ({'version': 1}, {'version': True}, {'version': 3},
                      {'version': 2, 'enabled': True},
                      {'version': 2, 'execution': {'magic': True}},
                      {'version': 2, 'resourceGuard': {'enabled': 1}},
                      {'version': 2, 'resourceGuard': {'usage_fraction': float('nan')}},
                      {'version': 2, 'resourceGuard': {'max_consecutive_denials': 0}},
                      {'version': 2, 'execution': {'gpu_slots': 3}}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                normalize_runtime_options(value)
        with self.assertRaisesRegex(ValueError, 'conflict'):
            normalize_runtime_options({'version': 2, 'resourceGuard': {'enabled': False}},
                                      legacy_performance={'enabled': True})
        for value in ({'enabled': 'false'}, {'enabled': False, 'usage_fraction': 1.0},
                      {'enabled': False, 'minimum_spare_mib': 0}, {'unknown': True}):
            with self.subTest(legacy=value), self.assertRaises(ValueError):
                normalize_runtime_options(legacy_performance=value)

    def test_guard_only_change_keeps_execution_and_monitoring_unchanged(self):
        from native.worker.training_configuration import normalize_runtime_options
        fresh = normalize_runtime_options()
        disabled = normalize_runtime_options({'version': 2, 'resourceGuard': {'enabled': False}})
        self.assertFalse(disabled['requested']['resourceGuard']['enabled'])
        self.assertEqual(disabled['requested']['execution'], fresh['requested']['execution'])
        self.assertEqual(disabled['requested']['monitoring'], {'profile_interval': 30})

    def test_legacy_disabled_migrates_actual_effective_features_without_reenabling(self):
        from native.worker.training_configuration import normalize_runtime_options
        report = normalize_runtime_options(legacy_performance={
            'version': 1, 'enabled': False, 'engine': 'fastergs',
            'loss_mode': 'auto', 'camera_prefetch': True,
            'capacity_storage': True, 'surfel_auxiliary': True})
        requested = report['requested']
        self.assertEqual(requested['version'], 2)
        self.assertFalse(requested['resourceGuard']['enabled'])
        self.assertEqual(requested['execution'], {
            'image_staging': False, 'camera_prefetch': False,
            'capacity_storage': False, 'surfel_auxiliary': False,
            'loss_mode': 'stock', 'engine': 'fastergs',
            'gpu_slots': 2, 'pinned_mib': 256, 'gpu_mib': 512})
        self.assertEqual(report['migration']['source'], 'performanceOptions')
        self.assertEqual(report['migration']['from_version'], 1)
        self.assertTrue(report['migration']['notes'])


if __name__ == '__main__':
    unittest.main()
