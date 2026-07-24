#include "DurableArtifactStore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class DurableArtifactStoreTests final : public QObject {
  Q_OBJECT

private slots:
  void publishesAndRejectsCorruptedArtifacts();
};

void DurableArtifactStoreTests::publishesAndRejectsCorruptedArtifacts() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QDir root(temporary.path());
  const QString source = root.filePath(QStringLiteral("source.bin"));
  const QString destination =
      root.filePath(QStringLiteral("published/artifact.bin"));

  QFile sourceFile(source);
  QVERIFY(sourceFile.open(QIODevice::WriteOnly));
  QCOMPARE(sourceFile.write("first-version"), qint64(13));
  sourceFile.close();

  QString error;
  const gsw::DurableArtifact first =
      gsw::DurableArtifactStore::publish(source, destination, &error);
  QVERIFY2(first.isValid(), qPrintable(error));
  QCOMPARE(first.path, QDir::cleanPath(destination));
  QCOMPARE(first.size, qint64(13));
  QVERIFY2(gsw::DurableArtifactStore::verify(destination, &error),
           qPrintable(error));

  QFile corrupted(destination);
  QVERIFY(corrupted.open(QIODevice::Append));
  QCOMPARE(corrupted.write("broken"), qint64(6));
  corrupted.close();
  QVERIFY(!gsw::DurableArtifactStore::verify(destination, &error));
  QVERIFY(!error.isEmpty());

  QVERIFY(sourceFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QCOMPARE(sourceFile.write("second-version"), qint64(14));
  sourceFile.close();
  const gsw::DurableArtifact repaired =
      gsw::DurableArtifactStore::publish(source, destination, &error);
  QVERIFY2(repaired.isValid(), qPrintable(error));
  QCOMPARE(repaired.size, qint64(14));
  QVERIFY(repaired.sha256 != first.sha256);
  QVERIFY2(gsw::DurableArtifactStore::verify(destination, &error),
           qPrintable(error));
}

QTEST_GUILESS_MAIN(DurableArtifactStoreTests)
#include "DurableArtifactStoreTests.moc"
