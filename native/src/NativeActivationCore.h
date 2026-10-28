// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <memory>

namespace gsw {

struct NativeActivationResult final {
  bool ok = false;
  QString code;
  QString buildId;
};

// The same lock is held across trust, download, staging and pointer publication.
// It interoperates with the pre-existing publisher's exclusive .publish.lock.
class NativeInstallationLock final {
public:
  explicit NativeInstallationLock(const QString &installationRoot);
  ~NativeInstallationLock();
  NativeInstallationLock(const NativeInstallationLock &) = delete;
  NativeInstallationLock &operator=(const NativeInstallationLock &) = delete;
  [[nodiscard]] bool locked() const;
  [[nodiscard]] QString error() const;
  [[nodiscard]] QString installationRoot() const;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

class NativeUpdateTrustBridge;
class NativeDeveloperPublisher;

// There is no public constructor taking a verified bool or arbitrary JSON.
// Only the linked trust bridge can mint an online publisher admission.
class NativeVerifiedUpdate final {
  friend class NativeUpdateTrustBridge;
  friend class NativeActivationCore;
  QByteArray offer;
  QString descriptorSha256;
  QString installationRoot;
  NativeVerifiedUpdate() = default;
public:
  NativeVerifiedUpdate(const NativeVerifiedUpdate &) = default;
  [[nodiscard]] QString descriptorDigest() const { return descriptorSha256; }
};

class NativeActivationCore final {
public:
  static NativeActivationResult install(const QString &root, const QString &archive,
      const NativeVerifiedUpdate &, const NativeInstallationLock &);
private:
  friend class NativeDeveloperPublisher;
  static NativeActivationResult publish(const QString &root, const QString &archive,
      const QByteArray &offer, const NativeInstallationLock &, bool developerAdmission,
      const QString &descriptorSha256 = {});
};

// This adapter is built into a separate developer-only executable/test target,
// never linked into or exposed as a switch of the public update helper.
class NativeDeveloperPublisher final {
public:
  static NativeActivationResult publish(const QString &root, const QString &archive,
      const QByteArray &offer, const NativeInstallationLock &);
};

} // namespace gsw
