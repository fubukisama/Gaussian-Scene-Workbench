"""Public quality sampler lookahead and legacy-resume admission boundaries."""
import random
import unittest


class QualitySamplerResumeGuardTests(unittest.TestCase):
    def test_image_snapshot_predicts_actual_next_camera_without_advancing_any_rng(self):
        from native.worker.quality_random import QualityRandomStreams
        from native.worker.training_camera_lookahead import predict_next_camera
        streams = QualityRandomStreams(17)
        cameras = [object() for _ in range(5)]
        pending = list(range(5))
        global_before = random.getstate()
        for _ in range(13):
            snapshot = streams.image_sampler_state()
            self.assertIsInstance(snapshot, tuple)
            predicted = predict_next_camera(cameras, pending, snapshot)
            self.assertEqual(streams.image_sampler_state(), snapshot)
            self.assertEqual(random.getstate(), global_before)
            clone = random.Random(0)
            clone.setstate(snapshot)
            clone.random()
            self.assertEqual(streams.image_sampler_state(), snapshot)
            if not pending:
                pending = list(range(len(cameras)))
            actual = cameras[pending.pop(streams.choose_index(len(pending)))]
            self.assertIs(actual, predicted)

    def test_runtime_prefetch_uses_quality_snapshot_with_real_cpu_image_pipeline(self):
        try:
            import torch
        except ImportError:
            self.skipTest('configured CPU Torch runtime required')
        from types import SimpleNamespace
        from native.worker.quality_random import QualityRandomStreams
        from native.worker.training_runtime import NativeTrainingRuntime
        from native.worker.training_image_pipeline import TrainingImagePipeline, ImagePipelineConfig
        import os
        saved_environment = {key: value for key, value in os.environ.items() if key.startswith('GSW_TRAINING_IMAGE_')}

        def restore_environment():
            for key in list(os.environ):
                if key.startswith('GSW_TRAINING_IMAGE_'): os.environ.pop(key)
            os.environ.update(saved_environment)

        self.addCleanup(restore_environment)
        for backend in ('3dgs', '2dgs'):
            runtime = NativeTrainingRuntime({'runtimeOptions': {'version': 2,
                'execution': {'camera_prefetch': True, 'image_staging': True}}},
                torch, lambda *event: None, backend)
            # Use the real staging implementation on its supported CPU device;
            # no internal implementation or CUDA function is mocked.
            runtime.images = TrainingImagePipeline(torch, device='cpu',
                config=ImagePipelineConfig(enabled=True, slots=2, pinned_bytes=0, device_bytes=1024**2))
            self.addCleanup(runtime.images.close)
            cameras = [SimpleNamespace(original_image=torch.full((3, 1, 1), index / 8.)) for index in range(5)]
            pending = list(range(5))
            streams = QualityRandomStreams(17)
            current = cameras[pending.pop(streams.choose_index(len(pending)))]
            snapshot, global_before = streams.image_sampler_state(), random.getstate()
            ready = runtime.image(current, cameras, pending, python_rng_state=snapshot)
            self.assertTrue(torch.equal(ready.image, current.original_image))
            self.assertEqual(streams.image_sampler_state(), snapshot)
            self.assertEqual(random.getstate(), global_before)
            self.assertEqual(runtime.lookahead_opportunities, 1)
            self.assertEqual(runtime.images.stats()['transfers'], 2)
            following = cameras[pending.pop(streams.choose_index(len(pending)))]
            runtime.image(following, cameras, pending, python_rng_state=streams.image_sampler_state())
            self.assertGreaterEqual(runtime.images.stats()['cache_hits'], 1)

    def test_legacy_checkpoint_guard_rejects_explicit_quality_without_loading_files(self):
        from native.worker.training_quality_plan import reject_legacy_quality_checkpoint, QualityPlanError
        # A nonexistent path is intentional: this admission boundary must not
        # load a tuple or inspect GPU state before rejecting the combination.
        checkpoint = 'D:/Codex/owned-nonexistent-quality-legacy.pth'
        for requested in ({'version': 1, 'densification': {'strategy': 'mcmc'}},
                          {'version': 1, 'densification': {'strategy': 'abs_gs'}},
                          {'version': 1}):
            with self.subTest(requested=requested), self.assertRaises(QualityPlanError) as raised:
                reject_legacy_quality_checkpoint(requested, checkpoint)
            self.assertEqual(raised.exception.code, 'quality_legacy_checkpoint_unsupported')
        self.assertIsNone(reject_legacy_quality_checkpoint(None, checkpoint))
        self.assertIsNone(reject_legacy_quality_checkpoint({'version': 1}, None))

    def test_actual_trainer_entries_reject_quality_tuple_before_model_construction_on_cpu(self):
        import importlib.util
        import json
        import os
        from pathlib import Path
        import subprocess
        if importlib.util.find_spec('torch') is None:
            self.skipTest('configured CPU Torch runtime required')
        repository = Path(__file__).resolve().parents[2]
        runtimes = [('3dgs', Path('D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe'), repository / 'gaussian-splatting/train.py'),
                    ('2dgs', Path('D:/Apps/GSW/runtime/envs/two_dgs/python.exe'), Path(__file__).with_name('two_dgs_train.py'))]
        if any(not executable.is_file() for _, executable, _ in runtimes):
            self.skipTest('configured native trainer runtimes not_run: optional Windows executables unavailable')
        script = """import json,os,runpy,sys
from types import SimpleNamespace
from pathlib import Path
sys.path.insert(0,os.environ['OWNED_REPOSITORY'])
entry=Path(os.environ['OWNED_TRAINER'])
sys.path.insert(0,str(entry.parent))
module=runpy.run_path(str(entry),run_name='owned_cpu_guard_entry')
from native.worker.training_quality_plan import QualityPlanError
arguments=[object(),SimpleNamespace(optimizer_type='default'),object(),[],[],[],'D:/Codex/owned-nonexistent-quality-legacy.pth']
if os.environ['OWNED_BACKEND']=='3dgs': arguments.append(-1)
try:
    module['training'](*arguments)
except QualityPlanError as error:
    if error.code!='quality_legacy_checkpoint_unsupported': raise
    print(json.dumps({'backend':os.environ['OWNED_BACKEND'],'code':error.code,'cuda_initialized':module['torch'].cuda.is_initialized()}))
else: raise AssertionError('actual trainer did not reject before constructing a model')
"""
        for backend, executable, entry in runtimes:
            control = dict(qualityPlan={'version': 1, 'densification': {'strategy': 'mcmc'}})
            environment = dict(os.environ, CUDA_VISIBLE_DEVICES='-1', GSW_POSE_POLICY='', GSW_CAMERA_DATASET='',
                GSW_NATIVE_TRAINING_CONTROL=json.dumps(control), OWNED_REPOSITORY=str(repository),
                OWNED_TRAINER=str(entry), OWNED_BACKEND=backend, GSW_TWO_DGS_SOURCE='D:/Apps/GSW/runtime/2dgs', MPLBACKEND='Agg')
            result = subprocess.run([str(executable), '-B', '-c', script], env=environment,
                cwd=str(repository), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            record = json.loads(result.stdout.strip().splitlines()[-1])
            self.assertEqual(record['code'], 'quality_legacy_checkpoint_unsupported')
            self.assertFalse(record['cuda_initialized'])


if __name__ == '__main__':
    unittest.main()
