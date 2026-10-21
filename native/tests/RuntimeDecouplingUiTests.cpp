#include "TrainingDialog.h"
#include "AppLanguage.h"
#include "RuntimeGuardDialog.h"
#include "ExistingTrainingDialog.h"
#include "TrainingMonitorWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <cstdio>

int main(int argc, char **argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  qputenv("QT_QPA_PLATFORM", "offscreen");
  std::fprintf(stderr,"Runtime UI bootstrap: QApplication\n");std::fflush(stderr);
  QApplication app(argc, argv);
  std::fprintf(stderr,"Runtime UI bootstrap: QApplication ready\n");std::fflush(stderr);
  app.setOrganizationName(QStringLiteral("GSW-owned-runtime-ui-probe"));
  app.setApplicationName(QStringLiteral("GSW-owned-runtime-ui-probe"));
  const auto arguments=app.arguments();
  if(arguments.size()!=4){std::fprintf(stderr,"usage: RuntimeDecouplingUiTests <repository> <python> <owned-fixture-script>\n");return 2;}
  const QString repository=QFileInfo(arguments.at(1)).absoluteFilePath();
  const QString python=QFileInfo(arguments.at(2)).absoluteFilePath();
  const QString fixtureScript=QFileInfo(arguments.at(3)).absoluteFilePath();
  const QString fixtureRoot=qEnvironmentVariable("GSW_TEST_ROOT",QDir::tempPath());
  if(!QDir().mkpath(fixtureRoot))return 2;
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("runtime-ui-XXXXXX")));
  if (!temporary.isValid()) return 2;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
  gsw::AppLanguage::apply(QStringLiteral("zh_CN"), false);
  std::fprintf(stderr,"Runtime UI bootstrap: language ready\n");std::fflush(stderr);
  gsw::TrainingDialog dialog(temporary.path(), QStringLiteral("generated UI fixture"), temporary.path(), true, true);
  bool passed=true;
  int checks=0;
  const auto check=[&](const bool okay,const char *name){++checks;passed&=okay;std::printf("Runtime UI %s: %s\n",okay?"PASS":"FAIL",name);};
  const auto waitFor=[](const std::function<bool()> &condition){QElapsedTimer timer;timer.start();while(!condition()&&timer.elapsed()<8000){QEventLoop loop;QTimer::singleShot(10,&loop,&QEventLoop::quit);loop.exec();}return condition();};
  auto *guard=dialog.findChild<QCheckBox *>(QStringLiteral("trainingResourceGuard"));
  auto *staging=dialog.findChild<QCheckBox *>(QStringLiteral("trainingImageStaging"));
  auto *prefetch=dialog.findChild<QCheckBox *>(QStringLiteral("trainingCameraPrefetch"));
  auto *capacity=dialog.findChild<QCheckBox *>(QStringLiteral("trainingCapacityStorage"));
  auto *auxiliary=dialog.findChild<QCheckBox *>(QStringLiteral("trainingSurfelAuxiliary"));
  auto *backend=dialog.findChild<QComboBox *>(QStringLiteral("trainingBackendCombo"));
  auto *memory=dialog.findChild<QSpinBox *>(QStringLiteral("trainingMemoryPercent"));
  auto *spare=dialog.findChild<QSpinBox *>(QStringLiteral("trainingMinimumSpareMiB"));
  auto *denials=dialog.findChild<QSpinBox *>(QStringLiteral("trainingMaxConsecutiveDenials"));
  auto *loss=dialog.findChild<QComboBox *>(QStringLiteral("trainingLossModeCombo"));
  auto *engine=dialog.findChild<QComboBox *>(QStringLiteral("trainingEngineCombo"));
  auto *growth=dialog.findChild<QComboBox *>(QStringLiteral("trainingGrowthModeCombo"));
  auto *profile=dialog.findChild<QSpinBox *>(QStringLiteral("trainingProfileInterval"));
  auto *recommend=dialog.findChild<QPushButton *>(QStringLiteral("trainingApplyRecommendedExecution"));
  auto *hint=dialog.findChild<QLabel *>(QStringLiteral("trainingPrefetchDependencyHint"));
  check(guard&&staging&&prefetch&&capacity&&auxiliary&&backend&&memory&&spare&&denials&&loss&&engine&&growth&&profile&&recommend&&hint,
      "all public controls exist without a performance parent gate");
  if(!passed)return 1;
  const auto original=dialog.configuration();
  check(original.runtimeOptions==QJsonObject{{"version",2},{"resourceGuard",QJsonObject{{"enabled",true},{"mode","pause"},{"usage_fraction",0.9},{"minimum_spare_mib",512},{"max_consecutive_denials",3}}},
      {"execution",QJsonObject{{"image_staging",true},{"camera_prefetch",true},{"capacity_storage",true},{"surfel_auxiliary",true},{"loss_mode","auto"},{"engine","stock"},{"gpu_slots",2},{"pinned_mib",256},{"gpu_mib",512}}},
      {"monitoring",QJsonObject{{"profile_interval",30}}}},"defaults emit the exact literal canonical version2 contract");
  for(const QString &id:{QStringLiteral("3dgs"),QStringLiteral("2dgs")}){
    backend->setCurrentIndex(backend->findData(id));
    for(const bool optimized:{false,true}){
      staging->setChecked(optimized);prefetch->setChecked(optimized);capacity->setChecked(optimized);auxiliary->setChecked(optimized);
      loss->setCurrentIndex(loss->findData(optimized?QStringLiteral("auto"):QStringLiteral("stock")));
      const auto execution=dialog.configuration().runtimeOptions.value("execution");
      for(const bool protectedMode:{false,true}){
        guard->setChecked(protectedMode);
        const auto value=dialog.configuration();
        check(value.resourceGuardEnabled==protectedMode&&value.runtimeOptions.value("execution")==execution&&value.iterations==original.iterations&&value.resolution==original.resolution,
            "guard matrix retains execution requests and training data settings");
        check(staging->isEnabled()&&prefetch->isEnabled()&&capacity->isEnabled()&&auxiliary->isEnabled()==(id==QStringLiteral("2dgs")),
            "guard does not disable execution controls; backend applicability stays explicit");
      }
    }
  }
  guard->setChecked(false);memory->setValue(100);spare->setValue(0);denials->setValue(13);
  growth->setCurrentIndex(growth->findData(QStringLiteral("defer")));profile->setValue(101);
  staging->setChecked(false);prefetch->setChecked(true);capacity->setChecked(false);auxiliary->setChecked(false);
  loss->setCurrentIndex(loss->findData(QStringLiteral("stock")));engine->setCurrentIndex(engine->findData(QStringLiteral("fastergs")));
  const auto edited=dialog.configuration();
  check(edited.runtimeOptions.value("resourceGuard").toObject()==QJsonObject{{"enabled",false},{"mode","defer"},{"usage_fraction",1.0},{"minimum_spare_mib",0},{"max_consecutive_denials",13}},
      "unguarded request retains editable parameters without converting defer to disabled");
  QSet<QString> guardLabels,dependencyLabels,recommendedLabels,lossLabels;
  for(const QString &locale:gsw::AppLanguage::supported()){
    check(gsw::AppLanguage::apply(locale,false),"language switches immediately");
    check(dialog.configuration().runtimeOptions==edited.runtimeOptions,"language switching preserves every requested runtime value");
    guardLabels.insert(guard->text());dependencyLabels.insert(hint->text());recommendedLabels.insert(recommend->text());lossLabels.insert(loss->currentText());
    check(!guard->toolTip().isEmpty()&&!recommend->toolTip().isEmpty()&&prefetch->isChecked()&&!staging->isChecked(),
        "staging dependency is explained without overwriting its requested child");
  }
  check(guardLabels.size()==3&&dependencyLabels.size()==3&&recommendedLabels.size()==3&&lossLabels.size()==3,
      "new guard, dependency, preset and loss captions are distinct real three-language translations");
  const auto guardedBefore=edited.runtimeOptions.value("resourceGuard");
  recommend->click();
  const auto recommended=dialog.configuration();
  check(recommended.runtimeOptions.value("resourceGuard")==guardedBefore&&recommended.runtimeOptions.value("monitoring")==edited.runtimeOptions.value("monitoring")&&
      recommended.imageStaging&&recommended.cameraPrefetch&&recommended.capacityStorage&&recommended.surfelAuxiliary&&recommended.lossMode==QStringLiteral("auto")&&recommended.trainingEngine==QStringLiteral("stock"),
      "advertised execution preset does not mutate guard or monitoring");
  std::printf("RUNTIME_OPTIONS %s\n",QJsonDocument(recommended.runtimeOptions).toJson(QJsonDocument::Compact).constData());
  {
    gsw::TrainingMonitorWidget observed;
    observed.beginTraining(QStringLiteral("owned runtime observations"),QStringLiteral("3dgs"),50);
    gsw::WorkerStatus status;status.state=QStringLiteral("running");status.stage=QStringLiteral("train");status.iteration=9;
    status.runtimeConfiguration={{"requested",recommended.runtimeOptions},{"effective",recommended.runtimeOptions},{"reasons",QJsonArray{}},{"migration",QJsonObject{{"source","runtimeOptions"}}}};
    observed.updateStatus(status);
    auto *guardState=observed.findChild<QLabel*>(QStringLiteral("trainingResourceGuardState"));
    auto *failure=observed.findChild<QLabel*>(QStringLiteral("trainingFailureReason"));
    check(guardState&&failure,"monitor exposes retained runtime guard state and classified failure labels through its public status input");
    if(guardState&&failure){
      QSet<QString> disabledTexts;
      const auto samplesBefore=observed.telemetry().samples().size();
      for(const auto &locale:gsw::AppLanguage::supported()){
        gsw::AppLanguage::apply(locale,false);disabledTexts.insert(guardState->text());
        check(!guardState->isHidden()&&guardState->text()==QCoreApplication::translate("Workbench","显存保护已关闭；不会按软件资源阈值暂缓增密。实际 CUDA 内存不足仍会报错。"),
            "guard OFF is explicit and never represented as deferred growth");
      }
      check(disabledTexts.size()==3&&observed.telemetry().samples().size()==samplesBefore,"runtime guard text translates without ingesting more observations or resetting curves");
      const QStringList reasons{QStringLiteral("resource_probe_unavailable"),QStringLiteral("resource_estimate_unavailable"),QStringLiteral("cuda_oom"),QStringLiteral("training_exception")};
      QSet<QString> englishFailures;
      gsw::AppLanguage::apply(QStringLiteral("en_US"),false);
      for(const auto &reason:reasons){status.failureReason=reason;observed.updateStatus(status);englishFailures.insert(failure->text());check(!failure->isHidden(),"each typed failure remains visible");}
      check(englishFailures==QSet<QString>{QStringLiteral("VRAM Probe Failed"),QStringLiteral("Growth Peak Estimate Failed"),QStringLiteral("CUDA Out of Memory"),QStringLiteral("Training Exception")},
          "probe failure, estimate failure, real OOM and ordinary exception are four distinct public diagnoses");
      for(const auto &reason:reasons){
        status.failureReason=reason;observed.updateStatus(status);QSet<QString> captions;
        for(const auto &locale:gsw::AppLanguage::supported()){gsw::AppLanguage::apply(locale,false);captions.insert(failure->text());}
        check(captions.size()==3,"each retained failure diagnosis immediately translates into three distinct languages");
      }
      status.runtimeConfiguration={};status.failureReason.clear();observed.updateStatus(status);observed.finishTraining(false,false,true);
      for(const auto &locale:gsw::AppLanguage::supported()){gsw::AppLanguage::apply(locale,false);check(!failure->isHidden()&&!guardState->isHidden(),"later partial observations and pause retain classified runtime evidence across locale changes");}
      observed.beginTraining(QStringLiteral("next owned task"),QStringLiteral("2dgs"),10);
      check(guardState->isHidden()&&failure->isHidden(),"starting a different task clears only the previous task's runtime/failure presentation");
      status={};status.stage=QStringLiteral("train");observed.updateStatus(status);
      check(guardState->isHidden(),"missing runtime observation never guesses enabled or disabled guard defaults");
      auto activeRuntime=recommended.runtimeOptions;
      const QJsonObject activeGuard{{"enabled",true},{"mode","defer"},{"usage_fraction",1.0},{"minimum_spare_mib",0},{"max_consecutive_denials",17}};
      activeRuntime.insert(QStringLiteral("resourceGuard"),activeGuard);
      status.runtimeConfiguration={{"requested",activeRuntime},{"effective",activeRuntime},{"reasons",QJsonArray{}},{"migration",QJsonObject{}}};observed.updateStatus(status);
      QSet<QString> enabledTexts;
      for(const auto &locale:gsw::AppLanguage::supported()){gsw::AppLanguage::apply(locale,false);enabledTexts.insert(guardState->text());
        check(!guardState->isHidden()&&guardState->text().contains(QStringLiteral("100%"))&&guardState->text().contains(QStringLiteral("0 MiB"))&&guardState->text().contains(QStringLiteral("17")),
            "observed enabled guard shows the actual zero-reserve, 100-percent target and consecutive-denial policy");}
      check(enabledTexts.size()==3&&QJsonDocument::fromJson(guardState->toolTip().toUtf8()).object()==status.runtimeConfiguration,
          "enabled policy translates while exact requested/effective provenance remains inspectable");
    }
  }
  {
    for(const auto &backend:{QStringLiteral("3dgs"),QStringLiteral("2dgs")}) {
      gsw::TrainingMonitorWidget delivery;delivery.beginTraining(QStringLiteral("owned-delivery"),backend,10);
      gsw::WorkerStatus status;status.state=QStringLiteral("failed");status.stage=QStringLiteral("coordinate_export");status.iteration=10;
      status.trainingDelivery={{"training_status","done"},{"export_status","failed"},{"reason","owned_export_failure"},
          {"internal_model_path","owned-internal.ply"},{"iteration",10}};
      delivery.updateStatus(status);delivery.finishTraining(false,false,false);
      auto *label=delivery.findChild<QLabel*>(QStringLiteral("trainingDeliveryState"));
      check(label&&!label->isHidden(),"completed training exposes an independent coordinate-delivery status for both native backends");
      if(label){
        for(const auto &exportState:{QStringLiteral("running"),QStringLiteral("ready"),QStringLiteral("blocked"),QStringLiteral("failed"),QStringLiteral("cancelled")}) {
          status.trainingDelivery.insert(QStringLiteral("export_status"),exportState);delivery.updateStatus(status);delivery.finishTraining(false,exportState==QStringLiteral("cancelled"),false);
          QSet<QString> captions;
          for(const auto &locale:gsw::AppLanguage::supported()){
            gsw::AppLanguage::apply(locale,false);captions.insert(label->text());bool trainingDone=false;
            for(auto *candidate:delivery.findChildren<QLabel*>())trainingDone|=candidate->text()==QCoreApplication::translate("Workbench","训练已完成");
            check(trainingDone,"coordinate export failure/cancellation never relabels completed optimization as failed or cancelled");
          }
          check(captions.size()==3,"independent delivery states translate immediately in all three languages");
        }
        status.trainingDelivery={};delivery.updateStatus(status);check(!label->isHidden(),"partial statuses preserve the last independent delivery report");
        delivery.beginTraining(QStringLiteral("next-delivery-task"),backend,20);check(label->isHidden(),"a new training task clears only the previous delivery presentation");
      }
    }
    gsw::RuntimeGuardDialog legacyGuard(QJsonObject{});
    check(!legacyGuard.configuration().value("enabled").toBool(),"missing checkpoint guard policy does not silently opt legacy training into protection");
    const QJsonObject originalGuard{{"enabled",true},{"mode","pause"},{"usage_fraction",0.901234567891},{"minimum_spare_mib",1001},{"max_consecutive_denials",17}};
    gsw::RuntimeGuardDialog resume(originalGuard);
    check(resume.configuration()==originalGuard,"guard-only resume editor preserves the exact unedited fraction and all five original values");
    auto *enabled=resume.findChild<QCheckBox*>(QStringLiteral("resumeResourceGuard"));
    auto *target=resume.findChild<QDoubleSpinBox*>(QStringLiteral("resumeGuardMemoryPercent"));
    auto *spareControl=resume.findChild<QSpinBox*>(QStringLiteral("resumeGuardMinimumSpareMiB"));
    auto *denialControl=resume.findChild<QSpinBox*>(QStringLiteral("resumeGuardMaxConsecutiveDenials"));
    auto *mode=resume.findChild<QComboBox*>(QStringLiteral("resumeGuardMode"));
    check(enabled&&target&&spareControl&&denialControl&&mode,"resume exposes exactly the independent guard edit controls");
    if(!enabled||!target||!spareControl||!denialControl||!mode)return 1;
    enabled->setChecked(false);target->setValue(100.0);spareControl->setValue(0);denialControl->setValue(5);mode->setCurrentIndex(mode->findData(QStringLiteral("defer")));
    const QJsonObject changed{{"enabled",false},{"mode","defer"},{"usage_fraction",1.0},{"minimum_spare_mib",0},{"max_consecutive_denials",5}};
    check(resume.configuration()==changed,"guard-only edit produces no execution, data, optimizer or monitoring fields");
    QSet<QString> titles;
    for(const auto &locale:gsw::AppLanguage::supported()){gsw::AppLanguage::apply(locale,false);titles.insert(resume.windowTitle());check(resume.configuration()==changed,"resume editor language changes retain guard policy without launching a helper");}
    check(titles.size()==3,"resume guard dialog has distinct immediate three-language titles");
    auto invalid=originalGuard;invalid["usage_fraction"]=1.2;gsw::RuntimeGuardDialog rejected(invalid);
    auto *accept=rejected.findChild<QPushButton*>(QStringLiteral("resumeGuardAccept"));
    check(accept&&!accept->isEnabled()&&rejected.configuration()==invalid,"invalid input is refused without silently clamping and replacing the original request");
  }
  {
    const QString store=temporary.filePath(QStringLiteral("jobs"));QDir().mkpath(store);
    const QString output=temporary.filePath(QStringLiteral("output"));QDir().mkpath(output);
    const QString id(32,QLatin1Char('a'));const QString session(32,QLatin1Char('b'));
    const QString recordPath=QDir(store).filePath(id+QStringLiteral(".json"));
    const QJsonObject record{{"id",id},{"backend","3dgs"},{"status","running"},{"stage","train"},{"iteration",9},{"total_iterations",50},{"output_scene","generated CPU lease fixture"},
        {"native_control",QJsonObject{{"session",session},{"backend","3dgs"},{"output",output}}}};
    QFile jobFile(recordPath);if(!jobFile.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;
    const auto originalBytes=QJsonDocument(record).toJson();jobFile.write(originalBytes);jobFile.close();
    QFile configuration(QDir(store).filePath(QStringLiteral("configuration.json")));if(!configuration.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;configuration.write("{\"owned_cpu_fixture_only\":true}");configuration.close();
    auto env=QProcessEnvironment::systemEnvironment();env.insert(QStringLiteral("PYTHONUTF8"),QStringLiteral("1"));env.insert(QStringLiteral("PYTHONIOENCODING"),QStringLiteral("utf-8"));
    gsw::ExistingTrainingDialog observer(python,repository,env,store);
    auto *jobs=observer.findChild<QComboBox*>(QStringLiteral("existingTrainingJobCombo"));
    auto *pause=observer.findChild<QPushButton*>(QStringLiteral("existingTrainingPause"));
    auto *stop=observer.findChild<QPushButton*>(QStringLiteral("existingTrainingStop"));
    auto *resume=observer.findChild<QPushButton*>(QStringLiteral("existingTrainingResume"));
    auto *state=observer.findChild<QLabel*>(QStringLiteral("existingTrainingState"));
    check(jobs&&pause&&stop&&resume&&state&&!observer.isBusy()&&!observer.isConnected(),"observer construction starts no training or helper");
    if(!jobs||!pause||!stop||!resume||!state)return 1;
    observer.refreshJobs();check(waitFor([&]{return !observer.isBusy();})&&jobs->count()==1,"actual CPU inventory helper lists the existing owned record");
    observer.connectSelectedJob();check(waitFor([&]{return !observer.isBusy()&&!observer.currentReport().isEmpty();})&&observer.currentReport().value("liveness").toString()==QStringLiteral("unverified"),
        "recorded running without a live lease remains explicitly unverified");
    check(!pause->isEnabled()&&!stop->isEnabled()&&!resume->isEnabled(),"unverified running records cannot authorize pause, stop or duplicate resume");
    const auto *monitor=observer.findChild<QWidget*>(QStringLiteral("existingTrainingMonitor"));
    check(monitor&&monitor->isHidden()&&observer.currentReport().value("statusEvent").toObject().value("state").toString()==QStringLiteral("unknown"),
        "unverified active records cannot leak a false running state through the embedded monitor");
    QSet<QString> statusTexts,observerTitles;const auto inspected=observer.currentReport();
    for(const auto &locale:gsw::AppLanguage::supported()){gsw::AppLanguage::apply(locale,false);statusTexts.insert(state->text());observerTitles.insert(observer.windowTitle());check(observer.currentReport()==inspected&&jobs->currentData().toString()==id&&observer.isConnected(),"observer locale changes retain the selected identity, connection and raw report");}
    check(statusTexts.size()==3&&observerTitles.size()==3,"observer liveness and dialog titles translate immediately in three languages");
    observer.disconnectJob();jobFile.open(QIODevice::ReadOnly);check(jobFile.readAll()==originalBytes,"inspect, locale switch and detach do not mutate the recorded running job");jobFile.close();
    QProcess owned;owned.setProcessEnvironment(env);owned.setWorkingDirectory(repository);
    owned.start(python,{QStringLiteral("-B"),fixtureScript,QStringLiteral("--store"),store,QStringLiteral("--job-id"),id});
    check(waitFor([&]{return QFileInfo(QDir(store).filePath(QStringLiteral(".c/")+session+QStringLiteral(".json"))).isFile()||owned.state()==QProcess::NotRunning;})&&owned.state()!=QProcess::NotRunning,
        "bounded CPU-only process publishes a real OS-bound lease without a Gaussian trainer");
    observer.connectSelectedJob();check(waitFor([&]{return !observer.isBusy()&&observer.currentReport().value("liveness").toString()==QStringLiteral("live");})&&pause->isEnabled()&&stop->isEnabled(),
        "actual identity and heartbeat enable explicit controls for the owned CPU lease");
    observer.disconnectJob();check(owned.state()==QProcess::Running,"disconnect never owns or kills the observed process");
    observer.connectSelectedJob();check(waitFor([&]{return !observer.isBusy()&&observer.currentReport().value("liveness").toString()==QStringLiteral("live");}),"observer reconnects to the same process without launching another task");
    bool safeDefault=false;
    QTimer::singleShot(0,[&]{auto *box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());if(box){safeDefault=box->defaultButton()==box->button(QMessageBox::No);box->button(QMessageBox::No)->click();}});
    observer.stopJob();check(safeDefault&&owned.state()==QProcess::Running,"stop confirmation defaults to No and cancellation sends no stop request");
    const auto stageFixture=[&](const QString &stage){
      QFile current(jobFile.fileName());if(!current.open(QIODevice::ReadOnly))return false;
      auto value=QJsonDocument::fromJson(current.readAll()).object();current.close();value.insert(QStringLiteral("stage"),stage);
      QSaveFile updated(jobFile.fileName());const auto bytes=QJsonDocument(value).toJson();
      return updated.open(QIODevice::WriteOnly)&&updated.write(bytes)==bytes.size()&&updated.commit();
    };
    check(stageFixture(QStringLiteral("coordinate_export")),"advance only the owned CPU lease fixture into independent coordinate delivery");
    observer.refreshJobs();check(waitFor([&]{return !observer.isBusy()&&observer.currentReport().value("stage").toString()==QStringLiteral("coordinate_export");}),
        "refresh observes the existing job's coordinate-export stage without starting training");
    for(const auto &locale:gsw::AppLanguage::supported()){
      gsw::AppLanguage::apply(locale,false);
      check(!pause->isEnabled()&&stop->isEnabled()&&observer.isConnected()&&jobs->currentData().toString()==id&&
          observer.currentReport().value("stage").toString()==QStringLiteral("coordinate_export"),
          "completed optimization disables pause but keeps export cancellation and the same observed identity across languages");
    }
    if(!pause->isEnabled()) {observer.pauseJob();check(!observer.isBusy()&&owned.state()==QProcess::Running,
        "a direct public pause request during coordinate export launches no control helper and leaves the owner running");}
    check(stageFixture(QStringLiteral("train")),"return the owned fixture to optimization for the original pause lifecycle assertions");
    observer.refreshJobs();check(waitFor([&]{return !observer.isBusy()&&observer.currentReport().value("stage").toString()==QStringLiteral("train");})&&
        pause->isEnabled()&&stop->isEnabled(),"refresh re-enables pause only for actual training without replacing the lease");
    observer.pauseJob();check(waitFor([&]{return !observer.isBusy()&&observer.currentReport().value("recordedState").toString()==QStringLiteral("paused")&&owned.state()==QProcess::NotRunning;}),
        "explicit pause uses the validated atomic command and waits for actual terminal acknowledgement");
    check(observer.currentReport().value("liveness").toString()==QStringLiteral("ended")&&!resume->isEnabled(),"ended session without a complete checkpoint is not presented as optimizer-state resumable");
    observer.close();check(!observer.isConnected()&&!observer.isBusy(),"closing observer releases only its own helpers and connection");
    if(owned.state()!=QProcess::NotRunning){owned.kill();owned.waitForFinished(1000);}
    const QString fakeRoot=temporary.filePath(QStringLiteral("owned-helper-boundary"));
    QDir().mkpath(QDir(fakeRoot).filePath(QStringLiteral("native/worker")));
    const auto writeOwned=[&](const QString &relative,const QByteArray &data){QFile file(QDir(fakeRoot).filePath(relative));if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;return file.write(data)==data.size();};
    check(writeOwned(QStringLiteral("native/__init__.py"),QByteArray())&&writeOwned(QStringLiteral("native/worker/__init__.py"),QByteArray())&&
        writeOwned(QStringLiteral("native/worker/training_job_connection.py"),QByteArray("import time\ntime.sleep(20)\n")),
        "owned CPU helper boundary fixture is isolated from production modules and trainers");
    {
      gsw::ExistingTrainingDialog closing(python,fakeRoot,env,store);
      closing.refreshJobs();check(closing.isBusy(),"inspection can be cancelled while its own helper is running");
      QElapsedTimer timer;timer.start();closing.close();
      check(!closing.isBusy()&&!closing.isConnected()&&timer.elapsed()<2500,"closing a busy observer promptly destroys only its owned CPU helper");
    }
    {
      gsw::ExistingTrainingDialog retry(python,fakeRoot,env,store);
      retry.refreshJobs();check(waitFor([&]{return !retry.isBusy();})&&!retry.isConnected(),"five-second helper timeout cancels observation without creating or controlling training");
      auto *label=retry.findChild<QLabel*>(QStringLiteral("existingTrainingState"));
      check(label&&label->text()==QCoreApplication::translate("Workbench","现有作业检查超时；仅取消检查辅助进程，未停止训练。"),"helper timeout remains a distinct translated observation error");
      check(writeOwned(QStringLiteral("native/worker/training_job_connection.py"),QByteArray("print('{\"version\":1,\"jobs\":[]}')\n")),"owned helper boundary can be repaired without changing user data");
      retry.refreshJobs();check(waitFor([&]{return !retry.isBusy();})&&!retry.isConnected(),"retry succeeds after an observation helper failure without starting training");
      retry.disconnectJob();
    }
  }
  {
    const QString batchRoot=temporary.filePath(QStringLiteral(".gsw/training-batches/")+QString(32,QLatin1Char('c')));
    QDir().mkpath(batchRoot);const QString planPath=QDir(batchRoot).filePath(QStringLiteral("plan.json"));
    const QString runId=QStringLiteral("run-")+QString(24,QLatin1Char('d'));
    const auto planBytes=QJsonDocument(QJsonObject{{"schema","gsw.training_batch/1"},{"plan_sha256",QString(64,QLatin1Char('e'))},
      {"runs",QJsonArray{QJsonObject{{"id",runId}}}}}).toJson();
    QFile plan(planPath);if(!plan.open(QIODevice::WriteOnly))return 2;plan.write(planBytes);plan.close();
    const auto hash=QString::fromLatin1(QCryptographicHash::hash(planBytes,QCryptographicHash::Sha256).toHex());
    const auto stateBytes=QJsonDocument(QJsonObject{{"schema","gsw.training_batch_state/1"},{"plan_sha256",QString(64,QLatin1Char('e'))},
      {"plan_file_sha256",hash},{"status","paused"},{"runs",QJsonObject{{runId,QJsonObject{{"status","paused"}}}}}}).toJson();
    QFile stateFile(QDir(batchRoot).filePath(QStringLiteral("state.json")));if(!stateFile.open(QIODevice::WriteOnly))return 2;stateFile.write(stateBytes);stateFile.close();
    auto env=QProcessEnvironment::systemEnvironment();env.insert("PYTHONUTF8","1");
    gsw::ExistingTrainingDialog observer(python,repository,env,temporary.path());
    check(observer.connectBatch(planPath),"explicit batch observation accepts a saved plan without launching a coordinator");
    check(waitFor([&]{return !observer.isBusy()&&!observer.currentReport().isEmpty();}),"batch observation uses only the actual read-only CPU metadata helper");
    const auto report=observer.currentReport();
    check(report.value("kind")==QStringLiteral("training_batch")&&report.value("queueState")==QStringLiteral("paused")&&
      report.value("liveness")==QStringLiteral("unknown"),"batch and underlying queue state are separate from single-job records and unverified ownership");
    auto *pause=observer.findChild<QPushButton*>("existingTrainingPause");auto *stop=observer.findChild<QPushButton*>("existingTrainingStop");
    auto *resume=observer.findChild<QPushButton*>("existingTrainingResume");auto *label=observer.findChild<QLabel*>("existingTrainingState");
    check(pause&&stop&&resume&&label&&!pause->isEnabled()&&!stop->isEnabled()&&!resume->isEnabled(),"ambiguous legacy batch metadata cannot authorize control or duplicate resume");
    QSet<QString> texts;
    for(const auto &locale:gsw::AppLanguage::supported()){gsw::AppLanguage::apply(locale,false);texts.insert(label->text());check(observer.currentReport()==report&&observer.isConnected(),"batch locale switching preserves plan, queue and observation without repair or launch");}
    check(texts.size()==3,"batch queue and coordinator status translate immediately in all three languages");
    observer.close();plan.open(QIODevice::ReadOnly);stateFile.open(QIODevice::ReadOnly);
    check(plan.readAll()==planBytes&&stateFile.readAll()==stateBytes&&!QFileInfo::exists(QDir(batchRoot).filePath(".coordinator.json")),"closing batch observation keeps original queue bytes and creates no coordinator");
  }
  std::printf("Runtime UI checks: %d; result: %s\n",checks,passed?"PASS":"FAIL");
  return passed?0:1;
}
