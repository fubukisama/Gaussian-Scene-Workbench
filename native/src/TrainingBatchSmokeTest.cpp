#include "TrainingBatchSmokeTest.h"
#include "TrainingBatchDialog.h"
#include "AppLanguage.h"
#include "BackendLocator.h"
#include "../tests/UiFormVisualProbe.h"
#include <QCoreApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>

namespace gsw {
namespace {
bool batchWait(const std::function<bool()> &ready) {
  QElapsedTimer timer;timer.start();while(!ready()&&timer.elapsed()<30000){QEventLoop loop;QTimer::singleShot(10,&loop,&QEventLoop::quit);loop.exec();}return ready();
}
bool writeBatchJson(const QString&path,const QJsonObject&value){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(QJsonDocument(value).toJson())>0;}
QByteArray batchFileHash(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly)
      ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) : QByteArray();
}
}
bool runTrainingBatchSmokeTest() {
  bool passed = true;
  const bool visualForms = !qEnvironmentVariable("GSW_UI_SURFACE_EVIDENCE").isEmpty();
  const auto check = [&](bool ok, const char *detail) {
    std::fprintf(stderr, "Training batch %s: %s\n", ok ? "PASS" : "FAIL", detail);
    std::fflush(stderr); passed = passed && ok;
  };
  QTemporaryDir workspace;
  TrainingBatchDialog dialog({}, {}, workspace.path(), {}, QProcessEnvironment::systemEnvironment());
  QJsonObject request{{"schema", "gsw.training_batch_plan/1"},
      {"datasets", QJsonArray{QJsonObject{{"id", "dataset-a"}, {"path", "/readonly/generated/source"}, {"data_identity", ""}, {"display_name", "Unicode-source"}}}},
      {"matrix", QJsonObject{{"backend", QJsonArray{"3dgs", "2dgs"}}, {"trainingSeed", QJsonArray{7, 9}}, {"repeat", QJsonArray{1, 2}}}},
      {"base", QJsonObject{{"quality", "full"}, {"trainOptions", QJsonObject{{"iterations", 1000}, {"resolution", 1}}}}},
      {"initialization_policy", "dataset_declared"}, {"evaluation_policy", "declared_splits"},
      {"run_policy", QJsonObject{{"on_failure", "stop"}}}};
  dialog.setRequestDocument(request);
  auto *failurePolicy = dialog.findChild<QComboBox *>(QStringLiteral("batchFailurePolicy"));
  check(failurePolicy && failurePolicy->itemData(0).toString() == QStringLiteral("stop") &&
      failurePolicy->itemData(1).toString() == QStringLiteral("continue") && failurePolicy->currentData().toString() == QStringLiteral("stop"),
      "failure-policy userData contains stop/continue, not text consumed by the icon overload");
  check(dialog.requestDocument() == request, "editable dataset/matrix/base request round-trips without hard-coded experiment conditions");
  auto *start = dialog.findChild<QPushButton *>(QStringLiteral("batchStartButton"));
  auto *create = dialog.findChild<QPushButton *>(QStringLiteral("batchCreateButton"));
  check(start && create && !start->isEnabled() && !create->isEnabled(), "preview, creation and real training remain separate explicit actions");
  int launches = 0;
  QObject::connect(&dialog, &TrainingBatchDialog::runRequested, &dialog, [&](const QString &, bool) { ++launches; });
  const QString language = AppLanguage::current();
  for (const QString &locale : AppLanguage::supported()) {
    AppLanguage::apply(locale, false);
    check(dialog.requestDocument() == request && launches == 0, "language changes retain parameters and never start a worker");
    check(failurePolicy && failurePolicy->currentData().toString() == QStringLiteral("stop") &&
        failurePolicy->itemData(0).toString() == QStringLiteral("stop") && failurePolicy->itemData(1).toString() == QStringLiteral("continue"),
        "each locale switch preserves selected failure policy and both machine userData values");
  }
  AppLanguage::apply(language, false);
  dialog.previewPlan();
  check(!dialog.isRunning() && dialog.previewReport().isEmpty() && launches == 0, "an unavailable planning helper fails closed and cannot launch training");

  const QString root=BackendLocator::findRepositoryRoot(QCoreApplication::applicationDirPath(),qEnvironmentVariable("GSW_BACKEND_ROOT"));
  QString python=qEnvironmentVariable("GSW_BATCH_TEST_PYTHON");if(python.isEmpty())python=qEnvironmentVariable("GSW_QUALITY_TEST_PYTHON");if(python.isEmpty())python=BackendLocator::findGaussianPython(root);
  const QString source=workspace.filePath(QStringLiteral("dataset"));check(QDir().mkpath(source),"create owned batch dataset fixture");
  QJsonArray views;
  for(int index=0;index<3;++index){QImage image(20,14,QImage::Format_RGB32);image.fill(QColor(40+index,70,90));const QString name=QStringLiteral("image-%1.png").arg(index);check(image.save(QDir(source).filePath(name),"PNG"),"write generated batch image");views.append(QJsonObject{{"view_id",QStringLiteral("view-%1").arg(index)},{"image_path",name},{"split",index<2?"train":"test"},{"pixel_coordinates","pixel_border"},{"K",QJsonArray{QJsonArray{24.,0.,10.},QJsonArray{0.,25.,7.},QJsonArray{0.,0.,1.}}},{"c2w_opencv",QJsonArray{QJsonArray{1.,0.,0.,double(index)},QJsonArray{0.,1.,0.,0.},QJsonArray{0.,0.,1.,-4.},QJsonArray{0.,0.,0.,1.}}}});}
  check(writeBatchJson(QDir(source).filePath(QStringLiteral("camera_dataset.json")),QJsonObject{{"schema","gsw.camera_dataset/1"},{"pose_policy","locked"},{"source",QJsonObject{{"kind","manual"}}},{"scale",QJsonObject{{"status","arbitrary"}}},{"views",views},{"initialization",QJsonObject{{"kind","random"},{"count",16},{"seed",3},{"bounds",QJsonArray{QJsonArray{-.5,-.5,-.5},QJsonArray{.5,.5,.5}}},{"frame","source"}}}}),"write raw canonical batch source, without a training process");
  TrainingBatchDialog actual(python,root,workspace.path(),{},QProcessEnvironment::systemEnvironment());
  auto actualRequest=request;actualRequest["datasets"]=QJsonArray{QJsonObject{{"id","generated"},{"path",source},{"data_identity",""},{"display_name","Generated batch"}}};
  actualRequest["matrix"]=QJsonObject{{"backend",QJsonArray{"3dgs","2dgs"}},{"trainingSeed",QJsonArray{7,9}}};
  actual.setRequestDocument(actualRequest);int actualStarts=0;bool lastResume=false;
  QObject::connect(&actual,&TrainingBatchDialog::runRequested,&actual,[&](const QString&path,bool resume){check(!path.isEmpty(),"run request refers to a persistent plan");++actualStarts;lastResume=resume;});
  actual.show(); // Ordinary smoke does not activate; the opt-in native matrix owns activation on CI.
  actual.previewPlan();
  check(batchWait([&]{return !actual.isRunning();})&&actual.previewReport().value("runs").toArray().size()==4&&actualStarts==0,"real CPU preview expands two backends by two seeds without training");
  const auto pinned=actual.requestDocument().value("datasets").toArray();
  check(pinned.size()==1&&pinned[0].toObject().value("data_identity").toString().size()==64,"preview visibly pins actual admitted dataset identity");
  actual.createPlan();
  check(batchWait([&]{return !actual.isRunning();})&&!actual.planPath().isEmpty()&&actualStarts==0,"creating a durable plan does not emit a training launch");
  const QString plan=actual.planPath();
  if (visualForms) {
    const QString statePath = QDir(QFileInfo(plan).absolutePath()).filePath(QStringLiteral("state.json"));
    const QStringList inputNames = QDir(source).entryList(QDir::Files);
    bool inputBytesReadable = !inputNames.isEmpty();
    for (const QString &name : inputNames)
      inputBytesReadable = batchFileHash(QDir(source).filePath(name)).size() == 32 && inputBytesReadable;
    check(!actual.isRunning() && !actual.previewReport().isEmpty() && inputBytesReadable &&
        batchFileHash(plan).size() == 32 && batchFileHash(statePath).size() == 32 && actualStarts == 0,
        "native matrix begins with a real persisted both-backend plan and readable queue and input bytes, without training");
    const auto snapshot = [&]() {
      QJsonObject inputHashes;
      for (const QString &name : QDir(source).entryList(QDir::Files))
        inputHashes.insert(name, QString::fromLatin1(batchFileHash(QDir(source).filePath(name)).toHex()));
      return QJsonObject{{"requestDocument", actual.requestDocument()},
          {"previewReport", actual.previewReport()}, {"planPath", actual.planPath()},
          {"isRunning", actual.isRunning()}, {"trainingLaunches", actualStarts}, {"lastResume", lastResume},
          {"inputSha256", inputHashes}, {"planSha256", QString::fromLatin1(batchFileHash(plan).toHex())},
          {"stateSha256", QString::fromLatin1(batchFileHash(statePath).toHex())}};
    };
    check(tests::captureUiFormMatrix(visualForms, actual, QStringLiteral("training-batch-plan-created"),
        QSize(1100, 760), QSize(680, 620), snapshot),
        "native three-language light/dark wide/narrow evidence preserves both-backend plan, queue and input bytes without launching training");
  }
  TrainingBatchDialog reopened(python,root,workspace.path(),{},QProcessEnvironment::systemEnvironment());
  int reopenedStarts=0;QString reopenedPlan;bool reopenedResume=false;
  QObject::connect(&reopened,&TrainingBatchDialog::runRequested,&reopened,[&](const QString&path,bool resume){++reopenedStarts;reopenedPlan=path;reopenedResume=resume;});
  check(reopened.loadPlan(plan)&&reopenedStarts==0&&reopened.previewReport().value("runs").toArray().size()==4,"opening a saved plan and state never resumes or starts training");
  actual.startBatch();check(actualStarts==1&&!lastResume,"only explicit Start emits a serial-worker launch request");
  const QString savedStatePath=QDir(QFileInfo(plan).absolutePath()).filePath(QStringLiteral("state.json"));
  QFile savedStateFile(savedStatePath);QByteArray savedStateBytes;
  if(savedStateFile.open(QIODevice::ReadOnly)){savedStateBytes=savedStateFile.readAll();savedStateFile.close();}
  const auto unchangedState=[&](const QByteArray&expected){QFile file(savedStatePath);return file.open(QIODevice::ReadOnly)&&file.readAll()==expected;};
  auto interruptedRunning=QJsonDocument::fromJson(savedStateBytes).object();interruptedRunning["status"]="running";
  const QByteArray runningBytes=QJsonDocument(interruptedRunning).toJson();
  auto*resumeButton=reopened.findChild<QPushButton*>(QStringLiteral("batchResumeButton"));
  auto*observeButton=reopened.findChild<QPushButton*>(QStringLiteral("batchObserveButton"));
  int observations=0;QString observedPlan;
  QObject::connect(&reopened,&TrainingBatchDialog::observeRequested,&reopened,[&](const QString&path){++observations;observedPlan=path;});
  check(writeBatchJson(savedStatePath,interruptedRunning)&&reopened.loadPlan(plan)&&reopenedStarts==0&&
      unchangedState(runningBytes)&&resumeButton&&!resumeButton->isEnabled()&&observeButton&&observeButton->isEnabled(),
      "legacy running metadata remains byte-preserving and observation-only, without inventing a stopped owner or checkpoint");
  reopened.resumeBatch();check(reopenedStarts==0&&unchangedState(runningBytes),
      "an explicit invalid resume cannot emit a launch request for unverified legacy running metadata");
  if(observeButton)observeButton->click();
  check(observations==1&&observedPlan==plan&&reopenedStarts==0&&unchangedState(runningBytes),
      "only explicit Observe emits the same plan observation, without starting or rewriting the legacy queue");
  // These are pending, between-run queue metadata cases. They exercise only
  // public UI admission/signals; no optimizer checkpoint or actual resume is
  // fabricated or claimed by this CPU planning smoke.
  for(const QString&status:{QStringLiteral("paused"),QStringLiteral("interrupted")}){
    auto betweenRuns=QJsonDocument::fromJson(savedStateBytes).object();betweenRuns["status"]=status;
    const QByteArray expected=QJsonDocument(betweenRuns).toJson();const int before=reopenedStarts;
    check(writeBatchJson(savedStatePath,betweenRuns)&&reopened.loadPlan(plan)&&reopenedStarts==before&&
        unchangedState(expected)&&resumeButton&&resumeButton->isEnabled(),
        status==QStringLiteral("paused")?"opening a paused between-run queue preserves bytes and never launches training":
        "opening an interrupted between-run queue preserves bytes and never launches training");
    if(resumeButton)resumeButton->click();
    check(reopenedStarts==before+1&&reopenedPlan==plan&&reopenedResume&&unchangedState(expected),
        status==QStringLiteral("paused")?"only explicit Resume emits exactly one same-plan resume request for a paused queue":
        "only explicit Resume emits exactly one same-plan resume request for an interrupted queue");
    reopened.refreshHistory();
    check(reopenedStarts==before+1&&unchangedState(expected),
        "refreshing resumable queue metadata neither repeats its request nor rewrites state");
  }
  QFile restoredState(savedStatePath);check(restoredState.open(QIODevice::WriteOnly|QIODevice::Truncate)&&
      restoredState.write(savedStateBytes)==savedStateBytes.size(),"restore only the owned state fixture after the resume-request test");restoredState.close();
  actual.refreshHistory();check(actualStarts==1,"refreshing persisted state has no automatic launch behavior");
  auto*base=actual.findChild<QPlainTextEdit*>(QStringLiteral("batchBaseConfiguration"));
  if(base)base->setPlainText(QStringLiteral("{}"));
  check(actual.previewReport().isEmpty()&&actual.planPath().isEmpty()&&!actual.findChild<QPushButton*>(QStringLiteral("batchStartButton"))->isEnabled(),"editing configuration invalidates preview and the old plan launch reference");
  actual.setRequestDocument(actualRequest);actual.previewPlan();actual.cancelOperation();
  check(batchWait([&]{return !actual.isRunning();})&&actual.previewReport().isEmpty()&&actualStarts==1,"cancelling the owned planner does not cancel or start real training");
  reopened.close();actual.close();
  std::fprintf(stderr, "Training batch UI smoke: %s\n", passed ? "PASS" : "FAIL");
  return passed;
}
}
