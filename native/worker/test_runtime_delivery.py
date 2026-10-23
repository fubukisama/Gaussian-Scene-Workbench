"""Worker result delivery is separate from optimizer completion and retryable."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest



class RuntimeDeliveryTests(unittest.TestCase):
    def directory_link(self, link, target):
        """Create only an owned fixture link; never change Windows link policy."""
        if os.name == 'nt':
            import subprocess
            completed = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(link), str(target)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
                creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(completed.returncode, 0, repr(completed.stdout + completed.stderr))
        else:
            link.symlink_to(target, target_is_directory=True)

    def test_equivalent_output_spelling_preserves_completed_delivery_for_both_backends(self):
        """A filesystem alias expansion must not turn optimizer success into an exception.

        The controlled case adapts only the OS canonical-path boundary: this D:
        test volume does not necessarily provide distinct 8.3 names. Real short
        names are exercised separately below when the volume supplies one.
        """
        from unittest.mock import patch
        from native.worker.gsw_worker import publish_training_delivery
        from native.backend.tests.test_camera_scale_export import CameraScaleExportTests
        original_resolve = Path.resolve
        for backend in ('3dgs', '2dgs'):
            for spelling in ('unchanged', 'canonical-parent-expansion'):
                with self.subTest(backend=backend, spelling=spelling):
                    fixture = CameraScaleExportTests(); fixture.setUp()
                    try:
                        raw, _, metadata = fixture.fixture(backend)
                        output = raw.parent / 'owned output spelling'
                        point = output / 'point_cloud/iteration_4'; point.mkdir(parents=True)
                        model = point / 'point_cloud.ply'; model.write_bytes(raw.read_bytes())
                        sidecar = model.with_suffix('.camera-scale.json')
                        sidecar.write_text(json.dumps(metadata), encoding='utf-8')
                        checkpoint = output / 'checkpoint-evidence.bin'
                        checkpoint.write_bytes(b'complete optimizer and RNG state are retained')
                        before = {path: path.read_bytes() for path in (model, sidecar, checkpoint)}

                        def canonical_path(path, *args, **kwargs):
                            if spelling == 'canonical-parent-expansion' and path == output:
                                return output.parent / 'owned expanded output spelling'
                            return original_resolve(path, *args, **kwargs)

                        with patch.object(Path, 'resolve', canonical_path):
                            first = publish_training_delivery(output, 4)
                            repeated = publish_training_delivery(output, 4)
                        self.assertEqual(first['training_status'], 'done')
                        self.assertEqual(first['export_status'], 'ready')
                        self.assertEqual(first['internal_model_path'], str(model))
                        self.assertEqual(first['internal_model_relative_path'], 'point_cloud/iteration_4/point_cloud.ply')
                        self.assertEqual(first['model_relative_path'], '.gsw-deliveries/i4/point_cloud.metric-m.ply')
                        self.assertTrue(Path(first['model_path']).is_file())
                        self.assertEqual(repeated['model_path'], first['model_path'])
                        self.assertTrue(repeated['already_available'])
                        self.assertEqual({path: path.read_bytes() for path in before}, before)
                        receipt = json.loads((output / 'model-delivery.json').read_text(encoding='utf-8'))
                        self.assertEqual(receipt, repeated)
                    finally:
                        fixture.doCleanups()

    @unittest.skipUnless(os.name == 'nt', 'Windows real 8.3 alias behavior')
    def test_real_short_output_alias_delivers_the_same_model_and_preserves_source(self):
        import ctypes
        from native.worker.gsw_worker import publish_training_delivery
        from native.backend.tests.test_camera_scale_export import CameraScaleExportTests
        fixture = CameraScaleExportTests(); fixture.setUp()
        try:
            output = fixture.root / 'owned training result long directory name'; output.mkdir()
            buffer = ctypes.create_unicode_buffer(32768)
            size = ctypes.windll.kernel32.GetShortPathNameW(str(output), buffer, len(buffer))
            self.assertGreater(size, 0)
            self.assertLess(size, len(buffer))
            alias = Path(buffer.value)
            if os.path.normcase(str(alias)) == os.path.normcase(str(output)):
                self.skipTest('D test volume provides no distinct 8.3 alias; no policy or system files are changed')
            self.assertTrue(alias.samefile(output))
            for backend in ('3dgs', '2dgs'):
                with self.subTest(backend=backend):
                    raw, _, metadata = fixture.fixture(backend)
                    iteration = 4 if backend == '3dgs' else 8
                    point = output / ('point_cloud/iteration_' + str(iteration)); point.mkdir(parents=True)
                    model = point / 'point_cloud.ply'; model.write_bytes(raw.read_bytes())
                    sidecar = model.with_suffix('.camera-scale.json')
                    sidecar.write_text(json.dumps(metadata), encoding='utf-8')
                    before = {path: path.read_bytes() for path in (model, sidecar)}
                    first = publish_training_delivery(alias, iteration)
                    second = publish_training_delivery(output, iteration)
                    self.assertEqual(first['training_status'], 'done')
                    self.assertEqual(first['export_status'], 'ready')
                    self.assertEqual(second['export_status'], 'ready')
                    self.assertTrue(Path(first['model_path']).samefile(second['model_path']))
                    self.assertTrue(second['already_available'])
                    self.assertEqual({path: path.read_bytes() for path in before}, before)
        finally:
            fixture.doCleanups()

    def test_linked_point_cloud_cannot_deliver_a_foreign_completed_model(self):
        from native.worker.gsw_worker import publish_training_delivery
        base = Path('D:/Codex/qa-runtime-delivery'); base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            for linked_part in ('point-cloud-root', 'iteration-directory'):
                with self.subTest(linked_part=linked_part):
                    output = root / ('output-' + linked_part); output.mkdir()
                    foreign = root / ('foreign-' + linked_part)
                    point = foreign / 'iteration_4'; point.mkdir(parents=True)
                    model = point / 'point_cloud.ply'; model.write_bytes(b'foreign optimizer output must remain untouched')
                    checkpoint = foreign / 'checkpoint.bin'; checkpoint.write_bytes(b'foreign complete state')
                    if linked_part == 'point-cloud-root':
                        link, target = output / 'point_cloud', foreign
                    else:
                        (output / 'point_cloud').mkdir()
                        link, target = output / 'point_cloud/iteration_4', point
                    self.directory_link(link, target)
                    # Remove the link before TemporaryDirectory cleanup, including
                    # on failure. The foreign tree is itself owned test data.
                    try:
                        result = publish_training_delivery(output, 4)
                        self.assertEqual(result['training_status'], 'done')
                        self.assertEqual(result['export_status'], 'failed')
                        self.assertEqual(result['reason'], 'training_delivery_model_missing')
                        self.assertIsNone(result['model_path'])
                        self.assertEqual(model.read_bytes(), b'foreign optimizer output must remain untouched')
                        self.assertEqual(checkpoint.read_bytes(), b'foreign complete state')
                        self.assertFalse((output / '.gsw-deliveries').exists())
                        self.assertFalse((output / 'model-delivery.json').exists())
                    finally:
                        link.rmdir() if os.name == 'nt' else link.unlink()

    def test_output_root_and_ancestor_links_cannot_write_into_foreign_completed_output(self):
        from native.worker.gsw_worker import publish_training_delivery
        base = Path('D:/Codex/qa-runtime-delivery'); base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            for linked_part in ('output-root', 'output-ancestor'):
                with self.subTest(linked_part=linked_part):
                    foreign = root / ('foreign-' + linked_part); foreign.mkdir()
                    output = foreign if linked_part == 'output-root' else foreign / 'output'
                    point = output / 'point_cloud/iteration_4'; point.mkdir(parents=True)
                    model = point / 'point_cloud.ply'; model.write_bytes(b'foreign model bytes')
                    checkpoint = output / 'checkpoint.bin'; checkpoint.write_bytes(b'foreign complete state')
                    link = root / ('link-' + linked_part)
                    self.directory_link(link, foreign)
                    try:
                        linked_output = link if linked_part == 'output-root' else link / 'output'
                        with self.assertRaisesRegex(ValueError, 'link|reparse|outside the allowed root'):
                            publish_training_delivery(linked_output, 4)
                        self.assertEqual(model.read_bytes(), b'foreign model bytes')
                        self.assertEqual(checkpoint.read_bytes(), b'foreign complete state')
                        self.assertFalse((output / '.gsw-deliveries').exists())
                        self.assertFalse((output / 'model-delivery.json').exists())
                    finally:
                        link.rmdir() if os.name == 'nt' else link.unlink()

    def test_public_removal_rejects_escape_and_unlinks_reparse_entry_without_following_it(self):
        from native.worker.gsw_worker import remove_path
        base = Path('D:/Codex/qa-runtime-delivery'); base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary)
            allowed = root / 'allowed'; allowed.mkdir()
            foreign = root / 'foreign'; foreign.mkdir()
            marker = foreign / 'keep.bin'; marker.write_bytes(b'foreign data stays intact')
            for outside in (foreign, allowed / '..' / 'foreign'):
                with self.subTest(outside=str(outside)):
                    with self.assertRaisesRegex(ValueError, 'outside the allowed root'):
                        remove_path(outside, allowed)
                    self.assertEqual(marker.read_bytes(), b'foreign data stays intact')
            container = allowed / 'owned'; container.mkdir()
            link = container / 'foreign link'
            self.directory_link(link, foreign)
            try:
                self.assertTrue(remove_path(container, allowed))
                self.assertFalse(container.exists())
                self.assertEqual(marker.read_bytes(), b'foreign data stays intact')
                self.assertTrue(foreign.is_dir())
            finally:
                if link.exists():
                    link.rmdir() if os.name == 'nt' else link.unlink()

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
