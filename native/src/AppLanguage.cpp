#include "AppLanguage.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTranslator>
#include <QVariant>

namespace gsw {
QStringList AppLanguage::supported() {
  return {QStringLiteral("zh_CN"), QStringLiteral("en_US"), QStringLiteral("ja_JP")};
}

QString AppLanguage::displayName(const QString &language) {
  // Autonyms must remain readable regardless of the active UI language.
  if (language == QStringLiteral("zh_CN")) return QString::fromUtf8("简体中文");
  if (language == QStringLiteral("ja_JP")) return QString::fromUtf8("日本語");
  return QStringLiteral("English");
}

QString AppLanguage::saved() {
  const QString value = QSettings().value(QStringLiteral("ui/language"),
                                         QStringLiteral("zh_CN")).toString();
  return supported().contains(value) ? value : QStringLiteral("zh_CN");
}

QString AppLanguage::current() {
  return qApp->property("gswUiLanguage").toString();
}

bool AppLanguage::initialize(const QString &overrideLanguage) {
  const QString language = overrideLanguage.isEmpty() ? saved() : overrideLanguage;
  if (!supported().contains(language)) return false;
  auto *applicationCatalog = new QTranslator(qApp);
  if (!applicationCatalog->load(QStringLiteral(":/i18n/workbench_%1.qm").arg(language))) {
    delete applicationCatalog;
    return false;
  }
  if (language != QStringLiteral("en_US")) {
    auto *qtCatalog = new QTranslator(qApp);
    const QString suffix = language == QStringLiteral("zh_CN")
                               ? QStringLiteral("zh_CN") : QStringLiteral("ja");
    if (!qtCatalog->load(QStringLiteral(":/i18n/qtbase_%1.qm").arg(suffix))) {
      delete qtCatalog;
      delete applicationCatalog;
      return false;
    }
    qApp->installTranslator(qtCatalog);
  }
  qApp->installTranslator(applicationCatalog);
  qApp->setProperty("gswUiLanguage", language);
  // Native Windows file dialogs follow the OS locale, not the selected app locale.
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  return true;
}

bool AppLanguage::save(const QString &language) {
  if (!supported().contains(language)) return false;
  QSettings settings;
  settings.setValue(QStringLiteral("ui/language"), language);
  settings.sync();
  return settings.status() == QSettings::NoError;
}
} // namespace gsw
