#include "InitializationDialog.h"
#include "InitializationEditor.h"
#include "AppLanguage.h"
#include "UiFormVisualProbe.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
static bool waitFor(const std::function<bool()> &ready) {QElapsedTimer timer;timer.start();while(!ready()&&timer.elapsed()<30000){QEventLoop loop;QTimer::singleShot(10,&loop,&QEventLoop::quit);loop.exec();}return ready();}
static QByteArray digest(const QString &path){QFile file(path);return file.open(QIODevice::ReadOnly)?QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256):QByteArray();}
int main(int argc,char **argv) {
  const bool visualForms=gsw::tests::uiFormsRequested(argc,argv);
  if(!gsw::tests::configureUiFormsPlatform(visualForms)){
    std::fprintf(stderr,"UI forms visual mode requires GITHUB_ACTIONS=true on Windows.\n");return 2;
  }
  QApplication app(argc,argv);auto arguments=app.arguments();
  arguments.removeAll(QStringLiteral("--smoke-test-ui-forms"));if(arguments.size()!=3)return 2;
  const QString root=arguments[1],python=arguments[2];bool passed=true;
  const auto check=[&](bool ok,const char *text){std::printf("Initialization dialog %s: %s\n",ok?"PASS":"FAIL",text);passed&=ok;};
  QTemporaryDir workspace(QDir(qEnvironmentVariable("GSW_INITIALIZATION_TEST_ROOT", QDir::tempPath())).filePath(QStringLiteral("dialog-XXXXXX")));
  if(visualForms){
    if(!workspace.isValid())return 2;
    app.setOrganizationName(QStringLiteral("GSW-owned-initialization-ui-probe"));
    app.setApplicationName(QStringLiteral("GSW-owned-initialization-ui-probe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,workspace.path());
  }
  if(!gsw::tests::installUiFormsEvidence(app,visualForms,workspace.path())){
    std::fprintf(stderr,"UI forms evidence installation refused; preserve the existing evidence root.\n");return 2;
  }
  const QString source=workspace.filePath(QStringLiteral("source"));QDir().mkpath(source);
  QImage image(16,12,QImage::Format_RGB32);image.fill(QColor(31,61,91));check(image.save(QDir(source).filePath(QStringLiteral("image.png"))),"write owned image fixture");
  const QJsonObject initial{{"kind","random"},{"frame","source"},{"count",16},{"seed",0},{"priors",QJsonArray{}},{"bounds",QJsonArray{QJsonArray{0.,0.,2.},QJsonArray{1.,1.,3.}}},
    {"provenance",QJsonObject{{"source","analytic UI fixture"},{"unknown_source_note","retained"}}}};
  const QJsonObject manifest{{"schema","gsw.camera_dataset/1"},{"pose_policy","locked"},{"source",QJsonObject{{"kind","synthetic"}}},{"scale",QJsonObject{{"status","arbitrary"}}},
    {"initialization",initial},{"views",QJsonArray{QJsonObject{{"view_id","train-a"},{"image_path","image.png"},{"split","train"},{"pixel_coordinates","pixel_border"},
      {"K",QJsonArray{QJsonArray{20.,0.,8.},QJsonArray{0.,20.,6.},QJsonArray{0.,0.,1.}}},
      {"c2w_opencv",QJsonArray{QJsonArray{1.,0.,0.,.5},QJsonArray{0.,1.,0.,.5},QJsonArray{0.,0.,1.,0.},QJsonArray{0.,0.,0.,1.}}}}}}};
  const QString metadata=QDir(source).filePath(QStringLiteral("camera_dataset.json"));QFile file(metadata);check(file.open(QIODevice::WriteOnly)&&file.write(QJsonDocument(manifest).toJson())>0,"write raw canonical fixture");file.close();
  QProcess normalize;
#ifdef Q_OS_WIN
  normalize.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args){args->flags|=CREATE_NO_WINDOW;args->startupInfo->dwFlags|=STARTF_USESHOWWINDOW;args->startupInfo->wShowWindow=SW_HIDE;});
#endif
  normalize.setProgram(python);normalize.setArguments({"-B","-c","import sys;sys.path.insert(0,sys.argv[1]);from native.backend.camera_dataset import load_camera_dataset,write_camera_dataset;from pathlib import Path;d=load_camera_dataset(sys.argv[2]);write_camera_dataset(d,Path(sys.argv[2])/'camera_dataset.json');print(d['data_identity'])",root,source});normalize.start();
  check(normalize.waitForFinished(30000)&&normalize.exitCode()==0,"actual adapter admits known cameras without SfM");const QString identity=QString::fromUtf8(normalize.readAllStandardOutput()).trimmed();if(identity.size()!=64)return 1;
  const auto sourceHash=digest(metadata),imageHash=digest(QDir(source).filePath(QStringLiteral("image.png")));
  gsw::InitializationDialog dialog(python,root,workspace.path(),source,identity,initial,QProcessEnvironment::systemEnvironment());
  const auto checkScrollBoundary=[&]{
    auto *body=dialog.findChild<QScrollArea *>(QStringLiteral("initializationBodyScroll"));
    auto *cancel=dialog.findChild<QPushButton *>(QStringLiteral("initializationCancel"));
    auto *create=dialog.findChild<QPushButton *>(QStringLiteral("initializationCreateVersion"));
    auto *summary=dialog.findChild<QLabel *>(QStringLiteral("initializationSummary"));
    check(body&&body->widgetResizable()&&body->widget()&&cancel&&create&&summary&&
        body->isAncestorOf(summary)&&!body->isAncestorOf(cancel)&&!body->isAncestorOf(create),
        "wrapped reports and preview scroll independently of always-reachable create/cancel actions");
    check(dialog.minimumSizeHint().isValid()&&dialog.minimumSizeHint().height()<=620,
        "the dialog layout minimum fits the unchanged 620-logical-pixel narrow-window contract");
  };
  const auto inputsFitHorizontally=[&]{
    auto *body=dialog.findChild<QScrollArea *>(QStringLiteral("initializationBodyScroll"));
    auto *inputs=dialog.findChild<QScrollArea *>(QStringLiteral("initializationInputScroll"));
    bool usable=body&&inputs&&body->horizontalScrollBar()->maximum()==0&&inputs->horizontalScrollBar()->maximum()==0;
    const QStringList names={QStringLiteral("initializationKind"),QStringLiteral("initializationFrame"),
      QStringLiteral("initializationCount"),QStringLiteral("initializationSeed"),
      QStringLiteral("initializationBound00"),QStringLiteral("initializationBound01"),QStringLiteral("initializationBound02"),
      QStringLiteral("initializationBound10"),QStringLiteral("initializationBound11"),QStringLiteral("initializationBound12")};
    for(const QString &name:names){
      auto *field=dialog.findChild<QWidget *>(name);
      if(!field||!inputs){usable=false;continue;}
      const QRect fieldRect(field->mapTo(inputs->viewport(),QPoint()),field->size());
      usable=field->isVisibleTo(inputs->widget())&&fieldRect.left()>=0&&
        fieldRect.right()<inputs->viewport()->width()&&fieldRect.width()>0&&usable;
    }
    check(usable,"all actual initialization fields retain their full horizontal edit area in every language/theme/window size");
    return usable;
  };
  auto request=dialog.requestDocument();
  auto depthRequest=request;depthRequest["suggestion"]=QJsonObject{{"method","user_depth_interval"},{"view_id","train-a"},{"near",2.},{"far",3.}};
  check(dialog.setRequestDocument(depthRequest)&&dialog.requestDocument()==depthRequest,"full reusable request retains explicit suggestion parameters");
  auto invalidResolution=request;invalidResolution["resolution"]=1.5;
  check(!dialog.setRequestDocument(invalidResolution)&&dialog.requestDocument()==depthRequest,"fractional resolution is rejected without coercing the previous request");
  const auto rejectSuggestion=[&](const QJsonObject &suggestion,const char *message){
    auto invalid=depthRequest;invalid["suggestion"]=suggestion;
    check(!dialog.setRequestDocument(invalid)&&dialog.requestDocument()==depthRequest,message);
  };
  rejectSuggestion(QJsonObject{{"method","optical_axis_convergence"},{"half_extents",QJsonArray{1.,2.,3.,4.}}},
      "an imported four-component half-extent cannot be silently truncated into a valid three-component box");
  rejectSuggestion(QJsonObject{{"method","optical_axis_convergence"},{"near",2.}},
      "a cross-method active suggestion parameter is rejected rather than dropped");
  rejectSuggestion(QJsonObject{{"method","optical_axis_convergence"},{"view_ids",QJsonArray{"train-a","train-a"}}},
      "duplicate named view constraints are rejected without resetting the previous request");
  rejectSuggestion(QJsonObject{{"method","user_depth_interval"},{"view_id",7},{"near",2.},{"far",3.}},
      "camera view identifiers are not coerced from numeric values");
  rejectSuggestion(QJsonObject{{"method","user_depth_interval"},{"view_id","train-a"},{"near","2"},{"far",3.}},
      "camera-Z numeric strings are rejected instead of silently accepted as geometry values");
  rejectSuggestion(QJsonObject{{"method","user_depth_interval"},{"view_id","train-a"},{"near",3.},{"far",2.}},
      "unordered declared depth intervals preserve the previous valid request");
  auto malformedBounds=depthRequest;auto malformedInit=initial;malformedInit["bounds"]=QJsonArray{QJsonArray{0.,0.,2.}};malformedBounds["initialization"]=malformedInit;
  check(!dialog.setRequestDocument(malformedBounds)&&dialog.requestDocument()==depthRequest,
      "malformed loaded bounds cannot silently disappear and become an unbounded initializer");
  check(dialog.findChild<QPushButton *>(QStringLiteral("initializationLoadRequest"))&&dialog.findChild<QPushButton *>(QStringLiteral("initializationSaveRequest")),"reusable request import and export are reachable native controls");
  dialog.setRequestDocument(request);dialog.startInspection();
  check(waitFor([&]{return !dialog.isRunning();})&&dialog.report().value("status")=="ready"&&dialog.report().value("point_count")==16&&dialog.report().value("request")==request,
      "real CPU worker returns the bound actual initialization, full coverage and raw request");
  check(dialog.report().value("point_sha256").toString().size()==64&&dialog.report().value("coverage").toObject().value("not_surface_evidence").toBool(),"geometry identity and visibility do not claim a measured surface");
  const auto report=dialog.report();for(const auto &locale:gsw::AppLanguage::supported()){check(gsw::AppLanguage::apply(locale,false),"native language applies immediately");check(dialog.requestDocument()==request&&dialog.report()==report,"three-language switching retains the request and actual worker report without rerunning");checkScrollBoundary();}
  check(gsw::tests::captureUiFormMatrix(visualForms,dialog,QStringLiteral("initializer-3dgs-ready"),
      QSize(1100,760),QSize(560,620),[&]{return QJsonObject{{"request",dialog.requestDocument()},
        {"report",dialog.report()},{"running",dialog.isRunning()}};},inputsFitHorizontally),
      "owned 3DGS initialization native surface matrix preserves its public request and actual CPU report");
  const QString saved=workspace.filePath(QStringLiteral("init-report.json"));check(dialog.saveReport(saved)&&!dialog.saveReport(saved),"report save is explicit and cannot overwrite older evidence");
  auto twoD=request;twoD["backend"]="2dgs";dialog.setRequestDocument(twoD);dialog.startInspection();
  check(waitFor([&]{return !dialog.isRunning();})&&dialog.report().value("status")=="ready"&&dialog.report().value("backend")=="2dgs"&&
      dialog.report().value("clipping").toObject().value("surface_intersection_visibility_not_predicted").toBool()&&
      dialog.report().value("clipping").toObject().value("runtime_gpu_verified")==false,
      "2DGS uses its own clipping contract and does not relabel CPU geometry checks as GPU training proof");
  checkScrollBoundary();
  check(gsw::tests::captureUiFormMatrix(visualForms,dialog,QStringLiteral("initializer-2dgs-ready"),
      QSize(1100,760),QSize(560,620),[&]{return QJsonObject{{"request",dialog.requestDocument()},
        {"report",dialog.report()},{"running",dialog.isRunning()}};},inputsFitHorizontally),
      "owned 2DGS initialization native surface matrix preserves its backend-specific CPU report and request");
  auto optical=request;optical["suggestion"]=QJsonObject{{"method","optical_axis_convergence"}};dialog.setRequestDocument(optical);dialog.requestSuggestion();
  check(waitFor([&]{return !dialog.isRunning();})&&dialog.report().value("status")=="needs_input"&&!dialog.report().contains("suggested_initialization"),
      "degenerate optical axes require input and cannot manufacture an object box or an applicable suggestion");
  auto changed=request;auto noBounds=initial;noBounds.remove("bounds");changed["initialization"]=noBounds;dialog.setRequestDocument(changed);
  check(dialog.report().isEmpty(),"editing invalidates the old report before any new helper starts");dialog.startInspection();
  check(waitFor([&]{return !dialog.isRunning();})&&dialog.report().value("status")=="needs_input"&&dialog.report().value("point_count")==0,"camera-only input requires an explicit depth/range rather than a guessed trajectory box");
  auto *create=dialog.findChild<QPushButton *>(QStringLiteral("initializationCreateVersion"));check(create&&!create->isEnabled(),"an incomplete initialization cannot publish a version");
  dialog.setRequestDocument(request);dialog.startInspection();dialog.cancelOperation();
  check(waitFor([&]{return !dialog.isRunning();})&&dialog.report().isEmpty(),"cancel stops only the real owned initialization helper and clears stale evidence");
  check(dialog.setRequestDocument(depthRequest),"explicit named-training-view depth suggestion request is admitted");
  const QString config=workspace.filePath(QStringLiteral("request.json"));
  check(dialog.saveRequest(config)&&!dialog.saveRequest(config),"full request is saved without overwriting evidence");
  dialog.setRequestDocument(request);check(dialog.loadRequest(config)&&dialog.requestDocument()==depthRequest,"saved full precision configuration reopens with its suggestion and provenance");
  dialog.requestSuggestion();
  check(waitFor([&]{return !dialog.isRunning();})&&dialog.report().value("status")=="suggestion_requires_confirmation"&&dialog.requestDocument()==depthRequest,
      "real camera-Z depth suggestion is visible but not automatically applied");
  checkScrollBoundary();
  check(gsw::tests::captureUiFormMatrix(visualForms,dialog,QStringLiteral("initializer-suggestion-pending"),
      QSize(1100,760),QSize(560,620),[&]{return QJsonObject{{"request",dialog.requestDocument()},
        {"report",dialog.report()},{"running",dialog.isRunning()}};},inputsFitHorizontally),
      "owned suggestion native surface matrix cannot adopt or publish the unconfirmed suggestion");
  auto *apply=dialog.findChild<QPushButton *>(QStringLiteral("initializationApplySuggestion"));
  check(apply&&apply->isEnabled()&&create&&!create->isEnabled(),"a suggestion cannot publish before explicit adoption and rechecking");
  dialog.applySuggestion();
  check(waitFor([&]{return !dialog.isRunning();})&&dialog.report().value("status")=="ready"&&!dialog.requestDocument().contains("suggestion")&&dialog.requestDocument().value("initialization").toObject().value("provenance").toObject().contains("derived_suggestion"),
      "explicit adoption preserves assumptions and rechecks actual generated points");
  const QString pointHash=dialog.report().value("point_sha256").toString();QString version;QJsonObject versionSummary;int created=0;
  QObject::connect(&dialog,&gsw::InitializationDialog::versionCreated,[&](const QString &path,const QJsonObject &summary){++created;version=path;versionSummary=summary;});
  dialog.createVersion();
  const bool creationFinished=waitFor([&]{return !dialog.isRunning();});
  if(!creationFinished||created!=1||!QFileInfo(version).isDir()){
    const auto *diagnostic=dialog.findChild<QLabel *>(QStringLiteral("initializationSummary"));
    const auto *ownedProcess=dialog.findChild<QProcess *>();
    const auto bytes=QJsonDocument(QJsonObject{{"workspace",workspace.path()},{"versionSignalCount",created},{"versionPath",version},
      {"workerArguments",QJsonArray::fromStringList(ownedProcess?ownedProcess->arguments():QStringList{})},
      {"summary",diagnostic?diagnostic->text():QString()},{"report",dialog.report()}}).toJson(QJsonDocument::Compact);
    std::fprintf(stderr,"Create-version diagnostic: %s\n",bytes.constData());
  }
  check(creationFinished&&created==1&&QFileInfo(version).isDir()&&versionSummary.value("posePolicy")=="locked"&&versionSummary.value("dataIdentity").toString().size()==64,
      "the actual create-version worker publishes a distinct locked-camera dataset exactly once");
  const auto newMetadata=QDir(version).filePath(QStringLiteral("camera_dataset.json"));QFile versionFile(newMetadata);
  const bool versionOpened=versionFile.open(QIODevice::ReadOnly);const auto versionManifest=versionOpened?QJsonDocument::fromJson(versionFile.readAll()).object():QJsonObject{};
  const auto frozen=versionManifest.value("initialization").toObject();
  const auto artifact=frozen.value("materialization").toObject();
  check(frozen.value("kind")=="random"&&artifact.value("point_sha256")==pointHash&&digest(QDir(version).filePath(artifact.value("path").toString())).toHex()==artifact.value("sha256").toString(),
      "published random initialization has actual frozen points, a verified artifact hash and no false external-prior label");
  dialog.createVersion();check(created==1&&!dialog.isRunning(),"an already published version cannot be silently published again");
  gsw::InitializationDialog reopened(python,root,workspace.path(),version,versionSummary.value("dataIdentity").toString(),frozen,QProcessEnvironment::systemEnvironment());
  reopened.startInspection();
  const bool reopenedFinished=waitFor([&]{return !reopened.isRunning();});
  if(!reopenedFinished||reopened.report().value("status")!="ready"){
    const auto *diagnostic=reopened.findChild<QLabel *>(QStringLiteral("initializationSummary"));
    std::fprintf(stderr,"Reopen diagnostic: %s\nReport: %s\n",diagnostic?diagnostic->text().toUtf8().constData():"missing summary",
      QJsonDocument(reopened.report()).toJson(QJsonDocument::Compact).constData());
  }
  check(reopenedFinished&&reopened.report().value("status")=="ready"&&reopened.report().value("point_sha256")==pointHash,
      "reopening the actual derived dataset reuses the identical frozen point set");
  auto revised=reopened.requestDocument();auto revisedInit=revised.value("initialization").toObject();revisedInit["seed"]=1;revised["initialization"]=revisedInit;reopened.setRequestDocument(revised);reopened.startInspection();
  check(waitFor([&]{return !reopened.isRunning();})&&reopened.report().value("status")=="ready"&&reopened.report().value("point_sha256")!=pointHash&&!reopened.report().value("warnings").toArray().isEmpty(),
      "editing a saved recipe regenerates points instead of reusing stale materialization");
  auto *summary=reopened.findChild<QLabel *>(QStringLiteral("initializationSummary"));
  for(const auto &locale:gsw::AppLanguage::supported()){gsw::AppLanguage::apply(locale,false);
    const auto warning=QCoreApplication::translate("Workbench","初始化配方已改变：旧冻结点集已失效，将按当前种子、点数、框架和范围重新生成。");
    check(summary&&summary->text().contains(warning),"materialization invalidation warning is immediately visible in each language");}
  gsw::InitializationDialog unavailable(workspace.filePath(QStringLiteral("missing-python.exe")),root,workspace.path(),source,identity,initial,QProcessEnvironment::systemEnvironment());unavailable.startInspection();
  check(!unavailable.isRunning()&&unavailable.report().isEmpty(),"missing helper does not launch training or manufacture a report");
  const auto closeOwned=[&](bool inspect,bool running){
    QPointer<gsw::InitializationDialog> owned=new gsw::InitializationDialog(python,root,workspace.path(),source,identity,initial,QProcessEnvironment::systemEnvironment());
    owned->setAttribute(Qt::WA_DeleteOnClose);auto *process=owned->findChild<QProcess *>();QPointer<QProcess> processGuard=process;
    if(inspect){owned->startInspection();if(running)check(waitFor([&]{return !owned||process->state()==QProcess::Running;}),"actual owned helper entered running state before close");}
    QTimer::singleShot(0,owned,[owned]{if(owned)owned->close();});
    const bool deleted=waitFor([&]{return owned.isNull();});return deleted&&processGuard.isNull();
  };
  check(closeOwned(false,false),"idle close deletes the actual dialog without a visible window");
  check(closeOwned(true,false),"queued inspection close destroys the actual dialog and its owned helper");
  check(closeOwned(true,true),"running inspection close cancels and destroys only its owned helper");
  // Keep the caller's original test root. This additional owned child makes
  // the managed output exactly two characters deeper, exercising the real
  // final-artifact boundary rather than shortening a failing CI path.
  const QString boundaryWorkspace=workspace.filePath(QStringLiteral("z"));check(QDir().mkpath(boundaryWorkspace),"prepare a second workspace two characters deeper without shortening the original fixture");
  gsw::InitializationDialog boundary(python,root,boundaryWorkspace,source,identity,initial,QProcessEnvironment::systemEnvironment());
  boundary.startInspection();check(waitFor([&]{return !boundary.isRunning();})&&boundary.report().value("status")=="ready","two-character-deeper workspace still inspects the actual source initialization");
  int boundaryCreated=0;QObject::connect(&boundary,&gsw::InitializationDialog::versionCreated,[&](const QString &,const QJsonObject &){++boundaryCreated;});
  boundary.createVersion();
  const auto boundaryArgs=boundary.findChild<QProcess *>()->arguments();
  const int outputIndex=boundaryArgs.indexOf(QStringLiteral("--output-directory"));
  const QString boundaryOutput=outputIndex>=0&&outputIndex+1<boundaryArgs.size()?boundaryArgs[outputIndex+1]:QString();
  const int finalReportLength=QDir(boundaryOutput).filePath(QStringLiteral("source/initialization/reports/")+QString(64,QLatin1Char('0'))+QStringLiteral(".json")).size();
  const bool boundaryFinished=waitFor([&]{return !boundary.isRunning();});
  const auto *boundarySummary=boundary.findChild<QLabel *>(QStringLiteral("initializationSummary"));
  if(finalReportLength>=260){
    check(boundaryFinished&&boundaryCreated==0&&!boundaryOutput.isEmpty()&&!QFileInfo::exists(boundaryOutput)&&
        boundarySummary&&boundarySummary->text().contains(QStringLiteral("initialization_unsafe_output")),
        "a true over-MAX_PATH final artifact is explicitly refused without publishing a partial version");
  }else{
    check(boundaryFinished&&boundaryCreated==1&&QFileInfo(boundaryOutput).isDir()&&boundary.report().value("status")=="ready",
        "the two-character-deeper final output still publishes when all actual artifact paths fit");
  }
  check(digest(metadata)==sourceHash&&digest(QDir(source).filePath(QStringLiteral("image.png")))==imageHash,"inspection, language changes and cancellation leave the source metadata and image bytes unchanged");
  if(visualForms){
    check(!dialog.isVisible()&&QApplication::platformName()=="windows","the explicit remote visual probe has hidden its owned windows without closing the original dialog");
  }else{
  check(!dialog.isVisible()&&QApplication::platformName()=="offscreen","the production dialog is tested without show, GSW, OpenGL or a visible window");
  }
  gsw::tests::finishUiFormsEvidence(app,visualForms);
  return passed?0:1;
}
