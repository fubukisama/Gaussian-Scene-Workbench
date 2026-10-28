#include "FontResolutionEvidence.h"

#include <QApplication>
#include <QFont>
#include <QFontInfo>
#include <QGlyphRun>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QTextLayout>
#include <QtTest>

#include <array>
#include <iterator>

using namespace gsw;

class FontResolutionEvidenceTests final : public QObject {
  Q_OBJECT
private slots:
  void realFontsPreserveFallbacksAndProduceGlyphEvidence_data();
  void realFontsPreserveFallbacksAndProduceGlyphEvidence();
};

void FontResolutionEvidenceTests::realFontsPreserveFallbacksAndProduceGlyphEvidence_data() {
  QTest::addColumn<QStringList>("families");
  QTest::addColumn<qreal>("dpr");
  QTest::addColumn<bool>("pixelRole");
  struct FamilyCase { const char *locale; QStringList requested; };
  const std::array<FamilyCase, 3> cases{{
      {"zh_CN", {QStringLiteral("Microsoft YaHei UI")}},
      {"en_US", {QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI")}},
      {"ja_JP", {QStringLiteral("Yu Gothic UI"), QStringLiteral("Meiryo"), QStringLiteral("Microsoft YaHei UI")}}}};
  for (const auto &family : cases) {
    for (const qreal dpr : {1., 1.25, 1.5, 1.75, 2.}) {
      for (const bool pixelRole : {false, true}) {
        const QByteArray name = QStringLiteral("%1-dpr%2-%3").arg(QString::fromLatin1(family.locale))
            .arg(dpr).arg(pixelRole ? QStringLiteral("pixel-bold") : QStringLiteral("point-regular")).toLatin1();
        QTest::newRow(name.constData()) << family.requested << dpr << pixelRole;
      }
    }
  }
}

void FontResolutionEvidenceTests::realFontsPreserveFallbacksAndProduceGlyphEvidence() {
  QFETCH(QStringList, families);
  QFETCH(qreal, dpr);
  QFETCH(bool, pixelRole);
  QVERIFY(qobject_cast<QApplication *>(QCoreApplication::instance()));

  QFont font;
  font.setFamilies(families);
  font.setStyleStrategy(QFont::PreferAntialias);
  if (pixelRole) {
    font.setPixelSize(14);
    font.setWeight(QFont::Bold);
  } else {
    font.setPointSizeF(10.);
    font.setWeight(QFont::Normal);
  }
  const auto requestedFamilies = font.families();
  const auto requestedPointSize = font.pointSizeF();
  const auto requestedPixelSize = font.pixelSize();
  const auto requestedWeight = font.weight();
  const auto requestedStrategy = font.styleStrategy();

  QImage image(QSize(qRound(600 * dpr), qRound(100 * dpr)), QImage::Format_ARGB32_Premultiplied);
  QVERIFY(!image.isNull());
  image.setDevicePixelRatio(dpr);
  image.setDotsPerMeterX(3780); // Independent literal: 96 logical DPI after Qt rounding.
  image.setDotsPerMeterY(3780);
  image.fill(Qt::transparent);
  QCOMPARE(image.logicalDpiX(), 96);
  QCOMPARE(image.logicalDpiY(), 96);

  const auto assertRecord = [&](const QJsonObject &record) {
    QVERIFY(record.value("requestedFamilies").isArray());
    QCOMPARE(record.value("requestedFamilies").toArray(), QJsonArray::fromStringList(families));
    QCOMPARE(record.value("requestedPointSize").toDouble(), requestedPointSize);
    QCOMPARE(record.value("requestedPixelSize").toInt(), requestedPixelSize);
    QVERIFY(!record.value("resolvedFamily").toString().trimmed().isEmpty());
    QVERIFY(record.value("pointSize").isDouble());
    QVERIFY(record.value("pointSize").toDouble() > 0);
    QVERIFY(record.value("pixelSize").isDouble());
    QVERIFY(record.value("pixelSize").toInt() > 0);
    QVERIFY(record.value("weight").isDouble());
    QVERIFY(record.value("weight").toInt() >= 1 && record.value("weight").toInt() <= 1000);
    QVERIFY(record.contains("exactMatch"));
    QVERIFY2(record.value("exactMatch").isNull(),
             "An unqueried value must not masquerade as a positive or negative font-match result.");
    QCOMPARE(record.value("exactMatchQuery").toString(),
             QStringLiteral("not_queried_qt683_multifamily_crash"));
  };

  assertRecord(fontResolutionEvidence(font)); // Optional-device public path.
  const QJsonObject beforePainting = fontResolutionEvidence(font, &image);
  assertRecord(beforePainting);
  // Independent Qt observations use safe getters only. Never call exactMatch()
  // in this full-suite regression: the remote CDB already supplies the red.
  const QFontInfo actual(QFont(font, &image));
  QCOMPARE(beforePainting.value("resolvedFamily").toString(), actual.family());
  QCOMPARE(beforePainting.value("pixelSize").toInt(), actual.pixelSize());
  QCOMPARE(beforePainting.value("pointSize").toDouble(), actual.pointSizeF());
  QCOMPARE(beforePainting.value("weight").toInt(), int(actual.weight()));

  // Fixed QA glyph data, not a localized UI literal: 17 codepoints with no
  // combining marks or ligature-bearing pairs. Family fallback remains enabled.
  const char32_t probe[]{0x4c, 0x61, 0x74, 0x69, 0x6e, 0x20, 0x4e2d, 0x6587,
      0x20, 0x65e5, 0x672c, 0x8a9e, 0x20, 0x30, 0x31, 0x32, 0x33};
  static_assert(std::size(probe) == 17);
  const QString text = QString::fromUcs4(probe, qsizetype(std::size(probe)));
  QTextLayout layout(text, font, &image);
  layout.beginLayout();
  QTextLine line = layout.createLine();
  QVERIFY(line.isValid());
  line.setLineWidth(560);
  layout.endLayout();
  qsizetype glyphCount = 0, missingGlyphs = 0;
  const auto runs = layout.glyphRuns();
  QVERIFY(!runs.isEmpty());
  for (const QGlyphRun &run : runs) {
    const auto glyphs = run.glyphIndexes();
    QVERIFY(run.rawFont().isValid());
    QVERIFY(!run.rawFont().familyName().trimmed().isEmpty());
    glyphCount += glyphs.size();
    missingGlyphs += glyphs.count(0);
  }
  QCOMPARE(glyphCount, qsizetype(17));
  QCOMPARE(missingGlyphs, qsizetype(0));

  QPainter painter(&image);
  QVERIFY(painter.isActive());
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
  painter.setFont(font);
  painter.setPen(QColor(17, 31, 47));
  const auto duringPainting = fontResolutionEvidence(painter.font(), painter.device());
  assertRecord(duringPainting);
  layout.draw(&painter, QPointF(12.25, 10.5));
  QVERIFY(painter.end());

  qsizetype ink = 0, partialAlpha = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const int alpha = qAlpha(image.pixel(x, y));
      ink += alpha > 0;
      partialAlpha += alpha > 0 && alpha < 255;
    }
  }
  QVERIFY2(ink > 20, "The actual mixed-script glyphs must render visible image ink.");
  QVERIFY2(partialAlpha > 0, "TextAntialiasing must produce actual fractional-alpha glyph pixels, not only a flag.");
  QCOMPARE(font.families(), requestedFamilies);
  QCOMPARE(font.pointSizeF(), requestedPointSize);
  QCOMPARE(font.pixelSize(), requestedPixelSize);
  QCOMPARE(font.weight(), requestedWeight);
  QCOMPARE(font.styleStrategy(), requestedStrategy);
  QCOMPARE(fontResolutionEvidence(font, &image), beforePainting);
}

QTEST_MAIN(FontResolutionEvidenceTests)
#include "FontResolutionEvidenceTests.moc"
