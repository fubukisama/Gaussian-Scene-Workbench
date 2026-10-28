// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeActivationCore.h"
#include "NativeUpdateDownload.h"
#include <QJsonObject>
#include <atomic>
#include <memory>

namespace gsw {
struct NativeTrustResult final {
  bool ok = false;
  QString code;
  std::unique_ptr<NativeVerifiedUpdate> update;
  QJsonObject offer, acceptedState;
  NativeUpdateDownloadPolicy downloadPolicy;
};
class NativeUpdateTrustBridge final {
public:
  static NativeTrustResult request(const QString &operation, const QString &root,
      const QString &channel, const QString &expectedBuildId,
      const NativeInstallationLock &, const std::atomic_bool *cancelled = nullptr);
  static NativeTrustResult rollbackAdmission(const QString &root, const QString &channel,
      const QString &buildId, const NativeInstallationLock &,
      const std::atomic_bool *cancelled = nullptr);
};
} // namespace gsw
