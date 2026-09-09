#include "AppLanguage.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTranslator>
#include <QVariant>
#include <QPointer>
#include <QThread>
#include <QComboBox>
#include <QSignalBlocker>

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
  return qApp ? qApp->property("gswUiLanguage").toString() : QString();
}

bool AppLanguage::initialize(const QString &overrideLanguage) {
  const QString language = overrideLanguage.isEmpty() ? saved() : overrideLanguage;
  return apply(language, false);
}

AppLanguage *AppLanguage::instance() {
  static QPointer<AppLanguage> service;
  if (!service) service = new AppLanguage(qApp);
  return service;
}

void AppLanguage::onChanged(QObject *context, const std::function<void()> &callback) {
  QObject::connect(instance(), &AppLanguage::languageChanged, context, callback);
}

void AppLanguage::bind(QObject *object, const char *property, const char *source) {
  if (!object) return;
  const QByteArray name(property);
  const QByteArray key = QByteArrayLiteral("gswTranslation_") + name;
  const bool firstBinding = !object->property(key.constData()).isValid();
  object->setProperty(key.constData(), QByteArray(source));
  const auto refresh = [object, name, key]() {
    const QByteArray sourceText = object->property(key.constData()).toByteArray();
    object->setProperty(name.constData(), QCoreApplication::translate("Workbench", sourceText.constData()));
  };
  if (firstBinding) onChanged(object, refresh);
  refresh();
}

void AppLanguage::bindComboItem(QComboBox *combo, const int index, const char *source) {
  const QByteArray key(source);
  const auto refresh = [combo, index, key]() {
    const QSignalBlocker blocker(combo);
    combo->setItemText(index, QCoreApplication::translate("Workbench", key.constData()));
  };
  onChanged(combo, refresh);
  refresh();
}

bool AppLanguage::apply(const QString &language, const bool persist) {
  if (!qApp || QThread::currentThread() != qApp->thread()) return false;
  if (!supported().contains(language)) return false;
  auto *service = instance();
  if (language == current() && service->mApplicationCatalog)
    return !persist || save(language);
  auto *applicationCatalog = new QTranslator(service);
  if (!applicationCatalog->load(QStringLiteral(":/i18n/workbench_%1.qm").arg(language))) {
    delete applicationCatalog;
    return false;
  }
  QTranslator *qtCatalog = nullptr;
  if (language != QStringLiteral("en_US")) {
    qtCatalog = new QTranslator(service);
    const QString suffix = language == QStringLiteral("zh_CN")
                               ? QStringLiteral("zh_CN") : QStringLiteral("ja");
    if (!qtCatalog->load(QStringLiteral(":/i18n/qtbase_%1.qm").arg(suffix))) {
      delete qtCatalog;
      delete applicationCatalog;
      return false;
    }
  }
  // Load both catalogs and persist successfully before changing the live UI.
  if (persist && !save(language)) {
    delete qtCatalog;
    delete applicationCatalog;
    return false;
  }
  if (service->mApplicationCatalog) qApp->removeTranslator(service->mApplicationCatalog);
  if (service->mQtCatalog) qApp->removeTranslator(service->mQtCatalog);
  delete service->mApplicationCatalog;
  delete service->mQtCatalog;
  service->mApplicationCatalog = applicationCatalog;
  service->mQtCatalog = qtCatalog;
  if (qtCatalog) qApp->installTranslator(qtCatalog);
  qApp->installTranslator(applicationCatalog);
  qApp->setProperty("gswUiLanguage", language);
  // Native Windows file dialogs follow the OS locale, not the selected app locale.
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  emit service->languageChanged();
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
