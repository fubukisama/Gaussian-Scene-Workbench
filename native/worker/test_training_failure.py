"""Public failure events preserve the original failure and completed files."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest


class TrainingFailureTests(unittest.TestCase):
    def test_typed_allocator_failure_is_distinct_from_reported_text_and_normal_exception(self):
        from native.worker.training_failure import training_failure_record
        class CudaAllocationFailure(RuntimeError):
            pass
        class TorchAllocationFailure(RuntimeError):
            pass
        torch = SimpleNamespace(cuda=SimpleNamespace(OutOfMemoryError=CudaAllocationFailure),
                                OutOfMemoryError=TorchAllocationFailure)
        typed = training_failure_record(CudaAllocationFailure('allocator failure'), '3dgs', torch)
        self.assertEqual(typed['kind'], 'cuda_oom')
        self.assertEqual(training_failure_record(TorchAllocationFailure('alias'), '2dgs', torch)['kind'], 'cuda_oom')
        reported = training_failure_record(RuntimeError('CUDA out of memory. Tried to allocate 16 bytes.'), '3dgs', torch)
        self.assertEqual(reported['kind'], 'cuda_oom_reported')
        self.assertEqual(training_failure_record(RuntimeError('ordinary source failure'), '2dgs', torch)['kind'], 'training_exception')
        self.assertEqual(training_failure_record(MemoryError('host memory'), '3dgs', torch)['kind'], 'training_exception')
        self.assertNotIn('last_completed_iteration', typed)
        self.assertNotIn('checkpoint_preserved', typed)

    def test_failure_boundary_reraises_same_exception_and_never_overwrites_last_checkpoint(self):
        from native.worker.training_failure import report_training_failure
        events = []
        failure = RuntimeError('ordinary failure after checkpoint')
        with tempfile.TemporaryDirectory(prefix='gsw-failure-') as temporary:
            checkpoint = Path(temporary) / 'ready.json'
            checkpoint.write_bytes(b'last successful complete state')
            with self.assertRaises(RuntimeError) as captured:
                with report_training_failure('2dgs', None, lambda prefix, event: events.append((prefix, event))):
                    raise failure
            self.assertIs(captured.exception, failure)
            self.assertEqual(checkpoint.read_bytes(), b'last successful complete state')
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][0], '[gsw-training-error]')
        self.assertEqual(events[0][1]['kind'], 'training_exception')

    def test_actual_owned_process_exits_failed_after_structured_event_and_traceback(self):
        repository = Path(__file__).resolve().parents[2]
        code = "import json;from native.worker.training_failure import report_training_failure\nwith report_training_failure('3dgs', None, lambda p,e: print(p+' '+json.dumps(e),flush=True)):\n raise RuntimeError('owned fixture normal error')"
        result = subprocess.run([sys.executable, '-B', '-c', code], cwd=str(repository),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8',
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('RuntimeError: owned fixture normal error', result.stderr)
        line = result.stdout.strip()
        self.assertTrue(line.startswith('[gsw-training-error] '), line)
        event = json.loads(line.split(' ', 1)[1])
        self.assertEqual(event['backend'], '3dgs')
        self.assertEqual(event['kind'], 'training_exception')

    def test_installed_torch_typed_oom_is_classified_without_initializing_cuda(self):
        from native.worker.training_failure import report_training_failure
        try:
            import torch
        except ImportError:
            self.skipTest('Run under either supported training runtime for actual Torch exception types')
        exception_type = getattr(torch.cuda, 'OutOfMemoryError', getattr(torch, 'OutOfMemoryError', None))
        if exception_type is None:
            self.skipTest('This Torch version exposes only the explicitly inferred text fallback')
        initialized = torch.cuda.is_initialized()
        events = []
        with self.assertRaises(exception_type):
            with report_training_failure('3dgs', torch, lambda prefix, value: events.append(value)):
                raise exception_type('owned typed exception; no allocation')
        self.assertEqual(events[0]['kind'], 'cuda_oom')
        self.assertEqual(torch.cuda.is_initialized(), initialized)

    def test_manual_process_exit_and_broken_event_pipe_keep_original_exception_semantics(self):
        from native.worker.training_failure import report_training_failure
        events = []
        with self.assertRaises(SystemExit) as stopped:
            with report_training_failure('2dgs', None, lambda prefix, event: events.append(event)):
                raise SystemExit(75)
        self.assertEqual(stopped.exception.code, 75)
        self.assertEqual(events, [])
        original = RuntimeError('original training failure')
        def broken_pipe(prefix, event):
            raise BrokenPipeError('closed event pipe')
        with self.assertRaises(RuntimeError) as failed:
            with report_training_failure('3dgs', None, broken_pipe):
                raise original
        self.assertIs(failed.exception, original)


if __name__ == '__main__':
    unittest.main()
