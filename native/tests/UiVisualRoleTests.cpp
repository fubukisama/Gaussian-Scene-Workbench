#include "AppTheme.h"
#include "WorkbenchIcons.h"

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QTransform>
#include <QtTest>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>

using namespace gsw;

namespace {
struct InkExpectation {
  const char *name;
  UiTheme theme;
  QIcon::Mode mode;
  QIcon::State state;
  bool accent;
  WorkbenchIconSurface surface;
  const char *ink;
};

// Literal expectations freeze the approved semantic distinction. They are not
// derived from workbenchIconInk or a second copy of the production algorithm.
constexpr InkExpectation inkCases[] = {
    {"dark-normal", UiTheme::Dark, QIcon::Normal, QIcon::Off, false, WorkbenchIconSurface::Tinted, "#e7e9ea"},
    {"dark-active", UiTheme::Dark, QIcon::Active, QIcon::Off, false, WorkbenchIconSurface::Tinted, "#e7e9ea"},
    {"dark-selected", UiTheme::Dark, QIcon::Selected, QIcon::Off, false, WorkbenchIconSurface::Tinted, "#e7e9ea"},
    {"dark-selected-on", UiTheme::Dark, QIcon::Selected, QIcon::On, false, WorkbenchIconSurface::Tinted, "#e7e9ea"},
    {"dark-on", UiTheme::Dark, QIcon::Normal, QIcon::On, false, WorkbenchIconSurface::Tinted, "#8cbcf0"},
    {"dark-disabled", UiTheme::Dark, QIcon::Disabled, QIcon::Off, false, WorkbenchIconSurface::Tinted, "#929a9f"},
    {"dark-disabled-on", UiTheme::Dark, QIcon::Disabled, QIcon::On, false, WorkbenchIconSurface::Tinted, "#929a9f"},
    {"dark-accent", UiTheme::Dark, QIcon::Normal, QIcon::Off, true, WorkbenchIconSurface::Tinted, "#8cbcf0"},
    {"dark-solid-normal", UiTheme::Dark, QIcon::Normal, QIcon::Off, false, WorkbenchIconSurface::OnAccent, "#ffffff"},
    {"dark-solid-selected", UiTheme::Dark, QIcon::Selected, QIcon::On, false, WorkbenchIconSurface::OnAccent, "#ffffff"},
    {"dark-solid-on", UiTheme::Dark, QIcon::Normal, QIcon::On, false, WorkbenchIconSurface::OnAccent, "#ffffff"},
    {"dark-solid-disabled", UiTheme::Dark, QIcon::Disabled, QIcon::On, true, WorkbenchIconSurface::OnAccent, "#929a9f"},
    {"light-normal", UiTheme::Light, QIcon::Normal, QIcon::Off, false, WorkbenchIconSurface::Tinted, "#171717"},
    {"light-active", UiTheme::Light, QIcon::Active, QIcon::Off, false, WorkbenchIconSurface::Tinted, "#171717"},
    {"light-selected", UiTheme::Light, QIcon::Selected, QIcon::Off, false, WorkbenchIconSurface::Tinted, "#171717"},
    {"light-selected-on", UiTheme::Light, QIcon::Selected, QIcon::On, false, WorkbenchIconSurface::Tinted, "#171717"},
    {"light-on", UiTheme::Light, QIcon::Normal, QIcon::On, false, WorkbenchIconSurface::Tinted, "#135f9b"},
    {"light-disabled", UiTheme::Light, QIcon::Disabled, QIcon::Off, false, WorkbenchIconSurface::Tinted, "#6b6b6b"},
    {"light-disabled-on", UiTheme::Light, QIcon::Disabled, QIcon::On, false, WorkbenchIconSurface::Tinted, "#6b6b6b"},
    {"light-accent", UiTheme::Light, QIcon::Normal, QIcon::Off, true, WorkbenchIconSurface::Tinted, "#135f9b"},
    {"light-solid-normal", UiTheme::Light, QIcon::Normal, QIcon::Off, false, WorkbenchIconSurface::OnAccent, "#ffffff"},
    {"light-solid-selected", UiTheme::Light, QIcon::Selected, QIcon::On, false, WorkbenchIconSurface::OnAccent, "#ffffff"},
    {"light-solid-on", UiTheme::Light, QIcon::Normal, QIcon::On, false, WorkbenchIconSurface::OnAccent, "#ffffff"},
    {"light-solid-disabled", UiTheme::Light, QIcon::Disabled, QIcon::On, true, WorkbenchIconSurface::OnAccent, "#6b6b6b"},
};

struct DprExpectation {
  const char *name;
  qreal ratio;
  int iconPhysicalEdge;
  int canvasPhysicalEdge;
};
constexpr DprExpectation dprCases[] = {
    {"dpr100", 1.0, 20, 40},
    {"dpr125", 1.25, 25, 50},
    {"dpr150", 1.5, 30, 60},
    {"dpr175", 1.75, 35, 70},
    {"dpr200", 2.0, 40, 80},
};

void addInkColumns() {
  QTest::addColumn<int>("theme");
  QTest::addColumn<int>("mode");
  QTest::addColumn<int>("state");
  QTest::addColumn<bool>("accent");
  QTest::addColumn<int>("surface");
  QTest::addColumn<QColor>("expectedInk");
}

bool sameRgb(const QColor &actual, const QColor &expected, const int tolerance) {
  return std::abs(actual.red() - expected.red()) <= tolerance &&
         std::abs(actual.green() - expected.green()) <= tolerance &&
         std::abs(actual.blue() - expected.blue()) <= tolerance;
}
} // namespace

class UiVisualRoleTests final : public QObject {
  Q_OBJECT
private slots:
  void resolvesLiteralInkForEverySemanticSurface_data();
  void resolvesLiteralInkForEverySemanticSurface();
  void paintsLiteralInkAndTransparentPeripheryAtActualPixmapDpr_data();
  void paintsLiteralInkAndTransparentPeripheryAtActualPixmapDpr();
  void multipliesInkAlphaAndPreservesPainterState_data();
  void multipliesInkAlphaAndPreservesPainterState();
};

void UiVisualRoleTests::resolvesLiteralInkForEverySemanticSurface_data() {
  addInkColumns();
  for (const InkExpectation &expected : inkCases) {
    QTest::newRow(expected.name)
        << static_cast<int>(expected.theme) << static_cast<int>(expected.mode)
        << static_cast<int>(expected.state) << expected.accent
        << static_cast<int>(expected.surface) << QColor(expected.ink);
  }
}

void UiVisualRoleTests::resolvesLiteralInkForEverySemanticSurface() {
  QFETCH(int, theme);
  QFETCH(int, mode);
  QFETCH(int, state);
  QFETCH(bool, accent);
  QFETCH(int, surface);
  QFETCH(QColor, expectedInk);
  const QPalette palette = AppTheme::palette(static_cast<UiTheme>(theme));
  const QColor actual = workbenchIconInk(palette, static_cast<QIcon::Mode>(mode),
      static_cast<QIcon::State>(state), accent, static_cast<WorkbenchIconSurface>(surface));
  QCOMPARE(actual, expectedInk);
  QCOMPARE(actual.alpha(), 255);
}

void UiVisualRoleTests::paintsLiteralInkAndTransparentPeripheryAtActualPixmapDpr_data() {
  addInkColumns();
  QTest::addColumn<qreal>("requestedDpr");
  QTest::addColumn<int>("physicalEdge");
  for (const InkExpectation &expected : inkCases) {
    for (const DprExpectation &dpr : dprCases) {
      const QByteArray name = QByteArray(expected.name) + '-' + dpr.name;
      QTest::newRow(name.constData())
          << static_cast<int>(expected.theme) << static_cast<int>(expected.mode)
          << static_cast<int>(expected.state) << expected.accent
          << static_cast<int>(expected.surface) << QColor(expected.ink)
          << dpr.ratio << dpr.iconPhysicalEdge;
    }
  }
}

void UiVisualRoleTests::paintsLiteralInkAndTransparentPeripheryAtActualPixmapDpr() {
  QFETCH(int, theme);
  QFETCH(int, mode);
  QFETCH(int, state);
  QFETCH(bool, accent);
  QFETCH(int, surface);
  QFETCH(QColor, expectedInk);
  QFETCH(qreal, requestedDpr);
  QFETCH(int, physicalEdge);
  const QPalette palette = AppTheme::palette(static_cast<UiTheme>(theme));
  const std::array<WorkbenchIconId, 6> icons = {
      WorkbenchIconId::Move, WorkbenchIconId::Rotate, WorkbenchIconId::Scale,
      WorkbenchIconId::Transform, WorkbenchIconId::Save, WorkbenchIconId::Stop};
  for (const WorkbenchIconId id : icons) {
    const QIcon icon = workbenchIcon(id, palette, accent,
                                    static_cast<WorkbenchIconSurface>(surface));
    QVERIFY(!icon.isNull());
    const QPixmap pixmap = icon.pixmap(QSize(20, 20), requestedDpr,
        static_cast<QIcon::Mode>(mode), static_cast<QIcon::State>(state));
    QVERIFY(!pixmap.isNull());
    // This is the actual returned raster metadata, not an assumed UI percentage.
    QCOMPARE(pixmap.size(), QSize(physicalEdge, physicalEdge));
    QVERIFY(std::abs(pixmap.devicePixelRatio() - requestedDpr) < 0.001);
    QCOMPARE(pixmap.deviceIndependentSize(), QSizeF(20.0, 20.0));
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(image.size(), QSize(physicalEdge, physicalEdge));
    for (const QPoint corner : {QPoint(0, 0), QPoint(physicalEdge - 1, 0),
                                QPoint(0, physicalEdge - 1),
                                QPoint(physicalEdge - 1, physicalEdge - 1)})
      QCOMPARE(image.pixelColor(corner).alpha(), 0);
    int transparentPixels = 0;
    int corePixels = 0;
    int wrongInk = 0;
    int partialPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        const QColor pixel = image.pixelColor(x, y);
        if (pixel.alpha() == 0) ++transparentPixels;
        if (pixel.alpha() > 0 && pixel.alpha() < 255) ++partialPixels;
        if (pixel.alpha() >= 128) {
          ++corePixels;
          if (!sameRgb(pixel, expectedInk, 3)) ++wrongInk;
        }
      }
    }
    QVERIFY(transparentPixels >= image.width() * image.height() / 3);
    QVERIFY(corePixels >= 4);
    QVERIFY(partialPixels > 0);
    QCOMPARE(wrongInk, 0);
  }
}

void UiVisualRoleTests::multipliesInkAlphaAndPreservesPainterState_data() {
  QTest::addColumn<qreal>("dpr");
  QTest::addColumn<int>("physicalEdge");
  for (const DprExpectation &expected : dprCases)
    QTest::newRow(expected.name) << expected.ratio << expected.canvasPhysicalEdge;
}

void UiVisualRoleTests::multipliesInkAlphaAndPreservesPainterState() {
  QFETCH(qreal, dpr);
  QFETCH(int, physicalEdge);
  QImage image(QSize(physicalEdge, physicalEdge), QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(dpr);
  image.fill(Qt::transparent);
  QColor ink("#135f9b");
  ink.setAlpha(128);
  QPainter painter(&image);
  painter.setOpacity(0.5);
  painter.setPen(QColor("#d9ad5b"));
  const QPen originalPen = painter.pen();
  const QTransform originalTransform = painter.transform();
  const auto originalHints = painter.renderHints();
  paintWorkbenchIcon(painter, WorkbenchIconId::Stop, QRectF(10.0, 10.0, 20.0, 20.0), ink);
  QCOMPARE(painter.opacity(), 0.5);
  QCOMPARE(painter.pen(), originalPen);
  QCOMPARE(painter.transform(), originalTransform);
  QCOMPARE(painter.renderHints(), originalHints);
  painter.end();

  int maximumAlpha = 0;
  int coloredPixels = 0;
  int partialPixels = 0;
  const QRect protectedCenter(qFloor(9.0 * dpr), qFloor(9.0 * dpr),
                              qCeil(22.0 * dpr), qCeil(22.0 * dpr));
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (!protectedCenter.contains(x, y)) QCOMPARE(pixel.alpha(), 0);
      maximumAlpha = std::max(maximumAlpha, pixel.alpha());
      if (pixel.alpha() > 0 && pixel.alpha() < 56) ++partialPixels;
      if (pixel.alpha() >= 48) {
        ++coloredPixels;
        QVERIFY(sameRgb(pixel, QColor("#135f9b"), 4));
      }
    }
  }
  // 0.5 painter opacity times 128/255 ink alpha gives an independently known
  // maximum of about 64/255; anti-aliased edges must retain partial coverage.
  QVERIFY(maximumAlpha >= 56 && maximumAlpha <= 65);
  QVERIFY(coloredPixels >= 4);
  QVERIFY(partialPixels > 0);
}

QTEST_MAIN(UiVisualRoleTests)
#include "UiVisualRoleTests.moc"
