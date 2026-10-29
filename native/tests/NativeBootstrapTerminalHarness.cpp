// SPDX-License-Identifier: GPL-3.0-or-later
// Privileged setupLab controller; the bootstrap endpoint never uses this token.
#include "NativeBootstrapFixtureSecurity.h"
#include <aclapi.h>
#include <wintrust.h>
#include <softpub.h>
#include <chrono>
#include <iostream>

using namespace gsw::bootstrapfixture;
namespace {
// Narrow, value-free remote probe. Never serialize exception.what(), handles,
// SIDs, environment values, credentials, or the private desktop name.
struct LabDiagnostic {
  const char *stage = "start";
  bool childCreated = false, childResumed = false, childTokenValidated = false, childExited = false;
  DWORD childExitCode = 0;
  size_t outputBytes = 0;
  std::string childCode;
  Json access = Json::object();
  Json record(DWORD lastOsError) const {
    Json value{{"probeTag", "BOOTSTRAP-LAB-DIAG-1"}, {"stage", stage}, {"lastOsError", lastOsError},
        {"childCreated", childCreated}, {"childResumed", childResumed}, {"childTokenValidated", childTokenValidated},
        {"childExited", childExited}, {"capturedOutputBytes", outputBytes}};
    if (childExited) value["childExitCode"] = childExitCode;
    if (!childCode.empty()) value["childResponseCode"] = childCode;
    if (!access.empty()) value["restrictedAccess"] = access;
    return value;
  }
} diagnostic;
void stage(const char *name) { diagnostic.stage = name; SetLastError(ERROR_SUCCESS); }
struct Algorithm {
  BCRYPT_ALG_HANDLE value = nullptr;
  ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
};
struct Hash {
  BCRYPT_HASH_HANDLE value = nullptr;
  ~Hash() { if (value) BCryptDestroyHash(value); }
};
struct Image {
  Handle file;
  std::vector<Handle> parents;
  std::string sha256;
};
Image approvedImage(const std::wstring &path) {
  noLinks(path);
  Image image;
  auto parent = std::filesystem::path(path).parent_path();
  for (;;) {
    Handle lease(CreateFileW(api(parent.wstring()).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    need(bool(lease), "BOOTSTRAP_SOURCE_UNAVAILABLE");
    BY_HANDLE_FILE_INFORMATION info{};
    need(GetFileInformationByHandle(lease.value, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "BOOTSTRAP_SOURCE_INVALID");
    image.parents.push_back(std::move(lease));
    const auto next = parent.parent_path();
    if (next == parent) break;
    parent = next;
  }
  image.file = Handle(CreateFileW(api(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  need(bool(image.file), "BOOTSTRAP_SOURCE_UNAVAILABLE");
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER size{};
  need(GetFileInformationByHandle(image.file.value, &info) && info.nNumberOfLinks == 1 &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
      GetFileSizeEx(image.file.value, &size) && size.QuadPart > 0 && size.QuadPart <= 512LL * 1024 * 1024,
      "BOOTSTRAP_SOURCE_INVALID");
  std::array<wchar_t, 32768> final{};
  const auto length = GetFinalPathNameByHandleW(image.file.value, final.data(), DWORD(final.size()), FILE_NAME_NORMALIZED);
  need(length && length < final.size() && CompareStringOrdinal(final.data(), int(length), api(path).c_str(), -1, TRUE) == CSTR_EQUAL,
      "BOOTSTRAP_SOURCE_INVALID");
  Algorithm algorithm;
  need(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "BOOTSTRAP_SOURCE_HASH_FAILED");
  DWORD objectSize = 0, received = 0;
  need(BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &received, 0) >= 0,
      "BOOTSTRAP_SOURCE_HASH_FAILED");
  std::vector<BYTE> object(objectSize);
  Hash hash;
  need(BCryptCreateHash(algorithm.value, &hash.value, object.data(), objectSize, nullptr, 0, 0) >= 0, "BOOTSTRAP_SOURCE_HASH_FAILED");
  std::array<BYTE, 65536> block{};
  for (;;) {
    need(ReadFile(image.file.value, block.data(), DWORD(block.size()), &received, nullptr) != FALSE, "BOOTSTRAP_SOURCE_HASH_FAILED");
    if (!received) break;
    need(BCryptHashData(hash.value, block.data(), received, 0) >= 0, "BOOTSTRAP_SOURCE_HASH_FAILED");
  }
  std::array<BYTE, 32> digest{};
  need(BCryptFinishHash(hash.value, digest.data(), DWORD(digest.size()), 0) >= 0, "BOOTSTRAP_SOURCE_HASH_FAILED");
  constexpr char hex[] = "0123456789abcdef";
  for (const auto value : digest) { image.sha256 += hex[value >> 4]; image.sha256 += hex[value & 15]; }
  return image;
}
Json authenticodeObservation(const std::wstring &path) {
  WINTRUST_FILE_INFO file{};
  file.cbStruct = sizeof(file); file.pcwszFilePath = path.c_str();
  WINTRUST_DATA data{};
  data.cbStruct = sizeof(data); data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE; // Observation only, never an acceptance grant.
  data.dwUnionChoice = WTD_CHOICE_FILE; data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status = WinVerifyTrust(nullptr, &action, &data);
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &action, &data);
  const bool unsignedFile = status == TRUST_E_NOSIGNATURE || status == TRUST_E_SUBJECT_FORM_UNKNOWN || status == TRUST_E_PROVIDER_UNKNOWN;
  return {{"status", status == ERROR_SUCCESS ? "VALID" : unsignedFile ? "NOT_SIGNED" : "NOT_TRUSTED"},
      {"statusCode", DWORD(status)}, {"publisherAuthenticated", false}, {"policyChanged", false}};
}
void mediumOwnedDirectory(const std::wstring &path) {
  const auto base = localPath(wide(GSW_BOOTSTRAP_FIXTURE_ROOT));
  need(path == base || beneath(base, path), "BOOTSTRAP_LAB_PATH_INVALID");
  noLinks(path);
  std::filesystem::create_directories(std::filesystem::path(api(path)));
  const auto sddl = L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;" + currentUserSid() + L")S:(ML;OICI;NW;;;ME)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  need(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
      "BOOTSTRAP_LAB_ACL_FAILED");
  PACL dacl = nullptr, sacl = nullptr;
  BOOL present = FALSE, inherited = FALSE;
  const bool extracted = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &inherited) && present &&
      GetSecurityDescriptorSacl(descriptor, &present, &sacl, &inherited) && present;
  auto mutablePath = api(path);
  const auto status = extracted ? SetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, nullptr, nullptr, dacl, sacl) : ERROR_INVALID_SECURITY_DESCR;
  LocalFree(descriptor);
  need(status == ERROR_SUCCESS, "BOOTSTRAP_LAB_ACL_FAILED");
}
struct Desktop {
  HDESK value = nullptr;
  std::wstring fullName;
  Desktop() = default;
  Desktop(const Desktop &) = delete;
  Desktop(Desktop &&other) noexcept : value(other.value), fullName(std::move(other.fullName)) { other.value = nullptr; }
  ~Desktop() { if (value) CloseDesktop(value); }
};
Desktop privateDesktop() {
  const auto name = L"GSW-Bootstrap-Fixture-" + std::to_wstring(GetCurrentProcessId());
  const auto sddl = L"D:(A;;GA;;;SY)(A;;GA;;;" + currentUserSid() + L")S:(ML;;NW;;;ME)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  need(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
      "BOOTSTRAP_LAB_DESKTOP_FAILED");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  Desktop desktop;
  desktop.value = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, &attributes);
  LocalFree(descriptor);
  need(desktop.value != nullptr, "BOOTSTRAP_LAB_DESKTOP_FAILED");
  DWORD bytes = 0;
  GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, nullptr, 0, &bytes);
  need(bytes > 0 && bytes < 65536, "BOOTSTRAP_LAB_DESKTOP_FAILED");
  std::vector<wchar_t> station(bytes / sizeof(wchar_t) + 1);
  need(GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station.data(), bytes, &bytes) != FALSE,
      "BOOTSTRAP_LAB_DESKTOP_FAILED");
  desktop.fullName = std::wstring(station.data()) + L"\\" + name;
  return desktop;
}
Json restrictedAccess(HANDLE token, const std::wstring &image, const Desktop &desktop) {
  Json result;
  const auto separator = desktop.fullName.find(L'\\');
  need(separator != std::wstring::npos, "BOOTSTRAP_LAB_DESKTOP_FAILED");
  const auto stationName = desktop.fullName.substr(0, separator);
  const auto desktopName = desktop.fullName.substr(separator + 1);
  std::array<wchar_t, 32768> system{};
  const auto count = GetSystemDirectoryW(system.data(), DWORD(system.size()));
  need(count && count < system.size(), "BOOTSTRAP_LAB_ACCESS_PROBE_FAILED");
  if (!ImpersonateLoggedOnUser(token)) return {{"available", false}, {"osError", GetLastError()}};
  struct Revert { bool active = true; ~Revert() { if (active) RevertToSelf(); } } revert;
  result["available"] = true;
  const auto file = [&](const char *label, const std::wstring &path) {
    SetLastError(ERROR_SUCCESS);
    Handle opened(CreateFileW(api(path).c_str(), GENERIC_READ | GENERIC_EXECUTE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    const auto error = opened ? ERROR_SUCCESS : GetLastError();
    result[label] = {{"readExecuteAllowed", bool(opened)}, {"osError", error}};
  };
  file("bootstrapImage", image);
  for (const auto name : {L"ntdll.dll", L"kernel32.dll", L"kernelbase.dll", L"advapi32.dll"})
    file(utf8(name).c_str(), std::wstring(system.data()) + L"\\" + name);
  SetLastError(ERROR_SUCCESS);
  const auto station = OpenWindowStationW(stationName.c_str(), FALSE, WINSTA_READATTRIBUTES | WINSTA_ACCESSGLOBALATOMS);
  const auto stationError = station ? ERROR_SUCCESS : GetLastError();
  result["currentWindowStationMinimum"] = {{"allowed", station != nullptr}, {"osError", stationError}};
  if (station) CloseWindowStation(station);
  SetLastError(ERROR_SUCCESS);
  const auto openedDesktop = OpenDesktopW(desktopName.c_str(), 0, FALSE,
      DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
  const auto desktopError = openedDesktop ? ERROR_SUCCESS : GetLastError();
  result["ownedPrivateDesktopMinimum"] = {{"allowed", openedDesktop != nullptr}, {"osError", desktopError}};
  if (openedDesktop) CloseDesktop(openedDesktop);
  const bool restored = RevertToSelf() != FALSE;
  revert.active = !restored;
  need(restored, "BOOTSTRAP_LAB_REVERT_FAILED");
  return result;
}
std::vector<wchar_t> clientEnvironment(const std::wstring &bootstrap, const std::wstring &base) {
  std::array<wchar_t, 32768> windows{};
  const auto count = GetWindowsDirectoryW(windows.data(), DWORD(windows.size()));
  need(count && count < windows.size(), "BOOTSTRAP_LAB_ENVIRONMENT_FAILED");
  const auto profile = base + L"\\profile";
  for (const auto &relative : {L"profile", L"profile\\AppData\\Roaming", L"profile\\AppData\\Local", L"tmp"})
    mediumOwnedDirectory(base + L"\\" + relative);
  std::map<std::wstring, std::wstring> values{{L"APPDATA", profile + L"\\AppData\\Roaming"},
      {L"LOCALAPPDATA", profile + L"\\AppData\\Local"}, {L"USERPROFILE", profile},
      {L"TEMP", base + L"\\tmp"}, {L"TMP", base + L"\\tmp"}, {L"SYSTEMROOT", windows.data()}, {L"WINDIR", windows.data()},
      {L"PATH", std::filesystem::path(bootstrap).parent_path().wstring() + L";" + windows.data() + L"\\System32"}};
  std::vector<wchar_t> result;
  for (const auto &[key, value] : values) {
    const auto item = key + L"=" + value;
    result.insert(result.end(), item.begin(), item.end()); result.push_back(L'\0');
  }
  result.push_back(L'\0');
  return result;
}
struct AttributeList {
  std::vector<BYTE> bytes;
  LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
  ~AttributeList() { if (value) DeleteProcThreadAttributeList(value); }
};
struct OwnedProcess {
  Handle process, thread;
  bool exited = false;
  ~OwnedProcess() { if (process && !exited) { TerminateProcess(process.value, 70); WaitForSingleObject(process.value, 5000); } }
};
Json runClient(const std::wstring &bootstrap, const std::wstring &destination, const std::wstring &base,
    bool loaderProbe = false, bool requestPrivateDesktop = true) {
  stage("root_acl");
  mediumOwnedDirectory(base);
  stage("restrict_client_token");
  auto token = restrictedClientToken();
  stage("private_desktop");
  auto desktop = privateDesktop();
  stage("probe_client_access");
  diagnostic.access = restrictedAccess(token.value, bootstrap, desktop);
  stage("client_environment");
  auto environment = clientEnvironment(bootstrap, base);
  stage("client_pipes");
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE reader = nullptr, writer = nullptr;
  need(CreatePipe(&reader, &writer, &inherit, 0) != FALSE, "BOOTSTRAP_LAB_PIPE_FAILED");
  Handle output(reader), childOutput(writer);
  need(SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0) != FALSE, "BOOTSTRAP_LAB_PIPE_FAILED");
  Handle childInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
  need(bool(childInput), "BOOTSTRAP_LAB_PIPE_FAILED");
  AttributeList list;
  stage("client_handle_allowlist");
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  need(bytes > 0, "BOOTSTRAP_LAB_PROCESS_FAILED");
  list.bytes.resize(bytes);
  list.value = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.bytes.data());
  need(InitializeProcThreadAttributeList(list.value, 1, 0, &bytes) != FALSE, "BOOTSTRAP_LAB_PROCESS_FAILED");
  const HANDLE handles[]{childOutput.value, childInput.value};
  need(UpdateProcThreadAttribute(list.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      const_cast<HANDLE *>(handles), sizeof(handles), nullptr, nullptr) != FALSE, "BOOTSTRAP_LAB_PROCESS_FAILED");
  STARTUPINFOEXW start{};
  start.StartupInfo.cb = sizeof(start);
  start.StartupInfo.lpDesktop = requestPrivateDesktop ? desktop.fullName.data() : nullptr;
  start.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  start.StartupInfo.wShowWindow = SW_HIDE;
  start.StartupInfo.hStdInput = childInput.value;
  start.StartupInfo.hStdOutput = childOutput.value; start.StartupInfo.hStdError = childOutput.value;
  start.lpAttributeList = list.value;
  std::wstring arguments = L"\"" + bootstrap + L"\" --install \"" + destination + L"\"";
  PROCESS_INFORMATION information{};
  stage("create_client_process");
  need(CreateProcessAsUserW(token.value, bootstrap.c_str(), arguments.data(), nullptr, nullptr, TRUE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
      environment.data(), base.c_str(), &start.StartupInfo, &information) != FALSE, "BOOTSTRAP_CLIENT_CREATE_FAILED");
  diagnostic.childCreated = true;
  OwnedProcess child{Handle(information.hProcess), Handle(information.hThread)};
  stage("attach_owned_job");
  Handle job(CreateJobObjectW(nullptr, nullptr));
  need(bool(job), "BOOTSTRAP_LAB_JOB_FAILED");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  need(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
      AssignProcessToJobObject(job.value, child.process.value), "BOOTSTRAP_LAB_JOB_FAILED");
  HANDLE actualToken = nullptr;
  stage("verify_suspended_client_token");
  need(OpenProcessToken(child.process.value, TOKEN_QUERY, &actualToken) != FALSE, "BOOTSTRAP_CLIENT_TOKEN_QUERY_FAILED");
  Handle actual(actualToken);
  const auto facts = tokenFacts(actual.value);
  need(ordinaryClient(facts), "BOOTSTRAP_CLIENT_TOKEN_NOT_ORDINARY");
  diagnostic.childTokenValidated = true;
  childOutput.close(); childInput.close();
  stage("resume_client");
  need(ResumeThread(child.thread.value) != DWORD(-1), "BOOTSTRAP_CLIENT_RESUME_FAILED");
  diagnostic.childResumed = true;
  // A fixed marker-only diagnostic has no network/install work; the original
  // 180-second bootstrap deadline and outer 210-second test are unchanged.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(loaderProbe ? 10 : 180);
  std::string captured;
  const auto drain = [&] {
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr)) {
        need(GetLastError() == ERROR_BROKEN_PIPE, "BOOTSTRAP_CLIENT_CAPTURE_FAILED"); return;
      }
      if (!available) return;
      need(captured.size() + available <= 65536, "BOOTSTRAP_CLIENT_OUTPUT_LIMIT");
      std::vector<char> block(available);
      DWORD count = 0;
      need(ReadFile(output.value, block.data(), available, &count, nullptr) != FALSE, "BOOTSTRAP_CLIENT_CAPTURE_FAILED");
      captured.append(block.data(), count);
      diagnostic.outputBytes = captured.size();
    }
  };
  stage("capture_client");
  while (std::chrono::steady_clock::now() < deadline) {
    drain();
    if (WaitForSingleObject(child.process.value, 50) == WAIT_OBJECT_0) { child.exited = true; break; }
  }
  need(child.exited, "BOOTSTRAP_CLIENT_DEADLINE");
  drain();
  stage("read_client_exit");
  DWORD exit = 0;
  need(GetExitCodeProcess(child.process.value, &exit) != FALSE, "BOOTSTRAP_CLIENT_CAPTURE_FAILED");
  diagnostic.childExited = true;
  diagnostic.childExitCode = exit;
  if (loaderProbe) {
    static const std::string marker = "{\"schema\":\"gsw.bootstrap_loader_probe/1\",\"enteredMain\":true}\n";
    const bool entered = exit == 0 && captured == marker;
    stage("loader_control_observed");
    return {{"scope", "diagnostic-control-not-installation-acceptance"}, {"enteredMain", entered},
        {"childExitCode", exit}, {"capturedOutputBytes", captured.size()}, {"terminalToken", facts},
        {"privateDesktopRequested", requestPrivateDesktop}, {"restrictedAccess", diagnostic.access}};
  }
  stage("parse_child_response");
  const auto response = Json::parse(captured);
  if (response.is_object() && response.contains("code") && response["code"].is_string()) {
    const auto code = response["code"].get<std::string>();
    if (!code.empty() && code.size() <= 96 && std::all_of(code.begin(), code.end(), [](char c) {
          return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; })) diagnostic.childCode = code;
  }
  stage("validate_child_response");
  need(response.is_object() && response.value("schema", "") == "gsw.bootstrap_fixture_result/1" &&
      response.value("testOnly", false) && response.at("terminalToken") == facts,
      "BOOTSTRAP_CLIENT_RESPONSE_INVALID");
  stage("client_observed");
  return {{"childExitCode", exit}, {"childResponse", response}, {"terminalToken", facts},
      {"privateDesktop", true}, {"setupLabTokenUsedByClient", false}, {"passwordCreated", false}};
}
void receipt(const std::wstring &path, const Json &value) {
  noLinks(path);
  Handle file(CreateFileW(api(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  need(bool(file), "BOOTSTRAP_LAB_EVIDENCE_FAILED");
  const auto data = value.dump(-1, ' ', true) + "\n";
  DWORD written = 0;
  need(WriteFile(file.value, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size() && FlushFileBuffers(file.value),
      "BOOTSTRAP_LAB_EVIDENCE_FAILED");
}
}
int wmain(int argc, wchar_t **argv) {
  Json result{{"schema", "gsw.bootstrap_terminal_run/1"}, {"ok", false}, {"testOnly", true},
      {"production", false}, {"authenticodeClaim", false}};
  std::wstring evidence;
  std::wstring probe, probePin, destinationForProbe, baseForProbe;
  DWORD diagnosticOsError = ERROR_SUCCESS;
  try {
    stage("validate_lab_request");
    need(environment(L"GITHUB_ACTIONS") == L"true" && environment(L"RUNNER_OS") == L"Windows", "BOOTSTRAP_REMOTE_LAB_ONLY");
    need(argc == 9 || argc == 13, "BOOTSTRAP_LAB_INVALID_REQUEST");
    std::map<std::wstring, std::wstring> options;
    for (int i = 1; i < argc; i += 2) need(options.emplace(argv[i], argv[i + 1]).second, "BOOTSTRAP_LAB_INVALID_REQUEST");
    need((options.size() == 4 || options.size() == 6) && options.contains(L"--bootstrap") && options.contains(L"--destination") &&
        options.contains(L"--expected-sha256") && options.contains(L"--evidence"), "BOOTSTRAP_LAB_INVALID_REQUEST");
    if (options.size() == 6) {
      need(options.contains(L"--loader-probe") && options.contains(L"--expected-probe-sha256"), "BOOTSTRAP_LAB_INVALID_REQUEST");
      probe = localPath(options.at(L"--loader-probe")); probePin = options.at(L"--expected-probe-sha256");
      need(std::filesystem::path(probe).filename() == L"GSW Bootstrap Loader Probe.exe", "BOOTSTRAP_LAB_INVALID_REQUEST");
    }
    const auto bootstrap = localPath(options.at(L"--bootstrap"));
    const auto destination = localPath(options.at(L"--destination"));
    const auto base = localPath(wide(GSW_BOOTSTRAP_FIXTURE_ROOT));
    destinationForProbe = destination; baseForProbe = base;
    evidence = localPath(options.at(L"--evidence"));
    need(beneath(base, destination) && beneath(std::filesystem::path(base).parent_path().wstring(), evidence), "BOOTSTRAP_LAB_PATH_INVALID");
    noLinks(destination); noLinks(evidence);
    const auto approved = utf8(options.at(L"--expected-sha256"));
    need(approved.size() == 64 && approved.find_first_not_of("0123456789abcdef") == std::string::npos, "BOOTSTRAP_LAB_INVALID_REQUEST");
    auto image = approvedImage(bootstrap);
    need(image.sha256 == approved, "BOOTSTRAP_SOURCE_IDENTITY_MISMATCH");
    result["bootstrapSha256"] = image.sha256;
    result["sourceAuthority"] = "test-only-out-of-band-ci-artifact-pin";
    stage("observe_authenticode");
    result["authenticode"] = authenticodeObservation(bootstrap);
    need(result["authenticode"]["status"] == "NOT_SIGNED", "BOOTSTRAP_FIXTURE_IDENTITY_UNEXPECTED");
    result.update(runClient(bootstrap, destination, base));
    result["ok"] = true; result["code"] = "BOOTSTRAP_CLIENT_OBSERVED";
  } catch (const Failure &error) {
    result["code"] = error.code; result["systemCode"] = error.systemCode;
    result["exceptionKind"] = "native_failure"; diagnosticOsError = error.systemCode;
  } catch (const std::filesystem::filesystem_error &error) {
    result["code"] = "BOOTSTRAP_LAB_FAILED"; result["exceptionKind"] = "filesystem_error";
    result["filesystemErrorValue"] = error.code().value(); diagnosticOsError = DWORD(error.code().value());
  } catch (const Json::exception &error) {
    diagnosticOsError = GetLastError(); result["code"] = "BOOTSTRAP_LAB_FAILED";
    result["exceptionKind"] = "json_exception"; result["jsonExceptionId"] = error.id;
  } catch (const std::system_error &error) {
    result["code"] = "BOOTSTRAP_LAB_FAILED"; result["exceptionKind"] = "system_error";
    result["systemErrorValue"] = error.code().value(); diagnosticOsError = DWORD(error.code().value());
  } catch (const std::exception &) {
    diagnosticOsError = GetLastError(); result["code"] = "BOOTSTRAP_LAB_FAILED"; result["exceptionKind"] = "std_exception";
  } catch (...) {
    diagnosticOsError = GetLastError(); result["code"] = "BOOTSTRAP_LAB_FAILED"; result["exceptionKind"] = "unknown_exception";
  }
  result["labDiagnostic"] = diagnostic.record(diagnosticOsError);
  // Never turn these controls into a passing FIRST result or an installer
  // fallback. Both controls retain the exact ordinary-client token assertions.
  if (result["ok"] == false && diagnostic.childExited && diagnostic.childExitCode == 0xC0000142 && !probe.empty()) {
    Json controls{{"scope", "diagnostic-only"}, {"installationAcceptance", false}, {"cases", Json::array()}};
    try {
      const auto pin = utf8(probePin);
      need(pin.size() == 64 && pin.find_first_not_of("0123456789abcdef") == std::string::npos, "BOOTSTRAP_LAB_INVALID_REQUEST");
      auto approved = approvedImage(probe);
      need(approved.sha256 == pin, "BOOTSTRAP_SOURCE_IDENTITY_MISMATCH");
      controls["probeSha256"] = approved.sha256;
      for (const bool privateDesktopRequested : {true, false}) {
        diagnostic = LabDiagnostic{};
        try {
          controls["cases"].push_back(runClient(probe, destinationForProbe, baseForProbe, true, privateDesktopRequested));
        } catch (const Failure &error) {
          controls["cases"].push_back({{"scope", "diagnostic-control-not-installation-acceptance"},
              {"privateDesktopRequested", privateDesktopRequested}, {"code", error.code}, {"osError", error.systemCode},
              {"stage", diagnostic.stage}});
        } catch (...) {
          controls["cases"].push_back({{"scope", "diagnostic-control-not-installation-acceptance"},
              {"privateDesktopRequested", privateDesktopRequested}, {"code", "BOOTSTRAP_LOADER_CONTROL_FAILED"}, {"stage", diagnostic.stage}});
        }
      }
    } catch (const Failure &error) { controls["code"] = error.code; controls["osError"] = error.systemCode; }
      catch (...) { controls["code"] = "BOOTSTRAP_LOADER_CONTROL_FAILED"; }
    result["loaderDiagnostic"] = controls;
  }
  if (!evidence.empty()) {
    try { receipt(evidence, result); } catch (...) { result["ok"] = false; result["code"] = "BOOTSTRAP_LAB_EVIDENCE_FAILED"; }
  }
  std::cout << result.dump(-1, ' ', true) << '\n';
  return result["ok"] == true ? 0 : 2;
}
