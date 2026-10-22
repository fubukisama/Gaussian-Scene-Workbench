"""Atomic sibling ownership, bounded names and collision behavior."""

import os
import ctypes
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from native.backend import atomic_file


class AtomicSiblingTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get("GSW_CAMERA_P1_TEST_ROOT", tempfile.gettempdir())) / "gsw-atomic-file"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_text_and_binary_publication_use_ascii_siblings_no_longer_than_target(self):
        for name, mode, value in (("x.json", "w", "相机\n"), ("x", "wb", b"\x00\x01\n")):
            target = self.root / name
            with atomic_file.atomic_sibling(target, mode=mode) as (temporary, stream):
                self.assertEqual(temporary.parent, target.parent)
                self.assertLessEqual(len(temporary.name), len(target.name))
                self.assertTrue(temporary.name.isascii())
                self.assertFalse(target.exists())
                stream.write(value)
                stream.flush()
                os.fsync(stream.fileno())
                stream.close()
                os.replace(str(temporary), str(target))
            if mode == "w":
                self.assertEqual(target.read_text(encoding="utf-8"), value)
            else:
                self.assertEqual(target.read_bytes(), value)
        self.assertEqual({path.name for path in self.root.iterdir()}, {"x.json", "x"})

    def test_owned_planned_directory_uses_exact_name_and_preserves_collisions(self):
        target = self.root / ".u-planned-run"
        with atomic_file.owned_directory(target) as actual:
            self.assertEqual(actual, target.resolve())
            (actual / "owned.bin").write_bytes(b"temporary owned data")
        self.assertFalse(target.exists())
        target.mkdir()
        (target / "foreign.bin").write_bytes(b"existing collision")
        with self.assertRaises(FileExistsError):
            with atomic_file.owned_directory(target):
                self.fail("pre-existing directory is never owned")
        self.assertEqual((target / "foreign.bin").read_bytes(), b"existing collision")

    def test_owned_planned_directory_preserves_replacement_identity(self):
        target = self.root / ".u-owned"
        moved = self.root / "moved"
        with atomic_file.owned_directory(target) as actual:
            (actual / "owned.bin").write_bytes(b"original owned directory")
            actual.rename(moved)
            target.mkdir()
            (target / "foreign.bin").write_bytes(b"replacement directory")
        self.assertEqual((target / "foreign.bin").read_bytes(), b"replacement directory")
        self.assertEqual((moved / "owned.bin").read_bytes(), b"original owned directory")

    def test_uuid_collision_retries_exclusively_and_failed_write_preserves_old_and_foreign_files(self):
        target = self.root / "ready.json"
        target.write_bytes(b"old target")
        foreign = self.root / ("a" * len(target.name))
        foreign.write_bytes(b"foreign file")
        with mock.patch.object(atomic_file.uuid, "uuid4", side_effect=[SimpleNamespace(hex="a" * 32),
                                                                      SimpleNamespace(hex="b" * 32)]):
            with self.assertRaisesRegex(RuntimeError, "controlled write failure"):
                with atomic_file.atomic_sibling(target) as (temporary, stream):
                    stream.write("unpublished new content")
                    raise RuntimeError("controlled write failure")
        self.assertEqual(target.read_bytes(), b"old target")
        self.assertEqual(foreign.read_bytes(), b"foreign file")
        self.assertEqual(set(self.root.iterdir()), {target, foreign})

    def test_short_name_exhaustion_fails_explicitly_without_overwriting_any_file(self):
        target = self.root / "x"
        target.write_bytes(b"old target")
        foreign = []
        for name in "0123456789abcdef":
            path = self.root / name
            path.write_bytes(b"foreign file")
            foreign.append(path)
        with mock.patch.object(atomic_file.uuid, "uuid4", return_value=SimpleNamespace(hex="a" * 32)):
            with self.assertRaisesRegex(FileExistsError, "atomic_temporary_name_exhausted"):
                with atomic_file.atomic_sibling(target):
                    self.fail("an occupied short namespace must not yield a stream")
        self.assertEqual(target.read_bytes(), b"old target")
        self.assertTrue(all(path.read_bytes() == b"foreign file" for path in foreign))
        self.assertEqual(set(self.root.iterdir()), {target, *foreign})

    def test_a_random_name_equal_to_destination_is_never_opened_as_temporary(self):
        target = self.root / "a"
        with mock.patch.object(atomic_file.uuid, "uuid4", side_effect=[SimpleNamespace(hex="a" * 32),
                                                                      SimpleNamespace(hex="b" * 32)]):
            with atomic_file.atomic_sibling(target) as (temporary, stream):
                self.assertNotEqual(temporary, target)
                self.assertFalse(target.exists())
                stream.write("not published")
        self.assertEqual(list(self.root.iterdir()), [])

    def test_closed_stream_cleanup_does_not_close_a_reused_descriptor(self):
        target = self.root / "ready.json"
        replacement = None
        try:
            with atomic_file.atomic_sibling(target, mode="wb") as (temporary, stream):
                descriptor = stream.fileno()
                stream.close()
                other = os.open(str(self.root / "other.bin"), os.O_WRONLY | os.O_CREAT | os.O_EXCL
                                | getattr(os, "O_BINARY", 0), 0o666)
                if other != descriptor:
                    os.dup2(other, descriptor)
                    os.close(other)
                replacement = os.fdopen(descriptor, "wb")
            replacement.write(b"descriptor still owned by replacement")
            replacement.flush()
            self.assertFalse(temporary.exists())
        finally:
            if replacement is not None:
                replacement.close()
        self.assertEqual((self.root / "other.bin").read_bytes(), b"descriptor still owned by replacement")

    def test_cleanup_preserves_a_foreign_file_created_at_the_published_temporary_name(self):
        target = self.root / "ready.json"
        with atomic_file.atomic_sibling(target) as (temporary, stream):
            stream.write("published")
            stream.close()
            os.replace(str(temporary), str(target))
            temporary.write_bytes(b"new foreign file")
        self.assertEqual(target.read_text(encoding="utf-8"), "published")
        self.assertEqual(temporary.read_bytes(), b"new foreign file")

    def test_directory_publication_is_exclusive_bounded_and_cleans_nested_owned_content(self):
        target = self.root / "version"
        with atomic_file.atomic_sibling_directory(target) as temporary:
            self.assertEqual(temporary.parent, target.parent.resolve(strict=True))
            self.assertLessEqual(len(temporary.name), len(target.name))
            self.assertTrue(temporary.name.isascii())
            self.assertNotEqual(temporary.name.casefold(), target.name.casefold())
            (temporary / "nested").mkdir()
            (temporary / "nested" / "data").write_bytes(b"owned")
            os.rename(str(temporary), str(target))
            temporary.mkdir()
            (temporary / "foreign").write_bytes(b"preserve replacement")
        self.assertEqual((target / "nested" / "data").read_bytes(), b"owned")
        self.assertEqual((temporary / "foreign").read_bytes(), b"preserve replacement")
        with self.assertRaisesRegex(RuntimeError, "cancel owned directory"):
            with atomic_file.atomic_sibling_directory(self.root / "second") as cancelled:
                (cancelled / "nested").mkdir()
                (cancelled / "nested" / "file").write_bytes(b"not published")
                raise RuntimeError("cancel owned directory")
        self.assertFalse(cancelled.exists())

    def test_directory_collisions_and_equal_target_are_not_owned_or_removed(self):
        target = self.root / "a"
        foreign = self.root / "b"
        foreign.mkdir()
        (foreign / "preserve").write_bytes(b"foreign")
        with mock.patch.object(atomic_file.uuid, "uuid4", side_effect=[SimpleNamespace(hex="a" * 32),
                SimpleNamespace(hex="b" * 32), SimpleNamespace(hex="c" * 32)]):
            with atomic_file.atomic_sibling_directory(target) as temporary:
                self.assertEqual(temporary.name, "c")
        self.assertFalse(target.exists())
        self.assertEqual((foreign / "preserve").read_bytes(), b"foreign")
        with mock.patch.object(atomic_file.uuid, "uuid4", return_value=SimpleNamespace(hex="b" * 32)) as random:
            with self.assertRaisesRegex(FileExistsError, "atomic_temporary_name_exhausted"):
                with atomic_file.atomic_sibling_directory(target):
                    self.fail("occupied directory namespace must not yield")
            self.assertEqual(random.call_count, 128)
        self.assertEqual((foreign / "preserve").read_bytes(), b"foreign")

    def test_directory_cleanup_rejects_a_replaced_parent_and_never_follows_reparse(self):
        parent = self.root / "parent"
        parent.mkdir()
        with atomic_file.atomic_sibling_directory(parent / "version") as temporary:
            (temporary / "owned").write_bytes(b"owned")
            moved = self.root / "moved-parent"
            parent.rename(moved)
            parent.mkdir()
            foreign = parent / temporary.name
            foreign.mkdir()
            (foreign / "preserve").write_bytes(b"foreign parent replacement")
        self.assertEqual((foreign / "preserve").read_bytes(), b"foreign parent replacement")
        self.assertEqual((moved / temporary.name / "owned").read_bytes(), b"owned")
        outside = self.root / "outside"
        outside.mkdir()
        sentinel = outside / "preserve"
        sentinel.write_bytes(b"outside")
        try:
            with atomic_file.atomic_sibling_directory(self.root / "linked-version") as linked:
                os.symlink(str(outside), str(linked / "link"), target_is_directory=True)
        except OSError as error:
            if error.errno not in (1, 13) and getattr(error, "winerror", None) != 1314:
                raise
        self.assertEqual(sentinel.read_bytes(), b"outside")

    @unittest.skipUnless(os.name == "nt", "Windows 8.3 alias behavior")
    def test_read_only_existing_short_alias_identifies_the_same_real_parent(self):
        # Metadata only: never create test outputs in the user/system profile.
        original = Path(os.environ["USERPROFILE"])
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.windll.kernel32.GetShortPathNameW(str(original), buffer, len(buffer))
        self.assertGreater(size, 0)
        self.assertLess(size, len(buffer))
        alias = Path(buffer.value)
        if alias == original.resolve(strict=True):
            self.skipTest("existing profile has no different 8.3 alias; volume settings are not changed")
        self.assertTrue(os.path.samestat(alias.lstat(), original.lstat()))
        checked, identity = atomic_file._checked_directory(alias)
        self.assertEqual(checked, original.resolve(strict=True))
        self.assertTrue(os.path.samestat(identity, original.lstat()))

    @unittest.skipUnless(os.name == "nt", "Windows 8.3 alias behavior")
    def test_owned_short_alias_directory_publishes_in_the_canonical_d_parent(self):
        parent = self.root / "owned directory with an alias"
        parent.mkdir()
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.windll.kernel32.GetShortPathNameW(str(parent), buffer, len(buffer))
        self.assertGreater(size, 0)
        self.assertLess(size, len(buffer))
        alias = Path(buffer.value)
        if alias == parent.resolve(strict=True):
            self.skipTest("D test volume does not provide a distinct 8.3 alias; configuration is unchanged")
        target = parent / "version"
        with atomic_file.atomic_sibling_directory(alias / target.name) as temporary:
            self.assertEqual(temporary.parent, parent.resolve(strict=True))
            (temporary / "data").write_bytes(b"published through an alias")
            os.rename(str(temporary), str(target))
        self.assertEqual((target / "data").read_bytes(), b"published through an alias")
        self.assertEqual(set(parent.iterdir()), {target})


if __name__ == "__main__":
    unittest.main()
