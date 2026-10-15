"""Public deterministic camera-lookahead and legacy sampler compatibility."""
import random
import unittest


class CameraLookaheadTests(unittest.TestCase):
    def test_predicts_next_pending_camera_without_consuming_global_sampler(self):
        from native.worker.training_camera_lookahead import predict_next_camera
        saved = random.getstate()
        self.addCleanup(random.setstate, saved)
        random.seed(2026)
        cameras = [object() for _ in range(5)]
        pending = [0, 1, 2, 3, 4]
        current = pending.pop(random.randint(0, len(pending) - 1))
        self.assertEqual(current, 0)
        before = random.getstate()
        predicted = predict_next_camera(cameras, pending)
        self.assertIs(predicted, cameras[3])
        self.assertEqual(pending, [1, 2, 3, 4])
        self.assertEqual(random.getstate(), before)
        self.assertIs(cameras[pending.pop(random.randint(0, len(pending) - 1))], predicted)

    def test_empty_training_set_has_no_prediction_or_sampler_effect(self):
        from native.worker.training_camera_lookahead import predict_next_camera
        before = random.getstate()
        self.assertIsNone(predict_next_camera([], []))
        self.assertEqual(random.getstate(), before)

    def test_epoch_refill_and_saved_state_preserve_legacy_sample_order(self):
        from native.worker.training_camera_lookahead import predict_next_camera
        saved = random.getstate()
        self.addCleanup(random.setstate, saved)
        random.seed(2026)
        cameras = [object() for _ in range(5)]
        pending = list(range(5))
        seen = []
        for expected in (0, 3, 4, 1, 2):
            self.assertIs(predict_next_camera(cameras, pending), cameras[expected])
            seen.append(pending.pop(random.randint(0, len(pending) - 1)))
        self.assertEqual(seen, [0, 3, 4, 1, 2])
        checkpoint_rng = random.getstate()
        self.assertIs(predict_next_camera(cameras, pending), cameras[4])
        # Loading a saved optimizer/sampler state must not depend on any
        # unrelated random consumption in the current Python process.
        random.seed(77)
        before = random.getstate()
        self.assertIs(predict_next_camera(cameras, pending, checkpoint_rng), cameras[4])
        self.assertEqual(random.getstate(), before)
        self.assertEqual(pending, [])


if __name__ == "__main__":
    unittest.main()
