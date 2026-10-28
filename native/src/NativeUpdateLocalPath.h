// SPDX-License-Identifier: GPL-3.0-or-later
// Private filesystem spelling adapter, not an authorization boundary. Callers
// still inspect every ancestor and the identity/type of every opened handle.
#pragma once
#include <QDir>
#include <QRegularExpression>
#include <QString>

namespace gsw::updatepaths {
inline bool logicalLocalPath(const QString &path) {
  if (!QDir::isAbsolutePath(path) || path.startsWith(QLatin1String("//")) ||
      path.startsWith(QLatin1String("\\\\"))) return false;
#ifdef Q_OS_WIN
  // Only ordinary drive-qualified paths enter this adapter. In particular,
  // adding the internal extended prefix must not enable a caller's DOS device,
  // ADS, UNC, relative or normalization-ambiguous name.
  const QString portable = QDir::fromNativeSeparators(path);
  static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:/"));
  static const QRegularExpression device(QStringLiteral("^(CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|CLOCK\\$|COM[1-9]|LPT[1-9])(?:\\..*)?$"));
  if (!drive.match(portable).hasMatch() || portable.size() > 32760) return false;
  if (portable.size() == 3) return true;
  const auto parts = portable.mid(3).split(QLatin1Char('/'));
  for (const auto &part : parts) {
    if (part.isEmpty() || part.size() > 255 || part == QLatin1String(".") || part == QLatin1String("..") ||
        part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')) ||
        device.match(part.normalized(QString::NormalizationForm_KC).toUpper()).hasMatch()) return false;
    for (const auto character : part)
      if (character.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(character)) return false;
  }
#endif
  return true;
}

inline QString nativeApiPath(const QString &path) {
  if (!logicalLocalPath(path)) return {};
  const QString native = QDir::toNativeSeparators(QDir::cleanPath(path));
#ifdef Q_OS_WIN
  // Use W APIs with an internal extended-length spelling without changing the
  // installation identity or relying on the machine's LongPathsEnabled policy.
  return QStringLiteral("\\\\?\\") + native;
#else
  return native;
#endif
}
} // namespace gsw::updatepaths
