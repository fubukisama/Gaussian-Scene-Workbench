#pragma once

// Opt-in, bounded diagnostics for the existing remote grid bootstrap command.
// No Qt object, event processing, application state or rendering is required.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cwchar>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsw::qa {

enum class NativeGridQaPhase : unsigned int {
  MainEntry,
  TraceFileReady,
  TraceFileUnavailable,
  WindowsIdentityBegin,
  WindowsIdentityEnd,
  WindowsIdentityFailed,
  QApplicationBegin,
  QApplicationEnd,
  ThemeBegin,
  ThemeEnd,
  LanguageReady,
  MainWindowBegin,
  MainWindowEnd,
  ShowBegin,
  ShowEnd,
  GridBranch,
  GridDeadlineStarted,
  EventLoopBegin,
  GridFirstPoll,
  GridWindowExposed,
  GridWidgetValid,
  GridContextValid,
  GridFirstFrameObserved,
  GridInitialFrameReady,
  InitialFramebufferBegin,
  InitialFramebufferEnd,
  InitialInputBegin,
  InitialInputEnd,
  AxisViewBegin,
  AxisViewEnd,
  AxisPoseReady,
  AxisFrameRequested,
  AxisFrameReady,
  FinalFramebufferBegin,
  FinalFramebufferEnd,
  FinalScreenshotBegin,
  FinalScreenshotEnd,
  TimeoutDetected,
  TimeoutFramebufferBegin,
  TimeoutFramebufferEnd,
  GridVerdict,
  EventLoopEnd,
  Count
};

class NativeGridQaTrace final {
public:
  NativeGridQaTrace(const int argc, char *const argv[]) noexcept {
#ifdef _WIN32
    // The ordinary launch path performs no environment/file/logging operation.
    if (argc != 2 || argv == nullptr || argv[1] == nullptr ||
        std::strcmp(argv[1], "--smoke-test-infinite-grid") != 0 ||
        !environmentEquals("GITHUB_ACTIONS", "true") ||
        !environmentEquals("GITHUB_REPOSITORY", "fubukisama/Gaussian-Scene-Workbench") ||
        !environmentEquals("GSW_GRID_QA_TRACE", "1")) return;
    mEnabled = true;
    mStart = Clock::now();
    // Flush the earliest entry before opening even the optional trace file.
    // If file opening stalls, stderr still proves entry was observed.
    mark(NativeGridQaPhase::MainEntry);
    mFile = openFreshOwnedFile();
    if (mFile != INVALID_HANDLE_VALUE) {
      writeFile(mFirstLine, mFirstLineBytes);
      mark(mFile != INVALID_HANDLE_VALUE ? NativeGridQaPhase::TraceFileReady
                                         : NativeGridQaPhase::TraceFileUnavailable);
    } else {
      mark(NativeGridQaPhase::TraceFileUnavailable);
    }
#else
    (void)argc;
    (void)argv;
#endif
  }

  ~NativeGridQaTrace() noexcept {
#ifdef _WIN32
    if (mFile != INVALID_HANDLE_VALUE) CloseHandle(mFile);
#endif
  }
  NativeGridQaTrace(const NativeGridQaTrace &) = delete;
  NativeGridQaTrace &operator=(const NativeGridQaTrace &) = delete;

  [[nodiscard]] bool enabled() const noexcept { return mEnabled; }

  void mark(const NativeGridQaPhase phase) noexcept {
    if (!mEnabled || mSequence >= 64) return;
    const auto index = static_cast<unsigned int>(phase);
    if (index >= static_cast<unsigned int>(NativeGridQaPhase::Count)) return;
    const std::uint64_t bit = std::uint64_t{1} << index;
    if ((mSeen & bit) != 0) return; // First observation, never a polling log.
    mSeen |= bit;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - mStart).count();
    char line[192]{};
    const int bytes = std::snprintf(line, sizeof(line),
        "{\"schema\":\"gsw.grid_qa_trace/1\",\"seq\":%u,\"elapsedUs\":%lld,\"phase\":\"%s\"}\n",
        ++mSequence, static_cast<long long>(elapsed), phaseName(phase));
    if (bytes <= 0 || bytes >= static_cast<int>(sizeof(line))) return;
    std::fwrite(line, 1, static_cast<std::size_t>(bytes), stderr);
    std::fflush(stderr);
    if (phase == NativeGridQaPhase::MainEntry) {
      std::memcpy(mFirstLine, line, static_cast<std::size_t>(bytes));
      mFirstLineBytes = static_cast<unsigned long>(bytes);
    }
#ifdef _WIN32
    if (mFile != INVALID_HANDLE_VALUE) writeFile(line, static_cast<unsigned long>(bytes));
#endif
  }

private:
  using Clock = std::chrono::steady_clock;
  static_assert(static_cast<unsigned int>(NativeGridQaPhase::Count) <= 64);
  bool mEnabled = false;
  unsigned int mSequence = 0;
  std::uint64_t mSeen = 0;
  Clock::time_point mStart{};
  char mFirstLine[192]{};
  unsigned long mFirstLineBytes = 0;
#ifdef _WIN32
  HANDLE mFile = INVALID_HANDLE_VALUE;

  static bool environmentEquals(const char *key, const char *expected) noexcept {
    char value[128]{};
    const DWORD bytes = GetEnvironmentVariableA(key, value, 128);
    return bytes > 0 && bytes < 128 && std::strcmp(value, expected) == 0;
  }

  static HANDLE openFreshOwnedFile() noexcept {
    wchar_t path[2048]{};
    const DWORD length = GetEnvironmentVariableW(L"GSW_GRID_QA_TRACE_FILE", path, 2048);
    if (length < 4 || length >= 2048 || (path[0] != L'D' && path[0] != L'd') ||
        path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) return INVALID_HANDLE_VALUE;
    for (DWORD i = 2; i < length; ++i) {
      if (path[i] == L'/') path[i] = L'\\';
      if (path[i] == L':' || path[i] < 32 || path[i] == L'"' || path[i] == L'<' ||
          path[i] == L'>' || path[i] == L'|' || path[i] == L'?' || path[i] == L'*')
        return INVALID_HANDLE_VALUE;
    }
    if (length < 6 || _wcsicmp(path + length - 6, L".jsonl") != 0) return INVALID_HANDLE_VALUE;
    wchar_t windows[2048]{};
    const UINT windowsLength = GetWindowsDirectoryW(windows, 2048);
    if (windowsLength == 0 || windowsLength >= 2048 ||
        windows[0] == L'D' || windows[0] == L'd') return INVALID_HANDLE_VALUE;
    // All parents must already exist and be plain directories. This guard
    // neither creates directories nor follows compatibility/junction links.
    DWORD componentStart = 3;
    for (DWORD i = 3; i <= length; ++i) {
      if (i != length && path[i] != L'\\') continue;
      const DWORD componentLength = i - componentStart;
      if (componentLength == 0 || path[i - 1] == L'.' || path[i - 1] == L' ' ||
          (componentLength == 1 && path[componentStart] == L'.') ||
          (componentLength == 2 && path[componentStart] == L'.' && path[componentStart + 1] == L'.') ||
          reservedComponent(path + componentStart, componentLength))
        return INVALID_HANDLE_VALUE;
      if (i != length) {
        path[i] = L'\0';
        const DWORD attributes = GetFileAttributesW(path);
        path[i] = L'\\';
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return INVALID_HANDLE_VALUE;
      }
      componentStart = i + 1;
    }
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  }

  void writeFile(const char *line, const unsigned long bytes) noexcept {
    DWORD written = 0;
    if (!WriteFile(mFile, line, bytes, &written, nullptr) || written != bytes) {
      CloseHandle(mFile);
      mFile = INVALID_HANDLE_VALUE;
      mark(NativeGridQaPhase::TraceFileUnavailable);
    }
  }

  static bool reservedComponent(const wchar_t *value, const DWORD length) noexcept {
    wchar_t stem[5]{};
    DWORD count = 0;
    for (; count < length && value[count] != L'.'; ++count) {
      if (count >= 4) return false;
      const wchar_t character = value[count];
      stem[count] = character >= L'a' && character <= L'z' ? character - (L'a' - L'A') : character;
    }
    if (count == 3)
      return std::wcscmp(stem, L"CON") == 0 || std::wcscmp(stem, L"PRN") == 0 ||
          std::wcscmp(stem, L"AUX") == 0 || std::wcscmp(stem, L"NUL") == 0;
    return count == 4 && (std::wmemcmp(stem, L"COM", 3) == 0 || std::wmemcmp(stem, L"LPT", 3) == 0) &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00B9' || stem[3] == L'\u00B2' || stem[3] == L'\u00B3');
  }
#endif

  static const char *phaseName(const NativeGridQaPhase phase) noexcept {
    switch (phase) {
    case NativeGridQaPhase::MainEntry: return "main.entry";
    case NativeGridQaPhase::TraceFileReady: return "trace-file-ready";
    case NativeGridQaPhase::TraceFileUnavailable: return "trace-file-unavailable";
    case NativeGridQaPhase::WindowsIdentityBegin: return "windows-identity.begin";
    case NativeGridQaPhase::WindowsIdentityEnd: return "windows-identity.end";
    case NativeGridQaPhase::WindowsIdentityFailed: return "windows-identity.failed";
    case NativeGridQaPhase::QApplicationBegin: return "qapplication.begin";
    case NativeGridQaPhase::QApplicationEnd: return "qapplication.end";
    case NativeGridQaPhase::ThemeBegin: return "theme.begin";
    case NativeGridQaPhase::ThemeEnd: return "theme.end";
    case NativeGridQaPhase::LanguageReady: return "language.ready";
    case NativeGridQaPhase::MainWindowBegin: return "main-window.begin";
    case NativeGridQaPhase::MainWindowEnd: return "main-window.end";
    case NativeGridQaPhase::ShowBegin: return "show.begin";
    case NativeGridQaPhase::ShowEnd: return "show.end";
    case NativeGridQaPhase::GridBranch: return "grid.branch";
    case NativeGridQaPhase::GridDeadlineStarted: return "grid.deadline-started";
    case NativeGridQaPhase::EventLoopBegin: return "event-loop.begin";
    case NativeGridQaPhase::GridFirstPoll: return "grid.first-poll";
    case NativeGridQaPhase::GridWindowExposed: return "grid.window-exposed";
    case NativeGridQaPhase::GridWidgetValid: return "grid.widget-valid";
    case NativeGridQaPhase::GridContextValid: return "grid.context-valid";
    case NativeGridQaPhase::GridFirstFrameObserved: return "grid.first-frame-observed";
    case NativeGridQaPhase::GridInitialFrameReady: return "grid.initial-frame-ready";
    case NativeGridQaPhase::InitialFramebufferBegin: return "initial-framebuffer.begin";
    case NativeGridQaPhase::InitialFramebufferEnd: return "initial-framebuffer.end";
    case NativeGridQaPhase::InitialInputBegin: return "initial-input.begin";
    case NativeGridQaPhase::InitialInputEnd: return "initial-input.end";
    case NativeGridQaPhase::AxisViewBegin: return "axis-view.begin";
    case NativeGridQaPhase::AxisViewEnd: return "axis-view.end";
    case NativeGridQaPhase::AxisPoseReady: return "axis-pose.ready";
    case NativeGridQaPhase::AxisFrameRequested: return "axis-frame.requested";
    case NativeGridQaPhase::AxisFrameReady: return "axis-frame.ready";
    case NativeGridQaPhase::FinalFramebufferBegin: return "final-framebuffer.begin";
    case NativeGridQaPhase::FinalFramebufferEnd: return "final-framebuffer.end";
    case NativeGridQaPhase::FinalScreenshotBegin: return "final-screenshot.begin";
    case NativeGridQaPhase::FinalScreenshotEnd: return "final-screenshot.end";
    case NativeGridQaPhase::TimeoutDetected: return "grid.timeout-detected";
    case NativeGridQaPhase::TimeoutFramebufferBegin: return "timeout-framebuffer.begin";
    case NativeGridQaPhase::TimeoutFramebufferEnd: return "timeout-framebuffer.end";
    case NativeGridQaPhase::GridVerdict: return "grid.verdict";
    case NativeGridQaPhase::EventLoopEnd: return "event-loop.end";
    case NativeGridQaPhase::Count: break;
    }
    return "invalid-phase";
  }
};

} // namespace gsw::qa
