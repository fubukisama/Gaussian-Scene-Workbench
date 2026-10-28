#include "UiOverlayRaster.h"
#include "NavigationOverlayKey.h"

#include <QPainter>
#include <QtTest>
#include <limits>

using namespace gsw;

class UiOverlayRasterTests final : public QObject {
  Q_OBJECT
private slots:
  void nativePhysicalPixelsAndBoundedReuse();
  void invalidatesOnDprFontGeometryAndDisplayIdentity();
  void premultipliedTransparencyPreservesBackdrop();
  void rejectsUnboundedOrInvalidAllocation();
  void navigationIdentityTracksDisplayInputs();
  void retainsTargetLogicalFontDpi();
};

void UiOverlayRasterTests::nativePhysicalPixelsAndBoundedReuse() {
  for (const qreal dpr : {1., 1.25, 1.5, 1.75, 2.}) {
    QImage canvas(QSize(200 * dpr, 160 * dpr), QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::white);
    UiOverlayRaster tile;
    int calls = 0;
    auto draw = [&](QPainter &p, QJsonObject &) {
      ++calls;
      QCOMPARE(p.device()->devicePixelRatioF(), dpr);
      p.setPen(Qt::NoPen);
      p.setBrush(Qt::red);
      p.drawEllipse(QPointF(50.3, 45.7), 12.4, 12.4);
    };
    QPainter painter(&canvas);
    QVERIFY(tile.paint(painter, QRectF(30, 25, 45, 45), "one", draw));
    QVERIFY(!tile.lastHit());
    QCOMPARE(calls, 1);
    const auto generation = tile.generation();
    QVERIFY(tile.peakCpuBytes() <= UiOverlayRaster::byteLimit);
    QVERIFY(tile.requestedGpuBytes() < 100000);
    QVERIFY(tile.paint(painter, QRectF(30, 25, 45, 45), "one", draw));
    QVERIFY(tile.lastHit());
    QCOMPARE(calls, 1);
    QCOMPARE(tile.generation(), generation);
    painter.end();
    QCOMPARE(canvas.pixelColor(qRound(50.3 * dpr), qRound(45.7 * dpr)), QColor(Qt::red));
    QCOMPARE(canvas.pixelColor(0, 0), QColor(Qt::white));
  }
}

void UiOverlayRasterTests::invalidatesOnDprFontGeometryAndDisplayIdentity() {
  UiOverlayRaster tile;
  int calls = 0;
  auto draw = [&](QPainter &p, QJsonObject &) { ++calls; p.fillRect(QRectF(2, 2, 8, 8), Qt::green); };
  auto render = [&](qreal dpr, const QByteArray &key, QRectF bounds, int fontSize) {
    QImage canvas(QSize(100 * dpr, 100 * dpr), QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    QFont font = p.font(); font.setPixelSize(fontSize); p.setFont(font);
    return tile.paint(p, bounds, key, draw);
  };
  QVERIFY(render(1, "light-zh", QRectF(0, 0, 20, 20), 12));
  QVERIFY(render(1, "light-zh", QRectF(0, 0, 20, 20), 12));
  QCOMPARE(calls, 1);
  QVERIFY(render(1.25, "light-zh", QRectF(0, 0, 20, 20), 12));
  QVERIFY(render(1.25, "light-zh", QRectF(0, 0, 21, 20), 12));
  QVERIFY(render(1.25, "dark-en", QRectF(0, 0, 21, 20), 12));
  QVERIFY(render(1.25, "dark-en", QRectF(0, 0, 21, 20), 14));
  QCOMPARE(calls, 5);
  tile.clear();
  QCOMPARE(tile.peakCpuBytes(), quint64(0));
  QCOMPARE(tile.requestedGpuBytes(), quint64(0));
}

void UiOverlayRasterTests::premultipliedTransparencyPreservesBackdrop() {
  QImage image(80, 80, QImage::Format_ARGB32_Premultiplied);
  image.fill(QColor(20, 40, 60));
  QPainter p(&image);
  UiOverlayRaster tile;
  QVERIFY(tile.paint(p, QRectF(10, 10, 40, 40), "alpha", [](QPainter &r, QJsonObject &) {
    r.fillRect(QRectF(20, 20, 15, 15), QColor(200, 100, 50, 128));
  }));
  p.end();
  const QColor actual = image.pixelColor(25, 25);
  QVERIFY(std::abs(actual.red() - 110) <= 1);
  QVERIFY(std::abs(actual.green() - 70) <= 1);
  QVERIFY(std::abs(actual.blue() - 55) <= 1);
  QCOMPARE(image.pixelColor(15, 15), QColor(20, 40, 60));
}

void UiOverlayRasterTests::rejectsUnboundedOrInvalidAllocation() {
  QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
  QPainter p(&image);
  UiOverlayRaster tile;
  int calls = 0;
  auto draw = [&](QPainter &, QJsonObject &) { ++calls; };
  QVERIFY(!tile.paint(p, QRectF(0, 0, 100000, 100000), "huge", draw));
  QVERIFY(!tile.paint(p, QRectF(0, 0, 4000, 4000), "budget", draw));
  QVERIFY(!tile.paint(p, QRectF(0, 0, std::numeric_limits<double>::infinity(), 10), "infinite", draw));
  QCOMPARE(calls, 0);
  QCOMPARE(tile.requestedGpuBytes(), quint64(0));
}

void UiOverlayRasterTests::navigationIdentityTracksDisplayInputs() {
  NavigationGizmoLayout layout;
  layout.center = {80, 70}; layout.radius = 44;
  BlenderNavigationPresentationState state;
  const auto original = navigationOverlayKey(layout, state, QStringLiteral("zh_CN"));
  QVERIFY(original != navigationOverlayKey(layout, state, QStringLiteral("ja_JP")));
  const auto differs = [&](const auto &change) {
    auto l = layout; auto s = state; change(l, s);
    return navigationOverlayKey(l, s, QStringLiteral("zh_CN")) != original;
  };
  QVERIFY(differs([](auto &l, auto &) { l.center.rx() += .125; }));
  QVERIFY(differs([](auto &l, auto &) { l.handles[2].depth += .001F; }));
  QVERIFY(differs([](auto &l, auto &) { l.handles[1].hidden = true; }));
  QVERIFY(differs([](auto &l, auto &) { l.handles[3].aligned = true; }));
  QVERIFY(differs([](auto &l, auto &) { l.projectionLabel = QRectF(1, 2, 3, 4); }));
  QVERIFY(differs([](auto &, auto &s) { s.hover = {NavigationGizmoPart::Rotate, NavigationAxis::PositiveZ}; }));
  QVERIFY(differs([](auto &, auto &s) { s.press = {NavigationGizmoPart::Pan, NavigationAxis::None}; }));
  QVERIFY(differs([](auto &, auto &s) { s.interactionActive = true; }));
  QVERIFY(differs([](auto &, auto &s) { s.light = false; }));
  QVERIFY(differs([](auto &, auto &s) { s.orthographic = true; }));
  QVERIFY(differs([](auto &, auto &s) { s.cameraAvailable = false; }));
  QVERIFY(differs([](auto &, auto &s) { s.cameraActive = true; }));
  QVERIFY(differs([](auto &, auto &s) { s.backdrop = QColor(40, 60, 80); }));
}

void UiOverlayRasterTests::retainsTargetLogicalFontDpi() {
  UiOverlayRaster tile;
  for (const int dpi : {96, 120, 144, 168, 192}) {
    QImage image(80, 80, QImage::Format_ARGB32_Premultiplied);
    image.setDotsPerMeterX(qRound(dpi / .0254));
    image.setDotsPerMeterY(qRound(dpi / .0254));
    QPainter target(&image);
    QFont font = target.font(); font.setPointSizeF(10); target.setFont(font);
    bool observed = false;
    QVERIFY(tile.paint(target, QRectF(0, 0, 50, 50), "same-text", [&](QPainter &r, QJsonObject &) {
      observed = true;
      QCOMPARE(r.device()->logicalDpiX(), target.device()->logicalDpiX());
      QCOMPARE(r.device()->logicalDpiY(), target.device()->logicalDpiY());
      QCOMPARE(r.font(), target.font());
    }));
    QVERIFY(observed); // Same key, but the real point-font DPI has changed.
    QVERIFY(!tile.lastHit());
  }
}

QTEST_MAIN(UiOverlayRasterTests)
#include "UiOverlayRasterTests.moc"
