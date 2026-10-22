"""Independent solve-output publication, rollback and original-byte protection."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock

from PIL import Image

from native.backend.camera_admission import (
    prepare_solve_dataset_version, publish_solve_training_output, validate_solve_dataset_version,
)
from native.backend.camera_dataset import CameraDatasetError


def tree_hashes(root):
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob('*') if path.is_file()}


class SolveOutputPublicationTests(unittest.TestCase):
    def setUp(self):
        base = Path('D:/Apps/GSW/validation/acceptance-followup-oct22/paths/publication-unit')
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / 'source'
        self.source.mkdir()
        views = []
        for index, split in enumerate(('train', 'train', 'val', 'test')):
            image = self.source / ('photo-%d.png' % index)
            Image.new('RGB', (11, 9), (20 + index, 40, 60)).save(image)
            views.append({'image_path': image.name, 'K': [[14., 0., 5.5], [0., 15., 4.5], [0., 0., 1.]],
                'pixel_coordinates': 'pixel_border', 'split': split,
                'c2w_opencv': [[1., 0., 0., index * .2], [0., 1., 0., .1],
                               [0., 0., 1., -.6], [0., 0., 0., 1.]]})
        manifest = self.source / 'camera_dataset.json'
        manifest.write_text(json.dumps({'schema': 'gsw.camera_dataset/1', 'pose_policy': 'locked',
            'source': {'kind': 'synthetic'}, 'scale': {'status': 'arbitrary'}, 'views': views}), encoding='utf-8')
        self.original_hashes = tree_hashes(self.source)
        self.version = prepare_solve_dataset_version(manifest, self.root / 'solve')
        self.solve = Path(self.version['datasetPath'])
        (self.solve / 'sparse/0').mkdir(parents=True)
        (self.solve / 'sparse/0/result.bin').write_bytes(b'opaque reconstruction output; not a physical surface fixture')
        self.solve_hashes = tree_hashes(self.solve)
        self.output = self.root / 'result'
        self.training_source = {'version': 1, 'posePolicy': 'solve', 'datasetPath': '.gsw/solve-dataset',
            'cameraDatasetPath': str(manifest), 'cameraDatasetIdentity': self.version['sourceDataIdentity'],
            'resolutionPolicy': 'preserve'}

    def publish(self, **options):
        return publish_solve_training_output(self.solve, self.output, self.version['sourceDataIdentity'],
                                             self.training_source, **options)

    def previous_output(self):
        (self.output / '.gsw-resume').mkdir(parents=True)
        (self.output / 'previous.ply').write_bytes(b'previous model bytes')
        (self.output / '.gsw-resume/old.pth').write_bytes(b'previous full optimizer state')
        return tree_hashes(self.output)

    def assert_sources_unchanged(self):
        self.assertEqual(tree_hashes(self.source), self.original_hashes)
        self.assertEqual(tree_hashes(self.solve), self.solve_hashes)

    def test_success_preserves_previous_output_as_recoverable_hash_equal_backup(self):
        previous = self.previous_output()
        result = self.publish(overwrite=True)
        self.assertEqual(tree_hashes(Path(result['backupPath'])), previous)
        self.assertEqual(tree_hashes(Path(result['datasetPath'])), self.solve_hashes)
        self.assertFalse((self.output / 'previous.ply').exists())
        admitted = validate_solve_dataset_version(result['datasetPath'], self.output,
                                                  self.version['sourceDataIdentity'])
        self.assertEqual(admitted['solveDataIdentity'], self.version['solveDataIdentity'])
        journal = json.loads(Path(result['journalPath']).read_text(encoding='utf-8'))
        self.assertEqual(journal['state'], 'published')
        self.assertEqual(journal['backupPath'], result['backupPath'])
        self.assertEqual(journal['sourceTreeSha256'], result['sourceTreeSha256'])
        self.assertFalse(Path(journal['stagePath']).exists())
        self.assert_sources_unchanged()

    def test_publish_rename_failure_rolls_back_complete_previous_output(self):
        previous = self.previous_output()
        real_rename, failed = os.rename, []
        def fail_first_publication(source, destination, *args, **kwargs):
            if Path(destination) == self.output and not failed:
                failed.append(True)
                raise OSError('controlled publication rename failure')
            return real_rename(source, destination, *args, **kwargs)
        with mock.patch.object(os, 'rename', side_effect=fail_first_publication):
            with self.assertRaisesRegex(OSError, 'controlled publication rename failure'):
                self.publish(overwrite=True)
        self.assertEqual(tree_hashes(self.output), previous)
        journals = list((self.root / '.gsw-transactions').glob('*.json'))
        self.assertEqual(len(journals), 1)
        journal = json.loads(journals[0].read_text(encoding='utf-8'))
        self.assertEqual(journal['state'], 'rolled_back')
        self.assertFalse(Path(journal['backupPath']).exists())
        self.assertFalse(Path(journal['stagePath']).exists())
        self.assert_sources_unchanged()

    def test_cancelled_copy_leaves_old_model_and_supports_fresh_retry(self):
        previous = self.previous_output()
        real_open, cancel = io.open, threading.Event()
        def cancel_on_copy(file, mode='r', *args, **kwargs):
            stream = real_open(file, mode, *args, **kwargs)
            if mode == 'xb' and '/.gsw/solve-dataset/' in os.fsdecode(os.fspath(file)).replace('\\', '/'):
                cancel.set()
            return stream
        with mock.patch.object(io, 'open', side_effect=cancel_on_copy):
            with self.assertRaises(CameraDatasetError) as raised:
                self.publish(overwrite=True, cancelled=cancel.is_set)
        self.assertEqual(raised.exception.code, 'camera_import_cancelled')
        self.assertEqual(tree_hashes(self.output), previous)
        result = self.publish(overwrite=True)
        self.assertEqual(tree_hashes(Path(result['backupPath'])), previous)
        self.assert_sources_unchanged()

    def test_fresh_published_journal_failure_rolls_back_and_allows_retry(self):
        real_replace, failed = os.replace, []
        def fail_committed_journal(source, destination, *args, **kwargs):
            if (self.output.exists() and Path(destination).parent.name == '.gsw-transactions'
                    and not failed):
                failed.append(True)
                raise OSError('controlled fresh published journal failure')
            return real_replace(source, destination, *args, **kwargs)
        with mock.patch.object(os, 'replace', side_effect=fail_committed_journal):
            with self.assertRaisesRegex(OSError, 'controlled fresh published journal failure'):
                self.publish()
        self.assertFalse(self.output.exists(), 'failed fresh publication must not occupy the retry target')
        journals = list((self.root / '.gsw-transactions').glob('*.json'))
        self.assertEqual(len(journals), 1)
        journal = json.loads(journals[0].read_text(encoding='utf-8'))
        self.assertEqual(journal['state'], 'rolled_back')
        self.assertFalse(Path(journal['stagePath']).exists())
        result = self.publish()
        self.assertEqual(tree_hashes(Path(result['datasetPath'])), self.solve_hashes)
        self.assert_sources_unchanged()

    def test_concurrent_fresh_publish_has_one_complete_winner(self):
        start = threading.Barrier(2)
        def attempt():
            start.wait(timeout=5)
            try:
                return self.publish()
            except (CameraDatasetError, FileExistsError):
                return None
        with ThreadPoolExecutor(max_workers=2) as executor:
            results = list(executor.map(lambda unused: attempt(), range(2)))
        winners = [result for result in results if result]
        self.assertEqual(len(winners), 1)
        self.assertEqual(tree_hashes(Path(winners[0]['datasetPath'])), self.solve_hashes)
        self.assert_sources_unchanged()


if __name__ == '__main__':
    unittest.main()
