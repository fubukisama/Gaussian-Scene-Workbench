"""Exclusive atomic-publication siblings that never lengthen a target path.

The caller owns serialization, flush/fsync and final file/directory publication.
The contexts own only their exclusive temporary file or directory identity.
They do not enable paths which the operating system rejects for the destination
itself, and directory cleanup is not a hostile-concurrency security boundary.
"""

import codecs
from contextlib import contextmanager
import errno
import os
from pathlib import Path
import shutil
import stat
import uuid


def _remove_owned(path, identity):
    try:
        current = path.lstat()
    except FileNotFoundError:
        return
    # Never remove an already existing collision or a new file installed at
    # the old temporary name after the caller published its owned file.
    if stat.S_ISREG(current.st_mode) and os.path.samestat(identity, current):
        path.unlink()


def _checked_directory(path):
    path = Path(path).absolute()
    for item in (path, *path.parents):
        metadata = item.lstat()
        if (not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode)
                or getattr(metadata, "st_file_attributes", 0) & 0x400):
            raise ValueError("atomic_unsafe_directory")
    resolved = path.resolve(strict=True)
    if resolved != path:
        raise ValueError("atomic_unsafe_directory")
    return resolved, path.lstat()


def _remove_owned_directory(path, identity, parent, parent_identity):
    try:
        current_parent, metadata = _checked_directory(parent)
        current = path.lstat()
        if (current_parent != parent or not os.path.samestat(parent_identity, metadata)
                or not os.path.samestat(identity, current)
                or not stat.S_ISDIR(current.st_mode) or stat.S_ISLNK(current.st_mode)
                or getattr(current, "st_file_attributes", 0) & 0x400
                or path.resolve(strict=True).parent != parent):
            return
        # Do not traverse reparse points. Refuse cleanup if a caller or another
        # process inserted one, rather than following it outside the stage.
        for directory, subdirectories, files in os.walk(str(path), followlinks=False):
            for name in subdirectories + files:
                item = (Path(directory) / name).lstat()
                if stat.S_ISLNK(item.st_mode) or getattr(item, "st_file_attributes", 0) & 0x400:
                    return
        current_parent, metadata = _checked_directory(parent)
        if (current_parent != parent or not os.path.samestat(parent_identity, metadata)
                or not os.path.samestat(identity, path.lstat())
                or path.resolve(strict=True).parent != parent):
            return
        # Windows Python >=3.8 does not descend into directory junctions in
        # rmtree. These ownership/preflight checks are not a claim of complete
        # resistance to hostile concurrent path replacement on non-fd systems.
        shutil.rmtree(str(path))
    except (FileNotFoundError, NotADirectoryError, ValueError):
        return


@contextmanager
def atomic_sibling_directory(path):
    """Yield an exclusive same-parent stage whose basename never grows.

    The caller retains its fresh-destination guards and os.rename publication.
    The context removes only the original directory identity in the original
    verified parent; a replacement or a reparse tree is left untouched.
    """
    path = Path(path)
    if not path.name or path.name in (".", ".."):
        raise ValueError("atomic_invalid_target_or_mode")
    parent, parent_identity = _checked_directory(path.parent)
    target = parent / path.name
    length = min(32, len(path.name))
    temporary, identity = None, None
    for _ in range(128):
        candidate = target.with_name(uuid.uuid4().hex[:length])
        if candidate.name.casefold() == target.name.casefold():
            continue
        checked, metadata = _checked_directory(parent)
        if checked != parent or not os.path.samestat(parent_identity, metadata):
            raise ValueError("atomic_unsafe_directory")
        try:
            candidate.mkdir()
        except FileExistsError:
            continue
        temporary, identity = candidate, candidate.lstat()
        break
    if temporary is None:
        raise FileExistsError(errno.EEXIST, "atomic_temporary_name_exhausted", str(target))
    try:
        yield temporary
    finally:
        _remove_owned_directory(temporary, identity, parent, parent_identity)


@contextmanager
def atomic_sibling(path, mode="w", encoding="utf-8"):
    """Yield (same-directory temporary Path, exclusively owned file stream).

    The random ASCII basename is at most min(32, len(target.name)) characters,
    with no prefix or extension. A tiny basename has a finite namespace: after
    128 collisions it fails explicitly, preserving the target and all foreign
    files. The caller must close the stream before os.replace on Windows.
    Closing it again on exit is safe even if its descriptor has since been
    reused; the context never closes a bare descriptor after yielding.
    """
    path = Path(path)
    if mode not in ("w", "wb") or not path.name:
        raise ValueError("atomic_invalid_target_or_mode")
    if mode == "w":
        codecs.lookup(encoding)
    length = min(32, len(path.name))
    temporary, descriptor, identity = None, None, None
    for _ in range(128):
        candidate = path.with_name(uuid.uuid4().hex[:length])
        if candidate.name.casefold() == path.name.casefold():
            continue
        try:
            descriptor = os.open(str(candidate), os.O_WRONLY | os.O_CREAT | os.O_EXCL
                                 | getattr(os, "O_BINARY", 0), 0o666)
        except FileExistsError:
            continue
        temporary = candidate
        identity = os.fstat(descriptor)
        break
    if temporary is None:
        raise FileExistsError(errno.EEXIST, "atomic_temporary_name_exhausted", str(path))
    stream = None
    try:
        stream = os.fdopen(descriptor, mode, **({"encoding": encoding} if mode == "w" else {}))
        descriptor = None  # Ownership belongs solely to the idempotent stream.
        yield temporary, stream
    finally:
        try:
            if stream is not None:
                stream.close()
            elif descriptor is not None:
                os.close(descriptor)  # Wrapper creation failed before yielding.
        finally:
            _remove_owned(temporary, identity)
