#include "AppTheme.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QDockWidget>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSize>
#include <QStyle>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QVariant>
#include <QtTest>
#include <QtMath>

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
  void usesNeutralLightSurfacesAndBlueInteractionColors();
  void paintsReadableLightControlsAndDistinctPanelStructure();
  void paintsVisibleLeftAndRightDockSeparators();
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
      const QImage activeImage = painted(active);
      const QImage mutedImage = painted(muted);
      const QImage dockImage = painted(dockHeader);
      const QImage tableHeaderImage = painted(*table.horizontalHeader());
      const QImage fieldImage = painted(field);
      const QImage buttonImage = painted(button);
      const QColor white = pixelAt(activeImage, QPoint(330, 8));
      const QColor header = pixelAt(dockImage, QPoint(350, 8));
      const QColor tableHeader = pixelAt(tableHeaderImage,
          QPoint(table.horizontalHeader()->width() - 12, 6));
      QCOMPARE(white, QColor("#ffffff"));
      QCOMPARE(header, QColor("#dedede"));
      QCOMPARE(tableHeader, QColor("#dedede"));
      QVERIFY(white.lightness() - header.lightness() >= 32);
      QCOMPARE(pixelAt(painted(menuBar), QPoint(80, 8)), QColor("#e7e7e7"));
      QCOMPARE(pixelAt(painted(toolbar), QPoint(80, 8)), QColor("#e7e7e7"));
      QVERIFY(contrast(darkestPixel(activeImage, active.rect()), white) >= 7.0);
      QVERIFY(contrast(darkestPixel(mutedImage, muted.rect()), white) >= 7.0);
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
  const QRegularExpression daySeparators(
      QStringLiteral("QMainWindow::separator[^\\{]*\\{[^\\}]*\\}\\s*"));
  for (const int scale : {90, 100, 150}) {
    AppTheme::apply(*qApp, scale, false);
    AppTheme::applyTheme(*qApp, UiTheme::Dark, false);
    QString darkGeometry = qApp->styleSheet();
    darkGeometry.remove(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}")));
    AppTheme::applyTheme(*qApp, UiTheme::Light, false);
    QString lightGeometry = qApp->styleSheet();
    // The light-only resize-handle enhancement is an explicit exception;
    // all other widget/font/dock-title geometry must remain identical.
    lightGeometry.remove(daySeparators);
    lightGeometry.remove(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}")));
    QCOMPARE(lightGeometry.trimmed(), darkGeometry.trimmed());
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QLabel#densityGuardWarning")));
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
