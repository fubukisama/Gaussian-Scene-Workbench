// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeStartupHealth.h"
#include "NativeInstallation.h"
#include "NativeUpdateLocalPath.h"
#include "../third_party/qt-process-quote/QtProcessQuote.h"
#include <QCoreApplication>
#include <QDir>
#ifdef GSW_UPDATER_FIXTURE_BUILD
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#endif
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <atomic>
#include <thread>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#endif

namespace gsw {
namespace {
constexpr auto pipeKey = "GSW_NATIVE_STARTUP_PIPE";
constexpr auto attemptKey = "GSW_NATIVE_STARTUP_ATTEMPT";
constexpr auto nonceKey = "GSW_NATIVE_STARTUP_NONCE";
constexpr qsizetype messageLimit = 8192;
#ifdef GSW_UPDATER_FIXTURE_BUILD
constexpr quint32 observationMs = 1500;
constexpr auto acceptedScope = "qt-fixture-window-surface";
#else
constexpr quint32 observationMs = 15000;
constexpr auto acceptedScope = "main-window-base-viewport";
#endif

#ifdef Q_OS_WIN
class Handle final {
public:
  Handle() = default;
  explicit Handle(HANDLE value) : h(value) {}
  ~Handle() { reset(); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&other) noexcept : h(other.release()) {}
  Handle &operator=(Handle &&other) noexcept { if (this != &other) { reset(); h = other.release(); } return *this; }
  bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
  HANDLE get() const { return h; }
  HANDLE release() { const auto value = h; h = INVALID_HANDLE_VALUE; return value; }
  void reset(HANDLE value = INVALID_HANDLE_VALUE) { if (valid()) CloseHandle(h); h = value; }
private:
  HANDLE h = INVALID_HANDLE_VALUE;
};

QString token(qsizetype bytes) {
  QByteArray random(bytes, '\0');
  if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(random.data()), ULONG(bytes),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
  return QString::fromLatin1(random.toHex());
}

QString creationTime(HANDLE process) {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return {};
  const auto ticks = (quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
  return QString::number(ticks, 16).rightJustified(16, QLatin1Char('0'));
}

QString imagePath(HANDLE process) {
  wchar_t buffer[32768]{};
  DWORD length = DWORD(std::size(buffer));
  if (!QueryFullProcessImageNameW(process, 0, buffer, &length)) return {};
  QString path = QDir::fromNativeSeparators(QString::fromWCharArray(buffer, int(length)));
  if (path.startsWith(QLatin1String("//?/"))) path.remove(0, 4);
  return QDir::cleanPath(path);
}

bool sameProcessObject(HANDLE first, HANDLE second) {
  // The documented Win10+ API is exported by the already-loaded OS
  // KernelBase.dll. Some supported desktop SDK layouts omit Kernelbase.lib;
  // resolve that exact system module rather than load/search a DLL or weaken
  // the check to PID equality. Missing API means neutral health evidence.
  using Compare = BOOL (WINAPI *)(HANDLE, HANDLE);
  const auto system = GetModuleHandleW(L"kernelbase.dll");
  const auto compare = system ? reinterpret_cast<Compare>(GetProcAddress(system, "CompareObjectHandles")) : nullptr;
  return compare && compare(first, second);
}

std::vector<wchar_t> environmentBlock(QProcessEnvironment environment) {
  const auto inherited = QProcessEnvironment::systemEnvironment();
  for (const auto *key : {"PATH", "SystemRoot"})
    if (!environment.contains(QLatin1String(key)) && inherited.contains(QLatin1String(key)))
      environment.insert(QLatin1String(key), inherited.value(QLatin1String(key)));
  auto entries = environment.toStringList();
  entries.sort(Qt::CaseInsensitive);
  std::vector<wchar_t> block;
  for (const auto &entry : entries) {
    if (entry.contains(QChar(u'\0')) || entry.size() > 32766) return {};
    const auto *begin = reinterpret_cast<const wchar_t *>(entry.utf16());
    block.insert(block.end(), begin, begin + entry.size());
    block.push_back(L'\0');
    if (block.size() > 1024 * 1024) return {};
  }
  if (block.empty()) block.push_back(L'\0');
  block.push_back(L'\0');
  return block;
}

bool createProcess(const QString &program, const QStringList &arguments,
    const QString &directory, const QProcessEnvironment &environment,
    STARTUPINFOW *startup, DWORD flags, bool inherit,
    PROCESS_INFORMATION *process) {
  QString command = qt683::createCommandLine(program, arguments);
  auto block = environmentBlock(environment);
  const QString application = updatepaths::nativeApiPath(program);
  const QString working = updatepaths::nativeApiPath(directory);
  if (application.isEmpty() || working.isEmpty() || block.empty() || command.size() >= 32767 ||
      command.contains(QChar(u'\0'))) return false;
  return CreateProcessW(reinterpret_cast<LPCWSTR>(application.utf16()),
      reinterpret_cast<LPWSTR>(command.data()), nullptr, nullptr, inherit,
      flags | CREATE_UNICODE_ENVIRONMENT, block.data(),
      reinterpret_cast<LPCWSTR>(working.utf16()), startup, process);
}

// A current-logon SID, rather than broad Everyone/Users or remote access.
// The DACL is only IPC containment; actual peer identity is checked separately.
PSECURITY_DESCRIPTOR logonSecurity() {
  HANDLE rawToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return nullptr;
  Handle processToken(rawToken);
  DWORD bytes = 0;
  GetTokenInformation(processToken.get(), TokenGroups, nullptr, 0, &bytes);
  if (!bytes || bytes > 1024 * 1024) return nullptr;
  std::vector<unsigned char> data(bytes);
  if (!GetTokenInformation(processToken.get(), TokenGroups, data.data(), bytes, &bytes)) return nullptr;
  const auto groups = reinterpret_cast<const TOKEN_GROUPS *>(data.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &sid)) return nullptr;
    const QString sddl = QStringLiteral("D:P(A;;GA;;;") + QString::fromWCharArray(sid) + QLatin1Char(')');
    LocalFree(sid);
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &security, nullptr)) return nullptr;
    return security;
  }
  return nullptr;
}

Handle serverPipe(const QString &name) {
  auto security = logonSecurity();
  if (!security) return {};
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};
  Handle pipe(CreateNamedPipeW(reinterpret_cast<LPCWSTR>(name.utf16()),
      PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, DWORD(messageLimit), DWORD(messageLimit), observationMs, &attributes));
  LocalFree(security);
  return pipe;
}

// Every cancelled overlapped operation is drained before its stack state or
// buffer is released. Closing a pipe/process HANDLE never terminates a child.
bool completeIo(HANDLE file, OVERLAPPED &operation, DWORD timeout, DWORD *bytes) {
  if (WaitForSingleObject(operation.hEvent, timeout) == WAIT_OBJECT_0)
    return GetOverlappedResult(file, &operation, bytes, FALSE);
  CancelIoEx(file, &operation);
  GetOverlappedResult(file, &operation, bytes, TRUE);
  return false;
}

bool connectPipe(HANDLE pipe, DWORD timeout) {
  Handle completion(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!completion.valid()) return false;
  OVERLAPPED operation{}; operation.hEvent = completion.get();
  if (ConnectNamedPipe(pipe, &operation)) return true;
  const DWORD error = GetLastError();
  if (error == ERROR_PIPE_CONNECTED) return true;
  DWORD ignored = 0;
  return error == ERROR_IO_PENDING && completeIo(pipe, operation, timeout, &ignored);
}

QByteArray readMessage(HANDLE pipe, DWORD timeout) {
  QByteArray bytes(messageLimit + 1, '\0');
  Handle completion(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!completion.valid()) return {};
  OVERLAPPED operation{}; operation.hEvent = completion.get();
  DWORD received = 0;
  const bool immediate = ReadFile(pipe, bytes.data(), DWORD(bytes.size()), &received, &operation);
  if (!immediate && (GetLastError() != ERROR_IO_PENDING ||
      !completeIo(pipe, operation, timeout, &received))) return {};
  if (!received || received > messageLimit) return {};
  bytes.resize(received);
  return bytes;
}

void sendReady(const QString &pipeName, const QByteArray &message) {
  Handle pipe(CreateFileW(reinterpret_cast<LPCWSTR>(pipeName.utf16()), GENERIC_WRITE, 0,
      nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (!pipe.valid()) return;
  Handle completion(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!completion.valid()) return;
  OVERLAPPED operation{}; operation.hEvent = completion.get();
  DWORD sent = 0;
  if (!WriteFile(pipe.get(), message.constData(), DWORD(message.size()), &sent, &operation) &&
      GetLastError() == ERROR_IO_PENDING) completeIo(pipe.get(), operation, 300, &sent);
}

bool canonicalCreated(const QByteArray &bytes, const NativeLaunchPlan &plan) {
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) return false;
  const auto raw = bytes.endsWith("\r\n") ? bytes.chopped(2)
      : bytes.endsWith('\n') ? bytes.chopped(1) : QByteArray();
  if (raw.isEmpty() || document.toJson(QJsonDocument::Compact) != raw) return false;
  const auto record = document.object();
  const QSet<QString> required{"schema", "phase", "code", "buildId", "executableSha256"};
  const auto keys = record.keys();
  return QSet<QString>(keys.cbegin(), keys.cend()) == required &&
      record.value("schema") == QLatin1String("gsw.update_event/1") &&
      record.value("phase") == QLatin1String("launched") &&
      record.value("code") == QLatin1String("PROCESS_CREATED") &&
      record.value("buildId") == plan.version && record.value("executableSha256") == plan.executableSha256;
}

#ifdef GSW_UPDATER_FIXTURE_BUILD
void traceHelperExchange(const char *phase, const NativeLaunchPlan &plan,
    HANDLE helper, DWORD helperPid, bool processCreation,
    const QByteArray &response, const QElapsedTimer &clock) noexcept {
  try {
    const auto marker = qEnvironmentVariable("GSW_UPDATE_FIXTURE_MARKER");
    if (marker.isEmpty() || !updatepaths::logicalLocalPath(marker)) return;
    const bool created = processCreation && canonicalCreated(response, plan);
    const bool signalled = WaitForSingleObject(helper, 0) == WAIT_OBJECT_0;
    DWORD exitCode = STILL_ACTIVE;
    const bool exitKnown = GetExitCodeProcess(helper, &exitCode);
    QJsonObject record{{"schema", "gsw.fixture_launch_transport_trace/1"}, {"testOnly", true},
      {"phase", QLatin1String(phase)}, {"callerPid", double(GetCurrentProcessId())},
      {"helperPid", double(helperPid)}, {"helperCreationTime", creationTime(helper)},
      {"helperImage", imagePath(helper)}, {"helperSignalled", signalled},
      {"helperExitKnown", exitKnown}, {"helperExitCode", double(exitCode)},
      {"buildId", plan.version}, {"executableSha256", plan.executableSha256},
      {"processCreation", processCreation}, {"canonicalCreated", created},
      {"responseBytes", double(response.size())}, {"elapsedMs", double(clock.elapsed())},
      {"utcMs", double(QDateTime::currentMSecsSinceEpoch())}};
    // Copy only a fully validated five-field record, never arbitrary output.
    if (created) record.insert(QStringLiteral("creationResponse"), QJsonDocument::fromJson(response).object());
    QFile file(marker + QStringLiteral(".launcher-trace.jsonl"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
      file.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
      file.flush();
    }
  } catch (...) {}
}
#endif
#endif
} // namespace

struct NativeStartupAttempt::Impl {
  QString build, digest, program, attempt, nonce, pipeName, birth;
  quint64 pid = 0;
#ifdef Q_OS_WIN
  Handle process, pipe;
#endif
};

NativeStartupAttempt::NativeStartupAttempt(std::unique_ptr<Impl> impl) : mImpl(std::move(impl)) {}
NativeStartupAttempt::~NativeStartupAttempt() = default;

std::unique_ptr<NativeStartupAttempt> NativeStartupAttempt::start(const NativeLaunchPlan &plan, QString *error) {
  auto state = std::make_unique<Impl>();
  state->build = plan.version; state->digest = plan.executableSha256;
  state->program = QDir::cleanPath(QDir::fromNativeSeparators(plan.program));
#ifdef Q_OS_WIN
  auto environment = plan.environment;
  for (const auto *key : {pipeKey, attemptKey, nonceKey}) environment.remove(QLatin1String(key));
  // Health setup is best-effort and never an additional launch prerequisite.
  state->attempt = token(16); state->nonce = token(32);
  if (!state->attempt.isEmpty() && !state->nonce.isEmpty()) {
    state->pipeName = QStringLiteral("\\\\.\\pipe\\GSW.Startup.") + state->attempt;
    state->pipe = serverPipe(state->pipeName);
    if (state->pipe.valid()) {
      environment.insert(QLatin1String(pipeKey), state->pipeName);
      environment.insert(QLatin1String(attemptKey), state->attempt);
      environment.insert(QLatin1String(nonceKey), state->nonce);
    }
  }
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  // Allocate the returned owner before creating the OS process. Once creation
  // succeeds, optional health metadata must not throw away that fact.
  auto attempt = std::unique_ptr<NativeStartupAttempt>(new NativeStartupAttempt(std::move(state)));
  if (!createProcess(plan.program, plan.arguments, plan.workingDirectory, environment,
      &startup, CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, false, &process)) {
    if (error) *error = QStringLiteral("managed_installation_start_failed");
    return {};
  }
  auto &created = *attempt->mImpl;
  created.process.reset(process.hProcess);
  Handle thread(process.hThread);
  created.pid = process.dwProcessId;
  try { created.birth = creationTime(created.process.get()); } catch (...) {}
  return attempt;
#else
  Q_UNUSED(plan);
  if (error) *error = QStringLiteral("managed_installation_start_failed");
  return {};
#endif
}

QJsonObject NativeStartupAttempt::initialStatus() const {
  const auto &state = *mImpl;
  return {{"schema", "gsw.native_startup_status/1"}, {"code", "UNCONFIRMED"},
      {"attempt", QJsonObject{{"attemptId", state.attempt}, {"buildId", state.build},
        {"executableSha256", state.digest}, {"pid", double(state.pid)},
        {"processCreationTime", state.birth}}}};
}

QJsonObject NativeStartupAttempt::observe() {
  auto result = initialStatus();
#ifdef Q_OS_WIN
  auto &state = *mImpl;
  if (!state.pipe.valid() || state.birth.isEmpty()) return result;
  const auto deadline = GetTickCount64() + observationMs;
  if (!connectPipe(state.pipe.get(), observationMs)) return result;
  ULONG clientPid = 0;
  if (!GetNamedPipeClientProcessId(state.pipe.get(), &clientPid) || clientPid != state.pid) return result;
  Handle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, clientPid));
  // hProcess is the exact object returned by our CreateProcessW, not a PID
  // reopened after the child could have exited and Windows reused its number.
  if (!peer.valid() || !sameProcessObject(state.process.get(), peer.get()) ||
      creationTime(peer.get()) != state.birth ||
      imagePath(peer.get()).compare(state.program, Qt::CaseInsensitive) != 0) return result;
  const auto now = GetTickCount64();
  if (now >= deadline) return result;
  const auto bytes = readMessage(state.pipe.get(), DWORD(deadline - now));
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject() ||
      document.toJson(QJsonDocument::Compact) != bytes) return result;
  const auto message = document.object();
  const QSet<QString> required{"schema", "attemptId", "nonce", "healthScope", "window"};
  const auto keys = message.keys();
  if (QSet<QString>(keys.cbegin(), keys.cend()) != required ||
      message.value("schema") != QLatin1String("gsw.native_startup_ready/1") ||
      message.value("attemptId") != state.attempt || message.value("nonce") != state.nonce ||
      message.value("healthScope") != QLatin1String(acceptedScope)) return result;
  const auto windowValue = message.value("window").toString();
  static const QRegularExpression hex(QStringLiteral("^[a-f0-9]{16}$"));
  bool valid = false;
  const auto value = windowValue.toULongLong(&valid, 16);
  const HWND window = reinterpret_cast<HWND>(quintptr(value));
  DWORD windowPid = 0;
  if (!valid || !hex.match(windowValue).hasMatch() || !value || !IsWindow(window) ||
      !IsWindowVisible(window) || !GetWindowThreadProcessId(window, &windowPid) || windowPid != state.pid) return result;
  // Evidence is consumed exactly once. No pipe token is persisted or exposed.
  state.pipe.reset(); state.nonce.clear();
  auto attempt = result.value("attempt").toObject();
  attempt.insert(QStringLiteral("healthScope"), QLatin1String(acceptedScope));
  result.insert(QStringLiteral("attempt"), attempt);
  result.insert(QStringLiteral("code"), QStringLiteral("HEALTHY"));
#endif
  return result;
}

void reportNativeStartupReady(quintptr mainWindowHandle, const char *scope) noexcept {
  try {
#ifdef Q_OS_WIN
  static std::atomic_bool submitted{false};
  if (!scope || QByteArray(scope) != acceptedScope || !mainWindowHandle) return;
  const auto pipe = qEnvironmentVariable(pipeKey);
  const auto attempt = qEnvironmentVariable(attemptKey);
  const auto nonce = qEnvironmentVariable(nonceKey);
  static const QRegularExpression id(QStringLiteral("^[a-f0-9]{32}$"));
  static const QRegularExpression secret(QStringLiteral("^[a-f0-9]{64}$"));
  if (!id.match(attempt).hasMatch() || !secret.match(nonce).hasMatch() ||
      pipe != QStringLiteral("\\\\.\\pipe\\GSW.Startup.") + attempt) return;
  const HWND window = reinterpret_cast<HWND>(mainWindowHandle);
  DWORD owner = 0;
  if (!IsWindow(window) || !IsWindowVisible(window) ||
      !GetWindowThreadProcessId(window, &owner) || owner != GetCurrentProcessId() || submitted.exchange(true)) return;
  const auto message = QJsonDocument(QJsonObject{{"schema", "gsw.native_startup_ready/1"},
      {"attemptId", attempt}, {"nonce", nonce}, {"healthScope", QLatin1String(acceptedScope)},
      {"window", QString::number(quint64(mainWindowHandle), 16).rightJustified(16, QLatin1Char('0'))}})
      .toJson(QJsonDocument::Compact);
  if (message.size() > messageLimit) return;
  // No event-loop dependency or GUI-thread I/O. The copied one-shot payload
  // is bounded; a missing server cannot delay painting or program shutdown.
  std::thread([pipe, message] { try { sendReady(pipe, message); } catch (...) {} }).detach();
#else
  Q_UNUSED(mainWindowHandle); Q_UNUSED(scope);
#endif
  } catch (...) {} // Optional readiness evidence must never escape into the UI.
}

bool nativeStartupHelperExchange(const QString &program, const NativeLaunchPlan &plan,
    const QByteArray &intent, bool processCreation, QByteArray *response, bool *creationUnconfirmed) {
  if (creationUnconfirmed) *creationUnconfirmed = false;
  if (!response || intent.size() > 64 * 1024) return false;
  response->clear();
#ifdef Q_OS_WIN
#ifdef GSW_UPDATER_FIXTURE_BUILD
  QElapsedTimer exchangeClock; exchangeClock.start();
#endif
  SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE inputRead = INVALID_HANDLE_VALUE, inputWrite = INVALID_HANDLE_VALUE;
  HANDLE outputRead = INVALID_HANDLE_VALUE, outputWrite = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&inputRead, &inputWrite, &inherited, 65536)) return false;
  Handle childInput(inputRead), parentInput(inputWrite);
  if (!CreatePipe(&outputRead, &outputWrite, &inherited, 16384)) return false;
  Handle parentOutput(outputRead), childOutput(outputWrite);
  Handle childError(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &inherited, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!childError.valid() || !SetHandleInformation(parentInput.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(parentOutput.get(), HANDLE_FLAG_INHERIT, 0)) return false;
  SIZE_T attributeBytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
  std::vector<unsigned char> attributes(attributeBytes);
  auto list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &attributeBytes)) return false;
  struct Attributes final { PPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup{list};
  HANDLE childHandles[]{childInput.get(), childOutput.get(), childError.get()};
  if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      childHandles, sizeof(childHandles), nullptr, nullptr)) return false;
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = childInput.get();
  startup.StartupInfo.hStdOutput = childOutput.get();
  startup.StartupInfo.hStdError = childError.get(); startup.lpAttributeList = list;
  PROCESS_INFORMATION info{};
  const QStringList arguments{processCreation ? QStringLiteral("--launch-current") : QStringLiteral("--verify-installed")};
  if (!createProcess(program, arguments, QFileInfo(program).absolutePath(), plan.environment, &startup.StartupInfo,
      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, true, &info)) return false;
  Handle process(info.hProcess), primaryThread(info.hThread);
#ifdef GSW_UPDATER_FIXTURE_BUILD
  traceHelperExchange("helper_created", plan, process.get(), info.dwProcessId,
      processCreation, {}, exchangeClock);
#endif
  if (creationUnconfirmed) *creationUnconfirmed = processCreation;
  childInput.reset(); childOutput.reset(); childError.reset();
  struct Writer final { Handle pipe; Handle done; QByteArray bytes; std::atomic_bool ok{false}; };
  auto writer = std::make_shared<Writer>();
  writer->pipe = std::move(parentInput);
  writer->done.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  writer->bytes = intent;
  if (!writer->done.valid()) {
    // Only our own fixed helper, before any intent was sent; never its app.
    TerminateProcess(process.get(), 2); WaitForSingleObject(process.get(), 1000);
    return false;
  }
  std::thread input([writer] {
    DWORD sent = 0;
    writer->ok = WriteFile(writer->pipe.get(), writer->bytes.constData(), DWORD(writer->bytes.size()), &sent, nullptr) &&
        sent == DWORD(writer->bytes.size());
    writer->pipe.reset(); SetEvent(writer->done.get());
  });
  const auto deadline = GetTickCount64() + 120000;
  bool received = false;
  while (GetTickCount64() < deadline) {
    DWORD available = 0;
    if (!PeekNamedPipe(parentOutput.get(), nullptr, 0, nullptr, &available, nullptr)) {
      const DWORD pipeError = GetLastError();
      DWORD exitCode = 1;
      // CRT stdout can close just before the process becomes signalled. Do
      // not turn that harmless inspection-exit race into an invalid plan.
      received = pipeError == ERROR_BROKEN_PIPE && !response->isEmpty() &&
          WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0 &&
          GetExitCodeProcess(process.get(), &exitCode) && exitCode == 0;
      break;
    }
    if (available) {
      if (available > 16384 - DWORD(response->size())) break;
      const auto old = response->size(); response->resize(old + available);
      DWORD read = 0;
      if (!ReadFile(parentOutput.get(), response->data() + old, available, &read, nullptr)) break;
      response->resize(old + read);
      if (processCreation && response->contains('\n')) { received = true; break; }
    }
    if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
      DWORD exitCode = 1;
      received = GetExitCodeProcess(process.get(), &exitCode) && exitCode == 0;
      // Drain after process exit before validating the single canonical record.
      DWORD trailing = 0;
      if (PeekNamedPipe(parentOutput.get(), nullptr, 0, nullptr, &trailing, nullptr) && trailing) {
        if (trailing > 16384 - DWORD(response->size())) { received = false; break; }
        const auto old = response->size(); response->resize(old + trailing);
        DWORD read = 0;
        if (!ReadFile(parentOutput.get(), response->data() + old, trailing, &read, nullptr)) { received = false; break; }
        response->resize(old + read);
      }
      break;
    }
    Sleep(5);
  }
  // Last drain is mandatory before deciding whether a monitor can be stopped.
  // A valid created record wins over a later observer exit/error/timeout.
  DWORD trailing = 0;
  if (PeekNamedPipe(parentOutput.get(), nullptr, 0, nullptr, &trailing, nullptr) && trailing &&
      trailing <= 16384 - DWORD(response->size())) {
    const auto old = response->size(); response->resize(old + trailing);
    DWORD read = 0;
    if (ReadFile(parentOutput.get(), response->data() + old, trailing, &read, nullptr)) response->resize(old + read);
    else response->resize(old);
  }
  const bool created = processCreation && canonicalCreated(*response, plan);
#ifdef GSW_UPDATER_FIXTURE_BUILD
  traceHelperExchange("helper_response", plan, process.get(), info.dwProcessId,
      processCreation, *response, exchangeClock);
#endif
  if (created) {
    received = true;
    if (creationUnconfirmed) *creationUnconfirmed = false;
  } else if (WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) {
    // Same scope as the previous QProcess transport's timeout: stop only the
    // directly created helper when no valid creation evidence was obtained.
    // The application (if creation raced lost evidence) is never terminated;
    // the caller reports START_UNCONFIRMED and must not automatically retry.
    TerminateProcess(process.get(), 2); WaitForSingleObject(process.get(), 1000);
  }
  if (WaitForSingleObject(writer->done.get(), 1000) == WAIT_OBJECT_0) input.join();
  else {
    CancelSynchronousIo(input.native_handle());
    // The thread owns its buffer/pipe/event until cancellation completes. It
    // cannot reference this stack, the returned response, or any GUI QObject.
    input.detach();
  }
  // Closing hProcess does not kill a helper that is still observing its app.
  // The caller authenticates PROCESS_CREATED independently of observer exit.
  return received && (created || writer->ok.load()) && response->size() <= 16384;
#else
  Q_UNUSED(program); Q_UNUSED(plan); Q_UNUSED(processCreation);
  return false;
#endif
}
} // namespace gsw
