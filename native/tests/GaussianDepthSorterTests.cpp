#include "GaussianDepthSorter.h"
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

using namespace gsw;

class GaussianDepthSorterTests : public QObject {
  Q_OBJECT
private slots:
  void matchesComparisonSort() {
    std::mt19937 random(42);
    std::uniform_real_distribution<float> coordinate(-500.0F, 500.0F);
    QVector<PointCloudVertex> source(512202);
    for (qsizetype i = 0; i < source.size(); ++i) {
      auto &v = source[i];
      v.x = coordinate(random); v.y = coordinate(random); v.z = coordinate(random);
      v.sourceIndex = static_cast<quint32>(i);
      v.red = coordinate(random); v.opacity = 0.42F; v.scaleY = 0.123F; v.rotationZ = 0.789F;
    }
    GaussianDepthSorter sorter;
    for (const QVector3D direction : {QVector3D(1, 0, 0), QVector3D(0, -1, 0),
         QVector3D(0, 0, 1), QVector3D(0.27F, -0.8F, 0.45F)}) {
      auto expected = source;
      std::sort(expected.begin(), expected.end(), [&](const auto &a, const auto &b) {
        const float da = a.x * direction.x() + a.y * direction.y() + a.z * direction.z();
        const float db = b.x * direction.x() + b.y * direction.y() + b.z * direction.z();
        return da == db ? a.sourceIndex < b.sourceIndex : da > db;
      });
      auto actual = source;
      sorter.sort(actual, direction);
      QCOMPARE(actual.size(), source.size());
      for (qsizetype i = 0; i < actual.size(); ++i) {
        // All attributes must survive the gather unchanged, not just position.
        QCOMPARE(std::memcmp(&actual[i], &expected[i], sizeof(PointCloudVertex)), 0);
      }
    }
  }

  void tiesSignedZeroAndReorderedInput() {
    QVector<PointCloudVertex> source(12);
    for (qsizetype i = 0; i < source.size(); ++i) {
      source[i].x = i % 2 ? -0.0F : 0.0F;
      source[i].sourceIndex = static_cast<quint32>(source.size() - i);
    }
    GaussianDepthSorter sorter;
    sorter.sort(source, QVector3D(1, 0, 0));
    for (qsizetype i = 0; i < source.size(); ++i) QCOMPARE(source[i].sourceIndex, quint32(i + 1));
    // Repeated calls, changed topology and empty data must not retain old entries.
    source.remove(3, 6);
    sorter.sort(source, QVector3D(0, 0, 0));
    QCOMPARE(source.size(), 6);
    source.resize(1); sorter.sort(source, QVector3D(1, 0, 0));
    QCOMPARE(source[0].sourceIndex, 1U);
    source.clear(); sorter.sort(source, QVector3D(1, 0, 0));
    QVERIFY(source.isEmpty());
  }

  void invalidDepthsAreDeterministic() {
    QVector<PointCloudVertex> vertices(5);
    const float depths[] = {-std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(), 0.0F,
        std::numeric_limits<float>::infinity(), -1.0F};
    for (int i = 0; i < 5; ++i) { vertices[i].x = depths[i]; vertices[i].sourceIndex = i; }
    GaussianDepthSorter sorter; sorter.sort(vertices, QVector3D(1, 0, 0));
    const quint32 indices[] = {3, 2, 4, 0, 1};
    for (int i = 0; i < 5; ++i) QCOMPARE(vertices[i].sourceIndex, indices[i]);
  }
};

QTEST_APPLESS_MAIN(GaussianDepthSorterTests)
#include "GaussianDepthSorterTests.moc"
