// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeActivationCore.h"

namespace gsw {
struct NativeInstallationLock::Impl { QString error = QStringLiteral("activation_not_implemented"); };
NativeInstallationLock::NativeInstallationLock(const QString &) : m(std::make_unique<Impl>()) {}
NativeInstallationLock::~NativeInstallationLock() = default;
bool NativeInstallationLock::locked() const { return false; }
QString NativeInstallationLock::error() const { return m->error; }
NativeActivationResult NativeActivationCore::install(const QString &root, const QString &archive,
    const NativeVerifiedUpdate &update, const NativeInstallationLock &lock) {
  return publish(root, archive, update.offer, lock, false);
}
NativeActivationResult NativeActivationCore::publish(const QString &, const QString &,
    const QByteArray &, const NativeInstallationLock &, bool) {
  return {false, QStringLiteral("activation_not_implemented"), {}};
}
} // namespace gsw
