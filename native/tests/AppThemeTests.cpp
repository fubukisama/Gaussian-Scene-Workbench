#include "AppTheme.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSize>
#include <QTemporaryDir>
#include <QVariant>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace gsw;

namespace {
double luminance(const QColor &color) {
  const auto channel = [](const double value) {
    return value <= 0.04045 ? value / 12.92
                            : std::pow((value + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
         0.0722 * channel(color.blueF());
}

double contrast(const QColor &first, const QColor &second) {
  const double a = luminance(first);
  const double b = luminance(second);
  return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}
} // namespace

class AppThemeTests final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void init();
  void defaultsToDarkAndRejectsUnknownThemeNames();
  void storesOnlyExplicitThemePreferences();
  void keepsLiveThemeDuringLanguageAndScaleChanges();
  void restoresSavedThemeOnInitialApplication();
  void providesReadablePalettesForBothThemes();
  void reusesOneScaledLayoutForBothThemes();
  void keepsAutomaticTextReadableAcrossCommonWindowSizes();
  void compensatesForOperatingSystemDisplayScale();
  void growsAutomaticScaleForHighResolutionWorkspaces();
  void fitsRequestedWindowResolutionInsideTheScreen();
  void keepsDockTitlesCompactAcrossScales();
  void rescalesDockExtentWhenDensityChanges();
  void clampsManualScaleToSupportedRange();

private:
  std::unique_ptr<QTemporaryDir> mSettingsRoot;
};

void AppThemeTests::initTestCase() {
  mSettingsRoot = std::make_unique<QTemporaryDir>(
      QDir::tempPath() + QStringLiteral("/gsw-app-theme-tests-XXXXXX"));
  QVERIFY(mSettingsRoot->isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     mSettingsRoot->path());
  QApplication::setOrganizationName(QStringLiteral("GSWThemeTests"));
  QApplication::setApplicationName(QStringLiteral("DisposableThemeSettings"));
}

void AppThemeTests::init() {
  QSettings().clear();
  qApp->setProperty("gswUiTheme", QVariant());
  qApp->setProperty("gswUiScalePercent", QVariant());
  qApp->setProperty("gswUiLanguage", QStringLiteral("en_US"));
  qApp->setStyleSheet(QString());
}

void AppThemeTests::defaultsToDarkAndRejectsUnknownThemeNames() {
  QCOMPARE(AppTheme::loadTheme(), UiTheme::Dark);
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Dark);
  for (const QString &name : {QString(), QStringLiteral("auto"),
                              QStringLiteral("LIGHT"), QStringLiteral("night")}) {
    QSettings().setValue(QStringLiteral("ui/theme"), name);
    QCOMPARE(AppTheme::loadTheme(), UiTheme::Dark);
  }
  QSettings().setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));
  QCOMPARE(AppTheme::loadTheme(), UiTheme::Light);
}

void AppThemeTests::storesOnlyExplicitThemePreferences() {
  QSettings().setValue(QStringLiteral("ui/manualScalePercent"), 110);
  QSettings().setValue(QStringLiteral("ui/scaleMode"), QStringLiteral("manual"));
  AppTheme::apply(*qApp, 110, false);
  AppTheme::applyTheme(*qApp, UiTheme::Light, false);
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Light);
  QVERIFY(!QSettings().contains(QStringLiteral("ui/theme")));
  QCOMPARE(qApp->property("gswUiScalePercent").toInt(), 110);
  QCOMPARE(QSettings().value(QStringLiteral("ui/manualScalePercent")).toInt(),
           110);
  QCOMPARE(AppTheme::loadScaleMode(), UiScaleMode::Manual);
  AppTheme::applyTheme(*qApp, UiTheme::Light, true);
  QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(),
           QStringLiteral("light"));
  QCOMPARE(AppTheme::loadTheme(), UiTheme::Light);
  AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Dark);
  QCOMPARE(AppTheme::loadTheme(), UiTheme::Light);
  AppTheme::applyTheme(*qApp, UiTheme::Dark, true);
  QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(),
           QStringLiteral("dark"));
}

void AppThemeTests::keepsLiveThemeDuringLanguageAndScaleChanges() {
  QLineEdit field;
  QPushButton button;
  button.setDisabled(true);
  AppTheme::applyTheme(*qApp, UiTheme::Light, false);
  for (const QString &language : {QStringLiteral("zh_CN"),
                                  QStringLiteral("en_US"),
                                  QStringLiteral("ja_JP")}) {
    qApp->setProperty("gswUiLanguage", language);
    for (const int scale : {90, 100, 150}) {
      AppTheme::apply(*qApp, scale, false);
      field.ensurePolished();
      button.ensurePolished();
      QCOMPARE(AppTheme::currentTheme(), UiTheme::Light);
      QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#f3f5f3"));
      QCOMPARE(qApp->font().pointSizeF(), 10.0 * scale / 100.0);
      QCOMPARE(qApp->property("gswUiScalePercent").toInt(), scale);
      if (language == QStringLiteral("ja_JP")) {
        QCOMPARE(qApp->font().families().first(), QStringLiteral("Yu Gothic UI"));
      } else if (language == QStringLiteral("en_US")) {
        QCOMPARE(qApp->font().families().first(), QStringLiteral("Segoe UI"));
      } else {
        QCOMPARE(qApp->font().families().first(),
                 QStringLiteral("Microsoft YaHei UI"));
      }
      QVERIFY(!qApp->styleSheet().contains(QLatin1Char('@')));
    }
  }
  const QFont previousFont = qApp->font();
  const QSize previousMinimum = field.minimumSize();
  AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
  QCOMPARE(qApp->font(), previousFont);
  QCOMPARE(field.minimumSize(), previousMinimum);
  QCOMPARE(qApp->property("gswUiScalePercent").toInt(), 150);
  QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#1b1d1f"));
  QVERIFY(!QSettings().contains(QStringLiteral("ui/theme")));
}

void AppThemeTests::restoresSavedThemeOnInitialApplication() {
  QSettings().setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));
  AppTheme::apply(*qApp, 90, false);
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Light);
  QCOMPARE(qApp->property("gswUiTheme").toString(), QStringLiteral("light"));
  qApp->setProperty("gswUiTheme", QVariant());
  QSettings().setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
  AppTheme::apply(*qApp, 90, false);
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Dark);
}

void AppThemeTests::providesReadablePalettesForBothThemes() {
  for (const UiTheme theme : {UiTheme::Dark, UiTheme::Light}) {
    const QPalette colors = AppTheme::palette(theme);
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
      QVERIFY(contrast(colors.color(group, QPalette::WindowText),
                       colors.color(group, QPalette::Window)) >= 7.0);
      QVERIFY(contrast(colors.color(group, QPalette::Text),
                       colors.color(group, QPalette::Base)) >= 7.0);
      QVERIFY(contrast(colors.color(group, QPalette::ButtonText),
                       colors.color(group, QPalette::Button)) >= 7.0);
      QVERIFY(contrast(colors.color(group, QPalette::HighlightedText),
                       colors.color(group, QPalette::Highlight)) >= 4.5);
      QVERIFY(contrast(colors.color(group, QPalette::ToolTipText),
                       colors.color(group, QPalette::ToolTipBase)) >= 7.0);
      QVERIFY(contrast(colors.color(group, QPalette::Link),
                       colors.color(group, QPalette::Base)) >= 4.5);
    }
    const auto disabled = QPalette::Disabled;
    QVERIFY(contrast(colors.color(disabled, QPalette::WindowText),
                     colors.color(disabled, QPalette::Window)) >= 3.0);
    QVERIFY(contrast(colors.color(disabled, QPalette::Text),
                     colors.color(disabled, QPalette::Base)) >= 3.0);
    QVERIFY(contrast(colors.color(disabled, QPalette::ButtonText),
                     colors.color(disabled, QPalette::Button)) >= 3.0);
    QVERIFY(contrast(colors.color(disabled, QPalette::HighlightedText),
                     colors.color(disabled, QPalette::Highlight)) >= 4.5);
  }
}

void AppThemeTests::reusesOneScaledLayoutForBothThemes() {
  for (const int scale : {90, 100, 150}) {
    AppTheme::apply(*qApp, scale, false);
    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QString darkGeometry = qApp->styleSheet();
    darkGeometry.remove(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}")));
    AppTheme::applyTheme(*qApp, UiTheme::Light, false);
    QString lightGeometry = qApp->styleSheet();
    lightGeometry.remove(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}")));
    QCOMPARE(lightGeometry, darkGeometry);
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QLabel#densityGuardWarning")));
  }
}

void AppThemeTests::keepsAutomaticTextReadableAcrossCommonWindowSizes() {
  QCOMPARE(AppTheme::recommendedScalePercent(QSize(1920, 1040),
                                             QSize(1573, 952)),
           100);
  QVERIFY(AppTheme::recommendedScalePercent(QSize(1366, 728),
                                            QSize(1280, 700)) >= 90);
}

void AppThemeTests::compensatesForOperatingSystemDisplayScale() {
  QCOMPARE(AppTheme::recommendedScalePercent(QSize(1707, 933),
                                             QSize(1570, 917), 1.5),
           90);
  QCOMPARE(AppTheme::recommendedScalePercent(QSize(2048, 1117),
                                             QSize(1900, 1000), 1.25),
           95);
}

void AppThemeTests::growsAutomaticScaleForHighResolutionWorkspaces() {
  QCOMPARE(AppTheme::recommendedScalePercent(QSize(2560, 1400),
                                             QSize(2400, 1280)),
           110);
  QCOMPARE(AppTheme::recommendedScalePercent(QSize(3840, 2120),
                                             QSize(3500, 1900)),
           125);
}

void AppThemeTests::fitsRequestedWindowResolutionInsideTheScreen() {
  const QSize fitted = AppTheme::fitWindowResolution(
      QSize(1920, 1080), QSize(1366, 728), QSize(940, 620));
  QVERIFY(fitted.width() <= 1311);
  QVERIFY(fitted.height() <= 699);
  QVERIFY(fitted.width() >= 940);
  QVERIFY(fitted.height() >= 620);
}

void AppThemeTests::keepsDockTitlesCompactAcrossScales() {
  QCOMPARE(AppTheme::dockTitleFontPixelSize(90), 14);
  QCOMPARE(AppTheme::dockTitleHeight(90), 19);
  QCOMPARE(AppTheme::dockTitleButtonSize(90), 14);

  QCOMPARE(AppTheme::dockTitleFontPixelSize(100), 15);
  QCOMPARE(AppTheme::dockTitleHeight(100), 21);
  QCOMPARE(AppTheme::dockTitleButtonSize(100), 15);

  QCOMPARE(AppTheme::dockTitleFontPixelSize(150), 23);
  QCOMPARE(AppTheme::dockTitleHeight(150), 32);
  QCOMPARE(AppTheme::dockTitleButtonSize(150), 23);
}

void AppThemeTests::rescalesDockExtentWhenDensityChanges() {
  QCOMPARE(AppTheme::rescaledDockExtent(350, 150, 90, 207), 210);
  QCOMPARE(AppTheme::rescaledDockExtent(150, 90, 150, 230), 250);
  QCOMPARE(AppTheme::rescaledDockExtent(120, 0, 100, 180), 180);
}

void AppThemeTests::clampsManualScaleToSupportedRange() {
  QCOMPARE(AppTheme::scaled(10, 60), 9);
  QCOMPARE(AppTheme::scaled(10, 180), 15);
}

QTEST_MAIN(AppThemeTests)

#include "AppThemeTests.moc"
