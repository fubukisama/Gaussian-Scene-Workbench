"""Worker result delivery is separate from optimizer completion and retryable."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest



class RuntimeDeliveryTests(unittest.TestCase):
    def test_unexpected_export_exception_preserves_optimizer_completion_and_raw_model(self):
        from unittest.mock import patch
        from native.worker.gsw_worker import publish_training_delivery
        base=Path(tempfile.gettempdir())/'gsw-runtime-delivery-tests';base.mkdir(parents=True,exist_ok=True)
        for failure in (RuntimeError('owned export failure'), MemoryError('owned allocation failure')):
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory(dir=base) as temporary:
                output=Path(temporary);point=output/'point_cloud/iteration_4';point.mkdir(parents=True)
                source=point/'point_cloud.ply';source.write_bytes(b'raw optimizer output is retained')
                checkpoint=output/'checkpoint-evidence.bin';checkpoint.write_bytes(b'last complete optimizer state')
                with patch('native.backend.camera_scale_export.deliver_training_result',side_effect=failure):
                    result=publish_training_delivery(output,4)
                self.assertEqual(result['training_status'],'done')
                self.assertEqual(result['export_status'],'failed')
                self.assertEqual(result['detail'],str(failure))
                self.assertEqual(Path(result['model_path']),source)
                self.assertEqual(source.read_bytes(),b'raw optimizer output is retained')
                self.assertEqual(checkpoint.read_bytes(),b'last complete optimizer state')
                self.assertEqual(json.loads((output/'model-delivery.json').read_text(encoding='utf-8')),result)

    def test_direct_worker_entry_claims_output_before_loading_compute_outside_repository_cwd(self):
        import os
        import subprocess
        import sys
        root=Path(__file__).resolve().parents[2]
        base=Path(tempfile.gettempdir())/'gsw-runtime-delivery-tests';base.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory=Path(temporary);store=directory/'jobs';store.mkdir()
            target=directory/'output/owned'
            (store/('a'*32+'.json')).write_text(json.dumps({'id':'a'*32,'backend':'3dgs','status':'running',
                'native_control':{'output':str(target)}}),encoding='utf-8')
            configuration={'repositoryRoot':str(root),'datasetPath':str(directory/'missing-input'),
                'outputRoot':str(target.parent),'outputScene':'owned','quality':'quick','backend':'3dgs',
                'nativeCheckpoint':True,'jobStore':str(store)}
            path=directory/'c.json';path.write_text(json.dumps(configuration),encoding='utf-8')
            env=os.environ.copy();env.pop('PYTHONPATH',None);env['PYTHONUTF8']='1'
            child=subprocess.run([sys.executable,'-B',str(root/'native/worker/gsw_worker.py'),'--config',str(path)],
                cwd=directory,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                encoding='utf-8',timeout=15,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            self.assertNotEqual(child.returncode,0)
            self.assertIn('output_session_existing_job_unverified',child.stdout+child.stderr)
            self.assertFalse(target.exists())
    def test_optimizer_success_does_not_close_the_control_pipe_before_delivery(self):
        import contextlib
        import io
        import threading
        from native.worker.gsw_worker import stream_job
        output=io.StringIO()
        with contextlib.redirect_stdout(output):
            state,error=stream_job(threading.Lock(),{'owned':{'status':'done','stage':'done'}},
                'owned',defer_success=True)
        self.assertEqual(state,'done')
        events=[json.loads(line.split('] ',1)[1]) for line in output.getvalue().splitlines() if line.startswith('[worker-event] ')]
        self.assertEqual(events[-1]['state'],'running')
        self.assertEqual(events[-1]['stage'],'coordinate_export')
        self.assertFalse(any(row['state']=='done' for row in events))
    def test_completed_training_delivers_metric_bundle_without_reinitializing_or_overwriting(self):
        from native.worker.gsw_worker import publish_training_delivery
        from native.backend.tests.test_camera_scale_export import CameraScaleExportTests
        for backend in ('3dgs','2dgs'):
            with self.subTest(backend=backend):
                fixture=CameraScaleExportTests();fixture.setUp()
                try:
                    raw,_,metadata=fixture.fixture(backend)
                    output=raw.parent/'job'
                    iteration=output/'point_cloud/iteration_8';iteration.mkdir(parents=True)
                    model=iteration/'point_cloud.ply';model.write_bytes(raw.read_bytes())
                    sidecar=model.with_suffix('.camera-scale.json')
                    sidecar.write_text(json.dumps(metadata),encoding='utf-8')
                    checkpoint=output/'checkpoint-evidence.bin';checkpoint.write_bytes(b'unchanged independently retained state')
                    before=hashlib.sha256(model.read_bytes()).hexdigest()
                    delivery=publish_training_delivery(output,8)
                    self.assertEqual(delivery['training_status'],'done')
                    self.assertEqual(delivery['export_status'],'ready')
                    self.assertNotEqual(Path(delivery['model_path']),model)
                    self.assertEqual(hashlib.sha256(model.read_bytes()).hexdigest(),before)
                    self.assertEqual(checkpoint.read_bytes(),b'unchanged independently retained state')
                    repeated=publish_training_delivery(output,8)
                    self.assertEqual(repeated['model_path'],delivery['model_path'])
                    self.assertTrue(repeated['already_available'])
                    receipt=json.loads((output/'model-delivery.json').read_text(encoding='utf-8'))
                    self.assertEqual(receipt['model_path'],delivery['model_path'])
                finally:
                    fixture.doCleanups()

    def test_export_failure_does_not_reclassify_training_or_modify_internal_result(self):
        from native.worker.gsw_worker import publish_training_delivery
        base=Path(tempfile.gettempdir())/'gsw-runtime-delivery-tests';base.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            output=Path(temporary);point=output/'point_cloud/iteration_4';point.mkdir(parents=True)
            source=point/'point_cloud.ply';source.write_bytes(b'invalid model stays intact')
            result=publish_training_delivery(output,4)
            self.assertEqual(result['training_status'],'done')
            self.assertEqual(result['export_status'],'failed')
            self.assertEqual(source.read_bytes(),b'invalid model stays intact')
            self.assertEqual(Path(result['model_path']),source)


if __name__=='__main__':unittest.main()
