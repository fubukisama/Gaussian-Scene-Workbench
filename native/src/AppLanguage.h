#pragma once

#include <QString>
#include <QStringList>

namespace gsw {
class AppLanguage {
public:
  static QStringList supported();
  static QString displayName(const QString &language);
  static QString saved();
  static QString current();
  static bool initialize(const QString &overrideLanguage = {});
  static bool save(const QString &language);
};
} // namespace gsw
