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
#include <shlwapi.h>
#endif

class NativeInstallationTests final : public QObject {
  Q_OBJECT
  QTemporaryDir temporary{QDir(QCoreApplication::applicationDirPath()).filePath("managed-installation-tests-XXXXXX")};
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
  void offlineInspectionNeedsNoHelperAndOnlineBootstrapFailsClosedWithoutOne() {
    const QString bootstrap = QDir(root).filePath("bootstrap.json");
    write(bootstrap, "{\"schema\":\"gsw.native_bootstrap/1\",\"protocol\":1,\"files\":[]}");
    auto plan = gsw::resolveNativeInstallation(root, false, {}, {});
    QVERIFY(plan.valid());
    QCOMPARE(plan.executableSha256, QString::fromLatin1(digest));
    QString error;
    QVERIFY(gsw::verifyNativeInstallationLaunch(plan, &error));
    write(bootstrap, "{\"schema\":\"gsw.native_bootstrap/1\",\"protocol\":2}");
    QVERIFY(!gsw::verifyNativeInstallationLaunch(plan, &error));
    QCOMPARE(error, QStringLiteral("managed_installation_integrity_failed"));
    write(bootstrap, "{\"schema\":\"gsw.native_bootstrap/1\",\"protocol\":2,\"protocol\":1}");
    QVERIFY(!gsw::verifyNativeInstallationLaunch(plan, &error));
    QCOMPARE(error, QStringLiteral("managed_installation_manifest_invalid"));
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
  void resolvesAnOwnedLongUnicodeInstallationWithoutExposingDeviceNamespace() {
#ifdef Q_OS_WIN
    while (root.size() < 350)
      root += QLatin1Char('/') + QString::fromUtf8("中文_日本語_") + QString(44, QLatin1Char('x'));
    write(QDir(root).filePath("bin/Gaussian Scene Workbench Launcher.exe"), "owned launcher fixture");
    write(QDir(root).filePath("versions/0.3.8-first/bin/Gaussian Scene Workbench.exe"), "owned application fixture");
    activate("0.3.8-first");
    const auto result = gsw::resolveNativeInstallation(root, false, {}, {});
    QVERIFY2(result.valid(), qPrintable(result.error));
    QCOMPARE(result.root, root);
    QVERIFY(result.program.size() > 350);
    QVERIFY(!result.program.startsWith(QLatin1String("\\\\?\\")));
    QCOMPARE(gsw::managedInstallationForApplication(result.program), root);
#endif
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
    // Deliberately dangling: the .lnk itself still exists and must be loaded,
    // not mistaken for a new shortcut by QFileInfo's target-following checks.
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
    const auto beforeDetails = gsw::readNativeShortcut(path);
    QVERIFY(beforeDetails.valid()); QCOMPARE(beforeDetails.description, "user description retained");
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
    const QString freshPath = QDir(root).filePath("fresh owned shortcut.lnk");
    QVERIFY2(gsw::writeNativeLauncherShortcut(freshPath, launcher, {}, "GSW.Test.Stable", &diagnostic), qPrintable(diagnostic));
    QVERIFY(QFileInfo(freshPath).isFile());
    const auto readback = gsw::readNativeShortcut(path);
    QVERIFY(readback.valid()); QCOMPARE(readback.description, "user description retained");
    QCOMPARE(readback.arguments, args);
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
  void legacyBootstrapCannotRestoreTheOldRunningApplicationDisplayName() {
#ifdef Q_OS_WIN
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QVERIFY(SUCCEEDED(com));
    const auto cleanup = qScopeGuard([] { CoUninitialize(); });
    const QString launcher = QDir(root).filePath("bin/Gaussian Scene Workbench Launcher.exe");
    const QString legacyFixture = QDir(QCoreApplication::applicationDirPath()).filePath(
        "gsw_native_legacy_brand_fixture.exe");
    QVERIFY(QFileInfo(legacyFixture).isFile());
    QVERIFY(QFile::remove(launcher));
    QVERIFY(QFile::copy(legacyFixture, launcher));
    for (const auto &version : {"0.3.8-first", "0.3.8-next"}) {
      const QString application = QDir(root).filePath(
          QString("versions/%1/bin/Gaussian Scene Workbench.exe").arg(version));
      QVERIFY(QFile::remove(application));
      QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), application));
    }
    const auto hash = [](const QString &path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) return QByteArray();
      return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    };
    const QByteArray launcherBefore = hash(launcher);
    digest = hash(QCoreApplication::applicationFilePath()).toHex();
    activate("0.3.8-next");
    const QByteArray pointerBefore = hash(QDir(root).filePath("current.json"));
    const auto resolveResource = [](const QString &reference) {
      wchar_t resolved[256]{};
      if (FAILED(SHLoadIndirectString(reinterpret_cast<LPCWSTR>(reference.utf16()),
                                     resolved, 256, nullptr))) return QString();
      return QString::fromWCharArray(resolved);
    };
    // Prove that this really models a retained pre-rename bootstrap, not two
    // binaries that coincidentally expose the same new string resource.
    const QString oldReference = QLatin1Char('@') + QDir::toNativeSeparators(launcher) + ",-201";
    QCOMPARE(resolveResource(oldReference), QStringLiteral("Gaussian Scene Workbench"));
    const QString runningApplication = QDir(root).filePath(
        "versions/0.3.8-first/bin/Gaussian Scene Workbench.exe");
    HWND window = CreateWindowExW(0, L"STATIC", L"owned hidden brand regression", WS_OVERLAPPED,
                                  0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    QVERIFY(window);
    const auto destroy = qScopeGuard([&] { DestroyWindow(window); });
    const auto clear = qScopeGuard([&] { gsw::clearManagedTaskbarRelaunch(window, runningApplication); });
    QVERIFY(gsw::setManagedTaskbarRelaunch(window, runningApplication, "GSW.Test.Stable"));
    IPropertyStore *store = nullptr;
    QVERIFY(SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store))));
    const auto release = qScopeGuard([&] { store->Release(); });
    const auto stringProperty = [&](REFPROPERTYKEY key) {
      PROPVARIANT value; PropVariantInit(&value);
      if (FAILED(store->GetValue(key, &value))) return QString();
      const QString result = value.vt == VT_LPWSTR && value.pwszVal
          ? QString::fromWCharArray(value.pwszVal) : QString();
      PropVariantClear(&value);
      return result;
    };
    // Relaunch stays on the stable entry, even if a newer slot is now active.
    QCOMPARE(stringProperty(PKEY_AppUserModel_RelaunchCommand),
        QLatin1Char('"') + QDir::toNativeSeparators(launcher) + QLatin1Char('"'));
    QCOMPARE(stringProperty(PKEY_AppUserModel_ID), QStringLiteral("GSW.Test.Stable"));
    const QString displayReference = stringProperty(PKEY_AppUserModel_RelaunchDisplayNameResource);
    QCOMPARE(displayReference, QLatin1Char('@') +
        QDir::toNativeSeparators(runningApplication) + ",-201");
    QCOMPARE(resolveResource(displayReference), QStringLiteral("Gaussian Splatting Workbench"));
    QCOMPARE(hash(launcher), launcherBefore);
    QCOMPARE(hash(QDir(root).filePath("current.json")), pointerBefore);
    gsw::clearManagedTaskbarRelaunch(window, runningApplication);
    for (const auto &key : {PKEY_AppUserModel_ID, PKEY_AppUserModel_RelaunchCommand,
                           PKEY_AppUserModel_RelaunchDisplayNameResource, PKEY_AppUserModel_RelaunchIconResource}) {
      PROPVARIANT value; PropVariantInit(&value);
      QVERIFY(SUCCEEDED(store->GetValue(key, &value)));
      QCOMPARE(value.vt, VARTYPE(VT_EMPTY));
      PropVariantClear(&value);
    }
#endif
  }
};
QTEST_GUILESS_MAIN(NativeInstallationTests)
#include "NativeInstallationTests.moc"
