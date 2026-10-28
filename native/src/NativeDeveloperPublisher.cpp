// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeActivationCore.h"
namespace gsw {
NativeActivationResult NativeDeveloperPublisher::publish(const QString &root,
    const QString &archive, const QByteArray &offer, const NativeInstallationLock &lock) {
  return NativeActivationCore::publish(root, archive, offer, lock, true);
}
} // namespace gsw
