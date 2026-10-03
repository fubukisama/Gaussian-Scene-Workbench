#include "TrainingInputSummary.h"

#include <QDir>
#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace gsw;

namespace {
bool writeImage(const QString &path, const QSize &size) {
  QImage image(size, QImage::Format_RGB32);
  image.fill(QColor(30, 70, 90));
  return image.save(path, "PNG");
}

bool writeBytes(const QString &path, const QByteArray &data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

bool directoryLink(const QString &target, const QString &link) {
#ifdef Q_OS_WIN
  // Windows developer mode permits an unprivileged symlink. Machines without
  // that capability skip only this fixture; normal scanning never needs it.
  constexpr DWORD allowUnprivileged = 0x2;
  return CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()),
                            reinterpret_cast<LPCWSTR>(target.utf16()),
                            SYMBOLIC_LINK_FLAG_DIRECTORY | allowUnprivileged);
#else
  return ::symlink(QFile::encodeName(target).constData(),
                   QFile::encodeName(link).constData()) == 0;
#endif
}
} // namespace

class TrainingInputSummaryTests final : public QObject {
  Q_OBJECT

private slots:
  void selectsInputBeforeUndistortionAndImagesAfterward();
  void recursesIntoImageSubdirectoriesAndFallsBack();
  void preservesMixedDimensionPairsAndCountsUnknownHeaders();
  void readsHeadersWithoutDecodingPixelPayloads();
  void roundsDimensionsLikePython_data();
  void roundsDimensionsLikePython();
  void avoidsDimensionOverflowAndRejectsUnsupportedRatios();
  void doesNotFollowLinksOutsideDatasetOrIntoLoops();
  void respectsCancellation();
  void dropsStaleRequestsAndCachesHeaders();
  void destructionDoesNotWaitForOrAccessTheOwner();
};

void TrainingInputSummaryTests::selectsInputBeforeUndistortionAndImagesAfterward() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("input")));
  QVERIFY(root.mkpath(QStringLiteral("images")));
  QVERIFY(writeImage(root.filePath(QStringLiteral("input/raw.png")), {1928, 1084}));
  QVERIFY(writeImage(root.filePath(QStringLiteral("images/prepared.png")), {1600, 900}));

  const auto before = scanTrainingInputSummary(root.path(), true);
  QCOMPARE(before.sourceDirectory, root.filePath(QStringLiteral("input")));
  QCOMPARE(before.imageCount, qint64(1));
  QCOMPARE(before.sizeGroups.front().sourceSize, QSize(1928, 1084));
  QCOMPARE(projectedTrainingImageSize(before.sizeGroups.front().sourceSize, 2),
           QSize(964, 542));
  const auto after = scanTrainingInputSummary(root.path(), false);
  QCOMPARE(after.sourceDirectory, root.filePath(QStringLiteral("images")));
  QCOMPARE(after.sizeGroups.front().sourceSize, QSize(1600, 900));
}

void TrainingInputSummaryTests::recursesIntoImageSubdirectoriesAndFallsBack() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("images/camera-a")));
  QVERIFY(writeImage(root.filePath(QStringLiteral("images/camera-a/nested.PNG")), {32, 24}));
  const auto nested = scanTrainingInputSummary(root.path(), true);
  QCOMPARE(nested.sourceDirectory, root.filePath(QStringLiteral("images")));
  QCOMPARE(nested.imageCount, qint64(1));
  QTemporaryDir loose;
  QVERIFY(loose.isValid());
  QVERIFY(writeImage(QDir(loose.path()).filePath(QStringLiteral("loose.png")), {20, 10}));
  QVERIFY(QDir(loose.path()).mkpath(QStringLiteral("output")));
  QVERIFY(writeImage(QDir(loose.path()).filePath(QStringLiteral("output/old-render.png")), {80, 60}));
  const auto looseSummary = scanTrainingInputSummary(loose.path(), false);
  QCOMPARE(looseSummary.sourceDirectory, loose.path());
  QCOMPARE(looseSummary.imageCount, qint64(1));
  QCOMPARE(looseSummary.sizeGroups.front().sourceSize, QSize(20, 10));
  QCOMPARE(scanTrainingInputSummary(root.filePath(QStringLiteral("missing")), false).imageCount,
           qint64(0));
}

void TrainingInputSummaryTests::preservesMixedDimensionPairsAndCountsUnknownHeaders() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("input")));
  QVERIFY(writeImage(root.filePath(QStringLiteral("input/landscape.png")), {30, 10}));
  QVERIFY(writeImage(root.filePath(QStringLiteral("input/portrait.png")), {10, 30}));
  QVERIFY(writeImage(root.filePath(QStringLiteral("input/second-portrait.png")), {10, 30}));
  QVERIFY(writeBytes(root.filePath(QStringLiteral("input/damaged.JPG")), "not an image"));
  QVERIFY(writeBytes(root.filePath(QStringLiteral("input/notes.txt")), "not counted"));
  const auto result = scanTrainingInputSummary(root.path(), true);
  QCOMPARE(result.imageCount, qint64(4));
  QCOMPARE(result.unknownSizeCount, qint64(1));
  QCOMPARE(result.sizeGroups.size(), qsizetype(2));
  QCOMPARE(result.sizeGroups.at(0).sourceSize, QSize(10, 30));
  QCOMPARE(result.sizeGroups.at(0).imageCount, qint64(2));
  QCOMPARE(result.sizeGroups.at(1).sourceSize, QSize(30, 10));
  QCOMPARE(result.sizeGroups.at(1).imageCount, qint64(1));
  // A fictitious 30 x 30 dimension must never be synthesized from extrema.
  QVERIFY(result.sizeGroups.at(0).sourceSize != QSize(30, 30));
}

void TrainingInputSummaryTests::readsHeadersWithoutDecodingPixelPayloads() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("images")));
  QByteArray header;
  QDataStream stream(&header, QIODevice::WriteOnly);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream << quint16(0x4d42) << quint32(54) << quint16(0) << quint16(0)
         << quint32(54) << quint32(40) << qint32(300) << qint32(200)
         << quint16(1) << quint16(24) << quint32(0) << quint32(180000)
         << qint32(0) << qint32(0) << quint32(0) << quint32(0);
  const QString image = root.filePath(QStringLiteral("images/header-only.bmp"));
  QVERIFY(writeBytes(image, header));
  QImageReader reader(image);
  QCOMPARE(reader.size(), QSize(300, 200));
  QVERIFY(reader.read().isNull()); // No pixel payload exists to decode.
  const auto result = scanTrainingInputSummary(root.path(), false);
  QCOMPARE(result.imageCount, qint64(1));
  QCOMPARE(result.unknownSizeCount, qint64(0));
  QCOMPARE(result.sizeGroups.front().sourceSize, QSize(300, 200));
}

void TrainingInputSummaryTests::roundsDimensionsLikePython_data() {
  QTest::addColumn<QSize>("source");
  QTest::addColumn<int>("ratio");
  QTest::addColumn<QSize>("projected");
  QTest::newRow("original") << QSize(1928, 1084) << 1 << QSize(1928, 1084);
  QTest::newRow("half") << QSize(1928, 1084) << 2 << QSize(964, 542);
  QTest::newRow("quarter") << QSize(1928, 1084) << 4 << QSize(482, 271);
  QTest::newRow("eighth-tie-even") << QSize(1928, 1084) << 8 << QSize(241, 136);
  QTest::newRow("odd-half-both-ties") << QSize(5, 7) << 2 << QSize(2, 4);
  QTest::newRow("quarter-both-ties") << QSize(10, 14) << 4 << QSize(2, 4);
  QTest::newRow("eighth-both-ties") << QSize(20, 28) << 8 << QSize(2, 4);
  QTest::newRow("tiny-not-silently-clamped") << QSize(1, 2) << 8 << QSize(0, 0);
}

void TrainingInputSummaryTests::roundsDimensionsLikePython() {
  QFETCH(QSize, source);
  QFETCH(int, ratio);
  QFETCH(QSize, projected);
  QCOMPARE(projectedTrainingImageSize(source, ratio), projected);
}

void TrainingInputSummaryTests::avoidsDimensionOverflowAndRejectsUnsupportedRatios() {
  const int maximum = std::numeric_limits<int>::max();
  QCOMPARE(projectedTrainingImageSize({maximum, maximum}, 1), QSize(maximum, maximum));
  QCOMPARE(projectedTrainingImageSize({maximum, maximum}, 8), QSize(268435456, 268435456));
  QCOMPARE(projectedTrainingImageSize({5, 7}, 0), QSize());
  QCOMPARE(projectedTrainingImageSize({5, 7}, 3), QSize());
  QCOMPARE(projectedTrainingImageSize({-1, 7}, 2), QSize());
}

void TrainingInputSummaryTests::doesNotFollowLinksOutsideDatasetOrIntoLoops() {
  QTemporaryDir temporary;
  QTemporaryDir outside;
  QVERIFY(temporary.isValid());
  QVERIFY(outside.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("images")));
  QVERIFY(writeImage(root.filePath(QStringLiteral("images/valid.png")), {10, 8}));
  QVERIFY(writeImage(QDir(outside.path()).filePath(QStringLiteral("foreign.png")), {99, 77}));
  if (!directoryLink(outside.path(), root.filePath(QStringLiteral("images/outside")))) {
    QSKIP("Creating directory symlinks is unavailable on this test machine.");
  }
  QVERIFY(directoryLink(root.path(), root.filePath(QStringLiteral("images/loop"))));
  const auto result = scanTrainingInputSummary(root.path(), false);
  QCOMPARE(result.imageCount, qint64(1));
  QCOMPARE(result.skippedLinkCount, qint64(2));
}

void TrainingInputSummaryTests::respectsCancellation() {
  auto cancellation = std::make_shared<std::atomic_bool>(true);
  const auto cancelled = scanTrainingInputSummary(QString(), true, cancellation);
  QVERIFY(cancelled.cancelled);
  QCOMPARE(cancelled.imageCount, qint64(0));

  TrainingInputSummaryScanner scanner;
  QSignalSpy ready(&scanner, &TrainingInputSummaryScanner::summaryReady);
  scanner.request(QString(), true);
  scanner.cancel();
  QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
  QCoreApplication::processEvents();
  QCOMPARE(ready.count(), 0);
}

void TrainingInputSummaryTests::dropsStaleRequestsAndCachesHeaders() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("input")));
  QVERIFY(root.mkpath(QStringLiteral("images")));
  QVERIFY(writeImage(root.filePath(QStringLiteral("input/raw.png")), {30, 20}));
  const QString prepared = root.filePath(QStringLiteral("images/prepared.png"));
  QVERIFY(writeImage(prepared, {16, 9}));
  TrainingInputSummaryScanner scanner;
  QSignalSpy ready(&scanner, &TrainingInputSummaryScanner::summaryReady);
  scanner.request(root.path(), true);
  scanner.request(root.path(), false);
  QTRY_COMPARE(ready.count(), 1);
  const auto newest = qvariant_cast<TrainingInputSummary>(ready.at(0).at(0));
  QCOMPARE(newest.sourceDirectory, root.filePath(QStringLiteral("images")));
  QCOMPARE(newest.sizeGroups.front().sourceSize, QSize(16, 9));
  QVERIFY(QFile::remove(prepared));
  scanner.request(root.path(), false);
  QTRY_COMPARE(ready.count(), 2);
  const auto cached = qvariant_cast<TrainingInputSummary>(ready.at(1).at(0));
  QCOMPARE(cached.sizeGroups.front().sourceSize, QSize(16, 9));
  QCOMPARE(cached.imageCount, qint64(1));
}

void TrainingInputSummaryTests::destructionDoesNotWaitForOrAccessTheOwner() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("images")));
  for (int index = 0; index < 64; ++index) {
    QVERIFY(writeImage(root.filePath(QStringLiteral("images/%1.png")).arg(index), {32, 24}));
  }
  auto *scanner = new TrainingInputSummaryScanner;
  QSignalSpy ready(scanner, &TrainingInputSummaryScanner::summaryReady);
  scanner->request(root.path(), false);
  delete scanner;
  QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
  QCoreApplication::processEvents();
  QCOMPARE(ready.count(), 0);
}

QTEST_GUILESS_MAIN(TrainingInputSummaryTests)

#include "TrainingInputSummaryTests.moc"
