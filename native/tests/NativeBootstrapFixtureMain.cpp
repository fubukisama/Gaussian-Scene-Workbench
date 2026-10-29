// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeBootstrapFixtureSecurity.h"
#include <iostream>

using namespace gsw::bootstrapfixture;
int wmain(int argc, wchar_t **argv) {
  Json result{{"schema", "gsw.bootstrap_fixture_result/1"}, {"ok", false}, {"testOnly", true},
      {"production", false}, {"authenticodeClaim", false}, {"targetMutated", false}};
  try {
    need(argc == 3 && std::wstring(argv[1]) == L"--install", "BOOTSTRAP_INVALID_REQUEST");
    const auto target = localPath(argv[2]);
    const auto base = wide(GSW_BOOTSTRAP_FIXTURE_ROOT);
    need(beneath(base, target), "BOOTSTRAP_OUTSIDE_OWNED_FIXTURE");
    noLinks(target);
    HANDLE raw = nullptr;
    need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "BOOTSTRAP_TOKEN_QUERY_FAILED");
    Handle token(raw);
    result["terminalToken"] = tokenFacts(token.value);
    need(ordinaryClient(result["terminalToken"]), "BOOTSTRAP_CLIENT_NOT_RESTRICTED");
    result["terminalEnvironment"] = clientEnvironmentFacts();
    need(result["terminalEnvironment"]["discoverableDeveloperTools"].empty() &&
        result["terminalEnvironment"]["sensitiveEnvironmentPresent"] == false, "BOOTSTRAP_CLIENT_DEVELOPER_DEPENDENCY");
    // Recognized, authorized fixture intent reaches a genuine semantic stub.
    // No install/trust/publication operation exists in this initial RED.
    result["code"] = "BOOTSTRAP_NOT_IMPLEMENTED";
  } catch (const Failure &error) {
    result["code"] = error.code;
    result["systemCode"] = error.systemCode;
  } catch (...) { result["code"] = "BOOTSTRAP_FIXTURE_FAILED"; }
  std::cout << result.dump(-1, ' ', true) << '\n';
  return result["ok"] == true ? 0 : 2;
}
