import unittest

from native.worker.training_step_profile import IterationProfiler, normalize_performance
from native.worker.gsw_worker import status_telemetry


class Clock:
    def __init__(self):
        self.value = 0.

    def __call__(self):
        return self.value

    def advance(self, seconds):
        self.value += seconds


class StepProfileTests(unittest.TestCase):
    def test_complete_step_includes_post_backward_work(self):
        clock = Clock()
        profiler = IterationProfiler('3dgs', interval=30, clock=clock)
        profiler.begin(30)
        with profiler.stage('transfer'):
            clock.advance(.010)
        with profiler.stage('render'):
            clock.advance(.020)
        with profiler.stage('backward'):
            clock.advance(.030)
        with profiler.stage('optimizer'):
            clock.advance(.040)
        with profiler.stage('preview'):
            clock.advance(.020)
        sample = profiler.finish()
        self.assertAlmostEqual(sample['wall_step_ms'], 120.)
        self.assertAlmostEqual(sample['stages_ms']['optimizer'], 40.)
        self.assertEqual(sample['iteration'], 30)
        self.assertEqual(sample['backend'], '3dgs')

    def test_sampling_does_not_fabricate_gpu_times(self):
        clock = Clock()
        profiler = IterationProfiler('2dgs', interval=30, clock=clock)
        profiler.begin(2)
        with profiler.stage('loss'):
            clock.advance(.010)
        self.assertIsNone(profiler.finish())
        profiler.begin(30)
        with profiler.stage('loss'):
            clock.advance(.010)
        sample = profiler.finish()
        self.assertEqual(sample['gpu_stages_ms'], {})
        self.assertNotIn('device_free_bytes', sample)

    def test_public_payload_rejects_invalid_numbers(self):
        good = {'version': 1, 'backend': '2dgs', 'iteration': 40,
                'wall_step_ms': 120., 'stages_ms': {'loss': 20.}, 'gpu_stages_ms': {}}
        self.assertIsNotNone(normalize_performance(good))
        for field, value in [('wall_step_ms', float('nan')), ('wall_step_ms', -1),
                             ('iteration', True), ('version', 2)]:
            self.assertIsNone(normalize_performance(dict(good, **{field: value})))
        self.assertIsNone(normalize_performance(dict(good, stages_ms={'private': 10.})))

    def test_worker_exposes_complete_sample_without_relabelling_old_cuda_interval(self):
        sample = {'version': 1, 'backend': '3dgs', 'iteration': 1,
                  'wall_step_ms': 200., 'stages_ms': {'optimizer': 20.}, 'gpu_stages_ms': {}}
        value = status_telemetry({'kind': 'training', 'trainingPerformance': sample,
                                  'iteration_milliseconds': 30.})
        self.assertEqual(value['trainingPerformance'], sample)
        self.assertEqual(value['iterationMilliseconds'], 30.)
        self.assertNotIn('trainingPerformance', status_telemetry({'kind': 'mesh', 'trainingPerformance': sample}))


if __name__ == '__main__':
    unittest.main()
