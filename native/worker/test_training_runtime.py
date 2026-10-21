import os
import unittest
from native.worker.training_runtime import performance_configuration, configure_image_environment, observed_memory, GrowthQuality, validate_resume_renderer
from types import SimpleNamespace


class RuntimeTests(unittest.TestCase):
    def test_guard_and_optimization_matrix_is_independent_for_both_backends(self):
        from native.worker.training_runtime import runtime_configuration
        for backend in ('3dgs', '2dgs'):
            for guard in (True, False):
                for optimize in (True, False):
                    wire = {'version': 2, 'resourceGuard': {'enabled': guard},
                            'execution': {'image_staging': optimize, 'camera_prefetch': optimize,
                                          'capacity_storage': optimize, 'surfel_auxiliary': optimize,
                                          'loss_mode': 'auto' if optimize else 'stock'}}
                    with self.subTest(backend=backend, guard=guard, optimize=optimize):
                        report = runtime_configuration({'backend': backend, 'runtimeOptions': wire})
                        self.assertEqual(report['requested']['resourceGuard']['enabled'], guard)
                        self.assertEqual(report['effective']['execution']['image_staging'], optimize)
                        self.assertEqual(report['effective']['execution']['capacity_storage'], optimize)
                        self.assertEqual(report['requested']['execution']['surfel_auxiliary'], optimize)
                        self.assertEqual(report['effective']['execution']['surfel_auxiliary'], optimize and backend == '2dgs')
                        self.assertEqual(report['effective']['execution']['loss_mode'], 'auto' if optimize else 'stock')
                        self.assertEqual(report['effective']['execution']['engine'], 'stock')

    def test_entry_migration_is_retained_without_becoming_an_execution_override(self):
        from native.worker.training_runtime import NativeTrainingRuntime
        from native.worker.training_configuration import normalize_runtime_options
        entry = normalize_runtime_options(legacy_performance={'enabled': False})
        runtime = NativeTrainingRuntime({'runtimeOptions': entry['requested'],
            'runtimeConfigurationReport': entry},
            SimpleNamespace(cuda=SimpleNamespace(is_available=lambda: False)),
            lambda *args: None, '2dgs')
        state = {}
        runtime.capture(state)
        self.assertEqual(state['runtime_configuration']['entry_migration'], entry)
        self.assertFalse(state['runtime_options']['execution']['image_staging'])
        changed = dict(entry, effective=normalize_runtime_options()['effective'])
        with self.assertRaisesRegex(ValueError, 'entry_configuration_report'):
            NativeTrainingRuntime({'runtimeOptions': entry['requested'],
                'runtimeConfigurationReport': changed},
                SimpleNamespace(cuda=SimpleNamespace(is_available=lambda: False)),
                lambda *args: None, '2dgs')

    def test_full_state_runtime_override_records_before_after_and_iteration(self):
        from native.worker.training_runtime import NativeTrainingRuntime
        from native.worker.training_configuration import normalize_runtime_options
        before = normalize_runtime_options({'version': 2, 'execution': {'image_staging': False}})['requested']
        after = normalize_runtime_options({'version': 2, 'resourceGuard': {'enabled': False}},
                                           checkpoint_runtime=before, resume=True)['requested']
        events = []
        runtime = NativeTrainingRuntime({'runtimeOptions': after},
            SimpleNamespace(cuda=SimpleNamespace(is_available=lambda: False)),
            lambda kind, value: events.append((kind, value)), '3dgs')
        runtime.restore_quality({'iteration': 6240, 'runtime_options': before,
                                 'growth_quality': {'consecutive_deferred': 2}})
        state = {}
        runtime.capture(state)
        self.assertEqual(state['runtime_policy_history'], [{
            'iteration': 6240, 'before': before, 'after': after,
            'reason': 'explicit_resume_runtime_override'}])
        self.assertEqual(state['runtime_options']['execution'], before['execution'])
        resumed = NativeTrainingRuntime({'runtimeOptions': after},
            SimpleNamespace(cuda=SimpleNamespace(is_available=lambda: False)),
            lambda *args: None, '3dgs')
        state['iteration'] = 6300
        resumed.restore_quality(state)
        second = {}
        resumed.capture(second)
        self.assertEqual(second['runtime_policy_history'], state['runtime_policy_history'])

    def test_disabled_guard_bypasses_unavailable_probe_without_disabling_staging(self):
        from native.worker.training_runtime import NativeTrainingRuntime
        def forbidden_probe():
            raise AssertionError('disabled guard must not query device memory')
        external_torch = SimpleNamespace(
            cuda=SimpleNamespace(is_available=lambda: False, mem_get_info=forbidden_probe),
            device=lambda value: SimpleNamespace(type=value))
        for backend in ('3dgs', '2dgs'):
            events = []
            runtime = NativeTrainingRuntime({'runtimeOptions': {
                'version': 2, 'resourceGuard': {'enabled': False}}},
                external_torch, lambda kind, value: events.append((kind, value)), backend)
            runtime.report_images([])
            self.assertTrue(next(value for kind, value in events
                                 if kind == '[gsw-training-image-cache]')['bounded'])
            self.assertTrue(runtime.admit(object(), .1, 1., 5))
            self.assertFalse(runtime.pause_for_resources())
            growth = next(value for kind, value in events if kind == '[gsw-training-growth]')
            self.assertEqual(growth['state'], 'bypassed')
            self.assertEqual(growth['reason'], 'guard_disabled')
            self.assertFalse(growth['observation_valid'])
            state = {}
            runtime.capture(state)
            self.assertTrue(state['runtime_options']['execution']['image_staging'])
            self.assertFalse(state['runtime_options']['resourceGuard']['enabled'])

    def test_resource_pause_threshold_is_configurable_and_retains_failure_reason(self):
        quality = GrowthQuality('pause', max_consecutive_denials=2)
        quality.observe(False, 'probe_unavailable')
        self.assertFalse(quality.should_pause())
        quality.observe(False, 'probe_unavailable')
        self.assertTrue(quality.should_pause())
        self.assertEqual(quality.snapshot()['last_reason'], 'probe_unavailable')
        deferred = GrowthQuality('defer', max_consecutive_denials=1)
        deferred.observe(False, 'estimate_unavailable')
        self.assertFalse(deferred.should_pause())

    def test_guard_off_keeps_requested_staging_and_other_execution_features(self):
        value = performance_configuration({'runtimeOptions': {
            'version': 2, 'resourceGuard': {'enabled': False}}})
        self.assertFalse(value['resource_guard_enabled'])
        self.assertTrue(value['image_staging'])
        self.assertEqual(value['loss_mode'], 'auto')
        self.assertTrue(value['camera_prefetch'])
        self.assertTrue(value['capacity_storage'])
        environment = {}
        configure_image_environment(value, environment)
        self.assertEqual(environment['GSW_TRAINING_IMAGE_PIPELINE'], 'bounded')

    def test_loss_execution_record_starts_unobserved_and_retains_fallback_changes(self):
        from native.worker.training_runtime import LossExecutionRecord
        record = LossExecutionRecord('fused_l1_ssim')
        self.assertEqual(record.snapshot()['last_effective'], 'unobserved')
        record.observe(1, {'effective': 'fused_l1_ssim', 'reason': ''})
        record.observe(2, {'effective': 'stock', 'reason': 'fused_loss_input_not_supported'})
        record.observe(3, {'effective': 'fused_l1_ssim', 'reason': ''})
        value = record.snapshot()
        self.assertEqual(value['counts'], {'fused_l1_ssim': 2, 'stock': 1})
        self.assertEqual([item['iteration'] for item in value['transitions']], [1, 2, 3])
        self.assertEqual(value['last_effective'], 'fused_l1_ssim')

    def test_loss_execution_history_survives_full_state_resume_and_explicit_mode_change(self):
        from native.worker.training_runtime import LossExecutionRecord
        previous = LossExecutionRecord('fused_l1_ssim')
        previous.observe(1, {'effective': 'fused_l1_ssim', 'reason': ''})
        previous.observe(2, {'effective': 'stock', 'reason': 'fused_loss_input_not_supported'})
        saved = previous.snapshot()
        resumed = LossExecutionRecord('stock', saved)
        self.assertEqual(resumed.snapshot()['counts'], {'fused_l1_ssim': 1, 'stock': 1})
        self.assertEqual(resumed.snapshot()['requested_modes'], ['fused_l1_ssim', 'stock'])
        self.assertEqual(resumed.snapshot()['session_last_effective'], 'unobserved')
        resumed.observe(3, {'effective': 'stock', 'reason': ''})
        result = resumed.snapshot()
        self.assertEqual(result['requested'], 'stock')
        self.assertEqual(result['counts'], {'fused_l1_ssim': 1, 'stock': 2})
        self.assertEqual([item['iteration'] for item in result['transitions']], [1, 2, 3])
        self.assertFalse(result['prior_history_unavailable'])
        self.assertEqual(result['session_observations'], 1)
        self.assertEqual(saved['counts'], {'fused_l1_ssim': 1, 'stock': 1})

    def test_legacy_checkpoint_without_loss_history_is_explicitly_unknown_not_invented(self):
        from native.worker.training_runtime import LossExecutionRecord
        fresh = LossExecutionRecord('stock').snapshot()
        legacy = LossExecutionRecord('stock', None)
        self.assertFalse(fresh['prior_history_unavailable'])
        self.assertTrue(legacy.snapshot()['prior_history_unavailable'])
        self.assertEqual(legacy.snapshot()['counts'], {})
        self.assertEqual(legacy.snapshot()['last_effective'], 'unobserved')
        legacy.observe(4408, {'effective': 'stock', 'reason': ''})
        self.assertEqual(legacy.snapshot()['counts'], {'stock': 1})
        self.assertTrue(legacy.snapshot()['prior_history_unavailable'])

    def test_invalid_loss_history_does_not_block_model_resume_or_invent_previous_counts(self):
        from copy import deepcopy
        from native.worker.training_runtime import LossExecutionRecord
        previous = LossExecutionRecord('stock')
        previous.observe(1, {'effective': 'stock', 'reason': ''})
        good = previous.snapshot()
        invalid = ['invalid', {}, {'counts': {'stock': 5}}]
        for key, value in (('counts', {'stock': True}), ('counts', {'stock': float('nan')}),
                           ('last_effective', 'unknown'), ('requested_modes', ['unknown']),
                           ('history_truncated', 'false'),
                           ('transitions', [{'iteration': True, 'effective': 'stock', 'reason': ''}])):
            state = deepcopy(good)
            state[key] = value
            invalid.append(state)
        for state in invalid:
            with self.subTest(state=state):
                restored = LossExecutionRecord('fused_l1_ssim', state)
                result = restored.snapshot()
                self.assertTrue(result['history_invalid'])
                self.assertTrue(result['prior_history_unavailable'])
                self.assertEqual(result['counts'], {})
                restored.observe(2, {'effective': 'fused_l1_ssim', 'reason': ''})
                self.assertEqual(restored.snapshot()['counts'], {'fused_l1_ssim': 1})

    def test_first_compute_and_observed_engine_changes_are_explicit_transition_events(self):
        from native.worker.training_runtime import LossExecutionRecord
        record = LossExecutionRecord('stock')
        self.assertTrue(record.observe(1, {'effective': 'stock', 'reason': ''}))
        self.assertFalse(record.observe(2, {'effective': 'stock', 'reason': ''}))
        resumed = LossExecutionRecord('stock', record.snapshot())
        self.assertTrue(resumed.observe(3, {'effective': 'stock', 'reason': ''}))
        self.assertFalse(resumed.observe(4, {'effective': 'stock', 'reason': ''}))
        self.assertTrue(resumed.observe(5, {'effective': 'fused_l1_ssim', 'reason': ''}))
        self.assertEqual(resumed.snapshot()['counts'], {'stock': 4, 'fused_l1_ssim': 1})

    def test_restored_loss_transition_history_is_bounded_without_losing_observed_counts(self):
        from native.worker.training_runtime import LossExecutionRecord
        transitions = [{'iteration': index, 'effective': 'stock' if index % 2 else 'fused_l1_ssim',
                        'reason': ''} for index in range(1, 301)]
        state = {'requested': 'fused_l1_ssim', 'last_effective': 'fused_l1_ssim',
                 'counts': {'stock': 150, 'fused_l1_ssim': 150},
                 'transitions': transitions, 'history_truncated': False}
        record = LossExecutionRecord('stock', state)
        self.assertEqual(len(record.snapshot()['transitions']), 256)
        self.assertTrue(record.snapshot()['history_truncated'])
        record.observe(301, {'effective': 'stock', 'reason': ''})
        result = record.snapshot()
        self.assertEqual(len(result['transitions']), 256)
        self.assertEqual(result['transitions'][0]['iteration'], 46)
        self.assertEqual(result['counts'], {'stock': 151, 'fused_l1_ssim': 150})
        self.assertEqual(len(state['transitions']), 300)

    def test_native_defaults_are_aggressive_but_bounded(self):
        value = performance_configuration({'backend': '3dgs'})
        self.assertTrue(value['enabled'])
        self.assertEqual(value['usage_fraction'], .9)
        self.assertEqual(value['minimum_spare_mib'], 512)
        self.assertEqual(value['engine'], 'stock')
        self.assertEqual(value['gpu_slots'], 2)

    def test_exact_storage_staging_features_are_individually_controllable(self):
        value = performance_configuration({})
        for key in ('camera_prefetch', 'capacity_storage', 'surfel_auxiliary'):
            self.assertTrue(value[key])
            disabled = performance_configuration({'performance': {key: False}})
            self.assertFalse(disabled[key])
            with self.assertRaises(ValueError):
                performance_configuration({'performance': {key: 'false'}})

    def test_execution_profile_reports_settings_not_a_quality_guarantee(self):
        from native.worker.training_runtime import execution_quality_profile
        profile = execution_quality_profile('2dgs',
            {'resolution': 1}, {'iterations': 30000, 'lambda_normal': .05},
            {'depth_ratio': 0.0}, performance_configuration({}),
            {'cameras': 59, 'dimensions': [[3804, 2142]]}, 'stock', 'fused_l1_ssim')
        self.assertEqual(profile['validation']['status'], 'unverified')
        self.assertEqual(profile['validation']['training_metric_scope'], 'training_fit')
        self.assertEqual(profile['settings']['optimization']['lambda_normal'], .05)
        self.assertEqual(profile['settings']['dataset']['resolution'], 1)
        self.assertEqual(profile['execution']['renderer'], 'stock')
        self.assertNotIn('quality_percent', profile)

    def test_legacy_resume_keeps_legacy_image_contract(self):
        value = performance_configuration({'resume': True, 'backend': '2dgs'})
        self.assertFalse(value['enabled'])
        environment = {'GSW_TRAINING_IMAGE_PIPELINE': 'bounded'}
        configure_image_environment(value, environment)
        self.assertNotIn('GSW_TRAINING_IMAGE_PIPELINE', environment)

    def test_checkpoint_policy_can_be_restored_without_new_optimizer_parameters(self):
        old = performance_configuration({'performance': {'enabled': True, 'usage_fraction': .85}})
        restored = performance_configuration({'resume': True}, checkpoint_performance=old)
        self.assertEqual(restored, old)
        for bad in ({'usage_fraction': float('nan')}, {'gpu_slots': 3}, {'engine': 'magic'}):
            with self.assertRaises(ValueError):
                performance_configuration({'performance': bad})

    def test_only_wholly_inactive_allocator_storage_is_classified_as_reusable(self):
        cuda = SimpleNamespace(mem_get_info=lambda: (1000, 10000), memory_stats=lambda: {
            'reserved_bytes.all.current': 4000, 'active_bytes.all.current': 1000,
            'inactive_split_bytes.all.current': 1000})
        observation = observed_memory(SimpleNamespace(cuda=cuda))
        self.assertEqual(observation.free_bytes, 1000)
        self.assertEqual(observation.reusable_cache_bytes, 2000)
        cuda.memory_stats = lambda: {'reserved_bytes.all.current': 4000}
        self.assertEqual(observed_memory(SimpleNamespace(cuda=cuda)).reusable_cache_bytes, 0)

    def test_memory_observation_keeps_device_and_allocator_quantities_distinct(self):
        cuda = SimpleNamespace(mem_get_info=lambda: (1000, 10000), memory_stats=lambda: {
            'allocated_bytes.all.current': 900, 'reserved_bytes.all.current': 4000,
            'active_bytes.all.current': 1000, 'inactive_split_bytes.all.current': 1000})
        result = observed_memory(SimpleNamespace(cuda=cuda))
        self.assertEqual(result.free_bytes, 1000)
        self.assertEqual(result.allocated_bytes, 900)
        self.assertEqual(result.reserved_bytes, 4000)
        self.assertEqual(result.active_bytes, 1000)
        self.assertEqual(result.inactive_split_bytes, 1000)
        self.assertEqual(result.reusable_cache_bytes, 2000)
        self.assertEqual(result.allocator_status, 'known')


    def test_quality_priority_pauses_after_sustained_pressure_and_restores_counter(self):
        quality = GrowthQuality('pause')
        quality.observe(False)
        quality.observe(False)
        self.assertFalse(quality.should_pause())
        restored = GrowthQuality('pause', quality.snapshot())
        restored.observe(False)
        self.assertTrue(restored.should_pause())
        resumed = GrowthQuality('pause', restored.snapshot())
        self.assertFalse(resumed.should_pause(), 'a resumed process must re-evaluate resources before auto-pause')
        resumed.observe(False)
        self.assertTrue(resumed.should_pause())
        restored.observe(True)
        self.assertFalse(restored.should_pause())
        self.assertEqual(restored.snapshot()['deferred_events'], 3)
        continued = GrowthQuality('defer')
        for _ in range(10):
            continued.observe(False)
        self.assertFalse(continued.should_pause())

    def test_full_state_resume_does_not_silently_change_renderer(self):
        validate_resume_renderer(None, 'stock')
        validate_resume_renderer('stock', 'stock')
        validate_resume_renderer('fastergs', 'fastergs')
        for previous, effective in [('fastergs', 'stock'), ('stock', 'fastergs')]:
            with self.assertRaises(ValueError):
                validate_resume_renderer(previous, effective)


if __name__ == '__main__':
    unittest.main()
