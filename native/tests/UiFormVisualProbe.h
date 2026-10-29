#pragma once

#include "AppLanguage.h"
#include "AppTheme.h"
#include "UiSurfaceEvidence.h"

#include <QApplication>
#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <QWindow>

#include <functional>

namespace gsw::tests {

inline bool uiFormsRequested(const int argc, char **argv) {
  for (int index = 1; index < argc; ++index)
    if (QByteArray(argv[index]) == QByteArrayLiteral("--smoke-test-ui-forms")) return true;
  return false;
}

inline bool configureUiFormsPlatform(const bool visual) {
  if (visual) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true")) return false;
    qputenv("QT_QPA_PLATFORM", "windows");
    return true;
#else
    return false;
#endif
  }
  qputenv("QT_QPA_PLATFORM", "offscreen");
  return true;
}

inline bool installUiFormsEvidence(QApplication &application, const bool visual,
                                  const QString &isolatedSettings) {
  if (!visual) return true;
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QApplication::platformName() != QStringLiteral("windows")) return false;
  application.setProperty("gswSmokeSettingsDirectory", isolatedSettings);
  const QString base = qEnvironmentVariable("GSW_UI_SURFACE_EVIDENCE");
  if (!QDir::isAbsolutePath(base) || !QFileInfo(base).isDir()) return false;
  const QString root = newUiSurfaceEvidenceRoot(base);
  if (!installUiSurfaceEvidence(root, qEnvironmentVariable("GSW_UI_SOURCE_COMMIT"))) return false;
  application.setProperty("gswUiFormEvidenceProcessRoot", root);
  return true;
}

inline void settleUiForm(const int milliseconds = 160) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

inline bool hasUiFormCapture(const QWidget &window, const QString &state,
                             const QString &locale, const UiTheme theme) {
  const QString root = qApp->property("gswUiFormEvidenceProcessRoot").toString();
  QFile file(QDir(root).filePath(QStringLiteral("ui-surfaces.json")));
  constexpr qint64 maximumReceiptBytes = 64LL * 1024 * 1024;
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > maximumReceiptBytes) return false;
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
  const QJsonObject receipt = document.object();
  if (receipt.value(QStringLiteral("sourceCommit")).toString().compare(
          qEnvironmentVariable("GSW_UI_SOURCE_COMMIT"), Qt::CaseInsensitive) != 0 ||
      receipt.value(QStringLiteral("platform")) != QStringLiteral("windows")) return false;
  const QString themeName = theme == UiTheme::Light ? QStringLiteral("light") : QStringLiteral("dark");
  for (const QJsonValue &entry : receipt.value(QStringLiteral("surfaces")).toArray()) {
    const QJsonObject record = entry.toObject();
    if (record.value(QStringLiteral("status")) != QStringLiteral("captured") ||
        record.value(QStringLiteral("class")) != QString::fromLatin1(window.metaObject()->className()) ||
        record.value(QStringLiteral("nativeWindowId")) != QString::number(quintptr(window.effectiveWinId()), 16) ||
        record.value(QStringLiteral("syntheticState")) != state ||
        record.value(QStringLiteral("objectName")) != window.objectName() ||
        record.value(QStringLiteral("locale")) != locale ||
        record.value(QStringLiteral("theme")) != themeName) continue;
    const QString image = record.value(QStringLiteral("image")).toString();
    return !image.isEmpty() && QFileInfo(QDir(root).filePath(image)).isFile() &&
        record.value(QStringLiteral("sha256")).toString().size() == 64 &&
        record.value(QStringLiteral("captureMethod")) ==
            QStringLiteral("QScreen::grabWindow(owned existing native client id)") &&
        record.value(QStringLiteral("dimensionsMatchWidgetDpr")).toBool() &&
        record.value(QStringLiteral("logicalSize")).toArray() ==
            QJsonArray{window.width(), window.height()} &&
        !record.value(QStringLiteral("visibleWidgetFontResolution")).toArray().isEmpty();
  }
  return false;
}

// The seam is an existing owned dialog and its public, immutable JSON view.
// This probe never accepts/rejects the dialog, edits data or starts a helper.
inline bool captureUiFormMatrix(const bool visual, QWidget &window, const QString &phase,
                                const QSize &standard, const QSize &narrow,
                                const std::function<QJsonObject()> &snapshot,
                                const std::function<bool()> &validateLayout = {}) {
  if (!visual) return true;
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QApplication::platformName() != QStringLiteral("windows") ||
      !qApp->findChild<QObject *>(QStringLiteral("gswUiSurfaceEvidenceAdapter"))) return false;
  const QJsonObject original = snapshot();
  const QString language = AppLanguage::current();
  const UiThemeMode themeMode = AppTheme::currentThemeMode();
  const int originalScale = qApp->property("gswUiScalePercent").isValid()
      ? qApp->property("gswUiScalePercent").toInt() : 100;
  const QRect geometry = window.geometry();
  const Qt::WindowStates state = window.windowState();
  const bool visible = window.isVisible();
  const QVariant evidenceState = window.property("gswUiEvidenceState");
  bool passed = true;
  for (const UiTheme theme : {UiTheme::Light, UiTheme::Dark}) {
    AppTheme::applyTheme(*qApp, theme, false);
    AppTheme::apply(*qApp, 100, false);
    for (const QString &locale : AppLanguage::supported()) {
      passed = AppLanguage::apply(locale, false) && passed;
      // Standalone form probes do not have MainWindow's language/font refresh
      // owner. Apply the same public font policy immediately in this adapter.
      AppTheme::apply(*qApp, 100, false);
      for (const bool isNarrow : {false, true}) {
        const QString synthetic = phase + (isNarrow ? QStringLiteral("-narrow") : QStringLiteral("-standard"));
        const QSize desired = isNarrow ? narrow : standard;
        QScreen *screen = window.screen() ? window.screen() : QGuiApplication::primaryScreen();
        if (!screen) { passed = false; continue; }
        const QRect available = screen->availableGeometry().adjusted(12, 32, -12, -32);
        const QSize target = desired.boundedTo(available.size()).expandedTo(window.minimumSize());
        window.setProperty("gswUiEvidenceState", synthetic);
        window.showNormal();
        window.resize(target);
        window.move(available.center() - QPoint(window.width() / 2, window.height() / 2));
        // This is explicit synthetic UI acceptance on a hosted Windows runner,
        // never an interactive desktop test or a production always-on-top flag.
        window.raise();
        window.activateWindow();
        if (window.windowHandle()) window.windowHandle()->requestActivate();
        bool captured = false;
        for (int attempt = 0; attempt < 3 && !captured; ++attempt) {
          settleUiForm();
          captured = hasUiFormCapture(window, synthetic, locale, theme);
        }
        const bool unchanged = snapshot() == original;
        const bool layoutUsable = !validateLayout || validateLayout();
        const bool native = window.isVisible() && window.windowHandle() &&
                            window.windowHandle()->isExposed() && window.size() == target;
        qInfo().noquote() << "UI_FORM_MATRIX" << phase << locale
                         << (theme == UiTheme::Light ? "light" : "dark")
                         << (isNarrow ? "narrow" : "standard")
                         << "native" << native << "captured" << captured
                         << "layoutUsable" << layoutUsable
                         << "publicSnapshotUnchanged" << unchanged;
        passed = captured && native && layoutUsable && unchanged && passed;
      }
    }
  }
  // Hide, not close: observer close disconnects, and initializer close cancels.
  if (!visible) window.hide();
  AppLanguage::apply(language, false);
  AppTheme::applyThemeMode(*qApp, themeMode, false);
  AppTheme::apply(*qApp, originalScale, false);
  window.setGeometry(geometry);
  window.setWindowState(state);
  window.setProperty("gswUiEvidenceState", evidenceState);
  if (visible) window.show();
  passed = snapshot() == original && window.isVisible() == visible && passed;
  return passed;
}

inline void finishUiFormsEvidence(QApplication &application, const bool visual) {
  if (!visual) return;
  QTimer::singleShot(0, &application, &QCoreApplication::quit);
  application.exec(); // aboutToQuit lets the existing adapter mark its receipt finished.
}

} // namespace gsw::tests
