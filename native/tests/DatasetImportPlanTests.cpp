#include "DatasetImportPlan.h"
#include "MediaProjectBootstrap.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace gsw;

namespace {
bool writeBytes(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
} // namespace

class DatasetImportPlanTests final : public QObject {
  Q_OBJECT

private slots:
  void collectsSupportedMediaFromFilesAndFolders();
  void writesWorkerConfigurationWithPortableSourceMetadata();
  void disambiguatesRelativePathsFromSameNamedDirectories();
  void keepsFileNamesForLooseFileSelections();
  void rejectsRequestsWithoutSupportedMedia();
  void rejectsUnsafeWindowsSceneNames_data();
  void rejectsUnsafeWindowsSceneNames();
  void suggestsSceneNamesFromFoldersAndVideos();
  void usesMeaningfulParentForGenericImageFolders();
};

void DatasetImportPlanTests::collectsSupportedMediaFromFilesAndFolders() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());

  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("capture/nested")));
  const QString imagePath = root.filePath(QStringLiteral("capture/frame.jpg"));
  const QString videoPath = root.filePath(QStringLiteral("capture/nested/walk.MP4"));
  const QString ignoredPath = root.filePath(QStringLiteral("capture/readme.txt"));
  QVERIFY(writeBytes(imagePath, QByteArrayLiteral("image")));
  QVERIFY(writeBytes(videoPath, QByteArrayLiteral("video-data")));
  QVERIFY(writeBytes(ignoredPath, QByteArrayLiteral("ignored")));

  DatasetImportRequest request;
  request.sceneName = QStringLiteral("managed-capture");
  request.sourcePaths = {root.filePath(QStringLiteral("capture")), imagePath};
  request.framesPerSecond = 2.5;

  QString error;
  const auto plan = DatasetImportPlan::create(request, &error);
  QVERIFY2(plan.has_value(), qPrintable(error));
  QCOMPARE(plan->sceneName(), QStringLiteral("managed-capture"));
  QCOMPARE(plan->sourceCount(), 2);
  QCOMPARE(plan->imageCount(), 1);
  QCOMPARE(plan->videoCount(), 1);
  QCOMPARE(plan->totalBytes(), qint64(15));
}

void DatasetImportPlanTests::writesWorkerConfigurationWithPortableSourceMetadata() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());

  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("capture/nested")));
  const QString imagePath = root.filePath(QStringLiteral("capture/frame.jpg"));
  const QString videoPath = root.filePath(QStringLiteral("capture/nested/walk.mp4"));
  QVERIFY(writeBytes(imagePath, QByteArrayLiteral("image")));
  QVERIFY(writeBytes(videoPath, QByteArrayLiteral("video-data")));

  DatasetImportRequest request;
  request.sceneName = QStringLiteral("managed-capture");
  request.sourcePaths = {root.filePath(QStringLiteral("capture"))};
  request.framesPerSecond = 3.0;
  request.overwrite = true;

  QString error;
  const auto plan = DatasetImportPlan::create(request, &error);
  QVERIFY2(plan.has_value(), qPrintable(error));

  const QString configPath = root.filePath(QStringLiteral("jobs/import.json"));
  const QString repositoryRoot = root.filePath(QStringLiteral("repository"));
  const QString datasetRoot = root.filePath(QStringLiteral("project/datasets"));
  QVERIFY2(plan->writeWorkerConfiguration(configPath, repositoryRoot, datasetRoot, &error),
           qPrintable(error));
  QCOMPARE(plan->managedDatasetPath(datasetRoot),
           QDir(datasetRoot).filePath(QStringLiteral("managed-capture")));

  QFile configFile(configPath);
  QVERIFY(configFile.open(QIODevice::ReadOnly));
  const QJsonObject config = QJsonDocument::fromJson(configFile.readAll()).object();
  QCOMPARE(config.value(QStringLiteral("task")).toString(), QStringLiteral("import"));
  QCOMPARE(config.value(QStringLiteral("scene")).toString(), QStringLiteral("managed-capture"));
  QCOMPARE(config.value(QStringLiteral("fps")).toDouble(), 3.0);
  QCOMPARE(config.value(QStringLiteral("overwrite")).toBool(), true);
  QCOMPARE(config.value(QStringLiteral("repositoryRoot")).toString(),
           QDir::cleanPath(QFileInfo(repositoryRoot).absoluteFilePath()));
  QCOMPARE(config.value(QStringLiteral("datasetRoot")).toString(),
           QDir::cleanPath(QFileInfo(datasetRoot).absoluteFilePath()));
  QCOMPARE(config.value(QStringLiteral("projectRoot")).toString(),
           QDir::cleanPath(QFileInfo(datasetRoot).absolutePath()));

  const QJsonArray files = config.value(QStringLiteral("files")).toArray();
  QCOMPARE(files.size(), 2);
  QSet<QString> relativePaths;
  QHash<QString, QString> mediaTypesByName;
  for (const QJsonValue &value : files) {
    const QJsonObject file = value.toObject();
    QVERIFY(QFileInfo(file.value(QStringLiteral("path")).toString()).isAbsolute());
    QVERIFY(!file.value(QStringLiteral("name")).toString().isEmpty());
    QVERIFY(!file.value(QStringLiteral("type")).toString().isEmpty());
    QVERIFY(file.value(QStringLiteral("size")).toInteger() > 0);
    QVERIFY(file.value(QStringLiteral("lastModified")).toInteger() > 0);
    relativePaths.insert(file.value(QStringLiteral("relativePath")).toString());
    mediaTypesByName.insert(file.value(QStringLiteral("name")).toString(),
                            file.value(QStringLiteral("type")).toString());
  }
  QCOMPARE(relativePaths,
           QSet<QString>({QStringLiteral("capture/frame.jpg"),
                          QStringLiteral("capture/nested/walk.mp4")}));
  QCOMPARE(mediaTypesByName.value(QStringLiteral("frame.jpg")),
           QStringLiteral("image/jpeg"));
  QCOMPARE(mediaTypesByName.value(QStringLiteral("walk.mp4")),
           QStringLiteral("video/mp4"));
}

void DatasetImportPlanTests::disambiguatesRelativePathsFromSameNamedDirectories() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());

  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("camera-a/capture/nested")));
  QVERIFY(root.mkpath(QStringLiteral("camera-b/capture/nested")));
  QVERIFY(writeBytes(root.filePath(QStringLiteral("camera-a/capture/nested/frame.jpg")),
                     QByteArrayLiteral("first")));
  QVERIFY(writeBytes(root.filePath(QStringLiteral("camera-b/capture/nested/frame.jpg")),
                     QByteArrayLiteral("second")));

  DatasetImportRequest request;
  request.sceneName = QStringLiteral("two-cameras");
  request.sourcePaths = {
      root.filePath(QStringLiteral("camera-a/capture")),
      root.filePath(QStringLiteral("camera-b/capture")),
  };

  QString error;
  const auto plan = DatasetImportPlan::create(request, &error);
  QVERIFY2(plan.has_value(), qPrintable(error));

  const QString configPath = root.filePath(QStringLiteral("jobs/import.json"));
  QVERIFY2(plan->writeWorkerConfiguration(
               configPath, root.filePath(QStringLiteral("repository")),
               root.filePath(QStringLiteral("project/datasets")), &error),
           qPrintable(error));

  QFile configFile(configPath);
  QVERIFY(configFile.open(QIODevice::ReadOnly));
  const QJsonArray files =
      QJsonDocument::fromJson(configFile.readAll())
          .object()
          .value(QStringLiteral("files"))
          .toArray();
  QCOMPARE(files.size(), 2);
  QSet<QString> relativePaths;
  for (const QJsonValue &value : files) {
    relativePaths.insert(
        value.toObject().value(QStringLiteral("relativePath")).toString());
  }
  QCOMPARE(relativePaths,
           QSet<QString>({QStringLiteral("capture/nested/frame.jpg"),
                          QStringLiteral("capture_2/nested/frame.jpg")}));
}

void DatasetImportPlanTests::keepsFileNamesForLooseFileSelections() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());

  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("camera-a")));
  QVERIFY(root.mkpath(QStringLiteral("camera-b")));
  const QString imagePath =
      root.filePath(QStringLiteral("camera-a/frame.jpg"));
  const QString videoPath =
      root.filePath(QStringLiteral("camera-b/walk.mp4"));
  QVERIFY(writeBytes(imagePath, QByteArrayLiteral("image")));
  QVERIFY(writeBytes(videoPath, QByteArrayLiteral("video")));

  DatasetImportRequest request;
  request.sceneName = QStringLiteral("loose-files");
  request.sourcePaths = {imagePath, videoPath};

  QString error;
  const auto plan = DatasetImportPlan::create(request, &error);
  QVERIFY2(plan.has_value(), qPrintable(error));

  const QString configPath = root.filePath(QStringLiteral("jobs/import.json"));
  QVERIFY2(plan->writeWorkerConfiguration(
               configPath, root.filePath(QStringLiteral("repository")),
               root.filePath(QStringLiteral("project/datasets")), &error),
           qPrintable(error));

  QFile configFile(configPath);
  QVERIFY(configFile.open(QIODevice::ReadOnly));
  const QJsonArray files =
      QJsonDocument::fromJson(configFile.readAll())
          .object()
          .value(QStringLiteral("files"))
          .toArray();
  QSet<QString> relativePaths;
  for (const QJsonValue &value : files) {
    relativePaths.insert(
        value.toObject().value(QStringLiteral("relativePath")).toString());
  }
  QCOMPARE(relativePaths,
           QSet<QString>({QStringLiteral("frame.jpg"),
                          QStringLiteral("walk.mp4")}));
}

void DatasetImportPlanTests::rejectsRequestsWithoutSupportedMedia() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString ignoredPath = QDir(temporary.path()).filePath(QStringLiteral("notes.txt"));
  QVERIFY(writeBytes(ignoredPath, QByteArrayLiteral("not media")));

  DatasetImportRequest request;
  request.sceneName = QStringLiteral("empty-capture");
  request.sourcePaths = {ignoredPath};

  QString error;
  const auto plan = DatasetImportPlan::create(request, &error);
  QVERIFY(!plan.has_value());
  QVERIFY(error.contains(QStringLiteral("image"), Qt::CaseInsensitive));
  QVERIFY(error.contains(QStringLiteral("video"), Qt::CaseInsensitive));
}

void DatasetImportPlanTests::rejectsUnsafeWindowsSceneNames_data() {
  QTest::addColumn<QString>("sceneName");
  QTest::newRow("current-directory") << QStringLiteral(".");
  QTest::newRow("reserved-con") << QStringLiteral("CON");
  QTest::newRow("reserved-with-extension") << QStringLiteral("nul.capture");
  QTest::newRow("reserved-com-port") << QStringLiteral("COM1");
  QTest::newRow("reserved-lpt-port") << QStringLiteral("lpt9");
  QTest::newRow("trailing-dot") << QStringLiteral("capture.");
  QTest::newRow("excessive-length") << QString(121, QLatin1Char('a'));
}

void DatasetImportPlanTests::rejectsUnsafeWindowsSceneNames() {
  QFETCH(QString, sceneName);
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString imagePath = QDir(temporary.path()).filePath(QStringLiteral("frame.jpg"));
  QVERIFY(writeBytes(imagePath, QByteArrayLiteral("image")));

  DatasetImportRequest request;
  request.sceneName = sceneName;
  request.sourcePaths = {imagePath};

  QString error;
  QVERIFY(!DatasetImportPlan::create(request, &error).has_value());
  QVERIFY(!error.isEmpty());
}

void DatasetImportPlanTests::suggestsSceneNamesFromFoldersAndVideos() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("room-capture")));
  const QString videoPath = root.filePath(QStringLiteral("walkthrough.mp4"));
  QVERIFY(writeBytes(videoPath, QByteArrayLiteral("video")));

  QCOMPARE(suggestedMediaSceneName({videoPath}),
           QStringLiteral("walkthrough"));
  QCOMPARE(suggestedMediaSceneName(
               {root.filePath(QStringLiteral("room-capture"))}),
           QStringLiteral("room-capture"));
}

void DatasetImportPlanTests::usesMeaningfulParentForGenericImageFolders() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  QVERIFY(root.mkpath(QStringLiteral("temple/source/originals/images")));
  const QString imagePath = root.filePath(
      QStringLiteral("temple/source/originals/images/frame.jpg"));
  QVERIFY(writeBytes(imagePath, QByteArrayLiteral("image")));

  QCOMPARE(suggestedMediaProjectName({imagePath}), QStringLiteral("temple"));
  QCOMPARE(suggestedMediaSceneName({imagePath}), QStringLiteral("temple"));
}

QTEST_GUILESS_MAIN(DatasetImportPlanTests)

#include "DatasetImportPlanTests.moc"
