# Qt Windows process argument quoting

The small quoting function is adapted from the official Qt **6.8.3** source,
qtbase commit `c07c2d5a527a644d36e7853d55132ae38921682f`:

https://github.com/qt/qtbase/blob/c07c2d5a527a644d36e7853d55132ae38921682f/src/corelib/io/qprocess_win.cpp

Original complete source SHA-256:
`a555666766d6e9d8504fb4d55125de38e3ea1578e390abc427dadfece6619067`.
The original file offers commercial, LGPL-3.0-only, GPL-2.0-only or GPL-3.0-only
licensing. This copy selects **GPL-3.0-only** and retains the original copyright
holders. `LICENSE.txt` is the original Qt `LICENSES/GPL-3.0-only.txt` at the same
commit (SHA-256
`8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903`).

Changes: a GSW namespace and name, inline/header placement, and removal of the
unused raw `nativeArguments` escape hatch. The actual escaping algorithm is
unchanged. GSW calls `CreateProcessW` with an explicit absolute application path;
it does not invoke a command shell or search PATH. The GSW process-ownership,
environment, IPC and health logic is separate and is not claimed to be Qt code.
