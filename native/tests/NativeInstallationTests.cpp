#include "NativeInstallation.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <QScopeGuard>
#ifdef Q_OS_WIN
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#endif

class NativeInstallationTests final : public QObject {
  Q_OBJECT
  QTemporaryDir temporary;
  QString root;
  QByteArray digest;
  void write(const QString &path, const QByteArray &bytes) {
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
  }
  void activate(const QString &version, const QByteArray &sha = {}) {
    QSaveFile file(QDir(root).filePath("current.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"schema", "gsw.native_active_version/1"},
        {"version", version}, {"executableSha256", QString::fromLatin1(sha.isEmpty() ? digest : sha)}}).toJson());
    QVERIFY(file.commit());
  }
private slots:
  void init() {
    QVERIFY(temporary.isValid());
    root = QDir(temporary.path()).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
    write(QDir(root).filePath("bin/Gaussian Scene Workbench Launcher.exe"), "owned launcher fixture");
    const QByteArray content("owned application fixture");
    digest = QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
    for (const auto &id : {"0.3.8-first", "0.3.8-next"})
      write(QDir(root).filePath(QString("versions/%1/bin/Gaussian Scene Workbench.exe").arg(id)), content);
    activate("0.3.8-first");
  }
  void stableEntrySwitchesVersionsWithoutChangingOldPlanOrFiles() {
    auto old = gsw::resolveNativeInstallation(root, false, {"--project", "owned project.gsw"}, {});
    QVERIFY(old.valid()); QCOMPARE(old.version, "0.3.8-first");
    activate("0.3.8-next");
    auto next = gsw::resolveNativeInstallation(root, false, old.arguments, {});
    QVERIFY(next.valid()); QCOMPARE(next.version, "0.3.8-next");
    QVERIFY(next.program != old.program); QVERIFY(QFileInfo(old.program).isFile());
    QCOMPARE(gsw::managedInstallationForApplication(old.program), root);
    QCOMPARE(gsw::managedInstallationForApplication(next.program), root);
    activate("0.3.8-first");
    QCOMPARE(gsw::resolveNativeInstallation(root, false, {}, {}).program, old.program);
  }
  void diagnosticsAreOptInForOneChildAndArgumentsAreNotShellCommands() {
    QProcessEnvironment inherited;
    inherited.insert("RETAIN_ME", "owned value");
    inherited.insert("GSW_RENDER_DIAGNOSTICS_DIR", "unrequested inherited directory");
    const QStringList arguments{"--project", "name with spaces & ; $(not-a-command).gsw"};
    auto normal = gsw::resolveNativeInstallation(root, false, arguments, inherited);
    QVERIFY(normal.valid()); QCOMPARE(normal.arguments, arguments);
    QCOMPARE(normal.environment.value("RETAIN_ME"), "owned value");
    QVERIFY(!normal.environment.contains("GSW_RENDER_DIAGNOSTICS_DIR"));
    const auto diagnostic = gsw::resolveNativeInstallation(root, true, arguments, inherited);
    QVERIFY(diagnostic.valid());
    QCOMPARE(diagnostic.environment.value("GSW_RENDER_DIAGNOSTICS_DIR"), QDir(root).filePath("diagnostics"));
    QCOMPARE(inherited.value("GSW_RENDER_DIAGNOSTICS_DIR"), "unrequested inherited directory");
  }
  void rejectsTraversalReservedVersionsAndUnrecognizedManifestFields() {
    for (const auto &version : {"../outside", "..", "CON", "a/child", "a\\child", "trailing."}) {
      activate(QString::fromLatin1(version));
      QVERIFY(!gsw::resolveNativeInstallation(root, false, {}, {}).valid());
    }
    activate("0.3.8-first");
    QFile file(QDir(root).filePath("current.json")); QVERIFY(file.open(QIODevice::ReadOnly));
    auto object = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    object.insert("program", "untrusted-other.exe");
    write(file.fileName(), QJsonDocument(object).toJson());
    QVERIFY(!gsw::resolveNativeInstallation(root, false, {}, {}).valid());
  }
  void rejectsCorruptionMissingFilesAndDoesNotGuessAnOlderVersion() {
    activate("0.3.8-next", QByteArray(64, '0'));
    auto result = gsw::resolveNativeInstallation(root, false, {}, {});
    QVERIFY(!result.valid()); QCOMPARE(result.error, "managed_installation_integrity_failed");
    activate("not-installed");
    QVERIFY(!gsw::resolveNativeInstallation(root, false, {}, {}).valid());
    write(QDir(root).filePath("current.json"), "{partial");
    QVERIFY(!gsw::resolveNativeInstallation(root, false, {}, {}).valid());
  }
  void windowsLinksPreserveUserOptionsAndUseStableApplicationIdentity() {
#ifdef Q_OS_WIN
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QVERIFY(SUCCEEDED(com));
    const auto cleanup = qScopeGuard([] { CoUninitialize(); });
    const QString path = QDir::toNativeSeparators(QDir(root).filePath("owned shortcut.lnk"));
    const QString launcher = QDir(root).filePath("bin/Gaussian Scene Workbench Launcher.exe");
    IShellLinkW *original = nullptr;
    QVERIFY(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&original))));
    const auto releaseOriginal = qScopeGuard([&] { original->Release(); });
    QVERIFY(SUCCEEDED(original->SetPath(L"C:\\Windows\\not-executed.exe")));
    QVERIFY(SUCCEEDED(original->SetHotkey(MAKEWORD('G', HOTKEYF_CONTROL | HOTKEYF_ALT))));
    QVERIFY(SUCCEEDED(original->SetShowCmd(SW_SHOWMAXIMIZED)));
    QVERIFY(SUCCEEDED(original->SetDescription(L"user description retained")));
    IPersistFile *save = nullptr;
    QVERIFY(SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&save))));
    const auto releaseSave = qScopeGuard([&] { save->Release(); });
    QVERIFY(SUCCEEDED(save->Save(reinterpret_cast<LPCOLESTR>(path.utf16()), TRUE)));
    // Verify the on-disk fixture first; retaining it is an actual contract, not
    // a comparison against an unpersisted original COM object's cached state.
    IShellLinkW *before = nullptr;
    QVERIFY(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&before))));
    const auto releaseBefore = qScopeGuard([&] { before->Release(); });
    IPersistFile *beforeFile = nullptr;
    QVERIFY(SUCCEEDED(before->QueryInterface(IID_PPV_ARGS(&beforeFile))));
    const auto releaseBeforeFile = qScopeGuard([&] { beforeFile->Release(); });
    QVERIFY(SUCCEEDED(beforeFile->Load(reinterpret_cast<LPCOLESTR>(path.utf16()), STGM_READ)));
    wchar_t beforeDescription[4096]{};
    QVERIFY(SUCCEEDED(before->GetDescription(beforeDescription, 4096)));
    QCOMPARE(QString::fromWCharArray(beforeDescription), "user description retained");
    const QString args = "--project \"name with spaces & ;.gsw\"";
    QString diagnostic;
    QVERIFY2(gsw::writeNativeLauncherShortcut(path, launcher, args, "GSW.Test.Stable", &diagnostic), qPrintable(diagnostic));
    IShellLinkW *loaded = nullptr;
    QVERIFY(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loaded))));
    const auto releaseLoaded = qScopeGuard([&] { loaded->Release(); });
    IPersistFile *load = nullptr;
    QVERIFY(SUCCEEDED(loaded->QueryInterface(IID_PPV_ARGS(&load))));
    const auto releaseLoad = qScopeGuard([&] { load->Release(); });
    QVERIFY(SUCCEEDED(load->Load(reinterpret_cast<LPCOLESTR>(path.utf16()), STGM_READ)));
    wchar_t buffer[4096]{};
    QVERIFY(SUCCEEDED(loaded->GetPath(buffer, 4096, nullptr, SLGP_RAWPATH)));
    QCOMPARE(QDir::fromNativeSeparators(QString::fromWCharArray(buffer)), launcher);
    QVERIFY(SUCCEEDED(loaded->GetArguments(buffer, 4096)));
    QCOMPARE(QString::fromWCharArray(buffer), args);
    QVERIFY(SUCCEEDED(loaded->GetDescription(buffer, 4096)));
    QCOMPARE(QString::fromWCharArray(buffer), "user description retained");
    WORD hotkey = 0; int show = 0;
    QVERIFY(SUCCEEDED(loaded->GetHotkey(&hotkey))); QVERIFY(SUCCEEDED(loaded->GetShowCmd(&show)));
    QCOMPARE(hotkey, MAKEWORD('G', HOTKEYF_CONTROL | HOTKEYF_ALT)); QCOMPARE(show, SW_SHOWMAXIMIZED);
    IPropertyStore *store = nullptr;
    QVERIFY(SUCCEEDED(loaded->QueryInterface(IID_PPV_ARGS(&store))));
    const auto releaseStore = qScopeGuard([&] { store->Release(); });
    PROPVARIANT value; PropVariantInit(&value);
    QVERIFY(SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)));
    QCOMPARE(QString::fromWCharArray(value.pwszVal), "GSW.Test.Stable"); PropVariantClear(&value);
    QVERIFY(!gsw::writeNativeLauncherShortcut(QDir(root).filePath("not-a-link.txt"), launcher, {}, "GSW.Test.Stable"));
#endif
  }
  void hiddenWindowsRelaunchThroughTheStableEntryAfterVersionSwitch() {
#ifdef Q_OS_WIN
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QVERIFY(SUCCEEDED(com)); const auto cleanup = qScopeGuard([] { CoUninitialize(); });
    HWND window = CreateWindowExW(0, L"STATIC", L"owned remote test", WS_OVERLAPPED,
                                  0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    QVERIFY(window); const auto destroy = qScopeGuard([&] { DestroyWindow(window); });
    const QString oldApplication = QDir(root).filePath("versions/0.3.8-first/bin/Gaussian Scene Workbench.exe");
    activate("0.3.8-next");
    QVERIFY(gsw::setManagedTaskbarRelaunch(window, oldApplication, "GSW.Test.Stable"));
    IPropertyStore *store = nullptr;
    QVERIFY(SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store))));
    const auto release = qScopeGuard([&] { store->Release(); });
    PROPVARIANT value; PropVariantInit(&value);
    QVERIFY(SUCCEEDED(store->GetValue(PKEY_AppUserModel_RelaunchCommand, &value)));
    QCOMPARE(QString::fromWCharArray(value.pwszVal), QLatin1Char('"') +
        QDir::toNativeSeparators(QDir(root).filePath("bin/Gaussian Scene Workbench Launcher.exe")) + QLatin1Char('"'));
    PropVariantClear(&value);
    QVERIFY(SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)));
    QCOMPARE(QString::fromWCharArray(value.pwszVal), "GSW.Test.Stable"); PropVariantClear(&value);
    gsw::clearManagedTaskbarRelaunch(window, oldApplication);
    QVERIFY(SUCCEEDED(store->GetValue(PKEY_AppUserModel_RelaunchCommand, &value)));
    QCOMPARE(value.vt, VARTYPE(VT_EMPTY)); PropVariantClear(&value);
    QVERIFY(gsw::setManagedTaskbarRelaunch(nullptr, "not-a-managed-install.exe", "GSW.Test.Stable"));
#endif
  }
};
QTEST_GUILESS_MAIN(NativeInstallationTests)
#include "NativeInstallationTests.moc"
