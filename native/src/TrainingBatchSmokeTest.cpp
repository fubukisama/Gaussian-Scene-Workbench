#include "TrainingBatchSmokeTest.h"
#include "TrainingBatchDialog.h"
#include "AppLanguage.h"
#include "BackendLocator.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
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
}
bool runTrainingBatchSmokeTest() {
  bool passed = true;
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
  actual.show(); // Remote test-owned window only; no raising, activation or local execution.
  actual.previewPlan();
  check(batchWait([&]{return !actual.isRunning();})&&actual.previewReport().value("runs").toArray().size()==4&&actualStarts==0,"real CPU preview expands two backends by two seeds without training");
  const auto pinned=actual.requestDocument().value("datasets").toArray();
  check(pinned.size()==1&&pinned[0].toObject().value("data_identity").toString().size()==64,"preview visibly pins actual admitted dataset identity");
  actual.createPlan();
  check(batchWait([&]{return !actual.isRunning();})&&!actual.planPath().isEmpty()&&actualStarts==0,"creating a durable plan does not emit a training launch");
  const QString plan=actual.planPath();
  TrainingBatchDialog reopened(python,root,workspace.path(),{},QProcessEnvironment::systemEnvironment());
  int reopenedStarts=0;QObject::connect(&reopened,&TrainingBatchDialog::runRequested,&reopened,[&](const QString&,bool){++reopenedStarts;});
  check(reopened.loadPlan(plan)&&reopenedStarts==0&&reopened.previewReport().value("runs").toArray().size()==4,"opening a saved plan and state never resumes or starts training");
  actual.startBatch();check(actualStarts==1&&!lastResume,"only explicit Start emits a serial-worker launch request");
  const QString savedStatePath=QDir(QFileInfo(plan).absolutePath()).filePath(QStringLiteral("state.json"));
  QFile savedStateFile(savedStatePath);QByteArray savedStateBytes;
  if(savedStateFile.open(QIODevice::ReadOnly)){savedStateBytes=savedStateFile.readAll();savedStateFile.close();}
  auto interruptedRunning=QJsonDocument::fromJson(savedStateBytes).object();interruptedRunning["status"]="running";
  check(writeBatchJson(savedStatePath,interruptedRunning)&&reopened.loadPlan(plan)&&reopenedStarts==0&&
      reopened.findChild<QPushButton*>(QStringLiteral("batchResumeButton"))->isEnabled(),
      "a persisted running state can request verification after a crash but never starts merely on load");
  reopened.resumeBatch();check(reopenedStarts==1,"only an explicit resume action emits the request for a persisted running state");
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
