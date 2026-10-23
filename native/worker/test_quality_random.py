"""Public independent-stream behavior for paired quality experiments."""
import unittest
import importlib.util

from native.worker.quality_random import QualityRandomStreams


class QualityRandomTests(unittest.TestCase):
    def test_method_noise_does_not_change_the_actual_camera_sequence(self):
        baseline = QualityRandomStreams(17)
        candidate = QualityRandomStreams(17)
        left, right = [], []
        for _ in range(12):
            left.append(baseline.choose_index(7))
            candidate.strategy_random().random()
            candidate.strategy_random().random()
            right.append(candidate.choose_index(7))
        self.assertEqual(left, right)
        self.assertGreater(len(set(left)), 1)

    def test_stream_checkpoint_restores_both_sequences_and_rejects_another_seed(self):
        running = QualityRandomStreams(17)
        for _ in range(5):
            running.choose_index(9)
            running.strategy_random().random()
        saved = running.state_dict()
        expected = [(running.choose_index(9), running.strategy_random().random()) for _ in range(8)]
        resumed = QualityRandomStreams(17)
        resumed.load_state_dict(saved)
        self.assertEqual([(resumed.choose_index(9), resumed.strategy_random().random()) for _ in range(8)], expected)
        with self.assertRaisesRegex(ValueError, "quality_rng_identity_mismatch"):
            QualityRandomStreams(29).load_state_dict(saved)

    @unittest.skipUnless(importlib.util.find_spec("torch"), "real CPU Torch runtime required")
    def test_actual_torch_strategy_generator_restores_without_consuming_global_rng(self):
        import torch
        before = torch.get_rng_state().clone()
        running = QualityRandomStreams(17)
        generator = running.torch_generator("cpu")
        torch.randn(5, generator=generator)
        saved = running.state_dict()
        expected = torch.randn(7, generator=generator)
        resumed = QualityRandomStreams(17)
        resumed.load_state_dict(saved)
        self.assertTrue(torch.equal(expected, torch.randn(7, generator=resumed.torch_generator("cpu"))))
        self.assertTrue(torch.equal(before, torch.get_rng_state()))


if __name__ == "__main__":
    unittest.main()
