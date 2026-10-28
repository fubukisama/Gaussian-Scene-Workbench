#include "UiFormGallerySmokeTest.h"

#include "MainWindow.h"
#include "NativeViewport.h"
#include "DatasetImportDialog.h"
#include "ReconstructionDialog.h"
#include "TrainingDialog.h"
#include "MeshGenerationDialog.h"
#include "ModelExportDialog.h"
#include "TaskDetailsDialog.h"
#include "TaskRecord.h"
#include "GenerationHistoryDialog.h"
#include "GenerationHistoryStore.h"
#include "QualityComparisonDialog.h"
#include "QualityMeasurementDialog.h"
#include "ScaleConstraintsDialog.h"
#include "InitializationDialog.h"
#include "TrainingBatchDialog.h"
#include "ExistingTrainingDialog.h"
#include "RuntimeGuardDialog.h"
#include "ResourceBudgetDialog.h"
#include "ViewportGridDialog.h"
#include "../tests/UiFormVisualProbe.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSharedPointer>
#include <QTemporaryDir>

#include <functional>

namespace gsw {
namespace {
QJsonArray vector(const QVector3D &v) { return {v.x(), v.y(), v.z()}; }
QJsonArray rotation(const QQuaternion &q) { return {q.scalar(), q.x(), q.y(), q.z()}; }
QJsonArray coordinate(const SceneCoordinate3D &v) { return {v.x, v.y, v.z}; }

QString digest(const QJsonObject &object) {
  return QString::fromLatin1(QCryptographicHash::hash(
      QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

QJsonObject viewportSnapshot(MainWindow &window, NativeViewport &viewport) {
  const auto angles = viewport.viewOrbitAngles();
  const auto policy = viewport.resourceBudgetPolicy();
  return {{"scenePath", viewport.scenePath()}, {"sceneCount", qint64(viewport.sceneObjectCount())},
      {"windowTitle", window.windowTitle()}, {"windowModified", window.isWindowModified()},
      {"target", vector(viewport.viewTarget())}, {"distance", viewport.viewDistance()},
      {"orbit", QJsonArray{angles.yawDegrees, angles.pitchDegrees, angles.rollDegrees}},
      {"translation", vector(viewport.modelTranslation())}, {"rotation", rotation(viewport.modelRotation())},
      {"scale", vector(viewport.modelScale())}, {"orthographic", viewport.orthographicProjection()},
      {"renderMode", int(viewport.renderMode())}, {"editLocked", viewport.editToolsLocked()},
      {"grid", viewport.viewportGridSettings().toJson().value_or(QJsonObject{})},
      {"coordinates", sceneCoordinateInfoToJson(viewport.sceneReferenceCoordinates())},
      {"budgetPolicy", QJsonObject{{"mode", int(policy.mode)}, {"ramMiB", policy.ramLimitMiB},
                                  {"gpuMiB", policy.gpuLimitMiB}}}};
}

bool noLinksToRoot(QFileInfo cursor) {
  for (;;) {
    if (!cursor.exists() || !cursor.isDir() || cursor.isSymLink() || cursor.isJunction()) return false;
    if (cursor.absoluteFilePath() == cursor.absolutePath()) return true;
    cursor = QFileInfo(cursor.absolutePath());
  }
}

bool writeOwned(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
         file.write(bytes) == bytes.size();
}

QJsonObject ownedFiles(const QString &root) {
  QJsonObject files;
  QDirIterator entries(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (entries.hasNext()) {
    const QString path = entries.next();
    QFile file(path);
    if (files.size() >= 64 || !file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
      return {{"unreadableOwnedFixture", true}};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {{"unreadableOwnedFixture", true}};
    files.insert(QDir(root).relativeFilePath(path), QString::fromLatin1(hash.result().toHex()));
  }
  return files;
}

QJsonObject importRequest(const DatasetImportRequest &r) {
  return {{"sceneName", r.sceneName}, {"sourcePaths", QJsonArray::fromStringList(r.sourcePaths)},
      {"framesPerSecond", r.framesPerSecond}, {"overwrite", r.overwrite}, {"cameraInput", r.cameraInput}};
}

QJsonObject reconstructionRequest(const ReconstructionConfiguration &r) {
  return {{"colmapExecutable", r.colmapExecutable}, {"preset", r.preset}, {"matching", r.matching},
      {"cameraModel", r.cameraModel}, {"featureMaxImageSize", r.featureMaxImageSize},
      {"featureMaxNumFeatures", r.featureMaxNumFeatures}, {"mapperMaxRuntimeSeconds", r.mapperMaxRuntimeSeconds},
      {"matcherGuided", r.matcherGuided}, {"useGpu", r.useGpu}, {"singleCamera", r.singleCamera},
      {"reset", r.reset}, {"qualityRecovery", r.qualityRecovery}};
}

QJsonObject exportRequest(const ModelExportOptions &r) {
  QJsonArray deleted;
  for (qsizetype index = 0; index < r.deletedVertices.size(); ++index) deleted.append(r.deletedVertices.testBit(index));
  return {{"sourcePath", r.sourcePath}, {"destinationPath", r.destinationPath}, {"format", int(r.format)},
      {"deletedVertices", deleted}, {"applyTransform", r.applyTransform},
      {"translation", vector(r.transform.translation)}, {"rotation", rotation(r.transform.rotation)},
      {"scale", vector(r.transform.scale)}, {"pivot", coordinate(r.pivot)},
      {"coordinates", sceneCoordinateInfoToJson(r.coordinates)}, {"sceneUnitScale", r.sceneUnitScale},
      {"sceneUnit", int(r.sceneUnit)}, {"sceneUnitDeclared", r.sceneUnitDeclared},
      {"spzVersion", r.spzVersion}, {"spzMaximumShDegree", r.spzMaximumShDegree}, {"spzQuality", r.spzQuality},
      {"cameraCoordinateRecovery", r.cameraCoordinateRecovery}, {"recordedRepresentation", r.recordedRepresentation},
      {"cameraOutputUnit", r.cameraOutputUnit}, {"recoveryPythonPath", r.recoveryPythonPath},
      {"recoveryRepositoryRoot", r.recoveryRepositoryRoot}, {"hasProgressCallback", bool(r.progress)}};
}

QJsonObject experiment(const GenerationExperiment &r) {
  return {{"id", r.id}, {"pipeline", r.pipeline}, {"backend", r.backend}, {"displayName", r.displayName},
      {"datasetPath", r.datasetPath}, {"configurationPath", r.configurationPath}, {"outputRoot", r.outputRoot},
      {"resultPath", r.resultPath}, {"resultSceneId", r.resultSceneId}, {"status", r.status},
      {"createdAt", r.createdAt.toString(Qt::ISODateWithMs)}, {"updatedAt", r.updatedAt.toString(Qt::ISODateWithMs)},
      {"parameters", r.parameters}};
}

bool quietOwnedProcesses(QWidget &dialog) {
  QElapsedTimer limit;
  limit.start();
  for (;;) {
    bool running = false;
    for (const QProcess *process : dialog.findChildren<QProcess *>())
      running = running || process->state() != QProcess::NotRunning;
    if (!running) return true;
    if (limit.elapsed() >= 5000) return false;
    tests::settleUiForm(20);
  }
}
} // namespace

bool runUiFormGallerySmokeTest(MainWindow &window) {
  const QString flag = QStringLiteral("--smoke-test-ui-form-gallery");
  const QString groupName = qEnvironmentVariable("GSW_UI_FORM_GALLERY_GROUP");
  const QString output = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
  const QString evidenceBase = qEnvironmentVariable("GSW_UI_SURFACE_EVIDENCE");
  const QString settings = qApp ? qApp->property("gswSmokeSettingsDirectory").toString() : QString();
  if (!qApp || qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QApplication::platformName() != QStringLiteral("windows") ||
      !qApp->arguments().contains(flag) || (groupName != QStringLiteral("1") && groupName != QStringLiteral("2")) ||
      !QDir::isAbsolutePath(output) || !noLinksToRoot(QFileInfo(output)) ||
      !QDir::isAbsolutePath(settings) || !noLinksToRoot(QFileInfo(settings)) ||
      !QDir::isAbsolutePath(evidenceBase) || !noLinksToRoot(QFileInfo(evidenceBase)) ||
      !qApp->findChild<QObject *>(QStringLiteral("gswUiSurfaceEvidenceAdapter"))) {
    qCritical("UI_FORM_GALLERY: remote/explicit/isolated/evidence gate refused");
    return false;
  }
  auto *viewport = window.findChild<NativeViewport *>();
  if (!viewport || !viewport->scenePath().isEmpty() || viewport->sceneObjectCount() > 1) {
    qCritical("UI_FORM_GALLERY: requires the owned empty main window");
    return false;
  }
  const QString processRoot = QDir(evidenceBase).filePath(
      QStringLiteral("process-") + QString::number(QCoreApplication::applicationPid()));
  const QString previousProcessRoot = qApp->property("gswUiFormEvidenceProcessRoot").toString();
  if (!QFileInfo(processRoot).isDir() || (!previousProcessRoot.isEmpty() && previousProcessRoot != processRoot)) {
    qCritical("UI_FORM_GALLERY: existing main-owned evidence namespace does not match this process");
    return false;
  }
  // main has installed the adapter; never attempt a second installation.
  qApp->setProperty("gswUiFormEvidenceProcessRoot", processRoot);
  const QJsonObject mainBefore = viewportSnapshot(window, *viewport);
  QTemporaryDir fixtures(QDir(output).filePath(QStringLiteral("ui-form-gallery-XXXXXX")));
  if (!fixtures.isValid()) return false;
  const QString dataset = fixtures.filePath(QStringLiteral("dataset"));
  const QString images = QDir(dataset).filePath(QStringLiteral("images"));
  const QString jobs = fixtures.filePath(QStringLiteral("jobs"));
  const QString generated = fixtures.filePath(QStringLiteral("output"));
  if (!QDir().mkpath(images) || !QDir().mkpath(jobs) || !QDir().mkpath(generated)) return false;
  QImage image(16, 12, QImage::Format_RGB32);
  image.fill(QColor(31, 61, 91));
  const QString photo = QDir(images).filePath(QString::fromUtf8("fixture_日本語.png"));
  if (!image.save(photo)) return false;
  const QString mesh = fixtures.filePath(QStringLiteral("owned-mesh.ply"));
  if (!writeOwned(mesh, QByteArray(
      "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face 2\n"
      "property list uchar int vertex_indices\nend_header\n0 0 0 128 160 192\n"
      "1 0 0 128 160 192\n1 1 0 128 160 192\n0 1 0 128 160 192\n3 0 1 2\n3 0 2 3\n"))) return false;
  const QString configuration = QDir(jobs).filePath(QStringLiteral("owned-configuration.json"));
  if (!writeOwned(configuration, QJsonDocument(QJsonObject{{"owned_gallery", true}}).toJson())) return false;
  GenerationExperiment history;
  history.id = QStringLiteral("gallery-owned-experiment");
  history.pipeline = QStringLiteral("training"); history.backend = QStringLiteral("3dgs");
  history.displayName = QString::fromUtf8("Owned gallery / 日本語 / 中文");
  history.datasetPath = dataset; history.configurationPath = configuration; history.outputRoot = generated;
  history.resultPath = mesh; history.resultSceneId = QStringLiteral("gallery-owned-result");
  history.status = QStringLiteral("failed"); history.parameters = {{"owned_gallery", true}, {"iterations", 17}};
  QString historyError;
  if (!GenerationHistoryStore(fixtures.path()).upsert(history, &historyError)) {
    qCritical() << "UI_FORM_GALLERY: owned history setup failed" << historyError;
    return false;
  }
  const QJsonObject filesBefore = ownedFiles(fixtures.path());
  if (filesBefore.contains(QStringLiteral("unreadableOwnedFixture")) || filesBefore.size() < 4) return false;
  const QString python = qEnvironmentVariable("GSW_QUALITY_TEST_PYTHON");
  // Idle constructors do not execute this path. Every data input is the owned
  // fixture; no camera identity or optimizer checkpoint is fabricated as valid.
  const QString backendRoot = fixtures.path();
  const auto environment = QProcessEnvironment::systemEnvironment();
  QJsonArray rows;
  bool passed = true;
  int dialogCount = 0;
  const auto probe = [&](const QString &id, QDialog &dialog,
                         const std::function<QJsonObject()> &getter,
                         const char *claim = "PUBLIC_GETTER_FROZEN", const int narrowWidth = 680) {
    ++dialogCount;
    if (dialog.objectName().isEmpty()) dialog.setObjectName(QStringLiteral("qaFormGallery_") + id);
    const bool quiet = quietOwnedProcesses(dialog);
    tests::settleUiForm(160); // Finish constructor-only CPU header scans before freezing.
    int accepted = 0, rejected = 0;
    const auto acceptedConnection = QObject::connect(&dialog, &QDialog::accepted, &dialog, [&] { ++accepted; });
    const auto rejectedConnection = QObject::connect(&dialog, &QDialog::rejected, &dialog, [&] { ++rejected; });
    const QJsonObject before = getter();
    const bool matrix = quiet && tests::captureUiFormMatrix(true, dialog,
        QStringLiteral("gallery-") + id, QSize(1100, 760), QSize(narrowWidth, 620), getter);
    const QJsonObject after = getter();
    const bool retained = before == after;
    const bool mainRetained = viewportSnapshot(window, *viewport) == mainBefore;
    const bool cpuQuiet = quietOwnedProcesses(dialog);
    dialog.hide();
    QObject::disconnect(acceptedConnection);
    QObject::disconnect(rejectedConnection);
    rows.append(QJsonObject{{"inventoryId", id}, {"actualClass", QString::fromLatin1(dialog.metaObject()->className())},
        {"claim", QString::fromLatin1(claim)}, {"publicSnapshotSha256Before", digest(before)},
        {"publicSnapshotSha256After", digest(after)}, {"publicSnapshotUnchanged", retained},
        {"matrixPassed", matrix}, {"mainPublicStateUnchanged", mainRetained},
        {"ownedProcessesQuiescent", quiet && cpuQuiet}, {"acceptedSignalCount", accepted},
        {"rejectedSignalCount", rejected},
        {"deepSemanticsDelegatedToExistingFamilies", true}});
    const bool okay = quiet && matrix && retained && mainRetained && cpuQuiet && accepted == 0 && rejected == 0;
    passed = okay && passed;
    qInfo().noquote() << "UI_FORM_GALLERY" << id << claim << (okay ? "PASS" : "FAIL");
  };

  if (groupName == QStringLiteral("1")) {
    { DatasetImportDialog dialog(images, QString::fromLatin1("owned-gallery-fixture"), {photo}, fixtures.path(), true, &window);
      probe(QStringLiteral("DLG-01"), dialog, [&] { return importRequest(dialog.request()); }); }
    { ReconstructionDialog dialog(dataset, backendRoot, &window);
      probe(QStringLiteral("DLG-02"), dialog, [&] { return reconstructionRequest(dialog.configuration()); }); }
    { TrainingDialog dialog(dataset, QString::fromLatin1("owned-gallery-fixture"), generated, false, true, &window);
      probe(QStringLiteral("DLG-03"), dialog, [&] { const auto r = dialog.configuration();
        return QJsonObject{{"request", dialog.configurationDocument()}, {"backend", r.backend},
          {"quality", r.quality}, {"outputRoot", r.outputRoot}, {"outputScene", r.outputScene},
          {"outputStorageName", r.outputStorageName}, {"iterations", r.iterations}, {"resolution", r.resolution},
          {"runColmap", r.runColmap}, {"overwrite", r.overwrite}, {"posePolicy", r.posePolicy},
          {"cameraDatasetPath", r.cameraDatasetPath}, {"cameraDatasetIdentity", r.cameraDatasetIdentity},
          {"cameraDatasetSummary", dialog.cameraDatasetSummary()}, {"resolutionPolicy", r.resolutionPolicy},
          {"runtimeOptions", r.runtimeOptions}, {"qualityPlan", r.qualityPlan}}; }); }
    { MeshGenerationDialog dialog(generated, &window);
      probe(QStringLiteral("DLG-04"), dialog, [&] { return dialog.configuration(); }); }
    { ModelExportOptions options; options.sourcePath = mesh;
      options.destinationPath = fixtures.filePath(QStringLiteral("not-exported.ply"));
      ModelExportDialog dialog(options, true, false, {mesh}, &window);
      probe(QStringLiteral("DLG-05"), dialog, [&] { return exportRequest(dialog.options()); }); }
    { auto record = QSharedPointer<TaskRecord>::create(QString::fromUtf8("Owned task 日本語"), generated);
      record->appendLog(QString::fromLatin1("owned raw log 0.901234567891\n")); record->finish(QStringLiteral("failed"));
      TaskDetailsDialog dialog(record, &window);
      probe(QStringLiteral("DLG-06"), dialog, [&] {
        return QJsonObject{{"recordName", record->name()}, {"recordState", record->state()},
          {"recordLog", record->logText()}, {"recordOutput", record->outputDirectory()},
          {"started", record->startedAt().toString(Qt::ISODateWithMs)},
          {"finished", record->finishedAt().toString(Qt::ISODateWithMs)}};
      }, "UI_ONLY_OPEN_TASK_RECORD_PRESERVED"); }
    { GenerationHistoryDialog dialog(fixtures.path(), &window);
      probe(QStringLiteral("DLG-07"), dialog, [&] {
        return QJsonObject{{"selectedAction", int(dialog.selectedAction())}, {"record", experiment(dialog.selectedRecord())}};
      }); }
    { QualityComparisonDialog dialog(python, backendRoot, fixtures.path(), environment, &window);
      probe(QStringLiteral("DLG-08"), dialog, [&] {
        return QJsonObject{{"contract", dialog.contract()}, {"report", dialog.comparisonReport()}, {"running", dialog.isRunning()}};
      }); }
  } else {
    { QualityMeasurementDialog dialog(python, backendRoot, fixtures.path(), environment, &window);
      probe(QStringLiteral("DLG-09"), dialog, [&] {
        return QJsonObject{{"manifest", dialog.manifestPath()}, {"reportPath", dialog.reportPath()},
          {"report", dialog.measurementReport()}, {"running", dialog.isRunning()}};
      }); }
    { ScaleConstraintsDialog dialog(python, backendRoot, fixtures.path(), dataset, QString(), environment, &window);
      probe(QStringLiteral("DLG-10"), dialog, [&] {
        return QJsonObject{{"constraints", dialog.constraintDocument()}, {"source", dialog.sourceDescriptor()},
          {"report", dialog.solutionReport()}, {"resultInfo", dialog.resultDatasetInfo()},
          {"resultPath", dialog.resultDatasetPath()}, {"versionPath", dialog.resultVersionPath()}, {"running", dialog.isRunning()}};
      }); }
    { const QJsonObject initialization{{"kind", "random"}, {"frame", "source"}, {"count", 16}, {"seed", 0},
          {"bounds", QJsonArray{QJsonArray{0.,0.,2.}, QJsonArray{1.,1.,3.}}},
          {"provenance", QJsonObject{{"source", "owned uninspected UI gallery"}}}};
      InitializationDialog dialog(python, backendRoot, fixtures.path(), dataset, QString(), initialization, environment, &window);
      probe(QStringLiteral("DLG-11"), dialog, [&] {
        return QJsonObject{{"request", dialog.requestDocument()}, {"report", dialog.report()}, {"running", dialog.isRunning()}};
      }); }
    { TrainingBatchDialog dialog(python, backendRoot, fixtures.path(), {}, environment, &window);
      probe(QStringLiteral("DLG-12"), dialog, [&] {
        return QJsonObject{{"request", dialog.requestDocument()}, {"preview", dialog.previewReport()},
          {"plan", dialog.planPath()}, {"running", dialog.isRunning()}};
      }); }
    { ExistingTrainingDialog dialog(python, backendRoot, environment, jobs, &window);
      probe(QStringLiteral("DLG-13"), dialog, [&] {
        return QJsonObject{{"report", dialog.currentReport()}, {"connected", dialog.isConnected()}, {"busy", dialog.isBusy()}};
      }); }
    { RuntimeGuardDialog dialog(QJsonObject{{"enabled", true}, {"mode", "pause"},
          {"usage_fraction", .901234567891}, {"minimum_spare_mib", 1001}, {"max_consecutive_denials", 17}}, &window);
      probe(QStringLiteral("DLG-14"), dialog, [&] { return dialog.configuration(); }, "PUBLIC_GETTER_FROZEN", 360); }
    { ResourceBudgetDialog dialog(viewport, &window);
      probe(QStringLiteral("DLG-15"), dialog, [&] { const auto p = viewport->resourceBudgetPolicy();
        return QJsonObject{{"viewportPolicy", QJsonObject{{"mode", int(p.mode)},
          {"ramMiB", p.ramLimitMiB}, {"gpuMiB", p.gpuLimitMiB}}}};
      }, "UI_ONLY_OPEN_VIEWPORT_POLICY_PRESERVED"); }
    { ViewportGridDialog dialog(viewport->viewportGridSettings(), viewport->sceneReferenceCoordinates(), &window);
      probe(QStringLiteral("DLG-16"), dialog, [&] { return dialog.settings().toJson().value_or(QJsonObject{}); }); }
  }

  const QJsonObject filesAfter = ownedFiles(fixtures.path());
  const bool filesRetained = !filesAfter.contains(QStringLiteral("unreadableOwnedFixture")) && filesAfter == filesBefore;
  const bool mainRetained = viewportSnapshot(window, *viewport) == mainBefore;
  passed = dialogCount == 8 && filesRetained && mainRetained && passed;
  const QJsonObject summary{{"schema", "gsw.ui_form_gallery/1"}, {"group", groupName},
      {"sourceCommit", qEnvironmentVariable("GSW_UI_SOURCE_COMMIT")}, {"dialogCount", dialogCount},
      {"maximumIntendedDialogCaptures", 96}, {"evidenceProcessRoot", processRoot}, {"rows", rows},
      {"ownedFixtureFilesUnchanged", filesRetained}, {"mainPublicStateUnchanged", mainRetained},
      {"status", passed ? "PASS" : "FAIL"}, {"visualReview", "NOT_RUN"},
      {"noDialogAccepted", true}, {"noTrainingOrGenerationStarted", true},
      {"idleUninspectedFormsDoNotClaimValidInputsOrDeepSemantics", true}};
  const QString summaryPath = QDir(output).filePath(QStringLiteral("gallery-group-%1-process-%2.json")
      .arg(groupName).arg(QCoreApplication::applicationPid()));
  if (!writeOwned(summaryPath, QJsonDocument(summary).toJson(QJsonDocument::Indented))) {
    qCritical("UI_FORM_GALLERY: cannot save the owned summary");
    passed = false;
  }
  qInfo() << "UI_FORM_GALLERY result" << passed << "dialogs" << dialogCount
          << "fixturesUnchanged" << filesRetained << "emptyMainStateUnchanged" << mainRetained;
  return passed;
}
} // namespace gsw
