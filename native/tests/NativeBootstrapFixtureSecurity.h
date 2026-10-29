// SPDX-License-Identifier: GPL-3.0-or-later
// Private to the remote, test-only first-install tracer. No production target.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "json.hpp"

#ifndef GSW_BOOTSTRAP_FIXTURE_ONLY
#error This source must never be built into a production target.
#endif

namespace gsw::bootstrapfixture {
using Json = nlohmann::json;
struct Failure { std::string code; DWORD systemCode = 0; };
[[noreturn]] inline void fail(const char *code) { throw Failure{code, GetLastError()}; }
inline void need(bool value, const char *code) { if (!value) fail(code); }

struct Handle final {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&other) noexcept : value(other.value) { other.value = nullptr; }
  Handle &operator=(Handle &&other) noexcept {
    if (this != &other) { close(); value = other.value; other.value = nullptr; }
    return *this;
  }
  ~Handle() { close(); }
  void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
  explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

inline std::string utf8(const std::wstring &value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
  need(size > 0, "BOOTSTRAP_UTF8_INVALID");
  std::string result(size_t(size), '\0');
  need(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), size, nullptr, nullptr) == size,
      "BOOTSTRAP_UTF8_INVALID");
  return result;
}
inline std::wstring wide(const std::string &value) {
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
  need(size > 0, "BOOTSTRAP_UTF8_INVALID");
  std::wstring result(size_t(size), L'\0');
  need(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), size) == size,
      "BOOTSTRAP_UTF8_INVALID");
  return result;
}
inline std::wstring environment(const wchar_t *name) {
  const auto count = GetEnvironmentVariableW(name, nullptr, 0);
  if (!count) return {};
  need(count < 32768, "BOOTSTRAP_ENVIRONMENT_INVALID");
  std::wstring value(count, L'\0');
  const auto received = GetEnvironmentVariableW(name, value.data(), count);
  need(received && received < count, "BOOTSTRAP_ENVIRONMENT_INVALID");
  value.resize(received);
  return value;
}
inline std::wstring localPath(std::wstring path) {
  std::replace(path.begin(), path.end(), L'/', L'\\');
  need(path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' && path[2] == L'\\' && path.size() < 32760, "BOOTSTRAP_LOCAL_PATH_REQUIRED");
  for (const auto &part : std::filesystem::path(path).relative_path()) {
    const auto name = part.wstring();
    need(!name.empty() && name != L"." && name != L".." && name.back() != L'.' && name.back() != L' ' &&
        name.find_first_of(L"<>:\"|?*") == std::wstring::npos, "BOOTSTRAP_LOCAL_PATH_REQUIRED");
  }
  std::array<wchar_t, 32768> full{};
  const auto size = GetFullPathNameW(path.c_str(), DWORD(full.size()), full.data(), nullptr);
  need(size && size < full.size(), "BOOTSTRAP_LOCAL_PATH_REQUIRED");
  path.assign(full.data(), size);
  while (path.size() > 3 && path.back() == L'\\') path.pop_back();
  return path;
}
inline std::wstring api(const std::wstring &path) { return L"\\\\?\\" + localPath(path); }
inline void noLinks(const std::wstring &path) {
  auto cursor = std::filesystem::path(localPath(path));
  for (;;) {
    const auto attributes = GetFileAttributesW(api(cursor.wstring()).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const auto error = GetLastError();
      need(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "BOOTSTRAP_PATH_UNAVAILABLE");
    } else need(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "BOOTSTRAP_REPARSE_REFUSED");
    const auto parent = cursor.parent_path();
    if (parent == cursor) break;
    cursor = parent;
  }
}
inline bool beneath(const std::wstring &root, const std::wstring &child) {
  const auto prefix = localPath(root) + L"\\";
  const auto normalized = localPath(child);
  return normalized.size() > prefix.size() &&
      CompareStringOrdinal(prefix.data(), int(prefix.size()), normalized.data(), int(prefix.size()), TRUE) == CSTR_EQUAL;
}

inline std::vector<BYTE> tokenData(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
  DWORD size = 0;
  GetTokenInformation(token, kind, nullptr, 0, &size);
  need(size > 0 && size <= 128 * 1024, "BOOTSTRAP_TOKEN_QUERY_FAILED");
  std::vector<BYTE> result(size);
  need(GetTokenInformation(token, kind, result.data(), size, &size) != 0, "BOOTSTRAP_TOKEN_QUERY_FAILED");
  return result;
}
inline bool administrative(PSID sid) {
  for (const auto kind : {WinBuiltinAdministratorsSid, WinBuiltinPowerUsersSid, WinBuiltinBackupOperatorsSid,
                         WinBuiltinAccountOperatorsSid, WinBuiltinSystemOperatorsSid, WinBuiltinPrintOperatorsSid})
    if (IsWellKnownSid(sid, kind)) return true;
  return false;
}
inline Json tokenFacts(HANDLE token) {
  const auto groupData = tokenData(token, TokenGroups);
  const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(groupData.data());
  bool admin = false;
  for (DWORD i = 0; i < groups->GroupCount; ++i)
    if (administrative(groups->Groups[i].Sid) && (groups->Groups[i].Attributes & SE_GROUP_ENABLED) &&
        !(groups->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY)) admin = true;
  const auto integrityData = tokenData(token, TokenIntegrityLevel);
  const auto *label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(integrityData.data());
  need(IsValidSid(label->Label.Sid) != FALSE, "BOOTSTRAP_TOKEN_QUERY_FAILED");
  const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
  need(count > 0, "BOOTSTRAP_TOKEN_QUERY_FAILED");
  const auto integrity = *GetSidSubAuthority(label->Label.Sid, DWORD(count - 1));
  const auto elevationData = tokenData(token, TokenElevation);
  const bool elevated = reinterpret_cast<const TOKEN_ELEVATION *>(elevationData.data())->TokenIsElevated != 0;
  const auto inertData = tokenData(token, TokenSandBoxInert);
  const bool inert = *reinterpret_cast<const DWORD *>(inertData.data()) != 0;
  const auto userData = tokenData(token, TokenUser);
  const auto user = reinterpret_cast<const TOKEN_USER *>(userData.data())->User.Sid;
  const bool system = IsWellKnownSid(user, WinLocalSystemSid) || IsWellKnownSid(user, WinLocalServiceSid) || IsWellKnownSid(user, WinNetworkServiceSid);
  const auto privilegeData = tokenData(token, TokenPrivileges);
  const auto *privileges = reinterpret_cast<const TOKEN_PRIVILEGES *>(privilegeData.data());
  LUID notify{};
  need(LookupPrivilegeValueW(nullptr, SE_CHANGE_NOTIFY_NAME, &notify) != FALSE, "BOOTSTRAP_TOKEN_QUERY_FAILED");
  DWORD enabled = 0, unexpected = 0;
  for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
    const auto &entry = privileges->Privileges[i];
    if (entry.Attributes & SE_PRIVILEGE_ENABLED) {
      ++enabled;
      if (entry.Luid.HighPart != notify.HighPart || entry.Luid.LowPart != notify.LowPart) ++unexpected;
    }
  }
  return { {"restricted", IsTokenRestricted(token) != FALSE}, {"integrityRid", integrity}, {"adminEnabled", admin},
      {"elevated", elevated}, {"sandboxInert", inert}, {"systemAccount", system},
      {"enabledPrivilegeCount", enabled}, {"unexpectedEnabledPrivilegeCount", unexpected} };
}
inline bool ordinaryClient(const Json &facts) {
  return facts.at("restricted") == true && facts.at("integrityRid") == SECURITY_MANDATORY_MEDIUM_RID &&
      facts.at("adminEnabled") == false && facts.at("elevated") == false && facts.at("sandboxInert") == false &&
      facts.at("systemAccount") == false && facts.at("unexpectedEnabledPrivilegeCount") == 0;
}
inline Handle restrictedClientToken() {
  HANDLE original = nullptr;
  need(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &original) != FALSE,
      "BOOTSTRAP_LAB_TOKEN_FAILED");
  Handle owner(original);
  const auto userData = tokenData(owner.value, TokenUser);
  const auto groupData = tokenData(owner.value, TokenGroups);
  const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(groupData.data());
  std::vector<SID_AND_ATTRIBUTES> disabled, restricting;
  restricting.push_back({reinterpret_cast<const TOKEN_USER *>(userData.data())->User.Sid, 0});
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    const auto &entry = groups->Groups[i];
    if (administrative(entry.Sid) || (entry.Attributes & SE_GROUP_OWNER)) disabled.push_back({entry.Sid, 0});
    else if ((entry.Attributes & SE_GROUP_ENABLED) && !(entry.Attributes & (SE_GROUP_USE_FOR_DENY_ONLY | SE_GROUP_INTEGRITY)))
      restricting.push_back({entry.Sid, 0});
  }
  HANDLE restricted = nullptr;
  // In particular, never set SANDBOX_INERT: Windows application policy stays on.
  need(CreateRestrictedToken(owner.value, DISABLE_MAX_PRIVILEGE | LUA_TOKEN,
      DWORD(disabled.size()), disabled.data(), 0, nullptr, DWORD(restricting.size()), restricting.data(), &restricted) != FALSE,
      "BOOTSTRAP_LAB_TOKEN_FAILED");
  Handle result(restricted);
  PSID medium = nullptr;
  need(ConvertStringSidToSidW(L"S-1-16-8192", &medium) != FALSE, "BOOTSTRAP_LAB_TOKEN_FAILED");
  TOKEN_MANDATORY_LABEL label{{medium, SE_GROUP_INTEGRITY}};
  const bool changed = SetTokenInformation(result.value, TokenIntegrityLevel, &label,
      DWORD(sizeof(label) + GetLengthSid(medium))) != FALSE;
  LocalFree(medium);
  need(changed && ordinaryClient(tokenFacts(result.value)), "BOOTSTRAP_LAB_NOT_ORDINARY_CLIENT");
  return result;
}

inline std::wstring currentUserSid() {
  HANDLE raw = nullptr;
  need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "BOOTSTRAP_LAB_TOKEN_FAILED");
  Handle token(raw);
  const auto data = tokenData(token.value, TokenUser);
  wchar_t *text = nullptr;
  need(ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(data.data())->User.Sid, &text) != FALSE,
      "BOOTSTRAP_LAB_TOKEN_FAILED");
  std::wstring value(text);
  LocalFree(text);
  return value;
}
inline Json clientEnvironmentFacts() {
  Json tools = Json::array();
  const auto path = environment(L"PATH");
  for (const auto name : {L"git.exe", L"gh.exe", L"python.exe", L"python3.exe", L"py.exe", L"node.exe", L"pwsh.exe"}) {
    std::array<wchar_t, 32768> found{};
    if (SearchPathW(path.c_str(), name, nullptr, DWORD(found.size()), found.data(), nullptr)) tools.push_back(utf8(name));
  }
  bool sensitive = false;
  for (const auto name : {L"GITHUB_TOKEN", L"GH_TOKEN", L"GIT_ASKPASS", L"PYTHONPATH", L"NODE_PATH", L"PSModulePath", L"OPENAI_API_KEY",
                         L"GSW_UPDATER_FIXTURE_PROFILE_PATH", L"GSW_UPDATER_PRODUCTION_PROFILE_PATH"})
    sensitive = sensitive || !environment(name).empty();
  return {{"discoverableDeveloperTools", tools}, {"sensitiveEnvironmentPresent", sensitive}};
}
} // namespace gsw::bootstrapfixture
