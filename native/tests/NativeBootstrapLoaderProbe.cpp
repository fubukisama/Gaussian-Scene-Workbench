// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic control only: no install, token mutation, trust, network or GUI.
#include <windows.h>
#ifndef GSW_BOOTSTRAP_FIXTURE_ONLY
#error Remote bootstrap diagnostics only.
#endif
int wmain() {
  static constexpr char marker[] = "{\"schema\":\"gsw.bootstrap_loader_probe/1\",\"enteredMain\":true}\n";
  const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!output || output == INVALID_HANDLE_VALUE) return 3;
  DWORD written = 0;
  return WriteFile(output, marker, DWORD(sizeof(marker) - 1), &written, nullptr) && written == sizeof(marker) - 1 ? 0 : 4;
}
