import os
import unittest
from native.worker.training_runtime import performance_configuration, configure_image_environment, observed_memory, GrowthQuality, validate_resume_renderer
from types import SimpleNamespace


class RuntimeTests(unittest.TestCase):
    def test_native_defaults_are_aggressive_but_bounded(self):
        value = performance_configuration({'backend': '3dgs'})
        self.assertTrue(value['enabled'])
        self.assertEqual(value['usage_fraction'], .9)
        self.assertEqual(value['minimum_spare_mib'], 512)
        self.assertEqual(value['engine'], 'stock')
        self.assertEqual(value['gpu_slots'], 2)

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
