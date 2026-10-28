#include "UiCrashDiagnostics.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>
#include <QVariant>

#include <cstdint>
#include <cstring>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <DbgHelp.h>
#endif

namespace gsw {
#ifdef Q_OS_WIN
namespace {
constexpr DWORD stackLimit = 32;
constexpr DWORD textLimit = 16 * 1024;
constexpr DWORD writerWaitMs = 3000;
constexpr int pathCapacity = 2048;
using DumpWriter = decltype(&MiniDumpWriteDump);

struct CrashState {
  volatile LONG installation = 0;
  volatile LONG armed = 0;
  volatile LONG first = 0;
  volatile LONG stopping = 0;
  HANDLE text = nullptr, dump = nullptr, request = nullptr, completed = nullptr, worker = nullptr;
  HMODULE dbgHelp = nullptr;
  PVOID handler = nullptr;
  DumpWriter writeDump = nullptr;
  DWORD pid = 0, tid = 0, textBytes = 0;
  USHORT frameCount = 0;
  PVOID frames[stackLimit]{};
  EXCEPTION_RECORD record{};
  CONTEXT context{};
  EXCEPTION_POINTERS pointers{};
  wchar_t textPath[pathCapacity]{}, dumpPath[pathCapacity]{};
  bool textCreated = false, dumpCreated = false;
};
CrashState crash;

// Fixed storage/Win32 writes only. No QString, printf, iostream, allocation or
// Qt/CRT logging lock on either exception-handler output path.
struct Text {
  char bytes[1024]{};
  DWORD length = 0;
  void append(const char *value) noexcept {
    while (*value && length < sizeof(bytes)) bytes[length++] = *value++;
  }
  void hex(std::uintptr_t value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    append("0x");
    bool started = false;
    for (int shift = int(sizeof(value) * 8) - 4; shift >= 0; shift -= 4) {
      const auto digit = (value >> shift) & 15;
      if (digit || started || shift == 0) {
        if (length < sizeof(bytes)) bytes[length++] = digits[digit];
        started = true;
      }
    }
  }
};

void writeText(const Text &text) noexcept {
  if (!crash.text || crash.text == INVALID_HANDLE_VALUE || crash.textBytes >= textLimit) return;
  const DWORD remaining = textLimit - crash.textBytes;
  const DWORD count = text.length < remaining ? text.length : remaining;
  DWORD written = 0;
  if (count && WriteFile(crash.text, text.bytes, count, &written, nullptr))
    crash.textBytes += written;
}

void addressLine(const char *name, const void *address, const DWORD index,
                 const bool moduleName) noexcept {
  Text text;
  text.append(name);
  text.append("["); text.hex(index); text.append("]=");
  text.hex(reinterpret_cast<std::uintptr_t>(address));
  MEMORY_BASIC_INFORMATION memory{};
  if (VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) &&
      memory.Type == MEM_IMAGE && memory.AllocationBase) {
    const auto base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
    text.append(" module_base="); text.hex(base);
    text.append(" module_offset="); text.hex(reinterpret_cast<std::uintptr_t>(address) - base);
    // Module-name lookup is worker-only, never in the VEH/loader-fault path.
    if (moduleName) {
      wchar_t path[1024]{};
      const DWORD count = GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase),
                                            path, DWORD(sizeof(path) / sizeof(path[0])));
      if (count && count < sizeof(path) / sizeof(path[0])) {
        DWORD begin = 0;
        for (DWORD i = 0; i < count; ++i) if (path[i] == L'\\' || path[i] == L'/') begin = i + 1;
        const DWORD length = count - begin < 64 ? count - begin : 64;
        char utf8[257]{};
        const int converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path + begin,
            int(length), utf8, 256, nullptr, nullptr);
        if (converted > 0) {
          utf8[converted] = '\0';
          text.append(" module="); text.append(utf8);
        }
      }
    }
  } else {
    text.append(" module_offset=unavailable");
  }
  text.append("\r\n");
  writeText(text);
}

bool selectedCode(const DWORD code) noexcept {
  switch (code) {
  case 0xc0000005UL: // Access violation.
  case 0xc0000006UL: // In-page error.
  case 0xc000001dUL: // Illegal instruction.
  case 0xc000008dUL: case 0xc000008eUL: case 0xc000008fUL:
  case 0xc0000090UL: case 0xc0000091UL: case 0xc0000092UL: case 0xc0000093UL:
  case 0xc0000094UL: case 0xc0000095UL: case 0xc0000096UL:
  case 0xc00000fdUL: // Stack overflow (skip stack walking in this case).
  case 0xc0000374UL: // Heap corruption.
  case 0xc0000409UL: // Stack buffer overrun; fail-fast may bypass VEH entirely.
  case 0xc000041dUL: // Fatal user callback exception.
    return true;
  default:
    return false; // Do not consume C++ EH, breakpoints, single-step or guard pages.
  }
}

DWORD WINAPI dumpWorker(void *) {
  if (WaitForSingleObject(crash.request, INFINITE) != WAIT_OBJECT_0 ||
      InterlockedCompareExchange(&crash.stopping, 0, 0)) return 0;
  addressLine("fault", crash.record.ExceptionAddress, 0, true);
  for (DWORD i = 0; i < crash.frameCount && i < stackLimit; ++i)
    addressLine("handler_stack", crash.frames[i], i, true);
  MINIDUMP_EXCEPTION_INFORMATION exception{};
  exception.ThreadId = crash.tid;
  exception.ExceptionPointers = &crash.pointers;
  exception.ClientPointers = FALSE;
  constexpr auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo);
  const BOOL okay = crash.writeDump(GetCurrentProcess(), crash.pid, crash.dump, flags,
                                    &exception, nullptr, nullptr);
  const DWORD error = okay ? ERROR_SUCCESS : GetLastError();
  Text result;
  result.append("minidump_ok="); result.hex(okay ? 1 : 0);
  result.append(" win32_or_hresult="); result.hex(error);
  result.append(" dump_flags=0x1000\r\n");
  writeText(result);
  FlushFileBuffers(crash.dump);
  FlushFileBuffers(crash.text);
  SetEvent(crash.completed);
  return 0;
}

LONG CALLBACK observeException(EXCEPTION_POINTERS *info) noexcept {
  const DWORD savedError = GetLastError();
  if (!InterlockedCompareExchange(&crash.armed, 0, 0) || !info ||
      !info->ExceptionRecord || !info->ContextRecord ||
      !selectedCode(info->ExceptionRecord->ExceptionCode) ||
      InterlockedCompareExchange(&crash.first, 1, 0)) {
    SetLastError(savedError);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  crash.tid = GetCurrentThreadId();
  crash.record = *info->ExceptionRecord;
  crash.record.ExceptionRecord = nullptr; // Bounded primary record, not a borrowed chain.
  if (crash.record.NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS)
    crash.record.NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS;
  crash.context = *info->ContextRecord;
  crash.pointers.ExceptionRecord = &crash.record;
  crash.pointers.ContextRecord = &crash.context;
  if (crash.record.ExceptionCode != 0xc00000fdUL)
    crash.frameCount = CaptureStackBackTrace(0, stackLimit, crash.frames, nullptr);
  Text text;
  text.append("observation=first_selected_first_chance_seh\r\nexception_code=");
  text.hex(crash.record.ExceptionCode);
  text.append(" process_id="); text.hex(crash.pid);
  text.append(" thread_id="); text.hex(crash.tid);
  text.append("\r\noriginal_exception_is_not_suppressed=1\r\n");
  writeText(text);
  addressLine("exception_address", crash.record.ExceptionAddress, 0, false);
  // All setup/allocation/DbgHelp calls occur before the handler or on its
  // prestarted worker. This finite handshake never owns a Qt/CRT logging lock.
  if (SetEvent(crash.request)) WaitForSingleObject(crash.completed, writerWaitMs);
  SetLastError(savedError);
  return EXCEPTION_CONTINUE_SEARCH;
}

void closeHandle(HANDLE &handle) noexcept {
  if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  handle = nullptr;
}

void release(const bool failedInstall) noexcept {
  InterlockedExchange(&crash.armed, 0);
  if (crash.handler) { RemoveVectoredExceptionHandler(crash.handler); crash.handler = nullptr; }
  if (crash.worker) {
    InterlockedExchange(&crash.stopping, 1);
    if (crash.request) SetEvent(crash.request);
    // Do not unload/free anything beneath a worker that is still in DbgHelp.
    if (WaitForSingleObject(crash.worker, 1000) != WAIT_OBJECT_0) return;
  }
  closeHandle(crash.worker);
  closeHandle(crash.request);
  closeHandle(crash.completed);
  closeHandle(crash.dump);
  closeHandle(crash.text);
  if (crash.dumpCreated && !InterlockedCompareExchange(&crash.first, 0, 0))
    DeleteFileW(crash.dumpPath); // Our empty CREATE_NEW reservation only.
  if (failedInstall && crash.textCreated) DeleteFileW(crash.textPath);
  crash.dumpCreated = crash.textCreated = false;
  if (crash.dbgHelp) { FreeLibrary(crash.dbgHelp); crash.dbgHelp = nullptr; }
  crash.writeDump = nullptr;
  InterlockedExchange(&crash.installation, 0);
}

// Function-static owner remains alive through stack-owned QApplication teardown.
struct ProcessLifetime {
  ~ProcessLifetime() { release(false); }
};

bool noLinksToRoot(QFileInfo cursor) {
  for (;;) {
    if (!cursor.exists() || !cursor.isDir() || cursor.isSymLink() || cursor.isJunction()) return false;
    if (cursor.absoluteFilePath() == cursor.absolutePath()) return true;
    cursor = QFileInfo(cursor.absolutePath());
  }
}

bool ownedDirectory(const QString &path, QString &canonical) {
  if (path.contains(QChar::Null) || !QDir::isAbsolutePath(path)) return false;
  const QString clean = QDir::cleanPath(path);
  // Local drive only: do not block exception handling on a network/device path.
  if (clean.size() < 3 || clean.at(1) != QLatin1Char(':') ||
      !((clean.at(0) >= QLatin1Char('A') && clean.at(0) <= QLatin1Char('Z')) ||
        (clean.at(0) >= QLatin1Char('a') && clean.at(0) <= QLatin1Char('z')))) return false;
  QFileInfo directory(clean);
  if (!directory.exists()) {
    const QFileInfo parent(directory.absolutePath());
    if (!noLinksToRoot(parent) || !QDir(parent.absoluteFilePath()).mkdir(directory.fileName())) return false;
    directory.refresh();
  }
  if (!noLinksToRoot(directory)) return false;
  canonical = directory.canonicalFilePath();
  return !canonical.isEmpty();
}

bool fixedPath(const QString &path, wchar_t (&destination)[pathCapacity]) {
  const QString native = QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(path);
  if (native.size() >= pathCapacity || native.contains(QChar::Null)) return false;
  for (qsizetype i = 0; i < native.size(); ++i) destination[i] = wchar_t(native.utf16()[i]);
  destination[native.size()] = L'\0';
  return true;
}

bool loadSystemWriter() {
  wchar_t system[MAX_PATH + 1]{};
  const UINT count = GetSystemDirectoryW(system, MAX_PATH + 1);
  if (!count || count > MAX_PATH) return false;
  const QString expected = QDir(QString::fromWCharArray(system)).filePath(QStringLiteral("dbghelp.dll"));
  const QString native = QDir::toNativeSeparators(expected);
  crash.dbgHelp = LoadLibraryExW(reinterpret_cast<LPCWSTR>(native.utf16()), nullptr,
                                LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!crash.dbgHelp) return false;
  wchar_t actual[pathCapacity]{};
  const DWORD length = GetModuleFileNameW(crash.dbgHelp, actual, pathCapacity);
  if (!length || length >= pathCapacity) return false;
  const QString actualCanonical = QFileInfo(QString::fromWCharArray(actual)).canonicalFilePath();
  const QString expectedCanonical = QFileInfo(expected).canonicalFilePath();
  if (actualCanonical.isEmpty() || expectedCanonical.isEmpty() ||
      actualCanonical.compare(expectedCanonical, Qt::CaseInsensitive) != 0) return false;
  const FARPROC symbol = GetProcAddress(crash.dbgHelp, "MiniDumpWriteDump");
  if (!symbol) return false;
  static_assert(sizeof(symbol) == sizeof(crash.writeDump));
  std::memcpy(&crash.writeDump, &symbol, sizeof(symbol));
  return true;
}
} // namespace
#endif

bool installUiCrashDiagnostics(const QString &absoluteOwnedDir) {
#ifdef Q_OS_WIN
  auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
  if (!application || QThread::currentThread() != application->thread() ||
      qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QApplication::platformName() != QStringLiteral("windows")) return false;
  const QString isolated = application->property("gswSmokeSettingsDirectory").toString();
  if (!QDir::isAbsolutePath(isolated) || !noLinksToRoot(QFileInfo(isolated))) return false;
  const QRegularExpression flag(QStringLiteral("^--smoke-test(?:-[a-z0-9-]+)?$"));
  bool explicitSmoke = false;
  for (const QString &argument : application->arguments())
    explicitSmoke = explicitSmoke || flag.match(argument).hasMatch();
  if (!explicitSmoke || InterlockedCompareExchange(&crash.installation, 1, 0)) return false;
  static ProcessLifetime lifetime;
  Q_UNUSED(lifetime);
  InterlockedExchange(&crash.first, 0);
  InterlockedExchange(&crash.stopping, 0);
  crash.frameCount = 0;
  crash.textBytes = 0;
  QString directory;
  if (!ownedDirectory(absoluteOwnedDir, directory) || !loadSystemWriter()) {
    release(true);
    return false;
  }
  crash.pid = GetCurrentProcessId();
  const QString stem = QStringLiteral("qa-crash-") + QString::number(crash.pid);
  if (!fixedPath(QDir(directory).filePath(stem + QStringLiteral(".dmp")), crash.dumpPath) ||
      !fixedPath(QDir(directory).filePath(stem + QStringLiteral(".txt")), crash.textPath)) {
    release(true); return false;
  }
  crash.dump = CreateFileW(crash.dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  crash.dumpCreated = crash.dump != INVALID_HANDLE_VALUE;
  if (!crash.dumpCreated) { release(true); return false; }
  crash.text = CreateFileW(crash.textPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  crash.textCreated = crash.text != INVALID_HANDLE_VALUE;
  if (!crash.textCreated) { release(true); return false; }
  Text armed;
  armed.append("GSW_QA_CRASH/1\r\nstatus=armed_no_selected_exception_seen\r\n");
  armed.append("dump=MiniDumpNormal|MiniDumpWithThreadInfo\r\n");
  armed.append("stack_scope=handler_current_thread_addresses_not_symbolized\r\n");
  writeText(armed);
  if (crash.textBytes != armed.length) { release(true); return false; }
  crash.request = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  crash.completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!crash.request || !crash.completed) { release(true); return false; }
  crash.worker = CreateThread(nullptr, 0, dumpWorker, nullptr, 0, nullptr);
  if (!crash.worker) { release(true); return false; }
  crash.handler = AddVectoredExceptionHandler(1, observeException);
  if (!crash.handler) { release(true); return false; }
  InterlockedExchange(&crash.armed, 1);
  InterlockedExchange(&crash.installation, 2);
  return true;
#else
  Q_UNUSED(absoluteOwnedDir);
  return false;
#endif
}

} // namespace gsw
