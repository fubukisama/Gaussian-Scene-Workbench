"""The native worker validates runtime requests before any training launch."""
import json
from pathlib import Path
import tempfile
import unittest

from native.worker.gsw_worker import load_configuration


class RuntimeWorkerConfigurationTests(unittest.TestCase):
    def test_malformed_optional_failure_metadata_preserves_valid_progress(self):
        from native.worker.gsw_worker import status_telemetry
        for malformed in (['bad'], 'invalid', 7, True):
            with self.subTest(value=malformed):
                result = status_telemetry({'trainingFailure': malformed, 'iteration': 3})
                self.assertEqual(result['iteration'], 3)
                self.assertNotIn('failureReason', result)

    def test_3dgs_surfel_request_is_retained_but_not_reported_as_applicable(self):
        from native.worker.training_runtime import runtime_configuration
        report = runtime_configuration({'backend': '3dgs', 'runtimeOptions': {'version': 2}})
        self.assertTrue(report['requested']['execution']['surfel_auxiliary'])
        self.assertFalse(report['effective']['execution']['surfel_auxiliary'])
        self.assertIn({'feature': 'execution.surfel_auxiliary', 'code': 'not_applicable_to_3dgs'}, report['reasons'])

    def test_runtime_and_failure_observations_survive_the_worker_status_interface(self):
        from native.worker.gsw_worker import status_telemetry
        from native.worker.training_configuration import normalize_runtime_options
        report = normalize_runtime_options({'version': 2, 'resourceGuard': {'enabled': False}})
        for reason in ('cuda_oom', 'cuda_oom_reported', 'training_exception',
                       'resource_probe_unavailable', 'resource_estimate_unavailable', 'vram_pressure'):
            snapshot = {'runtimeConfiguration': report, 'pause_reason': reason}
            telemetry = status_telemetry(snapshot)
            self.assertEqual(telemetry.get('runtimeConfiguration'), report)
            self.assertEqual(telemetry.get('failureReason'), reason)

    def test_unknown_runtime_option_is_rejected_at_the_public_file_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = {'repositoryRoot': temporary, 'datasetPath': temporary,
                      'outputRoot': temporary, 'outputScene': 'example',
                      'backend': '3dgs', 'quality': 'manual',
                      'runtimeOptions': {'version': 2, 'execution': {'silently_ignore': True}}}
            path = Path(temporary) / 'configuration.json'
            path.write_text(json.dumps(config), encoding='utf-8')
            before = path.read_bytes()
            with self.assertRaisesRegex(ValueError, 'runtime_unknown_key'):
                load_configuration(path)
            self.assertEqual(path.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
