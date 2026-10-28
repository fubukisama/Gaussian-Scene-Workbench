#include "AppTheme.h"
#include "WrappingCheckBox.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFormLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMargins>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSignalSpy>
#include <QSize>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QtTest>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
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

QImage painted(QWidget &widget) {
  widget.ensurePolished();
  return widget.grab().toImage().convertToFormat(QImage::Format_ARGB32);
}

QColor darkestPixel(const QImage &image, const QRect &logicalBounds) {
  const qreal ratio = image.devicePixelRatio();
  const QRect bounds(qFloor(logicalBounds.x() * ratio),
                     qFloor(logicalBounds.y() * ratio),
                     qCeil(logicalBounds.width() * ratio),
                     qCeil(logicalBounds.height() * ratio));
  QColor darkest(Qt::white);
  double lowest = 1.0;
  const QRect clipped = bounds.intersected(image.rect());
  for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
    for (int x = clipped.left(); x <= clipped.right(); ++x) {
      const QColor color = image.pixelColor(x, y);
      const double value = luminance(color);
      if (value < lowest) {
        lowest = value;
        darkest = color;
      }
    }
  }
  return darkest;
}

QColor pixelAt(const QImage &image, const QPoint &logicalPoint) {
  const qreal ratio = image.devicePixelRatio();
  return image.pixelColor(std::clamp(qRound(logicalPoint.x() * ratio),
                                    0, image.width() - 1),
                          std::clamp(qRound(logicalPoint.y() * ratio),
                                     0, image.height() - 1));
}

bool neutral(const QColor &color) {
  return color.red() == color.green() && color.green() == color.blue();
}

// Independent visual specification, not values read back from AppTheme.
struct VisualRoleExpectation {
  UiTheme theme;
  const char *action;
  const char *tint;
  const char *pressed;
  const char *ink;
  const char *focus;
  const char *hover;
  const char *progress;
  const char *disabledSurface;
  const char *disabledText;
  const char *window;
  const char *panel;
  const char *text;
  const char *field;
};
constexpr VisualRoleExpectation visualRoles[] = {
    {UiTheme::Dark, "#2d6bb2", "#243c56", "#2b4866", "#8cbcf0", "#78aef1",
     "#303438", "#2d6bb2", "#242629", "#929a9f", "#17191b", "#1b1d1f",
     "#e7e9ea", "#151719"},
    {UiTheme::Light, "#2467a5", "#e4effb", "#d5e5f7", "#135f9b", "#135f9b",
     "#e1e3e5", "#aecbe8", "#ededed", "#6b6b6b", "#f3f3f3", "#ffffff",
     "#171717", "#ffffff"},
};

int pixelsNear(const QImage &image, const QRect &logicalBounds,
               const QColor &expected, const int tolerance = 3) {
  const qreal ratio = image.devicePixelRatio();
  const QRect physical(qFloor(logicalBounds.x() * ratio),
                       qFloor(logicalBounds.y() * ratio),
                       qCeil(logicalBounds.width() * ratio),
                       qCeil(logicalBounds.height() * ratio));
  const QRect bounds = physical.intersected(image.rect());
  int result = 0;
  for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
    for (int x = bounds.left(); x <= bounds.right(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (pixel.alpha() >= 250 &&
          std::abs(pixel.red() - expected.red()) <= tolerance &&
          std::abs(pixel.green() - expected.green()) <= tolerance &&
          std::abs(pixel.blue() - expected.blue()) <= tolerance)
        ++result;
    }
  }
  return result;
}
} // namespace

class AppThemeTests final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void init();
  void defaultsToAutomaticAndRetainsManualPreferences();
  void resolvesTimeBoundariesAndSchedulesPromptChecks();
  void switchesAutomaticallyWithoutOverwritingPolicyOrInputs();
  void manualOverridesIgnoreAutomaticClockChecks();
  void storesOnlyExplicitThemePreferences();
  void keepsLiveThemeDuringLanguageAndScaleChanges();
  void restoresSavedThemeOnInitialApplication();
  void providesReadablePalettesForBothThemes();
  void usesNeutralLightSurfacesAndBlueInteractionColors();
  void paintsReadableLightControlsAndDistinctPanelStructure();
  void paintsVisibleLeftAndRightDockSeparators();
  void paintsUnifiedResizableSplitterHandles();
  void paintsHeaderRemainderAndContinuousDialogSurfaces();
  void paintsMatchingScrollableDialogBodies();
  void keepsWrappingCheckBoxResponsiveAndInteractive();
  void reusesOneScaledLayoutForBothThemes();
  void keepsAutomaticTextReadableAcrossCommonWindowSizes();
  void compensatesForOperatingSystemDisplayScale();
  void growsAutomaticScaleForHighResolutionWorkspaces();
  void fitsRequestedWindowResolutionInsideTheScreen();
  void boundsEffectiveWindowMinimumByAvailableClientArea_data();
  void boundsEffectiveWindowMinimumByAvailableClientArea();
  void keepsDockTitlesCompactAcrossScales();
  void rescalesDockExtentWhenDensityChanges();
  void clampsManualScaleToSupportedRange();
  void freezesSharedActionRolesAndPaletteSemantics();
  void paintsSharedRolesAndTransparentLayoutLabels();
  void keepsKeyboardFocusAndInteractionGeometryStable();

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
  qApp->setProperty("gswUiThemeMode", QVariant());
  qApp->setProperty("gswUiScalePercent", QVariant());
  qApp->setProperty("gswUiLanguage", QStringLiteral("en_US"));
  qApp->setStyleSheet(QString());
}

void AppThemeTests::defaultsToAutomaticAndRetainsManualPreferences() {
  QCOMPARE(AppTheme::loadThemeMode(), UiThemeMode::Automatic);
  QCOMPARE(AppTheme::currentThemeMode(), UiThemeMode::Automatic);
  QCOMPARE(AppTheme::loadTheme(QTime(12, 0)), UiTheme::Light);
  QCOMPARE(AppTheme::loadTheme(QTime(20, 0)), UiTheme::Dark);
  QCOMPARE(AppTheme::loadTheme(QTime(1, 12)), UiTheme::Dark);
  for (const QString &name : {QString(), QStringLiteral("auto"),
                              QStringLiteral("LIGHT"), QStringLiteral("night")}) {
    QSettings().setValue(QStringLiteral("ui/theme"), name);
    QCOMPARE(AppTheme::loadThemeMode(), UiThemeMode::Automatic);
    QCOMPARE(AppTheme::loadTheme(QTime(12, 0)), UiTheme::Light);
    QCOMPARE(AppTheme::loadTheme(QTime(20, 0)), UiTheme::Dark);
    QCOMPARE(AppTheme::loadTheme(QTime(1, 12)), UiTheme::Dark);
  }
  QSettings().setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));
  QCOMPARE(AppTheme::loadThemeMode(), UiThemeMode::Light);
  QCOMPARE(AppTheme::loadTheme(QTime(20, 0)), UiTheme::Light);
  QSettings().setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
  QCOMPARE(AppTheme::loadThemeMode(), UiThemeMode::Dark);
  QCOMPARE(AppTheme::loadTheme(QTime(12, 0)), UiTheme::Dark);
}

void AppThemeTests::resolvesTimeBoundariesAndSchedulesPromptChecks() {
  QCOMPARE(AppTheme::themeForTime(QTime(0, 0)), UiTheme::Dark);
  QCOMPARE(AppTheme::themeForTime(QTime(5, 59, 59, 999)), UiTheme::Dark);
  QCOMPARE(AppTheme::themeForTime(QTime(6, 0)), UiTheme::Light);
  QCOMPARE(AppTheme::themeForTime(QTime(17, 59, 59, 999)), UiTheme::Light);
  QCOMPARE(AppTheme::themeForTime(QTime(18, 0)), UiTheme::Dark);
  QCOMPARE(AppTheme::themeForTime(QTime(23, 59, 59, 999)), UiTheme::Dark);
  QCOMPARE(AppTheme::themeForTime(QTime()), UiTheme::Dark);
  for (int minute = 0; minute < 24 * 60; ++minute) {
    const QTime time(minute / 60, minute % 60);
    QCOMPARE(AppTheme::themeForTime(time),
             minute >= 6 * 60 && minute < 18 * 60 ? UiTheme::Light : UiTheme::Dark);
  }
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime(0, 0)), 30000);
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime(5, 59, 40)), 20000);
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime(5, 59, 59, 999)), 1);
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime(6, 0)), 30000);
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime(17, 59, 40)), 20000);
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime(17, 59, 59, 999)), 1);
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime(18, 0)), 30000);
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime(23, 59, 59, 999)), 30000);
  QCOMPARE(AppTheme::automaticThemeCheckInterval(QTime()), 30000);
}

void AppThemeTests::switchesAutomaticallyWithoutOverwritingPolicyOrInputs() {
  AppTheme::apply(*qApp, 110, false);
  QLineEdit field(QStringLiteral("unchanged input"));
  field.setSelection(0, 9);
  AppTheme::applyThemeMode(*qApp, UiThemeMode::Automatic, true, QTime(17, 59));
  const QFont font = qApp->font();
  const int scale = qApp->property("gswUiScalePercent").toInt();
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Light);
  QCOMPARE(AppTheme::currentThemeMode(), UiThemeMode::Automatic);
  QCOMPARE(AppTheme::loadThemeMode(), UiThemeMode::Automatic);
  QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(17, 59, 59, 999)));
  QVERIFY(AppTheme::refreshAutomaticTheme(*qApp, QTime(18, 0)));
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Dark);
  QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#1b1d1f"));
  QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(), QStringLiteral("auto"));
  QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(23, 59)));
  QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(0, 0)));
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Dark);
  QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#1b1d1f"));
  QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(1, 12)));
  QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(5, 59, 59, 999)));
  QVERIFY(AppTheme::refreshAutomaticTheme(*qApp, QTime(6, 0)));
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Light);
  QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#ffffff"));
  // A backwards clock edit is handled identically to an ordinary boundary.
  QVERIFY(AppTheme::refreshAutomaticTheme(*qApp, QTime(19, 0)));
  QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(1, 12)));
  QVERIFY(AppTheme::refreshAutomaticTheme(*qApp, QTime(12, 0)));
  QVERIFY(AppTheme::refreshAutomaticTheme(*qApp, QTime(5, 59)));
  QVERIFY(AppTheme::refreshAutomaticTheme(*qApp, QTime(6, 0)));
  QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime()));
  for (const QString &language : {QStringLiteral("zh_CN"), QStringLiteral("en_US"), QStringLiteral("ja_JP")}) {
    qApp->setProperty("gswUiLanguage", language);
    AppTheme::apply(*qApp, 110, false);
    QCOMPARE(AppTheme::currentThemeMode(), UiThemeMode::Automatic);
    QCOMPARE(AppTheme::currentTheme(), UiTheme::Light);
    QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(), QStringLiteral("auto"));
  }
  qApp->setProperty("gswUiLanguage", QStringLiteral("en_US"));
  AppTheme::apply(*qApp, 110, false);
  QCOMPARE(qApp->font(), font);
  QCOMPARE(qApp->property("gswUiScalePercent").toInt(), scale);
  QCOMPARE(field.text(), QStringLiteral("unchanged input"));
  QCOMPARE(field.selectedText(), QStringLiteral("unchanged"));
}

void AppThemeTests::manualOverridesIgnoreAutomaticClockChecks() {
  AppTheme::applyThemeMode(*qApp, UiThemeMode::Automatic, true, QTime(12, 0));
  for (const UiTheme theme : {UiTheme::Light, UiTheme::Dark}) {
    AppTheme::applyTheme(*qApp, theme, false);
    QCOMPARE(AppTheme::currentThemeMode(), theme == UiTheme::Light ? UiThemeMode::Light : UiThemeMode::Dark);
    QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(12, 0)));
    QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(20, 0)));
    QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(0, 0)));
    QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(5, 59, 59, 999)));
    QVERIFY(!AppTheme::refreshAutomaticTheme(*qApp, QTime(6, 0)));
    QCOMPARE(AppTheme::currentTheme(), theme);
    QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(), QStringLiteral("auto"));
  }
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
      QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#ffffff"));
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
  qApp->setProperty("gswUiThemeMode", QVariant());
  QSettings().setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
  AppTheme::apply(*qApp, 90, false);
  QCOMPARE(AppTheme::currentTheme(), UiTheme::Dark);
  QCOMPARE(AppTheme::currentThemeMode(), UiThemeMode::Dark);
  qApp->setProperty("gswUiTheme", QVariant());
  qApp->setProperty("gswUiThemeMode", QVariant());
  QSettings().setValue(QStringLiteral("ui/theme"), QStringLiteral("auto"));
  AppTheme::apply(*qApp, 90, false);
  QCOMPARE(AppTheme::currentThemeMode(), UiThemeMode::Automatic);
  QCOMPARE(AppTheme::currentTheme(), AppTheme::themeForTime(QTime::currentTime()));
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
      if (theme == UiTheme::Light) {
        QVERIFY(contrast(colors.color(group, QPalette::PlaceholderText),
                         colors.color(group, QPalette::Base)) >= 7.0);
      }
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

void AppThemeTests::usesNeutralLightSurfacesAndBlueInteractionColors() {
  const QPalette colors = AppTheme::palette(UiTheme::Light);
  for (const auto role : {QPalette::Window, QPalette::Base, QPalette::AlternateBase,
                          QPalette::Button, QPalette::WindowText, QPalette::Text,
                          QPalette::ButtonText, QPalette::PlaceholderText,
                          QPalette::Midlight, QPalette::Mid, QPalette::Dark}) {
    QVERIFY(neutral(colors.color(QPalette::Active, role)));
  }
  QCOMPARE(colors.color(QPalette::Window), QColor("#ffffff"));
  QCOMPARE(colors.color(QPalette::WindowText), QColor("#171717"));
  QCOMPARE(colors.color(QPalette::PlaceholderText), QColor("#444444"));
  QCOMPARE(colors.color(QPalette::Highlight), QColor("#2467a5"));
  QCOMPARE(colors.color(QPalette::Link), QColor("#135f9b"));
}

void AppThemeTests::paintsReadableLightControlsAndDistinctPanelStructure() {
  for (const QString &language : {QStringLiteral("zh_CN"),
                                  QStringLiteral("en_US"),
                                  QStringLiteral("ja_JP")}) {
    qApp->setProperty("gswUiLanguage", language);
    for (const int scale : {90, 100, 150}) {
      AppTheme::apply(*qApp, scale, false);
      AppTheme::applyTheme(*qApp, UiTheme::Light, false);
      QWidget panel;
      panel.resize(520, 360);
      QLabel active(QStringLiteral("MMMM 0123456789"), &panel);
      active.setGeometry(16, 10, 340, 44);
      QLabel muted(QStringLiteral("MMMM 0123456789"), &panel);
      muted.setObjectName(QStringLiteral("mutedLabel"));
      muted.setGeometry(16, 54, 340, 44);
      QWidget dockHeader(&panel);
      dockHeader.setObjectName(QStringLiteral("dockTitleBar"));
      dockHeader.setGeometry(16, 102, 360, 44);
      QLabel dockTitle(QStringLiteral("MMMM 0123456789"), &dockHeader);
      dockTitle.setObjectName(QStringLiteral("dockTitleLabel"));
      dockTitle.setGeometry(8, 0, 330, 43);
      QTableWidget table(1, 1, &panel);
      table.setHorizontalHeaderLabels({QStringLiteral("MMMM 0123456789")});
      table.verticalHeader()->hide();
      table.horizontalHeader()->setStretchLastSection(true);
      table.setGeometry(16, 150, 360, 70);
      QLineEdit field(QStringLiteral("MMMM 0123456789"), &panel);
      field.setFocusPolicy(Qt::NoFocus);
      field.setGeometry(16, 226, 360, 54);
      QPushButton button(QStringLiteral("MMMM"), &panel);
      button.setFocusPolicy(Qt::NoFocus);
      button.setAutoDefault(false);
      button.setGeometry(16, 292, 180, 54);
      QMenuBar menuBar(&panel);
      menuBar.setGeometry(390, 10, 110, 44);
      QToolBar toolbar(&panel);
      toolbar.setGeometry(390, 64, 110, 44);
      panel.show();
      QApplication::processEvents();
      QTest::qWait(20);
      // The user's stationary cursor may happen to be over this disposable
      // window. Sample the normal controls, not an incidental hover state.
      button.setAttribute(Qt::WA_UnderMouse, false);
      field.setAttribute(Qt::WA_UnderMouse, false);

      // Inspect the actual stylesheet-painted widgets. Palette-only checks
      // would miss a muted/header rule that still uses the former washed-out
      // green colours, or a native control whose boundary disappears.
      // Labels are transparent. Inspect their composed parent so their text
      // contrast and background match what is actually visible to the user.
      const QImage panelImage = painted(panel);
      const QImage dockImage = painted(dockHeader);
      const QImage tableHeaderImage = painted(*table.horizontalHeader());
      const QImage fieldImage = painted(field);
      const QImage buttonImage = painted(button);
      const QColor white = pixelAt(panelImage, active.pos() + QPoint(330, 8));
      const QColor header = pixelAt(dockImage, QPoint(350, 8));
      const QColor tableHeader = pixelAt(tableHeaderImage,
          QPoint(table.horizontalHeader()->width() - 12, 6));
      QCOMPARE(white, QColor("#ffffff"));
      QCOMPARE(header, QColor("#dedede"));
      QCOMPARE(tableHeader, QColor("#dedede"));
      QVERIFY(white.lightness() - header.lightness() >= 32);
      QCOMPARE(pixelAt(painted(menuBar), QPoint(80, 8)), QColor("#e7e7e7"));
      QCOMPARE(pixelAt(painted(toolbar), QPoint(80, 8)), QColor("#e7e7e7"));
      QVERIFY(contrast(darkestPixel(panelImage, active.geometry()), white) >= 7.0);
      QVERIFY(contrast(darkestPixel(panelImage, muted.geometry()), white) >= 7.0);
      QVERIFY(contrast(darkestPixel(dockImage, QRect(8, 2, 300, 38)), header) >= 7.0);
      QVERIFY(contrast(darkestPixel(tableHeaderImage,
          QRect(8, 3, 280, table.horizontalHeader()->height() - 8)), tableHeader) >= 7.0);
      QVERIFY(contrast(darkestPixel(fieldImage, QRect(12, 6, 300, 42)), white) >= 7.0);
      const QColor buttonBackground = pixelAt(buttonImage, QPoint(8, 12));
      QCOMPARE(buttonBackground, QColor("#f3f3f3"));
      QVERIFY(contrast(darkestPixel(buttonImage, QRect(12, 6, 150, 42)),
                       buttonBackground) >= 7.0);
      const QColor fieldBorder = darkestPixel(fieldImage, QRect(0, 24, 2, 6));
      const QColor buttonBorder = darkestPixel(buttonImage, QRect(0, 24, 2, 6));
      QVERIFY(neutral(fieldBorder) && neutral(buttonBorder));
      QVERIFY(contrast(fieldBorder, white) >= 3.0);
      QVERIFY(contrast(buttonBorder, buttonBackground) >= 3.0);
      const QColor dockBoundary = darkestPixel(dockImage,
          QRect(320, dockHeader.height() - 2, 20, 2));
      QVERIFY(neutral(dockBoundary));
      QVERIFY(header.lightness() - dockBoundary.lightness() >= 50);
      panel.hide();
    }
  }
}

void AppThemeTests::reusesOneScaledLayoutForBothThemes() {
  const QRegularExpression dayPresentation(
      QStringLiteral("(?:QMainWindow::separator[^\\{]*|"
                     "QSplitter::handle(?=\\s*\\{\\s*background: #dedede;))"
                     "\\s*\\{[^\\}]*\\}\\s*"));
  for (const int scale : {90, 100, 150}) {
    AppTheme::apply(*qApp, scale, false);
    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QString darkGeometry = qApp->styleSheet();
    darkGeometry.remove(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}")));
    AppTheme::applyTheme(*qApp, UiTheme::Light, false);
    QString lightGeometry = qApp->styleSheet();
    // Only the light-only resize handles are exceptions. Header remainder,
    // dialog surfaces and transparent labels now share both themes' rules.
    lightGeometry.remove(dayPresentation);
    lightGeometry.remove(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}")));
    QCOMPARE(lightGeometry.trimmed(), darkGeometry.trimmed());
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QLabel#densityGuardWarning")));
  }
}

void AppThemeTests::paintsUnifiedResizableSplitterHandles() {
  for (const int scale : {90, 100, 150}) {
    for (const Qt::Orientation orientation : {Qt::Horizontal, Qt::Vertical}) {
      AppTheme::apply(*qApp, scale, false);
      AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
      QSplitter splitter(orientation);
      auto *first = new QWidget(&splitter);
      auto *second = new QWidget(&splitter);
      splitter.addWidget(first);
      splitter.addWidget(second);
      splitter.setChildrenCollapsible(false);
      splitter.resize(orientation == Qt::Horizontal ? QSize(740, 360)
                                                    : QSize(360, 740));
      splitter.show();
      QApplication::processEvents();
      QTest::qWait(20);
      auto *handle = splitter.handle(1);
      QVERIFY(handle != nullptr);
      const auto extent = [&] {
        return orientation == Qt::Horizontal ? handle->width() : handle->height();
      };
      const int darkExtent = extent();
      QCOMPARE(darkExtent, splitter.style()->pixelMetric(
          QStyle::PM_SplitterWidth, nullptr, &splitter));
      handle->setAttribute(Qt::WA_UnderMouse, false);
      const QColor darkBody = pixelAt(painted(*handle), handle->rect().center());
      QCOMPARE(darkBody, QColor("#34383c"));

      AppTheme::applyTheme(*qApp, UiTheme::Light, false);
      QApplication::processEvents();
      handle->setAttribute(Qt::WA_UnderMouse, false);
      QCOMPARE(extent(), AppTheme::scaled(8, scale));
      const QImage dayHandle = painted(*handle);
      const QPoint edge = orientation == Qt::Horizontal
          ? QPoint(0, handle->height() / 2) : QPoint(handle->width() / 2, 0);
      QCOMPARE(pixelAt(dayHandle, handle->rect().center()), QColor("#dedede"));
      QCOMPARE(pixelAt(dayHandle, edge), QColor("#c8c8c8"));

      // Real handle events must still resize panes; wider grey strips are
      // interactive splitters, not decorative borders or viewport overlays.
      const int before = orientation == Qt::Horizontal ? first->width() : first->height();
      const QPoint start = handle->mapToGlobal(handle->rect().center());
      const QPoint end = start + (orientation == Qt::Horizontal ? QPoint(30, 0)
                                                                 : QPoint(0, 30));
      const auto mouse = [&](const QEvent::Type type, const QPoint &global,
                              const Qt::MouseButton button,
                              const Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(handle->mapFromGlobal(global)),
                          QPointF(global), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(handle, &event);
      };
      mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
      mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
      mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
      QApplication::processEvents();
      QVERIFY((orientation == Qt::Horizontal ? first->width() : first->height()) > before);
      QCOMPARE(splitter.count(), 2);
      QCOMPARE(first->parentWidget(), &splitter);
      QCOMPARE(second->parentWidget(), &splitter);

      AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
      QApplication::processEvents();
      handle->setAttribute(Qt::WA_UnderMouse, false);
      QCOMPARE(extent(), darkExtent);
      QCOMPARE(pixelAt(painted(*handle), handle->rect().center()), darkBody);
      splitter.hide();
    }
  }
}

void AppThemeTests::paintsHeaderRemainderAndContinuousDialogSurfaces() {
  for (const int scale : {90, 100, 150}) {
    AppTheme::apply(*qApp, scale, false);
    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QDialog dialog;
    dialog.resize(640, 410);
    QLabel label(QStringLiteral("Label"), &dialog);
    label.setGeometry(18, 18, 380, 40);
    QCheckBox check(QStringLiteral("Check"), &dialog);
    check.setGeometry(18, 66, 380, 40);
    check.setChecked(true);
    QRadioButton radio(QStringLiteral("Radio"), &dialog);
    radio.setGeometry(18, 114, 380, 40);
    radio.setChecked(true);
    QLineEdit field(QStringLiteral("unmodified"), &dialog);
    field.setFocusPolicy(Qt::NoFocus);
    field.setGeometry(18, 166, 380, 44);
    QTableWidget table(1, 4, &dialog);
    table.setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Size"),
                                      QStringLiteral("Type"), QStringLiteral("Date")});
    table.setGeometry(18, 228, 600, 160);
    table.verticalHeader()->hide();
    auto *header = table.horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Fixed);
    for (int column = 0; column < 4; ++column) header->resizeSection(column, 75);
    dialog.show();
    QApplication::processEvents();
    QTest::qWait(20);
    const QPoint freeSpace(580, 30);
    const QPoint emptyLabel(370, 10);
    const QPoint emptyHeader(header->viewport()->width() - 8,
                             header->viewport()->height() / 2);
    QVERIFY(emptyHeader.x() > header->length());
    const QColor darkDialog("#17191b");
    const QImage darkFrame = painted(dialog);
    QCOMPARE(pixelAt(painted(*header->viewport()), emptyHeader), QColor("#24272a"));
    QCOMPARE(pixelAt(darkFrame, freeSpace), darkDialog);
    for (QWidget *control : {static_cast<QWidget *>(&label),
                             static_cast<QWidget *>(&check),
                             static_cast<QWidget *>(&radio)}) {
      QCOMPARE(pixelAt(darkFrame, control->pos() + emptyLabel), darkDialog);
    }
    QCOMPARE(pixelAt(painted(field), QPoint(370, 10)), QColor("#151719"));
    QCOMPARE(pixelAt(painted(*table.viewport()), QPoint(580, 80)), QColor("#191b1d"));
    QVERIFY(check.isChecked());
    QVERIFY(radio.isChecked());

    AppTheme::applyTheme(*qApp, UiTheme::Light, false);
    QApplication::processEvents();
    QCOMPARE(pixelAt(painted(*header->viewport()), emptyHeader), QColor("#dedede"));
    const QColor dayDialog = pixelAt(painted(dialog), freeSpace);
    QCOMPARE(dayDialog, QColor("#f3f3f3"));
    const QImage dayFrame = painted(dialog);
    for (QWidget *control : {static_cast<QWidget *>(&label),
                             static_cast<QWidget *>(&check),
                             static_cast<QWidget *>(&radio)}) {
      // Transparent children only acquire their parent background in the
      // composed dialog; grabbing the child alone can legitimately yield alpha 0.
      QCOMPARE(pixelAt(dayFrame, control->pos() + emptyLabel), dayDialog);
    }
    QCOMPARE(pixelAt(painted(field), QPoint(370, 10)), QColor("#ffffff"));
    QCOMPARE(pixelAt(painted(*table.viewport()), QPoint(580, 80)), QColor("#ffffff"));
    QCOMPARE(field.text(), QStringLiteral("unmodified"));
    QCOMPARE(header->length(), 300);
    QVERIFY(check.isChecked());
    QVERIFY(radio.isChecked());

    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QApplication::processEvents();
    QCOMPARE(pixelAt(painted(*header->viewport()), emptyHeader), QColor("#24272a"));
    const QImage restoredDarkFrame = painted(dialog);
    QCOMPARE(pixelAt(restoredDarkFrame, freeSpace), darkDialog);
    for (QWidget *control : {static_cast<QWidget *>(&label),
                             static_cast<QWidget *>(&check),
                             static_cast<QWidget *>(&radio)}) {
      QCOMPARE(pixelAt(restoredDarkFrame, control->pos() + emptyLabel), darkDialog);
    }
    QCOMPARE(pixelAt(painted(field), QPoint(370, 10)), QColor("#151719"));
    QCOMPARE(pixelAt(painted(*table.viewport()), QPoint(580, 80)), QColor("#191b1d"));
    QCOMPARE(field.text(), QStringLiteral("unmodified"));
    QVERIFY(check.isChecked());
    QVERIFY(radio.isChecked());
    dialog.hide();
  }
}

void AppThemeTests::paintsMatchingScrollableDialogBodies() {
  for (const int scale : {90, 100, 150}) {
    AppTheme::apply(*qApp, scale, false);
    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QDialog dialog;
    dialog.resize(500, 460);
    QScrollArea scroll(&dialog);
    scroll.setObjectName(QStringLiteral("dialogBodyScroll"));
    scroll.setFrameShape(QFrame::NoFrame);
    scroll.setGeometry(18, 18, 464, 360);
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("dialogBody"));
    body->resize(400, 300);
    scroll.setWidget(body);
    QLabel label(QStringLiteral("Label"), body);
    label.setGeometry(14, 14, 360, 44);
    QLineEdit field(QStringLiteral("unchanged"), body);
    field.setFocusPolicy(Qt::NoFocus);
    field.setGeometry(14, 74, 360, 44);
    QCheckBox check(QStringLiteral("Check"), body);
    check.setGeometry(14, 134, 360, 44);
    check.setChecked(true);
    // Pure layout wrappers must not introduce a white rectangle into the
    // grey dialog. Their fields must still use the explicit white field rule.
    QWidget wrapper(body);
    wrapper.setProperty("gswLayoutContainer", true);
    wrapper.setGeometry(14, 194, 360, 88);
    QRadioButton radio(QStringLiteral("Radio"), &wrapper);
    radio.setGeometry(0, 0, 360, 40);
    radio.setChecked(true);
    QLineEdit nestedField(QStringLiteral("nested unchanged"), &wrapper);
    nestedField.setFocusPolicy(Qt::NoFocus);
    nestedField.setGeometry(0, 44, 360, 44);
    QDialogButtonBox footer(QDialogButtonBox::Cancel, &dialog);
    footer.setGeometry(18, 394, 464, 44);
    auto *cancel = footer.button(QDialogButtonBox::Cancel);
    QVERIFY(cancel != nullptr);
    cancel->setFocusPolicy(Qt::NoFocus);
    cancel->setAutoDefault(false);
    dialog.show();
    QApplication::processEvents();
    QTest::qWait(20);
    const QPoint bodyBlank(390, 290);
    const QPoint viewportBlank(450, 300);
    const QPoint wrapperBlank(350, 42);
    const QColor darkSurface("#17191b");
    const QImage darkFrame = painted(dialog);
    QCOMPARE(pixelAt(darkFrame, QPoint(490, 12)), darkSurface);
    QCOMPARE(pixelAt(darkFrame, body->mapTo(&dialog, bodyBlank)), darkSurface);
    QCOMPARE(pixelAt(darkFrame, scroll.viewport()->mapTo(&dialog, viewportBlank)), darkSurface);
    QCOMPARE(pixelAt(darkFrame, wrapper.mapTo(&dialog, wrapperBlank)), darkSurface);
    QCOMPARE(pixelAt(darkFrame, footer.pos() + QPoint(8, 10)), darkSurface);
    for (QWidget *control : {static_cast<QWidget *>(&label),
                             static_cast<QWidget *>(&check),
                             static_cast<QWidget *>(&radio)}) {
      QCOMPARE(pixelAt(darkFrame, control->mapTo(&dialog, QPoint(350, 10))), darkSurface);
    }
    QCOMPARE(pixelAt(painted(field), QPoint(350, 10)), QColor("#151719"));
    QCOMPARE(pixelAt(painted(nestedField), QPoint(350, 10)), QColor("#151719"));
    QCOMPARE(field.text(), QStringLiteral("unchanged"));
    QCOMPARE(nestedField.text(), QStringLiteral("nested unchanged"));
    QVERIFY(check.isChecked());
    QVERIFY(radio.isChecked());

    AppTheme::applyTheme(*qApp, UiTheme::Light, false);
    QApplication::processEvents();
    const QColor daySurface("#f3f3f3");
    QCOMPARE(pixelAt(painted(dialog), QPoint(490, 12)), daySurface);
    QCOMPARE(pixelAt(painted(*body), bodyBlank), daySurface);
    QCOMPARE(pixelAt(painted(*scroll.viewport()), viewportBlank), daySurface);
    const QImage frame = painted(dialog);
    QCOMPARE(pixelAt(frame, wrapper.mapTo(&dialog, wrapperBlank)), daySurface);
    QCOMPARE(pixelAt(frame, footer.pos() + QPoint(8, 10)), daySurface);
    for (QWidget *control : {static_cast<QWidget *>(&label),
                             static_cast<QWidget *>(&check),
                             static_cast<QWidget *>(&radio)}) {
      QCOMPARE(pixelAt(frame, control->mapTo(&dialog, QPoint(350, 10))), daySurface);
    }
    QCOMPARE(pixelAt(painted(field), QPoint(350, 10)), QColor("#ffffff"));
    QCOMPARE(pixelAt(painted(nestedField), QPoint(350, 10)), QColor("#ffffff"));
    cancel->setAttribute(Qt::WA_UnderMouse, false);
    QCOMPARE(pixelAt(painted(*cancel), QPoint(8, 12)), QColor("#f3f3f3"));
    QCOMPARE(field.text(), QStringLiteral("unchanged"));
    QCOMPARE(nestedField.text(), QStringLiteral("nested unchanged"));
    QVERIFY(check.isChecked());
    QVERIFY(radio.isChecked());

    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QApplication::processEvents();
    const QImage restoredDarkFrame = painted(dialog);
    QCOMPARE(pixelAt(restoredDarkFrame, QPoint(490, 12)), darkSurface);
    QCOMPARE(pixelAt(restoredDarkFrame, body->mapTo(&dialog, bodyBlank)), darkSurface);
    QCOMPARE(pixelAt(restoredDarkFrame, scroll.viewport()->mapTo(&dialog, viewportBlank)), darkSurface);
    QCOMPARE(pixelAt(restoredDarkFrame, wrapper.mapTo(&dialog, wrapperBlank)), darkSurface);
    QCOMPARE(pixelAt(restoredDarkFrame, footer.pos() + QPoint(8, 10)), darkSurface);
    for (QWidget *control : {static_cast<QWidget *>(&label),
                             static_cast<QWidget *>(&check),
                             static_cast<QWidget *>(&radio)}) {
      QCOMPARE(pixelAt(restoredDarkFrame, control->mapTo(&dialog, QPoint(350, 10))), darkSurface);
    }
    QCOMPARE(pixelAt(painted(field), QPoint(350, 10)), QColor("#151719"));
    QCOMPARE(pixelAt(painted(nestedField), QPoint(350, 10)), QColor("#151719"));
    QCOMPARE(field.text(), QStringLiteral("unchanged"));
    QCOMPARE(nestedField.text(), QStringLiteral("nested unchanged"));
    QVERIFY(check.isChecked());
    QVERIFY(radio.isChecked());
    dialog.hide();
  }
}

void AppThemeTests::keepsWrappingCheckBoxResponsiveAndInteractive() {
  // This is disposable test data, not a new user-facing caption.
  const QString caption = QStringLiteral(
      "Keep all original source images and previously generated outputs while "
      "rebuilding camera alignment with automatic reconstruction quality checks.");
  for (const UiTheme theme : {UiTheme::Dark, UiTheme::Light}) {
    AppTheme::applyTheme(*qApp, theme, false);
    QDialog dialog;
    QFormLayout form(&dialog);
    form.setContentsMargins(12, 12, 12, 12);
    form.setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form.setRowWrapPolicy(QFormLayout::WrapLongRows);
    form.setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    WrappingCheckBox check(caption, &dialog);
    check.setChecked(true);
    form.addRow(QString(), &check);
    QSignalSpy toggles(&check, &QCheckBox::toggled);
    QVERIFY(toggles.isValid());
    QVERIFY(check.sizePolicy().hasHeightForWidth());
    dialog.resize(460, 600);
    dialog.show();

    for (const int scale : {90, 150, 90}) {
      AppTheme::apply(*qApp, scale, false);
      for (const int width : {240, 460}) {
        dialog.resize(width, 600);
        // Let the real form settle its height-for-width request. Never set the
        // checkbox height manually or invalidate again from a height-only resize.
        QApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(check.height() >= check.heightForWidth(check.width()), 1000);
        QCOMPARE(check.text(), caption);
        QVERIFY(check.isChecked());
        QCOMPARE(toggles.count(), 0);

        QStyleOptionButton option;
        option.initFrom(&check);
        option.text = caption;
        const QRect contents = check.style()->subElementRect(
            QStyle::SE_CheckBoxContents, &option, &check);
        const int firstLineHeight = std::max(check.fontMetrics().height(),
            check.style()->pixelMetric(QStyle::PM_IndicatorHeight, &option, &check));
        const int textTop = std::max(contents.top(),
            (firstLineHeight - check.fontMetrics().height()) / 2);
        const QRect wrappedText = check.fontMetrics().boundingRect(
            QRect(0, 0, std::max(1, contents.width()), 10000),
            Qt::AlignLeading | Qt::AlignTop | Qt::TextWordWrap | Qt::TextHideMnemonic,
            caption);
        QVERIFY(wrappedText.height() > firstLineHeight);
        const QPoint lastLine(contents.center().x(),
            textTop + wrappedText.height() - check.fontMetrics().height() / 2);
        QVERIFY(check.rect().contains(lastLine));
        QVERIFY(lastLine.y() >= firstLineHeight);

        QTest::mouseClick(&check, Qt::LeftButton, Qt::NoModifier, lastLine);
        QVERIFY(!check.isChecked());
        QCOMPARE(toggles.count(), 1);
        check.setFocus(Qt::OtherFocusReason);
        QTest::keyClick(&check, Qt::Key_Space);
        QVERIFY(check.isChecked());
        QCOMPARE(toggles.count(), 2);

        check.setEnabled(false);
        QTest::mouseClick(&check, Qt::LeftButton, Qt::NoModifier, lastLine);
        QTest::keyClick(&check, Qt::Key_Space);
        QVERIFY(check.isChecked());
        QCOMPARE(toggles.count(), 2);
        QCOMPARE(check.text(), caption);
        check.setEnabled(true);
        toggles.clear();
      }
    }
    dialog.hide();
  }
}

void AppThemeTests::paintsVisibleLeftAndRightDockSeparators() {
  const auto hasInk = [](const QImage &image, const QRect &logical,
                          const QColor &expected) {
    const qreal ratio = image.devicePixelRatio();
    const QRect pixels(qFloor(logical.x() * ratio), qFloor(logical.y() * ratio),
                       qCeil(logical.width() * ratio), qCeil(logical.height() * ratio));
    const QRect clipped = pixels.intersected(image.rect());
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
      for (int x = clipped.left(); x <= clipped.right(); ++x) {
        const QColor color = image.pixelColor(x, y);
        if (std::abs(color.red() - expected.red()) <= 2 &&
            std::abs(color.green() - expected.green()) <= 2 &&
            std::abs(color.blue() - expected.blue()) <= 2)
          return true;
      }
    }
    return false;
  };
  for (const int scale : {90, 100, 150}) {
    AppTheme::apply(*qApp, scale, false);
    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QMainWindow window;
    window.resize(740, 360);
    auto *center = new QWidget(&window);
    center->setMinimumSize(200, 160);
    window.setCentralWidget(center);
    auto *left = new QDockWidget(QStringLiteral("Left"), &window);
    auto *right = new QDockWidget(QStringLiteral("Right"), &window);
    left->setWidget(new QWidget(left));
    right->setWidget(new QWidget(right));
    window.addDockWidget(Qt::LeftDockWidgetArea, left);
    window.addDockWidget(Qt::RightDockWidgetArea, right);
    window.show();
    window.resizeDocks({left, right}, {120, 120}, Qt::Horizontal);
    QApplication::processEvents();
    QTest::qWait(20);
    const auto splits = [&] {
      const int top = center->geometry().center().y() - 20;
      return QList<QRect>{
          QRect(left->geometry().right() + 1, top,
                center->geometry().left() - left->geometry().right() - 1, 40),
          QRect(center->geometry().right() + 1, top,
                right->geometry().left() - center->geometry().right() - 1, 40)};
    };
    QVERIFY(!qApp->styleSheet().contains(QStringLiteral("QMainWindow::separator")));
    const auto darkSplits = splits();
    const int nativeExtent = window.style()->pixelMetric(
        QStyle::PM_DockWidgetSeparatorExtent, nullptr, &window);
    for (const QRect &split : darkSplits) {
      QVERIFY(split.isValid());
      QCOMPARE(split.width(), nativeExtent);
    }
    const QImage darkFrame = painted(window);

    AppTheme::applyTheme(*qApp, UiTheme::Light, false);
    QApplication::processEvents();
    const QImage lightFrame = painted(window);
    QCOMPARE(pixelAt(lightFrame, center->geometry().center()), QColor("#ffffff"));
    const auto lightSplits = splits();
    for (const QRect &split : lightSplits) {
      QVERIFY(split.isValid());
      QCOMPARE(split.width(), AppTheme::scaled(8, scale));
      // Match the neutral panel-header grey. The wider band separates areas
      // without introducing a high-contrast dark outline into the light UI.
      QVERIFY(hasInk(lightFrame, split, QColor("#dedede")));
      QVERIFY(hasInk(lightFrame, split, QColor("#c8c8c8")));
      QCOMPARE(pixelAt(lightFrame, split.center()), QColor("#dedede"));
      const QColor edge = darkestPixel(lightFrame, split);
      QVERIFY(neutral(edge));
      QVERIFY(edge.lightness() >= 195);
      QVERIFY(contrast(edge, QColor("#ffffff")) < 2.0);
    }

    const auto mouse = [&window](const QEvent::Type type, const QPoint &local,
                                 const Qt::MouseButton button,
                                 const Qt::MouseButtons buttons) {
      QMouseEvent event(type, QPointF(local), QPointF(window.mapToGlobal(local)),
                        button, buttons, Qt::NoModifier);
      QApplication::sendEvent(&window, &event);
    };
    const auto dragSeparator = [&](const int index, const int deltaX) {
      const QPoint start = splits()[index].center();
      const QPoint end = start + QPoint(deltaX, 0);
      // Exercise QMainWindow's real separator hit-test/resize sequence without
      // moving the physical cursor or touching any other application window.
      mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
      mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
      QApplication::processEvents();
      mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
      QApplication::processEvents();
      QTest::qWait(20);
      mouse(QEvent::MouseMove, center->geometry().center(),
            Qt::NoButton, Qt::NoButton);
      QApplication::processEvents();
    };
    const int dayLeftWidth = left->width();
    dragSeparator(0, 30);
    QVERIFY(left->width() > dayLeftWidth);
    const int dayRightWidth = right->width();
    dragSeparator(1, -30);
    QVERIFY(right->width() > dayRightWidth);
    QVERIFY(!left->isFloating() && !right->isFloating());
    QCOMPARE(window.dockWidgetArea(left), Qt::LeftDockWidgetArea);
    QCOMPARE(window.dockWidgetArea(right), Qt::RightDockWidgetArea);

    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QApplication::processEvents();
    QVERIFY(!qApp->styleSheet().contains(QStringLiteral("QMainWindow::separator")));
    const auto restored = splits();
    const QImage restoredFrame = painted(window);
    for (int index = 0; index < restored.size(); ++index) {
      QCOMPARE(restored[index].width(), darkSplits[index].width());
      QCOMPARE(pixelAt(restoredFrame, restored[index].center()),
               pixelAt(darkFrame, darkSplits[index].center()));
    }
    // The wider light bands remain real resize handles rather than
    // decorative frames. Keep both sidebars independently resizable in night.
    const int leftWidth = left->width();
    const int rightWidth = right->width();
    window.resizeDocks({left}, {leftWidth + 30}, Qt::Horizontal);
    QApplication::processEvents();
    QVERIFY(left->width() > leftWidth);
    window.resizeDocks({right}, {rightWidth + 30}, Qt::Horizontal);
    QApplication::processEvents();
    QVERIFY(right->width() > rightWidth);
    window.hide();
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

void AppThemeTests::boundsEffectiveWindowMinimumByAvailableClientArea_data() {
  QTest::addColumn<QSize>("design");
  QTest::addColumn<QSize>("available");
  QTest::addColumn<QMargins>("frame");
  QTest::addColumn<QSize>("expected");
  QTest::newRow("normal-screen-retains-design")
      << QSize(940, 620) << QSize(1920, 1040) << QMargins(8, 31, 8, 8) << QSize(940, 620);
  QTest::newRow("compact-high-DPR-client-area")
      << QSize(940, 620) << QSize(960, 500) << QMargins(4, 16, 4, 4) << QSize(936, 464);
  QTest::newRow("medium-client-height-is-bounded")
      << QSize(940, 620) << QSize(1097, 594) << QMargins(5, 18, 5, 5) << QSize(940, 555);
  QTest::newRow("one-pixel-screen-remains-positive")
      << QSize(940, 620) << QSize(1, 1) << QMargins(8, 31, 8, 8) << QSize(1, 1);
  QTest::newRow("safety-gap-can-use-nearly-all-screen")
      << QSize(940, 620) << QSize(17, 19) << QMargins() << QSize(1, 3);
  QTest::newRow("negative-frames-cannot-increase-client-budget")
      << QSize(940, 620) << QSize(960, 500) << QMargins(-4, -16, -4, -4) << QSize(940, 484);
  QTest::newRow("each-frame-side-is-clamped-independently")
      << QSize(940, 620) << QSize(960, 500) << QMargins(-4, 16, 4, -4) << QSize(940, 468);
  QTest::newRow("unknown-screen-keeps-readable-design")
      << QSize(940, 620) << QSize(-1, -1) << QMargins(8, 31, 8, 8) << QSize(940, 620);
  QTest::newRow("partly-invalid-screen-keeps-design")
      << QSize(940, 620) << QSize(-1, 500) << QMargins(8, 31, 8, 8) << QSize(940, 620);
  QTest::newRow("zero-screen-height-is-not-a-valid-observation")
      << QSize(940, 620) << QSize(960, 0) << QMargins(8, 31, 8, 8) << QSize(940, 620);
  QTest::newRow("invalid-design-is-normalized-positive")
      << QSize(0, -4) << QSize(960, 500) << QMargins() << QSize(1, 1);
  QTest::newRow("invalid-screen-does-not-preserve-negative-design")
      << QSize(0, 620) << QSize(-1, -1) << QMargins() << QSize(1, 620);
  const int maximum = std::numeric_limits<int>::max();
  const int minimum = std::numeric_limits<int>::min();
  QTest::newRow("large-frame-sums-do-not-overflow-int")
      << QSize(940, 620) << QSize(maximum, maximum)
      << QMargins(maximum, maximum, maximum, maximum) << QSize(1, 1);
  QTest::newRow("large-valid-budgets-use-full-int-range-safely")
      << QSize(maximum, maximum) << QSize(maximum, maximum)
      << QMargins() << QSize(2147483631, 2147483631);
  QTest::newRow("minimum-negative-frame-is-not-negated")
      << QSize(940, 620) << QSize(960, 500)
      << QMargins(minimum, minimum, minimum, minimum) << QSize(940, 484);
}

void AppThemeTests::boundsEffectiveWindowMinimumByAvailableClientArea() {
  QFETCH(QSize, design);
  QFETCH(QSize, available);
  QFETCH(QMargins, frame);
  QFETCH(QSize, expected);
  const QSize actual = AppTheme::effectiveWindowMinimum(design, available, frame);
  QCOMPARE(actual, expected); // Independent literal budgets, not recomputed by the test.
  QVERIFY(actual.width() >= 1 && actual.height() >= 1);
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

void AppThemeTests::freezesSharedActionRolesAndPaletteSemantics() {
  for (const VisualRoleExpectation &expected : visualRoles) {
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::Action), QColor(expected.action));
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::ActionTint), QColor(expected.tint));
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::ActionPressed), QColor(expected.pressed));
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::ActionInk), QColor(expected.ink));
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::Focus), QColor(expected.focus));
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::OnAction), QColor("#ffffff"));
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::Hover), QColor(expected.hover));
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::Progress), QColor(expected.progress));
    QVERIFY(AppTheme::color(expected.theme, UiColorRole::ActionTint) !=
            AppTheme::color(expected.theme, UiColorRole::ActionPressed));
    QVERIFY(contrast(QColor(expected.ink), QColor(expected.tint)) >= 4.5);
    QVERIFY(contrast(QColor("#ffffff"), QColor(expected.action)) >= 4.5);
    for (int index = 0; index < static_cast<int>(UiColorRole::Count); ++index) {
      const QColor color = AppTheme::color(expected.theme, static_cast<UiColorRole>(index));
      QVERIFY(color.isValid());
      QCOMPARE(color.alpha(), 255);
    }

    const QPalette colors = AppTheme::palette(expected.theme);
    for (const auto group : {QPalette::Active, QPalette::Inactive}) {
      QCOMPARE(colors.color(group, QPalette::Window), QColor(expected.panel));
      QCOMPARE(colors.color(group, QPalette::Base), QColor(expected.field));
      QCOMPARE(colors.color(group, QPalette::WindowText), QColor(expected.text));
      QCOMPARE(colors.color(group, QPalette::Text), QColor(expected.text));
      QCOMPARE(colors.color(group, QPalette::Highlight), QColor(expected.action));
      QCOMPARE(colors.color(group, QPalette::Accent), QColor(expected.action));
      QCOMPARE(colors.color(group, QPalette::Link), QColor(expected.ink));
      QCOMPARE(colors.color(group, QPalette::HighlightedText), QColor("#ffffff"));
      QCOMPARE(colors.color(group, QPalette::ToolTipText),
               expected.theme == UiTheme::Dark ? QColor("#ffffff") : QColor("#171717"));
      QVERIFY(contrast(colors.color(group, QPalette::WindowText),
                       QColor(expected.panel)) >= 7.0);
    }
    QCOMPARE(colors.color(QPalette::Disabled, QPalette::WindowText), QColor(expected.disabledText));
    QCOMPARE(colors.color(QPalette::Disabled, QPalette::Base), QColor(expected.disabledSurface));
    QVERIFY(contrast(colors.color(QPalette::Disabled, QPalette::Text),
                     QColor(expected.disabledSurface)) >= 3.0);
    // Meaningful result colors do not become the application action color.
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::Success),
             expected.theme == UiTheme::Dark ? QColor("#66c1a8") : QColor("#176b37"));
    QCOMPARE(AppTheme::color(expected.theme, UiColorRole::Warning),
             expected.theme == UiTheme::Dark ? QColor("#d9ad5b") : QColor("#805409"));
    QVERIFY(AppTheme::color(expected.theme, UiColorRole::Success) != QColor(expected.action));
    QVERIFY(AppTheme::color(expected.theme, UiColorRole::Warning) != QColor(expected.action));
  }
}

void AppThemeTests::paintsSharedRolesAndTransparentLayoutLabels() {
  for (const VisualRoleExpectation &expected : visualRoles) {
    AppTheme::applyTheme(*qApp, expected.theme, false);
    AppTheme::apply(*qApp, 100, false);
    QDialog dialog;
    dialog.resize(580, 400);
    QLabel caption(QStringLiteral("Fixture label"), &dialog);
    caption.setGeometry(16, 16, 420, 34);
    QToolButton toggle(&dialog);
    toggle.setText(QStringLiteral("Tool"));
    toggle.setCheckable(true);
    toggle.setFocusPolicy(Qt::NoFocus);
    toggle.setGeometry(16, 62, 220, 42);
    QLineEdit field(QStringLiteral("0.901234567891_日本語"), &dialog);
    field.setFocusPolicy(Qt::NoFocus);
    field.setGeometry(16, 120, 420, 42);
    QCheckBox check(QStringLiteral("Fixture option"), &dialog);
    check.setChecked(true);
    check.setFocusPolicy(Qt::NoFocus);
    check.setGeometry(16, 178, 420, 38);
    QProgressBar progress(&dialog);
    progress.setRange(0, 100);
    progress.setValue(100);
    progress.setTextVisible(false);
    progress.setGeometry(16, 232, 420, 30);
    QPushButton disabled(QStringLiteral("MMMM 0123456789"), &dialog);
    disabled.setAutoDefault(false);
    disabled.setEnabled(false);
    disabled.setGeometry(16, 280, 420, 46);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog, 2000));
    QApplication::processEvents();
    toggle.setAttribute(Qt::WA_UnderMouse, false);

    const QString originalText = field.text();
    const QRect originalToggleGeometry = toggle.geometry();
    const QImage composed = painted(dialog);
    QCOMPARE(pixelAt(composed, caption.pos() + QPoint(400, 10)), QColor(expected.window));
    QCOMPARE(pixelAt(composed, check.pos() + QPoint(400, 10)), QColor(expected.window));
    QCOMPARE(pixelAt(painted(field), QPoint(400, 10)), QColor(expected.field));
    QCOMPARE(pixelAt(painted(progress), QPoint(400, 15)), QColor(expected.progress));
    QCOMPARE(progress.value(), 100);

    toggle.setChecked(true);
    QApplication::processEvents();
    QCOMPARE(pixelAt(painted(toggle), QPoint(200, 12)), QColor(expected.tint));
    QCOMPARE(toggle.geometry(), originalToggleGeometry);
    toggle.setDown(true);
    QApplication::processEvents();
    QCOMPARE(pixelAt(painted(toggle), QPoint(200, 12)), QColor(expected.pressed));
    QCOMPARE(toggle.geometry(), originalToggleGeometry);
    toggle.setDown(false);
    toggle.setChecked(false);
    toggle.setEnabled(false);
    field.setEnabled(false);
    QApplication::processEvents();
    const QImage disabledField = painted(field);
    QCOMPARE(pixelAt(disabledField, QPoint(400, 10)), QColor(expected.disabledSurface));
    QCOMPARE(pixelAt(painted(disabled), QPoint(400, 10)), QColor(expected.disabledSurface));
    QVERIFY(pixelsNear(disabledField, QRect(9, 5, 360, 31),
                       QColor(expected.disabledText), 5) >= 4);
    QVERIFY(contrast(QColor(expected.disabledText),
                     pixelAt(disabledField, QPoint(400, 10))) >= 3.0);
    QCOMPARE(toggle.geometry(), originalToggleGeometry);
    QCOMPARE(field.text(), originalText);
    QVERIFY(check.isChecked());
    QVERIFY(!QSettings().contains(QStringLiteral("ui/theme")));
    dialog.hide();
  }
}

void AppThemeTests::keepsKeyboardFocusAndInteractionGeometryStable() {
  for (const VisualRoleExpectation &expected : visualRoles) {
    for (const int scale : {90, 100, 150}) {
      AppTheme::applyTheme(*qApp, expected.theme, false);
      AppTheme::apply(*qApp, scale, false);
      QDialog dialog;
      QVBoxLayout layout(&dialog);
      layout.setContentsMargins(12, 12, 12, 12);
      layout.setSpacing(8);
      QLineEdit field(QStringLiteral("0.901234567891_日本語"), &dialog);
      QPushButton button(QStringLiteral("Fixture action"), &dialog);
      button.setAutoDefault(false);
      button.setCheckable(true);
      QToolButton tool(&dialog);
      tool.setText(QStringLiteral("Fixture tool"));
      tool.setCheckable(true);
      tool.setFocusPolicy(Qt::StrongFocus);
      QCheckBox check(QStringLiteral("Fixture check"), &dialog);
      QRadioButton radio(QStringLiteral("Fixture radio"), &dialog);
      QTabBar tabs(&dialog);
      tabs.addTab(QStringLiteral("First"));
      tabs.addTab(QStringLiteral("Second"));
      tabs.setFocusPolicy(Qt::StrongFocus);
      tabs.setExpanding(false);
      QTableWidget table(2, 2, &dialog);
      table.setItem(0, 0, new QTableWidgetItem(QStringLiteral("unchanged")));
      table.setCurrentCell(0, 0);
      table.setFixedHeight(AppTheme::scaled(110, scale));
      const QList<QWidget *> controls = {&field, &button, &tool, &check, &radio, &tabs, &table};
      for (QWidget *control : controls) layout.addWidget(control);
      QWidget::setTabOrder(&field, &button);
      QWidget::setTabOrder(&button, &tool);
      QWidget::setTabOrder(&tool, &check);
      QWidget::setTabOrder(&check, &radio);
      QWidget::setTabOrder(&radio, &tabs);
      QWidget::setTabOrder(&tabs, &table);
      dialog.resize(600, 680);
      dialog.show();
      QVERIFY(QTest::qWaitForWindowExposed(&dialog, 2000));
      dialog.activateWindow();
      field.setFocus(Qt::TabFocusReason);
      QTRY_VERIFY_WITH_TIMEOUT(field.hasFocus(), 2000);
      QApplication::processEvents();
      const auto geometries = [&] {
        QList<QRect> values;
        for (const QWidget *control : controls) values.append(control->geometry());
        return values;
      };
      const auto hints = [&] {
        QList<QSize> values;
        for (const QWidget *control : controls) values.append(control->sizeHint());
        return values;
      };
      const QList<QRect> originalGeometry = geometries();
      const QList<QSize> originalHints = hints();
      const QString originalText = field.text();
      const QFont originalFont = qApp->font();
      QVERIFY(pixelsNear(painted(field), field.rect(), QColor(expected.focus)) >= 8);

      QTest::keyClick(&field, Qt::Key_Tab);
      QTRY_VERIFY_WITH_TIMEOUT(button.hasFocus(), 1000);
      QVERIFY(pixelsNear(painted(button), button.rect(), QColor(expected.focus)) >= 8);
      QCOMPARE(geometries(), originalGeometry);
      QCOMPARE(hints(), originalHints);
      QTest::keyClick(&button, Qt::Key_Space);
      QVERIFY(button.isChecked());
      QCOMPARE(geometries(), originalGeometry);
      QCOMPARE(hints(), originalHints);
      QTest::mouseMove(&button, button.rect().center());
      QTest::mousePress(&button, Qt::LeftButton);
      QVERIFY(button.isDown());
      QCOMPARE(geometries(), originalGeometry);
      QCOMPARE(hints(), originalHints);
      QTest::mouseRelease(&button, Qt::LeftButton);
      QCOMPARE(geometries(), originalGeometry);
      QCOMPARE(hints(), originalHints);

      QTest::keyClick(&button, Qt::Key_Tab);
      QTRY_VERIFY_WITH_TIMEOUT(tool.hasFocus(), 1000);
      QVERIFY(pixelsNear(painted(tool), tool.rect(), QColor(expected.focus)) >= 8);
      QTest::keyClick(&tool, Qt::Key_Tab);
      QTRY_VERIFY_WITH_TIMEOUT(check.hasFocus(), 1000);
      QVERIFY(pixelsNear(painted(check), check.rect(), QColor(expected.focus)) >= 8);
      QTest::keyClick(&check, Qt::Key_Space);
      QVERIFY(check.isChecked());
      QTest::keyClick(&check, Qt::Key_Tab);
      QTRY_VERIFY_WITH_TIMEOUT(radio.hasFocus(), 1000);
      QVERIFY(pixelsNear(painted(radio), radio.rect(), QColor(expected.focus)) >= 8);
      QTest::keyClick(&radio, Qt::Key_Tab);
      QTRY_VERIFY_WITH_TIMEOUT(tabs.hasFocus(), 1000);
      const QRect tabBefore = tabs.tabRect(0);
      const int tabHeight = tabs.height();
      QTest::keyClick(&tabs, Qt::Key_Right);
      QCOMPARE(tabs.currentIndex(), 1);
      QCOMPARE(tabs.tabRect(0), tabBefore);
      QCOMPARE(tabs.height(), tabHeight);
      QTest::keyClick(&tabs, Qt::Key_Tab);
      QTRY_VERIFY_WITH_TIMEOUT(table.hasFocus() || table.viewport()->hasFocus(), 1000);
      QVERIFY(pixelsNear(painted(table), table.rect(), QColor(expected.focus)) >= 8);
      QCOMPARE(geometries(), originalGeometry);
      QCOMPARE(hints(), originalHints);
      QCOMPARE(field.text(), originalText);
      QCOMPARE(table.item(0, 0)->text(), QStringLiteral("unchanged"));
      QCOMPARE(qApp->font(), originalFont);
      QVERIFY(!QSettings().contains(QStringLiteral("ui/theme")));
      dialog.hide();
    }
  }
}

QTEST_MAIN(AppThemeTests)

#include "AppThemeTests.moc"
