#include <QCoreApplication>
#include <QDir>
#include <QString>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif

// Remote integration fixture only. Uses the same Unicode Windows boundary as
// test_windows_installer.ps1, not WScript.Shell's ANSI shortcut-name handling.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
  const auto args = app.arguments();
  if (args.size() != 4) return 2;
  const QString path = QDir::toNativeSeparators(args.at(1));
  const QString target = QDir::toNativeSeparators(args.at(2));
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return 3;
  IShellLinkW *link = nullptr; IPersistFile *file = nullptr;
  bool good = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)));
  if (good) good = SUCCEEDED(link->SetPath(reinterpret_cast<LPCWSTR>(target.utf16()))) &&
      SUCCEEDED(link->SetArguments(reinterpret_cast<LPCWSTR>(args.at(3).utf16()))) &&
      SUCCEEDED(link->SetDescription(L"retained user description")) && SUCCEEDED(link->SetShowCmd(SW_SHOWMAXIMIZED)) &&
      SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
  if (good) good = SUCCEEDED(file->Save(reinterpret_cast<LPCOLESTR>(path.utf16()), TRUE));
  if (file) file->Release(); if (link) link->Release(); CoUninitialize();
  return good ? 0 : 4;
#else
  return 2;
#endif
}
