// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <memory>

namespace gsw {
struct NativeLaunchPlan;

// Availability evidence only. Neither an attempt nor a ready acknowledgement
// admits an update, changes the active slot, or revokes an accepted build.
class NativeStartupAttempt final {
public:
  ~NativeStartupAttempt();
  NativeStartupAttempt(const NativeStartupAttempt &) = delete;
  NativeStartupAttempt &operator=(const NativeStartupAttempt &) = delete;
  [[nodiscard]] static std::unique_ptr<NativeStartupAttempt> start(
      const NativeLaunchPlan &plan, QString *error);
  [[nodiscard]] QJsonObject initialStatus() const;
  // Finite observation: missing/invalid acknowledgement or process exit is
  // UNCONFIRMED, not a failed build. Closing this object NEVER kills its child.
  [[nodiscard]] QJsonObject observe();
private:
  struct Impl;
  explicit NativeStartupAttempt(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> mImpl;
};

// One idempotent, asynchronous hook, called only after the real window and
// basic viewport have composed. No window creation, focus, training or restart.
void reportNativeStartupReady(quintptr mainWindowHandle, const char *scope) noexcept;

// Fixed-helper transport. Owns process handles rather than a QProcess whose
// destructor could kill a still-observing helper. For creation, a complete
// first response is returned immediately; the caller MUST validate its exact
// canonical schema and intended build/hash before accepting PROCESS_CREATED.
// For inspection, success additionally requires a normal exit code of zero.
[[nodiscard]] bool nativeStartupHelperExchange(const QString &program,
    const NativeLaunchPlan &plan, const QByteArray &intent,
    bool processCreation, QByteArray *response, bool *creationUnconfirmed);
} // namespace gsw
