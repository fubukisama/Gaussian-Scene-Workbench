#include "ExistingTrainingDialog.h"
#include "AppLanguage.h"
#include "ProcessSupervisor.h"
#include "TrainingMonitorWidget.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {
constexpr qsizetype MaximumOutput=16*1024*1024;
QString text(const char *source){return QCoreApplication::translate("Workbench",source);}
QString storePath(const QString &input){
  const auto path=QDir::cleanPath(QFileInfo(input.trimmed()).absoluteFilePath());
  const auto nested=QDir(path).filePath(QStringLiteral(".gsw/jobs/training"));
  return QFileInfo(nested).isDir()?nested:path;
}
bool jobId(const QString &value){static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));return pattern.match(value).hasMatch();}
QString liveLabel(const QString &state){
  if(state==QStringLiteral("live"))return text(AppLanguage::source("已验证存活"));
  if(state==QStringLiteral("ended"))return text(AppLanguage::source("会话已结束"));
  return text(AppLanguage::source("存活状态未核实（不能认定运行中）"));
}
QString stateLabel(const QString &state){
  if(state==QStringLiteral("running"))return text(AppLanguage::source("运行中"));
  if(state==QStringLiteral("queued")||state==QStringLiteral("ready"))return text(AppLanguage::source("等待中"));
  if(state==QStringLiteral("paused"))return text(AppLanguage::source("已暂停"));
  if(state==QStringLiteral("done")||state==QStringLiteral("completed"))return text(AppLanguage::source("已完成"));
  if(state==QStringLiteral("interrupted"))return text(AppLanguage::source("已中断；未自动恢复"));
  if(state==QStringLiteral("failed"))return text(AppLanguage::source("失败"));
  if(state==QStringLiteral("cancelled"))return text(AppLanguage::source("已取消"));
  if(state==QStringLiteral("cancelling")||state==QStringLiteral("cancelling_checkpoint")||state==QStringLiteral("pausing"))return text(AppLanguage::source("正在停止"));
  return state.isEmpty()?text(AppLanguage::source("未验证")):state;
}
bool terminal(const QString &state){return QStringList{QStringLiteral("paused"),QStringLiteral("done"),QStringLiteral("failed"),QStringLiteral("cancelled")}.contains(state);}
QString json(const QJsonValue &value){if(value.isObject())return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented));
  if(value.isArray())return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented));return QString();}
}
class ExistingTrainingDialog::State {
public:
  QString python,backend,attachedStore,attachedId,monitorJob,operation,errorDetail;
  QProcessEnvironment environment;
  QJsonArray jobs;
  QJsonObject report;
  const char *message=nullptr;
  bool connected=false,failedObservation=false,batch=false;
  QString batchPlan;
  QLineEdit *store=nullptr;
  QComboBox *job=nullptr;
  QLabel *state=nullptr,*runtimeState=nullptr;
  QPlainTextEdit *details=nullptr;
  TrainingMonitorWidget *monitor=nullptr;
  QPushButton *browse=nullptr,*refresh=nullptr,*connect=nullptr,*detach=nullptr,*pause=nullptr,*stop=nullptr,*resume=nullptr,*result=nullptr;
  QTimer *poll=nullptr,*timeout=nullptr;
  QProcess *helper=nullptr;
  QByteArray output,errors;
};

ExistingTrainingDialog::ExistingTrainingDialog(const QString &python,const QString &backendRoot,
    const QProcessEnvironment &environment,const QString &initialJobStore,QWidget *parent)
    :QDialog(parent),m(std::make_unique<State>()){
  setObjectName(QStringLiteral("existingTrainingDialog"));
  AppLanguage::bind(this,"windowTitle",AppLanguage::source("连接现有训练任务"));
  setMinimumSize(360,300);resize(840,760);
  m->python=python;m->backend=backendRoot;m->environment=environment;
  auto *layout=new QVBoxLayout(this);
  auto *hint=AppLanguage::text(new QLabel(this),AppLanguage::source(
      "连接仅观察现有作业，不创建训练。关闭或断开此窗口只停止本窗口的检查辅助进程，不停止已有训练；未核实的存活状态不会显示为正在运行。"));
  hint->setWordWrap(true);layout->addWidget(hint);
  auto *form=new QFormLayout;form->setRowWrapPolicy(QFormLayout::WrapLongRows);layout->addLayout(form);
  auto *storeRow=new QWidget(this);storeRow->setProperty("gswLayoutContainer",true);
  auto *storeLayout=new QHBoxLayout(storeRow);storeLayout->setContentsMargins(0,0,0,0);
  m->store=new QLineEdit(initialJobStore,storeRow);m->store->setObjectName(QStringLiteral("existingTrainingJobStore"));
  m->browse=AppLanguage::text(new QPushButton(storeRow),AppLanguage::source("浏览..."));
  m->browse->setObjectName(QStringLiteral("existingTrainingBrowse"));storeLayout->addWidget(m->store,1);storeLayout->addWidget(m->browse);
  form->addRow(text(AppLanguage::source("工程或训练作业目录")),storeRow);
  AppLanguage::text(qobject_cast<QLabel*>(form->labelForField(storeRow)),AppLanguage::source("工程或训练作业目录"));
  m->job=new QComboBox(this);m->job->setObjectName(QStringLiteral("existingTrainingJobCombo"));
  m->job->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m->job->setMinimumContentsLength(12);
  form->addRow(text(AppLanguage::source("已有作业")),m->job);
  AppLanguage::text(qobject_cast<QLabel*>(form->labelForField(m->job)),AppLanguage::source("已有作业"));
  auto *commands=new QGridLayout;layout->addLayout(commands);
  int commandIndex=0;
  const auto button=[this,commands,&commandIndex](const char *source,const char *name){auto *b=AppLanguage::text(new QPushButton(this),source);b->setObjectName(QString::fromLatin1(name));commands->addWidget(b,commandIndex/3,commandIndex%3);++commandIndex;return b;};
  m->refresh=button(AppLanguage::source("刷新作业"),"existingTrainingRefresh");
  m->connect=button(AppLanguage::source("连接观察"),"existingTrainingConnect");
  m->detach=button(AppLanguage::source("断开观察"),"existingTrainingDisconnect");
  m->pause=button(AppLanguage::source("暂停训练"),"existingTrainingPause");
  m->stop=button(AppLanguage::source("停止训练"),"existingTrainingStop");
  m->resume=button(AppLanguage::source("继续训练"),"existingTrainingResume");
  m->result=button(AppLanguage::source("查看成果"),"existingTrainingResult");
  m->state=new QLabel(this);m->state->setObjectName(QStringLiteral("existingTrainingState"));m->state->setTextFormat(Qt::PlainText);
  m->state->setWordWrap(true);m->state->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m->state);
  m->runtimeState=new QLabel(this);m->runtimeState->setObjectName(QStringLiteral("existingTrainingRuntimeState"));m->runtimeState->setTextFormat(Qt::PlainText);
  m->runtimeState->setWordWrap(true);layout->addWidget(m->runtimeState);
  m->monitor=new TrainingMonitorWidget(this);m->monitor->setObjectName(QStringLiteral("existingTrainingMonitor"));layout->addWidget(m->monitor,1);
  m->details=new QPlainTextEdit(this);m->details->setObjectName(QStringLiteral("existingTrainingDetails"));m->details->setReadOnly(true);
  m->details->setMaximumBlockCount(1000);m->details->setMaximumHeight(200);layout->addWidget(m->details);
  auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,this);AppLanguage::text(buttons->button(QDialogButtonBox::Close),AppLanguage::source("关闭"));
  connect(buttons,&QDialogButtonBox::rejected,this,&ExistingTrainingDialog::reject);layout->addWidget(buttons);
  m->poll=new QTimer(this);m->poll->setInterval(2000);
  connect(m->poll,&QTimer::timeout,this,[this]{if(m->connected&&!m->helper)startHelper(QStringLiteral("inspect"));});
  m->timeout=new QTimer(this);m->timeout->setSingleShot(true);m->timeout->setInterval(5000);
  connect(m->timeout,&QTimer::timeout,this,[this]{m->message=AppLanguage::source("现有作业检查超时；仅取消检查辅助进程，未停止训练。");m->failedObservation=true;cancelOwnedHelper();refreshPresentation();});
  connect(m->browse,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,text(AppLanguage::source("选择工程或训练作业目录")),m->store->text());if(!path.isEmpty())m->store->setText(path);});
  connect(m->store,&QLineEdit::textChanged,this,[this]{if(!m->connected){m->jobs={};m->job->clear();m->report={};m->message=nullptr;refreshPresentation();}});
  connect(m->job,&QComboBox::currentIndexChanged,this,[this]{refreshEnabled();});
  connect(m->refresh,&QPushButton::clicked,this,&ExistingTrainingDialog::refreshJobs);
  connect(m->connect,&QPushButton::clicked,this,&ExistingTrainingDialog::connectSelectedJob);
  connect(m->detach,&QPushButton::clicked,this,&ExistingTrainingDialog::disconnectJob);
  connect(m->pause,&QPushButton::clicked,this,&ExistingTrainingDialog::pauseJob);
  connect(m->stop,&QPushButton::clicked,this,&ExistingTrainingDialog::stopJob);
  connect(m->resume,&QPushButton::clicked,this,[this]{if(m->resume->isEnabled()){
    if(m->batch)emit resumeBatchRequested(m->batchPlan);
    else emit resumeRequested(m->report.value("configurationPath").toString(),m->report.value("outputDirectory").toString());}});
  connect(m->result,&QPushButton::clicked,this,[this]{if(m->result->isEnabled())emit resultRequested(m->report.value("outputDirectory").toString());});
  AppLanguage::onChanged(this,[this]{refreshPresentation();});
  refreshPresentation();
}
ExistingTrainingDialog::~ExistingTrainingDialog(){m->poll->stop();cancelOwnedHelper();}
QJsonObject ExistingTrainingDialog::currentReport()const{return m->report;}
bool ExistingTrainingDialog::isConnected()const{return m->connected;}
bool ExistingTrainingDialog::isBusy()const{return m->helper!=nullptr;}
bool ExistingTrainingDialog::connectBatch(const QString &planPath){
  if(m->helper||!QDir::isAbsolutePath(planPath)||QFileInfo(planPath).fileName()!=QStringLiteral("plan.json")||!QFileInfo(planPath).isFile())return false;
  disconnectJob();m->batch=true;m->batchPlan=QFileInfo(planPath).absoluteFilePath();m->store->setText(m->batchPlan);
  m->connected=true;m->failedObservation=false;m->message=nullptr;startHelper(QStringLiteral("inspect"));m->poll->start();return true;
}
void ExistingTrainingDialog::refreshJobs(){if(m->helper)return;startHelper(m->connected?QStringLiteral("inspect"):QStringLiteral("list"));}
void ExistingTrainingDialog::connectSelectedJob(){
  if(m->helper||m->connected||!jobId(m->job->currentData().toString()))return;
  m->attachedStore=storePath(m->store->text());m->attachedId=m->job->currentData().toString();m->connected=true;m->report={};m->failedObservation=false;
  startHelper(QStringLiteral("inspect"));m->poll->start();
}
void ExistingTrainingDialog::disconnectJob(){
  m->connected=false;m->poll->stop();cancelOwnedHelper();m->attachedStore.clear();m->attachedId.clear();m->monitorJob.clear();m->report={};m->failedObservation=false;m->batch=false;m->batchPlan.clear();
  m->message=AppLanguage::source("观察已断开；已有训练和原始数据保持不变。");m->errorDetail.clear();refreshPresentation();
}
void ExistingTrainingDialog::cancelOwnedHelper(){
  m->timeout->stop();if(!m->helper)return;auto *helper=m->helper;m->helper=nullptr;helper->disconnect(this);
  if(helper->state()!=QProcess::NotRunning){helper->kill();helper->waitForFinished(1000);}delete helper;
}
void ExistingTrainingDialog::startHelper(const QString &operation){
  if(m->helper)return;
  const QString module=m->batch?QStringLiteral("native.worker.training_batch_connection"):QStringLiteral("native.worker.training_job_connection");
  if(!QFileInfo(m->python).isFile()||!QFileInfo(QDir(m->backend).filePath(m->batch?QStringLiteral("native/worker/training_batch_connection.py"):QStringLiteral("native/worker/training_job_connection.py"))).isFile()){
    m->message=AppLanguage::source("现有作业检查后端不可用；未启动训练。");m->failedObservation=true;refreshPresentation();return;}
  const QString store=m->connected?m->attachedStore:storePath(m->store->text());
  if(m->store->text().trimmed().isEmpty()||(!m->batch&&!QFileInfo(store).isDir())||(m->batch&&!QFileInfo(m->batchPlan).isFile())){
    m->message=AppLanguage::source("请选择存在的工程或训练作业目录。");m->failedObservation=true;refreshPresentation();return;}
  m->operation=operation;m->output.clear();m->errors.clear();m->message=nullptr;m->errorDetail.clear();
  auto *helper=new QProcess(this);m->helper=helper;
  helper->setWorkingDirectory(m->backend);helper->setProcessEnvironment(m->environment.isEmpty()?QProcessEnvironment::systemEnvironment():m->environment);
#ifdef Q_OS_WIN
  helper->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args){args->flags|=CREATE_NO_WINDOW;args->startupInfo->dwFlags|=STARTF_USESHOWWINDOW;args->startupInfo->wShowWindow=SW_HIDE;});
#endif
  connect(helper,&QProcess::readyReadStandardOutput,this,[this]{readHelperOutput(false);});
  connect(helper,&QProcess::readyReadStandardError,this,[this]{readHelperOutput(true);});
  connect(helper,&QProcess::finished,this,[this](const int code,const QProcess::ExitStatus status){finishHelper(code,status==QProcess::NormalExit);});
  connect(helper,&QProcess::errorOccurred,this,[this](const QProcess::ProcessError error){if(error==QProcess::FailedToStart&&m->helper){m->errorDetail=m->helper->errorString();finishHelper(-1,false);}});
  QStringList arguments{QStringLiteral("-B"),QStringLiteral("-m"),module};
  if(m->batch){arguments<<QStringLiteral("--operation")<<(operation==QStringLiteral("inspect")?operation:QStringLiteral("command"))
      <<QStringLiteral("--plan")<<m->batchPlan;
    if(operation!=QStringLiteral("inspect"))arguments<<QStringLiteral("--command")<<operation<<QStringLiteral("--session")<<m->report.value("session").toString();
  }else{arguments<<QStringLiteral("--job-store")<<store;
    if(operation!=QStringLiteral("list")){arguments<<QStringLiteral("--job-id")<<m->attachedId;if(operation!=QStringLiteral("inspect"))arguments<<QStringLiteral("--command")<<operation;}}
  m->timeout->start();refreshEnabled();helper->start(m->python,arguments,QIODevice::ReadOnly);
}
void ExistingTrainingDialog::readHelperOutput(const bool errors){
  if(!m->helper)return;auto &target=errors?m->errors:m->output;
  m->helper->setReadChannel(errors?QProcess::StandardError:QProcess::StandardOutput);
  const qint64 budget=MaximumOutput-m->errors.size()-m->output.size();target+=m->helper->read(budget+1);
  if(m->errors.size()+m->output.size()>MaximumOutput){m->message=AppLanguage::source("现有作业检查输出超过 16 MiB；仅取消检查辅助进程。");m->failedObservation=true;cancelOwnedHelper();refreshPresentation();}
}
void ExistingTrainingDialog::finishHelper(const int exitCode,const bool normalExit){
  if(!m->helper)return;readHelperOutput(false);if(!m->helper)return;readHelperOutput(true);if(!m->helper)return;
  m->timeout->stop();auto *helper=m->helper;m->helper=nullptr;helper->deleteLater();
  QJsonParseError error;const auto doc=QJsonDocument::fromJson(m->output,&error);const auto report=doc.object();
  if(!normalExit||exitCode!=0||error.error!=QJsonParseError::NoError||!doc.isObject()||report.value("version").toInt()!=1||report.contains("error")){
    m->message=AppLanguage::source("现有作业检查失败：%1。仅保留旧记录，不据此控制训练。");m->failedObservation=true;
    m->errorDetail=report.value("error").toString(QString::fromUtf8(m->errors).left(8192));if(m->errorDetail.isEmpty())m->errorDetail=text(AppLanguage::source("无有效辅助报告"));refreshPresentation();return;}
  if(m->batch){
    bool valid=report.value("schema")==QStringLiteral("gsw.training_batch_connection/1");
    if(m->operation==QStringLiteral("inspect"))valid=valid&&report.value("kind")==QStringLiteral("training_batch")&&
      QDir::cleanPath(QDir::fromNativeSeparators(report.value("planPath").toString()))==m->batchPlan&&
      report.value("state").isObject()&&report.value("controlAvailable").isBool()&&
      QStringList{QStringLiteral("live"),QStringLiteral("unknown"),QStringLiteral("ended")}.contains(report.value("liveness").toString());
    else for(const auto &key:QStringList{QStringLiteral("session"),QStringLiteral("batchId"),QStringLiteral("planSha256"),QStringLiteral("planFileSha256")})
      valid=valid&&!report.value(key).toString().isEmpty()&&report.value(key)==m->report.value(key);
    if(!valid){m->message=AppLanguage::source("现有作业身份或状态报告无效；未发送控制命令。");m->failedObservation=true;refreshPresentation();return;}
    if(m->operation==QStringLiteral("inspect"))m->report=report;
    else{m->message=AppLanguage::source("控制请求已发送；等待现有训练确认，不代表已经暂停或停止。");QTimer::singleShot(0,this,[this]{if(m->connected&&!m->helper)startHelper(QStringLiteral("inspect"));});}
    m->failedObservation=false;refreshPresentation();emit reportUpdated(report);return;
  }
  if(m->operation==QStringLiteral("list")){
    if(!report.value("jobs").isArray()||report.value("jobs").toArray().size()>4096){m->message=AppLanguage::source("现有作业清单无效；未连接或启动训练。");m->failedObservation=true;refreshPresentation();return;}
    const auto jobs=report.value("jobs").toArray();for(const auto &entry:jobs){if(!entry.isObject()||!jobId(entry.toObject().value("jobId").toString())){m->message=AppLanguage::source("现有作业清单无效；未连接或启动训练。");m->failedObservation=true;refreshPresentation();return;}}
    const QString selected=m->job->currentData().toString();m->jobs=jobs;const QSignalBlocker blocker(m->job);m->job->clear();
    for(const auto &entry:jobs)m->job->addItem(QString(),entry.toObject().value("jobId").toString());
    const int selectedIndex=m->job->findData(selected);if(selectedIndex>=0)m->job->setCurrentIndex(selectedIndex);
  }else if(m->operation==QStringLiteral("inspect")){
    if(report.value("jobId").toString()!=m->attachedId||!report.value("job").isObject()||!report.value("controlAvailable").isBool()||
        !QStringList{QStringLiteral("live"),QStringLiteral("unverified"),QStringLiteral("ended")}.contains(report.value("liveness").toString())){
      m->message=AppLanguage::source("现有作业身份或状态报告无效；未发送控制命令。");m->failedObservation=true;refreshPresentation();return;}
    m->report=report;
    const bool verified=report.value("liveness").toString()==QStringLiteral("live")||
        (report.value("liveness").toString()==QStringLiteral("ended")&&terminal(report.value("recordedState").toString()));
    if(verified){
      if(m->monitorJob!=m->attachedId){m->monitorJob=m->attachedId;m->monitor->beginTraining(report.value("job").toObject().value("output_scene").toString(m->attachedId),report.value("backend").toString(),report.value("totalIterations").toInt());}
      const auto status=ProcessSupervisor::decodeStatusEvent(QJsonDocument(report.value("statusEvent").toObject()).toJson(QJsonDocument::Compact));
      if(status)m->monitor->updateStatus(*status);
      if(terminal(report.value("recordedState").toString()))m->monitor->finishTraining(report.value("recordedState").toString()==QStringLiteral("done"),report.value("recordedState").toString()==QStringLiteral("cancelled"),report.value("recordedState").toString()==QStringLiteral("paused"));
    }
  }else{m->message=AppLanguage::source("控制请求已发送；等待现有训练确认，不代表已经暂停或停止。");QTimer::singleShot(0,this,[this]{if(m->connected&&!m->helper)startHelper(QStringLiteral("inspect"));});}
  m->failedObservation=false;refreshPresentation();emit reportUpdated(report);
}
void ExistingTrainingDialog::requestCommand(const QString &command){if(m->failedObservation||!m->connected||m->helper||m->report.value("liveness").toString()!=QStringLiteral("live")||!m->report.value("controlAvailable").toBool()||
    (command==QStringLiteral("pause")&&m->report.value("stage").toString()==QStringLiteral("coordinate_export")))return;startHelper(command);}
void ExistingTrainingDialog::pauseJob(){requestCommand(QStringLiteral("pause"));}
void ExistingTrainingDialog::stopJob(){
  if(!m->stop->isEnabled())return;
  QMessageBox confirmation(QMessageBox::Question,text(m->batch?AppLanguage::source("取消已保存检查点的批量队列？"):AppLanguage::source("停止现有训练？")),
      text(m->batch?AppLanguage::source("将安全暂停活动训练并保留完整检查点与待执行队列。协调器结果标记为已取消，底层训练状态仍为已暂停；继续须显式新建会话。"):
        AppLanguage::source("将向已核实的训练会话发送停止请求。不会删除数据或覆盖已有完整检查点；停止不等于保存新的完整进度。是否继续？")),QMessageBox::Yes|QMessageBox::No,this);
  confirmation.setDefaultButton(QMessageBox::No);if(confirmation.exec()==QMessageBox::Yes)requestCommand(m->batch?QStringLiteral("cancel"):QStringLiteral("stop"));
}
void ExistingTrainingDialog::refreshEnabled(){
  const bool busy=m->helper!=nullptr;
  m->store->setEnabled(!m->connected&&!busy);m->browse->setEnabled(!m->connected&&!busy);m->job->setEnabled(!m->connected&&!busy);
  m->refresh->setEnabled(!busy);m->connect->setEnabled(!busy&&!m->connected&&jobId(m->job->currentData().toString()));m->detach->setEnabled(m->connected||busy);
  const bool live=m->connected&&!busy&&!m->failedObservation&&m->report.value("liveness").toString()==QStringLiteral("live")&&m->report.value("controlAvailable").toBool();
  m->pause->setEnabled(live&&m->report.value("stage").toString()!=QStringLiteral("coordinate_export"));m->stop->setEnabled(live);
  if(m->batch){m->job->setEnabled(false);m->connect->setEnabled(false);
    m->resume->setEnabled(m->connected&&!busy&&!m->failedObservation&&m->report.value("resumeAvailable").toBool());
    m->result->setEnabled(false);return;}
  const QString state=m->report.value("recordedState").toString();const QString liveness=m->report.value("liveness").toString();
  const bool ended=liveness==QStringLiteral("ended")||(liveness==QStringLiteral("unverified")&&terminal(state)&&m->report.value("workerIdentity").isUndefined());
  m->resume->setEnabled(m->connected&&!busy&&!m->failedObservation&&ended&&m->report.value("checkpointState").toString()==QStringLiteral("available_unverified")&&
      !m->report.value("configurationPath").toString().isEmpty()&&!m->report.value("outputDirectory").toString().isEmpty());
  m->result->setEnabled(m->connected&&!busy&&!m->report.value("outputDirectory").toString().isEmpty());
}
void ExistingTrainingDialog::refreshPresentation(){
  if(m->batch){
    m->monitor->hide();m->runtimeState->clear();
    m->state->setText(m->message?text(m->message).arg(m->errorDetail):text(AppLanguage::source("批量队列：%1；协调器：%2；存活状态：%3；待执行：%4"))
      .arg(stateLabel(m->report.value("queueState").toString()),stateLabel(m->report.value("coordinatorOutcome").toString()),
        liveLabel(m->report.value("liveness").toString()),QString::number(m->report.value("pendingRunIds").toArray().size())));
    m->details->setPlainText(json(m->report));refreshEnabled();return;
  }
  const QSignalBlocker blocker(m->job);
  for(int i=0;i<m->jobs.size()&&i<m->job->count();++i){auto row=m->jobs[i].toObject();
    const QString id=row.value("jobId").toString();
    // The list is an earlier snapshot. An accepted observation of this exact
    // attached job is authoritative for its current presentation, not a reason
    // to refresh the worker, change selection, or mutate the stored job.
    if(m->connected&&!m->failedObservation&&id==m->attachedId&&
        m->report.value("jobId").toString()==id){
      row.insert(QStringLiteral("recordedState"),m->report.value("recordedState"));
      row.insert(QStringLiteral("liveness"),m->report.value("liveness"));
    }
    const QString name=row.value("outputName").toString(row.value("jobId").toString());
    const QString caption=QStringLiteral("%1 | %2 | %3 | %4").arg(name,row.value("jobId").toString(),stateLabel(row.value("recordedState").toString()),liveLabel(row.value("liveness").toString()));m->job->setItemText(i,caption);m->job->setItemData(i,caption,Qt::ToolTipRole);}
  if(m->message){auto message=text(m->message);if(message.contains(QStringLiteral("%1")))message=message.arg(m->errorDetail);m->state->setText(message);}
  else if(!m->connected)m->state->setText(text(AppLanguage::source("未连接；刷新清单后明确选择已有作业。")));
  else if(m->report.isEmpty())m->state->setText(text(AppLanguage::source("正在检查所选作业；尚未确认存活状态。")));
  else m->state->setText(text(AppLanguage::source("记录状态：%1；存活状态：%2；迭代：%3 / %4。"))
      .arg(stateLabel(m->report.value("recordedState").toString()),liveLabel(m->report.value("liveness").toString()),
           m->report.value("iteration").isDouble()?QString::number(m->report.value("iteration").toInteger()):QStringLiteral("—"),
           m->report.value("totalIterations").isDouble()?QString::number(m->report.value("totalIterations").toInteger()):QStringLiteral("—")));
  const auto job=m->report.value("job").toObject();const auto runtime=m->report.value("runtimeConfiguration").toObject();
  m->monitor->setVisible(m->connected&&!m->failedObservation&&
      (m->report.value("liveness").toString()==QStringLiteral("live")||
       (m->report.value("liveness").toString()==QStringLiteral("ended")&&terminal(m->report.value("recordedState").toString()))));
  m->runtimeState->setText(runtime.isEmpty()?text(AppLanguage::source("执行与保护实效尚未观测；不按默认值推定。")):
      m->report.value("runtimeConfigurationObserved").toBool()?text(AppLanguage::source("以下请求与实效来自运行时观测；实际引擎仍以性能观测为准。")):
      text(AppLanguage::source("以下是已记录的配置实效，不是运行时执行证明。")));
  QStringList detail;
  if(!m->report.isEmpty())detail<<text(AppLanguage::source("连接身份与原因"))<<m->report.value("jobId").toString()<<m->report.value("reason").toString();
  if(!runtime.isEmpty())detail<<text(AppLanguage::source("请求设置"))<<json(runtime.value("requested"))<<text(AppLanguage::source("有效设置"))<<json(runtime.value("effective"))<<text(AppLanguage::source("回退与迁移原因"))<<json(runtime.value("reasons"))<<json(runtime.value("migration"));
  if(!job.value("trainingDelivery").toObject().isEmpty())
    detail<<text(AppLanguage::source("模型交付（训练成功与坐标导出分别记录）"))<<json(job.value("trainingDelivery"));
  const auto performance=job.value("trainingPerformance").toObject();
  const auto unknown=text(AppLanguage::source("未观测"));
  detail<<text(AppLanguage::source("渲染器：%1；损失：%2。"))
      .arg(performance.value("renderer_engine").toString(unknown),performance.value("loss_engine").toString(unknown));
  detail<<text(AppLanguage::source("停止或异常原因（原始记录）"))<<job.value("generation_issue").toString()<<job.value("error").toString();
  for(const auto &entry:job.value("log").toArray())if(entry.isString())detail<<entry.toString();
  m->details->setPlainText(detail.join(QLatin1Char('\n')).left(64*1024));refreshEnabled();
}
void ExistingTrainingDialog::reject(){disconnectJob();QDialog::reject();}
void ExistingTrainingDialog::closeEvent(QCloseEvent *event){disconnectJob();QDialog::closeEvent(event);}
}
