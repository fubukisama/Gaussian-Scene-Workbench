"""Exclusive atomic-publication siblings that never lengthen a target path.

The caller owns serialization, flush/fsync and publication with os.replace.
The context owns only its exclusive temporary stream and file. It does not
enable paths which the operating system rejects for the destination itself.
"""

import codecs
from contextlib import contextmanager
import errno
import os
from pathlib import Path
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
