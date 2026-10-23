"""Proof inputs remain deployable without retired HTML documentation."""
from pathlib import Path
import tempfile
import unittest

from training_kit.quality_rasterizer.build import glm_source_files


class SourceClosureTests(unittest.TestCase):
    def test_only_compiler_headers_and_license_are_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wanted = ('glm/glm.hpp', 'glm/detail/type_vec.inl', 'copying.txt')
            excluded = ('doc/html/api.html', 'doc/api.h', 'test/sample.cpp',
                        'readme.md', '.git/config', 'glm/.cache/generated.h')
            for name in wanted + excluded:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(name, encoding='utf-8')
            self.assertEqual({p.relative_to(root).as_posix() for p in glm_source_files(root)}, set(wanted))


if __name__ == '__main__':
    unittest.main()
