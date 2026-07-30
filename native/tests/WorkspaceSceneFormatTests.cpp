#include "WorkspaceDocument.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class WorkspaceSceneFormatTests final : public QObject {
  Q_OBJECT

private slots:
  void acceptsBigEndianPlySceneMetadata();
  void countsManagedImagesWithoutCountingArchivedOriginals();
  void fallsBackToInputWhenImagesDirectoryIsEmpty();
  void prefersInputWhenBothStandardDirectoriesContainImages();
  void rejectsNestedOnlyImagesThatTrainingCannotConsume();
};

void WorkspaceSceneFormatTests::acceptsBigEndianPlySceneMetadata() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString plyPath =
      QDir(temporary.path()).filePath(QStringLiteral("big-endian.ply"));
  QFile ply(plyPath);
  QVERIFY(ply.open(QIODevice::WriteOnly));
  ply.write("ply\n"
            "format binary_big_endian 1.0\n"
            "element vertex 1\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "element face 1\n"
            "property list uchar int vertex_indices\n"
            "end_header\n");
  ply.close();

  gsw::WorkspaceDocument document;
  QString error;
  QVERIFY(document.create(temporary.path(), &error));
  QVERIFY2(document.setScenePath(plyPath, &error), qPrintable(error));
  QCOMPARE(document.scenePath(), QDir::cleanPath(plyPath));
  QCOMPARE(document.sceneMetadata().format,
           QStringLiteral("binary_big_endian"));
  QCOMPARE(document.sceneMetadata().vertexCount, 1);
  QCOMPARE(document.sceneMetadata().faceCount, 1);
  QVERIFY(document.sceneMetadata().looksLikeMesh());
}

void WorkspaceSceneFormatTests::countsManagedImagesWithoutCountingArchivedOriginals() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir dataset(temporary.path());
  QVERIFY(dataset.mkpath(QStringLiteral("images")));
  QVERIFY(dataset.mkpath(QStringLiteral("source/originals")));

  QFile trainingImage(dataset.filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(trainingImage.open(QIODevice::WriteOnly));
  QCOMPARE(trainingImage.write("training"), qint64(8));
  trainingImage.close();

  QFile archivedOriginal(
      dataset.filePath(QStringLiteral("source/originals/frame.jpg")));
  QVERIFY(archivedOriginal.open(QIODevice::WriteOnly));
  QCOMPARE(archivedOriginal.write("original"), qint64(8));
  archivedOriginal.close();

  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset.path()), qint64(1));
}

void WorkspaceSceneFormatTests::fallsBackToInputWhenImagesDirectoryIsEmpty() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir dataset(temporary.path());
  QVERIFY(dataset.mkpath(QStringLiteral("images")));
  QVERIFY(dataset.mkpath(QStringLiteral("input")));

  QFile inputImage(dataset.filePath(QStringLiteral("input/frame.jpg")));
  QVERIFY(inputImage.open(QIODevice::WriteOnly));
  QCOMPARE(inputImage.write("input"), qint64(5));
  inputImage.close();

  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset.path()), qint64(1));
}

void WorkspaceSceneFormatTests::prefersInputWhenBothStandardDirectoriesContainImages() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir dataset(temporary.path());
  QVERIFY(dataset.mkpath(QStringLiteral("images")));
  QVERIFY(dataset.mkpath(QStringLiteral("input")));

  QFile imagesFrame(dataset.filePath(QStringLiteral("images/frame.jpg")));
  QVERIFY(imagesFrame.open(QIODevice::WriteOnly));
  QCOMPARE(imagesFrame.write("images"), qint64(6));
  imagesFrame.close();
  for (const QString &name : {QStringLiteral("first.jpg"),
                              QStringLiteral("second.png")}) {
    QFile inputFrame(dataset.filePath(QStringLiteral("input/") + name));
    QVERIFY(inputFrame.open(QIODevice::WriteOnly));
    QCOMPARE(inputFrame.write("input"), qint64(5));
  }

  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset.path()), qint64(2));
}

void WorkspaceSceneFormatTests::rejectsNestedOnlyImagesThatTrainingCannotConsume() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir dataset(temporary.path());
  QVERIFY(dataset.mkpath(QStringLiteral("images/nested")));

  QFile nestedFrame(dataset.filePath(QStringLiteral("images/nested/frame.jpg")));
  QVERIFY(nestedFrame.open(QIODevice::WriteOnly));
  QCOMPARE(nestedFrame.write("nested"), qint64(6));
  nestedFrame.close();

  QCOMPARE(gsw::WorkspaceDocument::countDatasetImages(dataset.path()), qint64(0));
}

QTEST_GUILESS_MAIN(WorkspaceSceneFormatTests)

#include "WorkspaceSceneFormatTests.moc"
