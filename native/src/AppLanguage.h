#pragma once

#include <QString>
#include <QStringList>
#include <QObject>
#include <functional>

class QTranslator;
class QComboBox;

namespace gsw {
class AppLanguage : public QObject {
  Q_OBJECT
public:
  static QStringList supported();
  static QString displayName(const QString &language);
  static QString saved();
  static QString current();
  static bool initialize(const QString &overrideLanguage = {});
  static bool save(const QString &language);
  static bool apply(const QString &language, bool persist = true);
  static AppLanguage *instance();
  static void onChanged(QObject *context, const std::function<void()> &callback);
  static void bind(QObject *object, const char *property, const char *source);
  static void bindComboItem(QComboBox *combo, int index, const char *source);
  static constexpr const char *source(const char *value) { return value; }
  template <typename T>
  static T *text(T *object, const char *source, const char *property = "text") {
    bind(object, property, source);
    return object;
  }

signals:
  void languageChanged();

private:
  explicit AppLanguage(QObject *parent) : QObject(parent) {}
  QTranslator *mApplicationCatalog = nullptr;
  QTranslator *mQtCatalog = nullptr;
};
} // namespace gsw
