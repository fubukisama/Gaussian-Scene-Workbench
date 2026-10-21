"""Runtime metadata remains bound without weakening full-state identity."""
import json
import pickle
from pathlib import Path
import tempfile
import unittest

from native.worker.training_checkpoint import TrainingCheckpoint, read_manifest, atomic_json


class PickleAdapter:
    save = staticmethod(pickle.dump)

    @staticmethod
    def load(path):
        with open(path, 'rb') as stream:
            return pickle.load(stream)


class RuntimeCheckpointBindingTests(unittest.TestCase):
    def test_legacy_policy_and_renderer_presence_are_bound_in_both_directions(self):
        for key in ('performance', 'renderer_engine'):
            for add_to_manifest in (False, True):
                with self.subTest(key=key, add=add_to_manifest), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    store = TrainingCheckpoint(root, {'output': str(root), 'session': 'fixture'})
                    state = {'iteration': 3, 'total': 19, 'identity': 'same'}
                    if not add_to_manifest:
                        state.update(performance={'version': 1, 'enabled': False}, renderer_engine='stock')
                    store.save(PickleAdapter, state)
                    manifest, payload = read_manifest(root)
                    before = payload.read_bytes()
                    self.assertEqual(store.load(PickleAdapter, 'same'), state)
                    if add_to_manifest:
                        manifest[key] = {'version': 1, 'enabled': False} if key == 'performance' else 'stock'
                    else:
                        del manifest[key]
                    atomic_json(root / '.gsw-resume' / 'ready.json', manifest)
                    with self.assertRaisesRegex(ValueError, 'performance|renderer'):
                        store.load(PickleAdapter, 'same')
                    self.assertEqual(payload.read_bytes(), before)

    def test_old_checkpoint_without_policy_metadata_remains_loadable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = TrainingCheckpoint(root, {'output': str(root), 'session': 'fixture'})
            state = {'iteration': 3, 'total': 19, 'identity': 'same'}
            manifest = store.save(PickleAdapter, state)
            self.assertNotIn('performance', manifest)
            self.assertNotIn('renderer_engine', manifest)
            self.assertEqual(store.load(PickleAdapter, 'same'), state)

    def test_probe_failure_pause_is_not_relabelled_as_vram_pressure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = TrainingCheckpoint(root, {'output': str(root), 'session': 'fixture'})
            state = {'iteration': 4, 'total': 19, 'identity': 'same',
                     'pause_reason': 'resource_probe_unavailable'}
            manifest = store.save(PickleAdapter, state)
            self.assertEqual(manifest.get('pause_reason'), 'resource_probe_unavailable')

    def test_new_runtime_metadata_tampering_is_rejected_without_changing_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = TrainingCheckpoint(root, {'output': str(root), 'session': 'fixture', 'backend': '3dgs'})
            state = {'version': 1, 'backend': '3dgs', 'iteration': 5, 'total': 17,
                     'identity': 'unchanged-model-identity', 'training_seed': 0,
                     'runtime_options': {'version': 2, 'resourceGuard': {'enabled': False}},
                     'runtime_configuration': {'requested': {'guard': 'off'}},
                     'runtime_policy_history': [{'iteration': 5, 'reason': 'explicit_resume_runtime_override'}]}
            store.save(PickleAdapter, state)
            manifest, payload = read_manifest(root)
            before = payload.read_bytes()
            self.assertEqual(manifest.get('runtime_options'), state['runtime_options'])
            self.assertEqual(store.load(PickleAdapter, state['identity']), state)
            manifest['runtime_options']['resourceGuard']['enabled'] = True
            atomic_json(root / '.gsw-resume' / 'ready.json', manifest)
            with self.assertRaisesRegex(ValueError, 'runtime'):
                store.load(PickleAdapter, state['identity'])
            self.assertEqual(payload.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
