#include "NativeInstallation.h"
#include "NativeStartupHealth.h"
#include "NativeUpdateLocalPath.h"
#include "../third_party/nlohmann/json.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QProcess>
#include <QThread>
#include <QUuid>
#include <set>
#include <stdexcept>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace gsw {
namespace {
bool pathExists(const QString &path) {
#ifdef Q_OS_WIN
  // QFileInfo treats .lnk as a symlink, so exists() tests the target instead
  // of the link file. Installation metadata must inspect the actual file.
  const auto api = updatepaths::nativeApiPath(path);
  return GetFileAttributesW(reinterpret_cast<LPCWSTR>(api.utf16())) != INVALID_FILE_ATTRIBUTES;
#else
  const QFileInfo file(path);
  return file.exists() || file.isSymbolicLink();
#endif
}
bool ordinaryPath(const QString &path) {
#ifdef Q_OS_WIN
  const auto api = updatepaths::nativeApiPath(path);
  const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(api.utf16()));
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  const QFileInfo file(path);
  return file.exists() && !file.isSymLink();
#endif
}

bool localOrdinaryTree(const QString &path) {
  if (!QDir::isAbsolutePath(path) || path.startsWith(QLatin1String("//")) ||
      path.startsWith(QLatin1String("\\\\"))) return false;
  QString cursor = QDir::cleanPath(path);
  for (;;) {
    if (!ordinaryPath(cursor)) return false;
    const QString parent = QFileInfo(cursor).absolutePath();
    if (parent == cursor) return true;
    cursor = parent;
  }
}

bool versionName(const QString &value) {
  static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$"));
  static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$"),
                                             QRegularExpression::CaseInsensitiveOption);
  return allowed.match(value).hasMatch() && !value.endsWith(QLatin1Char('.')) &&
      !reserved.match(value).hasMatch();
}

QString launcherFor(const QString &root) {
  return QDir(root).filePath(QLatin1String("bin/") + QString::fromLatin1(kNativeLauncherName));
}

bool holdOrdinaryFile(QFile &file, const QString &path) {
  if (!localOrdinaryTree(path) || !QFileInfo(path).isFile()) return false;
  file.setFileName(path);
#ifdef Q_OS_WIN
  const auto api = updatepaths::nativeApiPath(path);
  HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(api.utf16()), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  wchar_t buffer[32768]{};
  const auto count = GetFinalPathNameByHandleW(handle, buffer, 32768, FILE_NAME_NORMALIZED);
  QString actual = count > 0 && count < 32768 ? QString::fromWCharArray(buffer) : QString();
  if (actual.startsWith(QLatin1String("\\\\?\\"))) actual.remove(0, 4);
  if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      actual.compare(QDir::toNativeSeparators(QDir::cleanPath(path)), Qt::CaseInsensitive)) {
    CloseHandle(handle); return false;
  }
  const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
  if (descriptor < 0) { CloseHandle(handle); return false; }
  if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
    _close(descriptor); return false;
  }
  return true;
#else
  return file.open(QIODevice::ReadOnly);
#endif
}

int bootstrapProtocol(const QString &root) {
  QFile bootstrap;
  if (!holdOrdinaryFile(bootstrap, QDir(root).filePath(QStringLiteral("bootstrap.json"))) ||
      bootstrap.size() <= 0 || bootstrap.size() > 4 * 1024 * 1024) return 0;
  const auto raw = bootstrap.readAll();
  try {
    // Generic bounded bootstrap parsing only, never a TUF/state parser.
    std::vector<std::set<std::string>> keys;
    const auto value = nlohmann::json::parse(raw.constBegin(), raw.constEnd(),
        [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json &item) {
      if (depth > 32) throw std::runtime_error("bootstrap depth");
      if (event == nlohmann::json::parse_event_t::object_start) keys.emplace_back();
      if (event == nlohmann::json::parse_event_t::key &&
          (keys.empty() || !keys.back().insert(item.get<std::string>()).second))
        throw std::runtime_error("bootstrap duplicate");
      if (event == nlohmann::json::parse_event_t::object_end) keys.pop_back();
      return true;
    });
    if (!value.is_object() || value.value("schema", std::string()) != "gsw.native_bootstrap/1" ||
        !value.contains("protocol") || !value.at("protocol").is_number_integer()) return 0;
    const auto protocol = value.at("protocol").get<qint64>();
    return protocol == 1 || protocol == 2 ? int(protocol) : 0;
  } catch (...) { return 0; }
}

bool managedLaunch(const NativeLaunchPlan &plan, bool diagnostics, bool create, QString *error) {
  const auto fail = [&](const char *code) { if (error) *error = QString::fromLatin1(code); return false; };
  if (!plan.valid()) return fail("managed_installation_manifest_invalid");
  const int protocol = bootstrapProtocol(plan.root);
  if (!protocol) return fail("managed_installation_manifest_invalid");
  const auto hasTrustState = [&] {
    for (const auto &relative : {QStringLiteral(".update-trust-anchor"), QStringLiteral("updates/trust")}) {
      const auto path = QDir(plan.root).filePath(relative);
#ifdef Q_OS_WIN
      const auto api = updatepaths::nativeApiPath(path);
      if (api.isEmpty() || GetFileAttributesW(reinterpret_cast<LPCWSTR>(api.utf16())) != INVALID_FILE_ATTRIBUTES)
        return true;
      const auto error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return true;
#else
      if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) return true;
#endif
    }
    return false;
  }();
  // A retained public trust history cannot become an unsigned developer root
  // just because bootstrap metadata was reverted. Missing/corrupt state must
  // be rejected by the same independent read-only helper, never reset here.
  if (protocol == 1 && !hasTrustState) {
    if (!create) return true;
    QProcess child;
    child.setProgram(plan.program); child.setArguments(plan.arguments);
    child.setWorkingDirectory(plan.workingDirectory); child.setProcessEnvironment(plan.environment);
    return child.startDetached() || fail("managed_installation_start_failed");
  }
#ifdef GSW_UPDATER_FIXTURE_BUILD
  const auto name = QString::fromLatin1("GSW Update Fixture Helper.exe");
#else
  const auto name = QString::fromLatin1("Gaussian Scene Workbench Update Helper.exe");
#endif
  const QString helperPath = QDir(plan.root).filePath(QStringLiteral("bin/") + name);
  QFile heldHelper;
  if (!holdOrdinaryFile(heldHelper, helperPath)) return fail("managed_installation_integrity_failed");
  if (plan.arguments.size() > 128) return fail("managed_installation_integrity_failed");
  QJsonArray arguments;
  for (const auto &argument : plan.arguments) {
    if (argument.contains(QChar(u'\0')) || argument.toUtf8().size() > 4096)
      return fail("managed_installation_integrity_failed");
    if (create) arguments.append(argument);
  }
  const QJsonObject intent{{"schema", "gsw.native_launch_intent/1"}, {"buildId", plan.version},
      {"executableSha256", plan.executableSha256}, {"arguments", arguments},
      {"diagnostics", create && diagnostics}};
  const auto input = QJsonDocument(intent).toJson(QJsonDocument::Compact);
  if (input.size() > 64 * 1024) return fail("managed_installation_integrity_failed");
  QByteArray output;
  bool creationUnconfirmed = false;
  if (!nativeStartupHelperExchange(helperPath, plan, input, create, &output, &creationUnconfirmed))
    return fail(creationUnconfirmed ? "managed_installation_start_unconfirmed" : "managed_installation_integrity_failed");
  const auto invalidResponse = [&] {
    return fail(create ? "managed_installation_start_unconfirmed" : "managed_installation_integrity_failed");
  };
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(output, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) return invalidResponse();
  // The fixed helper emits exactly one canonical record. Equality rejects
  // duplicate keys, additional records and noncanonical transport ambiguity.
  const auto recordBytes = output.endsWith("\r\n") ? output.chopped(2)
      : output.endsWith('\n') ? output.chopped(1) : QByteArray();
  if (recordBytes.isEmpty() || QJsonDocument(document.object()).toJson(QJsonDocument::Compact) != recordBytes)
    return invalidResponse();
  const auto record = document.object();
  const QSet<QString> required{"schema", "phase", "code", "buildId", "executableSha256"};
  const auto names = record.keys();
  if (QSet<QString>(names.cbegin(), names.cend()) != required ||
      record.value("schema") != QLatin1String("gsw.update_event/1") ||
      record.value("phase") != (create ? QLatin1String("launched") : QLatin1String("launch_verified")) ||
      record.value("code") != (create ? QLatin1String("PROCESS_CREATED") : QLatin1String("VERIFIED")) ||
      record.value("buildId") != plan.version || record.value("executableSha256") != plan.executableSha256)
    return invalidResponse();
  return true;
}
} // namespace

bool verifyNativeInstallationLaunch(const NativeLaunchPlan &plan, QString *error) {
  return managedLaunch(plan, false, false, error);
}
bool startNativeInstallation(const NativeLaunchPlan &plan, bool diagnostics, QString *error) {
  return managedLaunch(plan, diagnostics, true, error);
}

NativeLaunchPlan resolveNativeInstallation(const QString &root, const bool diagnostics,
    const QStringList &arguments, const QProcessEnvironment &inherited) {
  NativeLaunchPlan result;
  result.error = QStringLiteral("managed_installation_manifest_invalid");
  result.root = QDir::cleanPath(root);
  if (!localOrdinaryTree(result.root) || !QFileInfo(result.root).isDir()) return result;
  const QString manifestPath = QDir(result.root).filePath(QStringLiteral("current.json"));
  if (!localOrdinaryTree(manifestPath)) return result;
  QFile manifest(manifestPath);
  if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 16384) return result;
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(manifest.readAll(), &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) return result;
  const auto object = document.object();
  const QSet<QString> required{QStringLiteral("schema"), QStringLiteral("version"),
                              QStringLiteral("executableSha256")};
  const auto keys = object.keys();
  if (QSet<QString>(keys.cbegin(), keys.cend()) != required ||
      object.value(QStringLiteral("schema")).toString() != QLatin1String("gsw.native_active_version/1")) return result;
  result.version = object.value(QStringLiteral("version")).toString();
  const QString digest = object.value(QStringLiteral("executableSha256")).toString();
  static const QRegularExpression sha(QStringLiteral("^[a-f0-9]{64}$"));
  if (!versionName(result.version) || !sha.match(digest).hasMatch()) return result;
  result.workingDirectory = QDir(result.root).filePath(QLatin1String("versions/") + result.version);
  result.program = QDir(result.workingDirectory).filePath(
      QLatin1String("bin/") + QString::fromLatin1(kNativeApplicationName));
  if (!localOrdinaryTree(result.program) || !QFileInfo(result.program).isFile() ||
      !localOrdinaryTree(launcherFor(result.root)) || !QFileInfo(launcherFor(result.root)).isFile()) return result;
  QFile application(result.program);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!application.open(QIODevice::ReadOnly) || !hash.addData(&application) ||
      QString::fromLatin1(hash.result().toHex()) != digest) {
    result.error = QStringLiteral("managed_installation_integrity_failed");
    return result;
  }
  result.arguments = arguments;
  result.executableSha256 = digest;
  result.environment = inherited;
  result.environment.insert(QStringLiteral("GSW_NATIVE_INSTALL_ROOT"), result.root);
  // Diagnostics are explicit for this one child, never a persistent setting.
  result.environment.remove(QStringLiteral("GSW_RENDER_DIAGNOSTICS_DIR"));
  if (diagnostics)
  {
    const QString diagnosticRoot = QDir(result.root).filePath(QStringLiteral("diagnostics"));
    if (pathExists(diagnosticRoot) &&
        (!localOrdinaryTree(diagnosticRoot) || !QFileInfo(diagnosticRoot).isDir())) return result;
    result.environment.insert(QStringLiteral("GSW_RENDER_DIAGNOSTICS_DIR"),
                              diagnosticRoot);
  }
  result.error.clear();
  return result;
}

QString nativeInstallationErrorText(const QString &code) {
  if (code == QLatin1String("managed_installation_start_unconfirmed"))
    return QCoreApplication::translate("Workbench", "启动请求已发送，但未收到进程创建确认。程序可能仍在启动，请先检查已有窗口，不要重复启动。工程和设置未修改。");
  if (code == QLatin1String("managed_installation_integrity_failed"))
    return QCoreApplication::translate("Workbench", "程序文件校验失败，已阻止启动。请重新安装已验证版本。");
  if (code == QLatin1String("managed_installation_start_failed"))
    return QCoreApplication::translate("Workbench", "无法启动当前版本。请检查安装文件，原有工程和设置未修改。");
  return QCoreApplication::translate("Workbench", "安装版本信息无效，未启动程序。请重新部署已验证版本。");
}

QString managedInstallationForApplication(const QString &applicationPath) {
  const QFileInfo application(applicationPath);
  if (application.fileName() != QString::fromLatin1(kNativeApplicationName) ||
      !localOrdinaryTree(applicationPath)) return {};
  QDir bin = application.absoluteDir();
  if (bin.dirName() != QLatin1String("bin") || !bin.cdUp() || !versionName(bin.dirName()) ||
      !bin.cdUp() || bin.dirName() != QLatin1String("versions") || !bin.cdUp()) return {};
  const QString root = bin.absolutePath();
  if (!localOrdinaryTree(launcherFor(root)) ||
      !localOrdinaryTree(QDir(root).filePath(QStringLiteral("current.json")))) return {};
  return root;
}

bool setManagedTaskbarRelaunch(void *windowHandle, const QString &applicationPath,
                             const QString &appUserModelId) {
  const QString root = managedInstallationForApplication(applicationPath);
  if (root.isEmpty()) return true; // Classic fixed-directory installer, no redirect.
#ifdef Q_OS_WIN
  IPropertyStore *store = nullptr;
  if (FAILED(SHGetPropertyStoreForWindow(static_cast<HWND>(windowHandle), IID_PPV_ARGS(&store)))) return false;
  const QString launcher = QDir::toNativeSeparators(launcherFor(root));
  const auto set = [&](REFPROPERTYKEY key, const QString &value) {
    PROPVARIANT variant;
    PropVariantInit(&variant);
    HRESULT status = InitPropVariantFromString(reinterpret_cast<LPCWSTR>(value.utf16()), &variant);
    if (SUCCEEDED(status)) status = store->SetValue(key, variant);
    PropVariantClear(&variant);
    return SUCCEEDED(status);
  };
  // Microsoft requires explicit window ID and both relaunch properties.
  // The taskbar reacts to the ID update; publish the relaunch values first.
  const bool good = set(PKEY_AppUserModel_RelaunchCommand, QLatin1Char('"') + launcher + QLatin1Char('"')) &&
      set(PKEY_AppUserModel_RelaunchDisplayNameResource, QLatin1Char('@') + launcher + QStringLiteral(",-201")) &&
      set(PKEY_AppUserModel_RelaunchIconResource, launcher + QStringLiteral(",0")) &&
      set(PKEY_AppUserModel_ID, appUserModelId);
  store->Release();
  return good;
#else
  Q_UNUSED(windowHandle); Q_UNUSED(appUserModelId);
  return true;
#endif
}

NativeShortcutInfo readNativeShortcut(const QString &path) {
  NativeShortcutInfo result;
  result.error = QStringLiteral("shortcut_read_invalid");
#ifdef Q_OS_WIN
  if (!localOrdinaryTree(path) || QFileInfo(path).suffix().compare(QLatin1String("lnk"), Qt::CaseInsensitive)) return result;
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return result;
  IShellLinkW *link = nullptr;
  IPersistFile *file = nullptr;
  bool good = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)));
  if (good) good = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
  const QString native = QDir::toNativeSeparators(path);
  if (good) good = SUCCEEDED(file->Load(reinterpret_cast<LPCOLESTR>(native.utf16()), STGM_READ));
  wchar_t buffer[32768]{};
  const auto take = [&](HRESULT status, QString &target) {
    if (FAILED(status)) return false;
    target = QString::fromWCharArray(buffer); buffer[0] = L'\0'; return true;
  };
  if (good) good = take(link->GetPath(buffer, 32768, nullptr, SLGP_RAWPATH), result.target) &&
      take(link->GetArguments(buffer, 32768), result.arguments) &&
      take(link->GetWorkingDirectory(buffer, 32768), result.workingDirectory) &&
      take(link->GetDescription(buffer, 4096), result.description) &&
      SUCCEEDED(link->GetShowCmd(&result.showCommand)) && SUCCEEDED(link->GetHotkey(&result.hotkey));
  if (file) file->Release();
  if (link) link->Release();
  if (SUCCEEDED(initialized)) CoUninitialize();
  if (good) result.error.clear();
#endif
  return result;
}

bool writeNativeLauncherShortcut(const QString &path, const QString &launcher,
                                const QString &arguments, const QString &appUserModelId, QString *diagnostic) {
  if (diagnostic) *diagnostic = QStringLiteral("shortcut_path_invalid");
#ifdef Q_OS_WIN
  if (!QDir::isAbsolutePath(path) || QFileInfo(path).suffix().compare(QLatin1String("lnk"), Qt::CaseInsensitive) ||
      !localOrdinaryTree(QFileInfo(path).absolutePath()) ||
      (pathExists(path) && !ordinaryPath(path)) || !localOrdinaryTree(launcher) ||
      !QFileInfo(launcher).isFile()) return false;
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (diagnostic) *diagnostic = QStringLiteral("shortcut_com_initialization");
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
  IShellLinkW *link = nullptr;
  IPersistFile *file = nullptr;
  IPropertyStore *store = nullptr;
  bool good = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&link)));
  const QString nativePath = QDir::toNativeSeparators(path);
  const QString nativeLauncher = QDir::toNativeSeparators(launcher);
  const QString directory = QDir::toNativeSeparators(QFileInfo(launcher).absolutePath());
  const QString stage = nativePath + QLatin1Char('.') + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".lnk");
  wchar_t description[4096]{};
  WORD hotkey = 0;
  int show = SW_SHOWNORMAL;
  QString phase = QStringLiteral("shortcut_load");
  if (good) good = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
  // Property handlers may commit immediately: copy first, then open only our
  // stage writable. The original must stay untouched until final publication.
  if (good && pathExists(path)) {
    phase = QStringLiteral("shortcut_stage_copy");
    good = QFile::copy(path, stage);
    if (good) { phase = QStringLiteral("shortcut_stage_load");
      good = SUCCEEDED(file->Load(reinterpret_cast<LPCOLESTR>(stage.utf16()), STGM_READWRITE)); }
  }
  if (good) good = SUCCEEDED(link->GetDescription(description, 4096)) &&
      SUCCEEDED(link->GetHotkey(&hotkey)) && SUCCEEDED(link->GetShowCmd(&show));
  if (good) { phase = QStringLiteral("shortcut_target"); good = SUCCEEDED(link->SetPath(reinterpret_cast<LPCWSTR>(nativeLauncher.utf16()))) &&
      SUCCEEDED(link->SetArguments(reinterpret_cast<LPCWSTR>(arguments.utf16()))) &&
      SUCCEEDED(link->SetWorkingDirectory(reinterpret_cast<LPCWSTR>(directory.utf16()))) &&
      SUCCEEDED(link->SetIconLocation(reinterpret_cast<LPCWSTR>(nativeLauncher.utf16()), 0)) &&
      SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store))); }
  if (good) {
    phase = QStringLiteral("shortcut_properties");
    PROPVARIANT value;
    PropVariantInit(&value);
    good = SUCCEEDED(InitPropVariantFromString(reinterpret_cast<LPCWSTR>(appUserModelId.utf16()), &value)) &&
           SUCCEEDED(store->SetValue(PKEY_AppUserModel_ID, value));
    PropVariantClear(&value);
    if (good) { phase = QStringLiteral("shortcut_property_commit"); good = SUCCEEDED(store->Commit()); }
    PropVariantClear(&value);
  }
  // Changing the link target can reset standard shell fields. Restore the user
  // options explicitly, after property-store changes and before the final save.
  if (good) { phase = QStringLiteral("shortcut_restore_options"); good = SUCCEEDED(link->SetDescription(description)) &&
      SUCCEEDED(link->SetHotkey(hotkey)) && SUCCEEDED(link->SetShowCmd(show)); }
  // Save a fresh sibling before atomically replacing the link. Never leave an
  // existing taskbar/desktop link partially written after an interrupted update.
  if (good) { phase = QStringLiteral("shortcut_stage_save"); good = SUCCEEDED(file->Save(reinterpret_cast<LPCOLESTR>(stage.utf16()), TRUE)); }
  if (store) store->Release();
  if (file) file->Release();
  if (link) link->Release();
  if (good) {
    phase = QStringLiteral("shortcut_readback");
    IShellLinkW *verify = nullptr;
    IPersistFile *verifyFile = nullptr;
    good = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&verify)));
    if (good) good = SUCCEEDED(verify->QueryInterface(IID_PPV_ARGS(&verifyFile)));
    if (good) good = SUCCEEDED(verifyFile->Load(reinterpret_cast<LPCOLESTR>(stage.utf16()), STGM_READ));
    wchar_t retainedDescription[4096]{}; WORD retainedHotkey = 0; int retainedShow = 0;
    if (good) good = SUCCEEDED(verify->GetDescription(retainedDescription, 4096)) &&
        QString::fromWCharArray(retainedDescription) == QString::fromWCharArray(description) &&
        SUCCEEDED(verify->GetHotkey(&retainedHotkey)) && retainedHotkey == hotkey &&
        SUCCEEDED(verify->GetShowCmd(&retainedShow)) && retainedShow == show;
    if (!good) phase = QStringLiteral("shortcut_retained_options_description_%1_%2_hotkey_%3_%4_show_%5_%6")
        .arg(QString::fromWCharArray(description).size()).arg(QString::fromWCharArray(retainedDescription).size())
        .arg(hotkey).arg(retainedHotkey).arg(show).arg(retainedShow);
    if (verifyFile) verifyFile->Release();
    if (verify) verify->Release();
  }
  if (good) {
    phase = QStringLiteral("shortcut_atomic_publish");
    good = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
      if (MoveFileExW(reinterpret_cast<LPCWSTR>(updatepaths::nativeApiPath(stage).utf16()),
                      reinterpret_cast<LPCWSTR>(updatepaths::nativeApiPath(nativePath).utf16()),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { good = true; break; }
      const DWORD error = GetLastError();
      if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) break;
      QThread::msleep(100);
    }
  }
  if (pathExists(stage)) QFile::remove(stage); // Only this call's fresh sibling.
  if (good) SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                           reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr);
  if (SUCCEEDED(initialized)) CoUninitialize();
  if (diagnostic) *diagnostic = good ? QString() : phase;
  return good;
#else
  Q_UNUSED(path); Q_UNUSED(launcher); Q_UNUSED(arguments); Q_UNUSED(appUserModelId);
  return false;
#endif
}

void clearManagedTaskbarRelaunch(void *windowHandle, const QString &applicationPath) {
#ifdef Q_OS_WIN
  if (managedInstallationForApplication(applicationPath).isEmpty()) return;
  IPropertyStore *store = nullptr;
  if (FAILED(SHGetPropertyStoreForWindow(static_cast<HWND>(windowHandle), IID_PPV_ARGS(&store)))) return;
  // Window property stores are immediate, not persistent. Microsoft requires
  // VT_EMPTY removal before destroying the HWND so the property resources free.
  PROPVARIANT empty; PropVariantInit(&empty);
  for (const auto &key : {PKEY_AppUserModel_ID, PKEY_AppUserModel_RelaunchCommand,
                          PKEY_AppUserModel_RelaunchDisplayNameResource, PKEY_AppUserModel_RelaunchIconResource})
    store->SetValue(key, empty);
  store->Release();
#else
  Q_UNUSED(windowHandle); Q_UNUSED(applicationPath);
#endif
}
} // namespace gsw
