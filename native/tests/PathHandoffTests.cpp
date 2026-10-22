#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

bool writeJson(const QString &path, const QJsonObject &value) {
  QSaveFile file(path);
  const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QJsonObject readJson(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}

QString hashFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash digest(QCryptographicHash::Sha256);
  return digest.addData(&file) ? QString::fromLatin1(digest.result().toHex()) : QString();
}

bool writeLog(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size();
}

bool run(const QString &program, const QStringList &arguments, const QString &workingDirectory,
         const QString &logPrefix, QJsonObject *record, const int deadline = 30000) {
  QProcess process;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
  environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
  environment.remove(QStringLiteral("QT_PLUGIN_PATH"));
  environment.remove(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"));
  process.setProcessEnvironment(environment);
  process.setWorkingDirectory(workingDirectory);
#ifdef Q_OS_WIN
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *parameters) {
    parameters->flags |= CREATE_NO_WINDOW;
    parameters->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
    parameters->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  QElapsedTimer timer;
  timer.start();
  process.start(program, arguments);
  const bool started = process.waitForStarted(5000);
  const bool completed = started && process.waitForFinished(deadline);
  if (started && !completed) { process.kill(); process.waitForFinished(5000); }
  const auto output = process.readAllStandardOutput();
  const auto error = process.readAllStandardError();
  const bool logsSaved = writeLog(logPrefix + QStringLiteral(".stdout.log"), output) &&
                         writeLog(logPrefix + QStringLiteral(".stderr.log"), error);
  *record = QJsonObject{{QStringLiteral("program"), program},
      {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
      {QStringLiteral("started"), started}, {QStringLiteral("completed"), completed},
      {QStringLiteral("exitCode"), process.exitCode()},
      {QStringLiteral("normalExit"), process.exitStatus() == QProcess::NormalExit},
      {QStringLiteral("elapsedMs"), timer.elapsed()}, {QStringLiteral("logsSaved"), logsSaved},
      {QStringLiteral("stdoutPath"), logPrefix + QStringLiteral(".stdout.log")},
      {QStringLiteral("stderrPath"), logPrefix + QStringLiteral(".stderr.log")}};
  if (!completed || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    std::fprintf(stderr, "Path handoff child failed: %s\n", error.right(3000).constData());
  return completed && logsSaved && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool makeSource(const QString &sourceRoot, QJsonObject *hashes) {
  QJsonArray views;
  const QStringList splits{QStringLiteral("train"), QStringLiteral("train"),
                           QStringLiteral("val"), QStringLiteral("test")};
  for (int index = 0; index < splits.size(); ++index) {
    const QString relative = QStringLiteral("%1/画像 %2.png").arg(splits[index]).arg(index);
    const QString path = QDir(sourceRoot).filePath(relative);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QImage image(128, 96, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x) {
        const unsigned int value = static_cast<unsigned int>((x * 73856093U) ^ (y * 19349663U) ^
            ((x * y + index * 41) * 83492791U));
        image.setPixel(x, y, qRgb((value >> 7) & 255, (value >> 16) & 255, (value >> 23) & 255));
      }
    if (!image.save(path, "PNG")) return false;
    hashes->insert(path, hashFile(path));
    const QJsonArray pose{QJsonArray{1., 0., 0., index * .013},
                          QJsonArray{0., 1., 0., index * -.007},
                          QJsonArray{0., 0., 1., -.47}, QJsonArray{0., 0., 0., 1.}};
    views.append(QJsonObject{{QStringLiteral("view_id"), QStringLiteral("path-view-%1").arg(index)},
        {QStringLiteral("image_path"), relative}, {QStringLiteral("split"), splits[index]},
        {QStringLiteral("K"), QJsonArray{QJsonArray{160., 0., 64.}, QJsonArray{0., 150., 48.},
                                         QJsonArray{0., 0., 1.}}},
        {QStringLiteral("pixel_coordinates"), QStringLiteral("pixel_border")},
        {QStringLiteral("c2w_opencv"), pose}});
  }
  const QString manifest = QDir(sourceRoot).filePath(QStringLiteral("camera_dataset.json"));
  if (!writeJson(manifest, QJsonObject{{QStringLiteral("schema"), QStringLiteral("gsw.camera_dataset/1")},
      {QStringLiteral("pose_policy"), QStringLiteral("locked")},
      {QStringLiteral("source"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("synthetic")}}},
      {QStringLiteral("scale"), QJsonObject{{QStringLiteral("status"), QStringLiteral("arbitrary")}}},
      {QStringLiteral("views"), views}})) return false;
  hashes->insert(manifest, hashFile(manifest));
  return true;
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  const auto arguments = application.arguments();
  if (arguments.size() < 5 || arguments.size() > 6) {
    std::fprintf(stderr, "usage: PathHandoffTests <repository> <python> <colmap> <owned-output-root> [backend-root]\n");
    return 2;
  }
  const QString repository = QFileInfo(arguments[1]).absoluteFilePath();
  const QString python = QFileInfo(arguments[2]).absoluteFilePath();
  const QString colmap = QFileInfo(arguments[3]).absoluteFilePath();
  const QString outputRoot = QFileInfo(arguments[4]).absoluteFilePath();
  const QString backend = arguments.size() == 6 ? QFileInfo(arguments[5]).absoluteFilePath() : repository;
  const QString helper = QDir(repository).filePath(QStringLiteral("native/tests/helpers/PathHandoffFixture.py"));
  if (!QFileInfo::exists(python) || !QFileInfo::exists(colmap) || !QFileInfo::exists(helper) ||
      !QDir().mkpath(outputRoot)) return 2;
  QTemporaryDir runDirectory(QDir(outputRoot).filePath(QStringLiteral("run-XXXXXXXXXXXX")));
  if (!runDirectory.isValid()) return 2;
  runDirectory.setAutoRemove(false); // Keep only this uniquely owned run as evidence.
  QJsonObject sourceHashes;
  const QString source = runDirectory.filePath(QStringLiteral("source"));
  if (!makeSource(source, &sourceHashes)) return 2;
  bool passed = true;
  int checks = 0;
  QJsonArray cases;
  const auto check = [&passed, &checks](const bool condition, const char *label) {
    ++checks; passed &= condition;
    std::printf("Path handoff %s: %s\n", condition ? "PASS" : "FAIL", label);
    std::fflush(stdout);
  };
  for (const bool deep : {false, true}) {
    const QString label = deep ? QStringLiteral("deep-unicode") : QStringLiteral("short");
    const QString caseRoot = runDirectory.filePath(label);
    if (!QDir().mkpath(caseRoot)) return 2;
    QString parent = deep ? QDir(caseRoot).filePath(QStringLiteral("日本語 空白")) : caseRoot;
    while (deep && parent.size() < 123) parent = QDir(parent).filePath(QStringLiteral("階層-abcdefghij"));
    const QString destination = QDir(parent).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')));
    const QString reportPath = QDir(caseRoot).filePath(QStringLiteral("prepare.json"));
    QJsonObject caseResult{{QStringLiteral("case"), label}, {QStringLiteral("destination"), destination},
                          {QStringLiteral("parentUtf16Units"), parent.size()}};
    QJsonObject processRecord;
    const bool prepared = run(python, {QStringLiteral("-I"), QStringLiteral("-B"), helper,
        QStringLiteral("prepare"), QStringLiteral("--backend-root"), backend,
        QStringLiteral("--source"), QDir(source).filePath(QStringLiteral("camera_dataset.json")),
        QStringLiteral("--destination"), destination, QStringLiteral("--report"), reportPath},
        repository, QDir(caseRoot).filePath(QStringLiteral("prepare")), &processRecord);
    caseResult.insert(QStringLiteral("prepareProcess"), processRecord);
    check(prepared, "Qt QProcess prepares a train-only solve version through the real Python API");
    const auto preparation = readJson(reportPath);
    caseResult.insert(QStringLiteral("preparation"), preparation);
    if (!prepared || !preparation.value(QStringLiteral("success")).toBool()) {
      cases.append(caseResult); continue;
    }
#ifdef Q_OS_WIN
    check(preparation.value(QStringLiteral("longPathsEnabled")).toInt(-1) == 0,
          "the actual Windows run has LongPathsEnabled disabled");
#endif
    check(!deep || parent.size() >= 123, "deep Unicode case exercises the historical parent-path scale");
    check(preparation.value(QStringLiteral("originalsUnchanged")).toBool(), "source image and camera metadata hashes remain unchanged");
    const auto files = preparation.value(QStringLiteral("files")).toArray();
    bool qtFilesOkay = files.size() == 2;
    QSet<QString> preparedViewIds;
    for (const auto &entry : files) {
      const auto item = entry.toObject();
      preparedViewIds.insert(item.value(QStringLiteral("viewId")).toString());
      const QString imagePath = QDir(destination).filePath(item.value(QStringLiteral("inputImagePath")).toString());
      const QString copyPath = QDir(caseRoot).filePath(QFileInfo(imagePath).fileName());
      const QString originalPath = QDir(source).filePath(item.value(QStringLiteral("sourceImagePath")).toString());
      const QImage loaded(imagePath);
      qtFilesOkay &= QFile::copy(imagePath, copyPath) && !loaded.isNull() && loaded.size() == QSize(128, 96) &&
          item.value(QStringLiteral("sourceSplit")).toString() == QStringLiteral("train") &&
          sourceHashes.value(originalPath) == item.value(QStringLiteral("sha256")) &&
          hashFile(imagePath) == item.value(QStringLiteral("sha256")).toString() &&
          hashFile(copyPath) == item.value(QStringLiteral("sha256")).toString();
    }
    qtFilesOkay &= preparedViewIds == QSet<QString>{QStringLiteral("path-view-0"), QStringLiteral("path-view-1")};
    check(qtFilesOkay, "Qt image read and file copy preserve prepared image dimensions and bytes");
    const QString database = QDir(destination).filePath(QStringLiteral("feature-db.sqlite"));
    const bool extracted = run(colmap, {QStringLiteral("feature_extractor"),
        QStringLiteral("--database_path"), database,
        QStringLiteral("--image_path"), QDir(destination).filePath(QStringLiteral("input")),
        QStringLiteral("--ImageReader.camera_model"), QStringLiteral("PINHOLE"),
        QStringLiteral("--FeatureExtraction.use_gpu"), QStringLiteral("0"),
        QStringLiteral("--FeatureExtraction.num_threads"), QStringLiteral("2"),
        QStringLiteral("--FeatureExtraction.max_image_size"), QStringLiteral("128"),
        QStringLiteral("--SiftExtraction.max_num_features"), QStringLiteral("256")},
        caseRoot, QDir(caseRoot).filePath(QStringLiteral("colmap")), &processRecord, 30000);
    caseResult.insert(QStringLiteral("colmapProcess"), processRecord);
    check(extracted, "real COLMAP CPU feature extraction accepts Qt-delivered paths");
    if (extracted) {
      const QString databaseReport = QDir(caseRoot).filePath(QStringLiteral("database.json"));
      const bool inspected = run(python, {QStringLiteral("-I"), QStringLiteral("-B"), helper,
          QStringLiteral("inspect"), QStringLiteral("--preparation"), reportPath,
          QStringLiteral("--database"), database, QStringLiteral("--report"), databaseReport},
          repository, QDir(caseRoot).filePath(QStringLiteral("inspect")), &processRecord);
      caseResult.insert(QStringLiteral("databaseProcess"), processRecord);
      const auto inspectedReport = readJson(databaseReport);
      caseResult.insert(QStringLiteral("database"), inspectedReport);
      check(inspected && inspectedReport.value(QStringLiteral("success")).toBool(),
            "external database readback contains every train image and no validation/test images");
    }
    cases.append(caseResult);
  }
  for (auto it = sourceHashes.begin(); it != sourceHashes.end(); ++it)
    check(hashFile(it.key()) == it.value().toString(), "original Qt-created fixture file remains byte-identical after all external calls");
  const QJsonObject result{{QStringLiteral("schema"), QStringLiteral("gsw.path_handoff_acceptance/1")},
      {QStringLiteral("passed"), passed}, {QStringLiteral("checks"), checks},
      {QStringLiteral("backendRoot"), backend}, {QStringLiteral("repository"), repository},
      {QStringLiteral("runRoot"), runDirectory.path()}, {QStringLiteral("sourceHashes"), sourceHashes},
      {QStringLiteral("cases"), cases}, {QStringLiteral("scope"), QStringLiteral("cpu_feature_handoff_not_sfm_training_or_accuracy")}};
  if (!writeJson(runDirectory.filePath(QStringLiteral("RESULT.json")), result)) return 2;
  std::printf("Path handoff evidence: %s\n", runDirectory.filePath(QStringLiteral("RESULT.json")).toUtf8().constData());
  return passed ? 0 : 1;
}
