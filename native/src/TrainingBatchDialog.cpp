#include "TrainingBatchDialog.h"
#include "AppLanguage.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {
constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
// Keep full native captions in each action group. Unlike a fixed horizontal
// row, this layout never squeezes a localized button below its own size hint.
class BatchActionFlow final : public QLayout {
public:
  explicit BatchActionFlow(bool alignRight = false) : mAlignRight(alignRight) {
    setContentsMargins(0, 0, 0, 0);
    setSpacing(6);
  }
  ~BatchActionFlow() override { while (auto *item = takeAt(0)) delete item; }
  void addItem(QLayoutItem *item) override { mItems.append(item); invalidate(); }
  int count() const override { return int(mItems.size()); }
  QLayoutItem *itemAt(int index) const override { return mItems.value(index, nullptr); }
  QLayoutItem *takeAt(int index) override {
    return index >= 0 && index < mItems.size() ? mItems.takeAt(index) : nullptr;
  }
  Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
  QSize sizeHint() const override {
    QSize result(0, 0);
    for (auto *item : mItems) {
      const QSize hint = actionSize(item);
      result.setWidth(result.width() + (result.width() ? spacing() : 0) + hint.width());
      result.setHeight(std::max(result.height(), hint.height()));
    }
    return result;
  }
  QSize minimumSize() const override {
    QSize result(0, 0);
    for (auto *item : mItems) result = result.expandedTo(actionSize(item));
    return result;
  }
  void setGeometry(const QRect &rectangle) override {
    QLayout::setGeometry(rectangle);
    arrange(rectangle, true);
  }
private:
  static QSize actionSize(const QLayoutItem *item) {
    const auto *widget = item->widget();
    return widget ? widget->sizeHint().expandedTo(widget->minimumSizeHint()) : item->sizeHint();
  }
  int arrange(const QRect &rectangle, bool apply) const {
    int top = rectangle.top(), rowWidth = 0, rowHeight = 0;
    qsizetype rowStart = 0;
    const auto finishRow = [&](qsizetype end) {
      if (!apply) return;
      int left = rectangle.left() + (mAlignRight ? std::max(0, rectangle.width() - rowWidth) : 0);
      for (qsizetype index = rowStart; index < end; ++index) {
        const QSize hint = actionSize(mItems.at(index));
        mItems.at(index)->setGeometry(QRect(left, top, hint.width(), rowHeight));
        left += hint.width() + spacing();
      }
    };
    for (qsizetype index = 0; index < mItems.size(); ++index) {
      const QSize hint = actionSize(mItems.at(index));
      const int nextWidth = rowWidth + (rowWidth ? spacing() : 0) + hint.width();
      if (index > rowStart && nextWidth > rectangle.width()) {
        finishRow(index);
        top += rowHeight + spacing();
        rowStart = index;
        rowWidth = rowHeight = 0;
      }
      rowWidth += (rowWidth ? spacing() : 0) + hint.width();
      rowHeight = std::max(rowHeight, hint.height());
    }
    finishRow(mItems.size());
    return mItems.isEmpty() ? 0 : top - rectangle.top() + rowHeight;
  }
  QList<QLayoutItem *> mItems;
  bool mAlignRight;
};
QString batchText(const char *source) { return QCoreApplication::translate("Workbench", source); }
QString encoded(const QJsonValue &value) {
  const auto bytes = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(bytes.mid(1, bytes.size() - 2));
}
bool duplicateKeys(const QByteArray &bytes) {
  QList<QSet<QString>> objects;
  for(qsizetype index=0;index<bytes.size();++index){
    if(bytes[index]=='{')objects.append(QSet<QString>{});
    else if(bytes[index]=='}'){if(!objects.isEmpty())objects.removeLast();}
    else if(bytes[index]=='"'){
      const qsizetype begin=index;bool escape=false;
      for(++index;index<bytes.size();++index){if(!escape&&bytes[index]=='"')break;if(!escape&&bytes[index]=='\\')escape=true;else escape=false;}
      qsizetype next=index+1;while(next<bytes.size()&&(bytes[next]==' '||bytes[next]=='\t'||bytes[next]=='\r'||bytes[next]=='\n'))++next;
      if(next<bytes.size()&&bytes[next]==':'&&!objects.isEmpty()){
        const auto values=QJsonDocument::fromJson("["+bytes.mid(begin,index-begin+1)+"]").array();
        const QString key=values.at(0).toString();if(objects.last().contains(key))return true;objects.last().insert(key);
      }
    }
  }
  return false;
}
bool readJson(const QString &path, QJsonObject *result) {
  QFile file(path);
  if (!QFileInfo(file).isFile() || file.size() > MaximumBytes || !file.open(QIODevice::ReadOnly)) return false;
  const auto bytes = file.read(MaximumBytes + 1);
  if (bytes.size() > MaximumBytes || !file.atEnd()) return false;
  QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
  if (!document.isObject() || error.error != QJsonParseError::NoError || duplicateKeys(bytes)) return false;
  *result = document.object(); return true;
}
bool freshJson(const QString &path, const QJsonObject &value) {
  const auto bytes = QJsonDocument(value).toJson(); QFile file(path);
  if (bytes.size() > MaximumBytes || QFileInfo(path).exists() || QFileInfo(path).isSymLink() ||
      !file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
  if (file.write(bytes) != bytes.size() || !file.flush()) { file.close(); file.remove(); return false; }
  file.close(); return true;
}
bool sha(const QJsonValue &value) { return QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(value.toString()).hasMatch(); }
void table(QTableWidget *widget, bool edit) {
  widget->setMinimumHeight(150); widget->setSelectionBehavior(QAbstractItemView::SelectRows);
  widget->setSelectionMode(QAbstractItemView::ExtendedSelection); widget->setWordWrap(false);
  widget->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  widget->horizontalHeader()->setStretchLastSection(true); widget->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  if (!edit) widget->setEditTriggers(QAbstractItemView::NoEditTriggers);
}
void put(QTableWidget *widget, int row, int column, const QString &value) {
  auto *item = new QTableWidgetItem(value); item->setToolTip(value); widget->setItem(row, column, item);
}
QString stateText(const QString &value) {
  if (value == QStringLiteral("ready")) return batchText(AppLanguage::source("等待显式启动"));
  if (value == QStringLiteral("running")) return batchText(AppLanguage::source("运行中"));
  if (value == QStringLiteral("paused")) return batchText(AppLanguage::source("已暂停"));
  if (value == QStringLiteral("interrupted")) return batchText(AppLanguage::source("已中断；未自动恢复"));
  if (value == QStringLiteral("completed")) return batchText(AppLanguage::source("已完成"));
  if (value == QStringLiteral("failed")) return batchText(AppLanguage::source("失败"));
  if (value == QStringLiteral("cancelled")) return batchText(AppLanguage::source("已取消"));
  return batchText(AppLanguage::source("未验证"));
}
}
class TrainingBatchDialog::State {
public:
  QString python, backend, workspace, plan, reportPath;
  QProcessEnvironment environment;
  QJsonObject original, report, persistentState, runRequest;
  QTableWidget *datasets = nullptr, *axes = nullptr, *runs = nullptr, *history = nullptr;
  QPlainTextEdit *base = nullptr, *details = nullptr;
  QComboBox *failurePolicy = nullptr;
  QWidget *inputs = nullptr;
  QLabel *summary = nullptr;
  QPushButton *preview = nullptr, *create = nullptr, *start = nullptr, *resume = nullptr,
      *cancel = nullptr, *load = nullptr, *save = nullptr, *refresh = nullptr, *loadPlan = nullptr,*observe=nullptr;
  QProcess *process = nullptr;
  QTimer *timeout = nullptr;
  std::unique_ptr<QTemporaryDir> scratch;
  QByteArray output, errors;
  const char *failure = nullptr, *message = nullptr;
  QString detail;
  bool loading = false, busy = false, creating = false, cancelled = false, closing = false;
  quint64 run = 0;
};

TrainingBatchDialog::TrainingBatchDialog(const QString &python, const QString &backendRoot, const QString &workspaceRoot,
    const QJsonObject &current, const QProcessEnvironment &environment, QWidget *parent) : QDialog(parent), m(std::make_unique<State>()) {
  m->python = python; m->backend = backendRoot; m->workspace = workspaceRoot; m->environment = environment;
  setObjectName(QStringLiteral("trainingBatchDialog")); AppLanguage::bind(this, "windowTitle", AppLanguage::source("通用训练批量计划"));
  resize(1120, 800); setMinimumSize(660, 440);
  auto *root = new QVBoxLayout(this); auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
  auto *body = new QWidget(scroll); auto *layout = new QVBoxLayout(body);
  auto *notice = AppLanguage::text(new QLabel(body), AppLanguage::source(
      "只预览和保存批量计划不会训练。启动与继续均需单独操作，GPU 任务串行执行。仅接受已导入的锁定相机数据集；测试图不参与训练，初始化与评估划分不由批量计划改变。"));
  notice->setWordWrap(true); notice->setTextFormat(Qt::PlainText); layout->addWidget(notice);
  m->inputs = new QWidget(body); auto *inputLayout = new QVBoxLayout(m->inputs); inputLayout->setContentsMargins(0,0,0,0);
  auto *datasetsLabel = AppLanguage::text(new QLabel(m->inputs), AppLanguage::source("数据集引用（稳定 ID、路径与身份）")); inputLayout->addWidget(datasetsLabel);
  m->datasets = new QTableWidget(0, 4, m->inputs); m->datasets->setObjectName(QStringLiteral("batchDatasetTable")); table(m->datasets, true);
  m->datasets->setColumnWidth(0,160); m->datasets->setColumnWidth(1,360); m->datasets->setColumnWidth(2,240); inputLayout->addWidget(m->datasets);
  auto *datasetButtons = new QHBoxLayout;
  auto *addDataset = AppLanguage::text(new QPushButton(m->inputs), AppLanguage::source("添加数据集引用"));
  auto *chooseDataset = AppLanguage::text(new QPushButton(m->inputs), AppLanguage::source("选择所选数据集目录…"));
  auto *removeDataset = AppLanguage::text(new QPushButton(m->inputs), AppLanguage::source("移除所选引用"));
  addDataset->setObjectName(QStringLiteral("batchAddDatasetButton")); chooseDataset->setObjectName(QStringLiteral("batchChooseDatasetButton"));
  for (auto *button : {addDataset, chooseDataset, removeDataset}) datasetButtons->addWidget(button); datasetButtons->addStretch(); inputLayout->addLayout(datasetButtons);
  auto *axesLabel = AppLanguage::text(new QLabel(m->inputs), AppLanguage::source("参数轴（参数路径与 JSON 非空列表）")); inputLayout->addWidget(axesLabel);
  m->axes = new QTableWidget(0, 2, m->inputs); m->axes->setObjectName(QStringLiteral("batchAxesTable")); table(m->axes,true); m->axes->setColumnWidth(0,300); inputLayout->addWidget(m->axes);
  auto *axisButtons = new QHBoxLayout;
  auto *addAxis = AppLanguage::text(new QPushButton(m->inputs), AppLanguage::source("添加参数轴"));
  auto *removeAxis = AppLanguage::text(new QPushButton(m->inputs), AppLanguage::source("移除所选参数轴"));
  addAxis->setObjectName(QStringLiteral("batchAddAxisButton")); axisButtons->addWidget(addAxis); axisButtons->addWidget(removeAxis); axisButtons->addStretch(); inputLayout->addLayout(axisButtons);
  auto *baseLabel = AppLanguage::text(new QLabel(m->inputs), AppLanguage::source("基础配置（高级 JSON；迭代数与分辨率必须明确）")); inputLayout->addWidget(baseLabel);
  m->base = new QPlainTextEdit(m->inputs); m->base->setObjectName(QStringLiteral("batchBaseConfiguration")); m->base->setMinimumHeight(140); inputLayout->addWidget(m->base);
  auto *policyNotice = AppLanguage::text(new QLabel(m->inputs), AppLanguage::source(
      "初始化：沿用数据集声明；评估：沿用已声明划分。trainingSeed 只控制训练随机过程，不改变初始化来源、测试划分或原始图像。repeat 表示同参数重复试验，不是优化器参数。"));
  policyNotice->setWordWrap(true); policyNotice->setTextFormat(Qt::PlainText); inputLayout->addWidget(policyNotice);
  auto *resolutionNotice = AppLanguage::text(new QLabel(m->inputs), AppLanguage::source(
      "批量分辨率比例仅支持 1、2、4、8；其他值不会被当作像素宽度执行。"));
  resolutionNotice->setWordWrap(true); resolutionNotice->setTextFormat(Qt::PlainText); inputLayout->addWidget(resolutionNotice);
  auto *form = new QFormLayout; m->failurePolicy = new QComboBox(m->inputs); m->failurePolicy->setObjectName(QStringLiteral("batchFailurePolicy"));
  m->failurePolicy->addItem(QString(), QVariant(QStringLiteral("stop")));
  m->failurePolicy->addItem(QString(), QVariant(QStringLiteral("continue")));
  AppLanguage::bindComboItem(m->failurePolicy, 0, AppLanguage::source("失败时停止，保留已有成果"));
  AppLanguage::bindComboItem(m->failurePolicy, 1, AppLanguage::source("失败时继续后续独立任务"));
  form->addRow(batchText(AppLanguage::source("失败策略")), m->failurePolicy);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(m->failurePolicy)), AppLanguage::source("失败策略")); inputLayout->addLayout(form);
  layout->addWidget(m->inputs);
  m->summary = new QLabel(body); m->summary->setObjectName(QStringLiteral("batchSummary")); m->summary->setWordWrap(true); m->summary->setTextFormat(Qt::PlainText); layout->addWidget(m->summary);
  m->runs = new QTableWidget(0,7,body); m->runs->setObjectName(QStringLiteral("batchRunPreview")); table(m->runs,false); layout->addWidget(m->runs);
  m->details = new QPlainTextEdit(body); m->details->setObjectName(QStringLiteral("batchPlanDetails")); m->details->setReadOnly(true); m->details->setMinimumHeight(140); layout->addWidget(m->details);
  auto *historyLabel = AppLanguage::text(new QLabel(body), AppLanguage::source("已保存计划与状态（打开不会启动或续训）")); layout->addWidget(historyLabel);
  m->history = new QTableWidget(0,4,body); m->history->setObjectName(QStringLiteral("batchPlanHistory")); table(m->history,false); m->history->setSelectionMode(QAbstractItemView::SingleSelection); layout->addWidget(m->history);
  auto *historyButtons = new QHBoxLayout;
  m->refresh = AppLanguage::text(new QPushButton(body), AppLanguage::source("刷新计划档案"));
  m->loadPlan = AppLanguage::text(new QPushButton(body), AppLanguage::source("查看所选计划"));
  m->refresh->setObjectName(QStringLiteral("batchRefreshHistoryButton")); m->loadPlan->setObjectName(QStringLiteral("batchLoadPlanButton"));
  historyButtons->addWidget(m->refresh); historyButtons->addWidget(m->loadPlan); historyButtons->addStretch(); layout->addLayout(historyButtons);
  scroll->setWidget(body); root->addWidget(scroll,1);
  auto *fileButtons = new BatchActionFlow;
  m->load = AppLanguage::text(new QPushButton(this), AppLanguage::source("导入批量请求…"));
  m->save = AppLanguage::text(new QPushButton(this), AppLanguage::source("保存批量请求…"));
  m->preview = AppLanguage::text(new QPushButton(this), AppLanguage::source("预览并核验数据身份"));
  m->create = AppLanguage::text(new QPushButton(this), AppLanguage::source("创建持久计划"));
  m->preview->setObjectName(QStringLiteral("batchPreviewButton")); m->create->setObjectName(QStringLiteral("batchCreateButton"));
  for (auto *button : {m->load,m->save,m->preview,m->create}) fileButtons->addWidget(button);
  root->addLayout(fileButtons);
  auto *runButtons = new BatchActionFlow(true);
  m->start = AppLanguage::text(new QPushButton(this), AppLanguage::source("启动所选批量计划")); m->start->setObjectName(QStringLiteral("batchStartButton"));
  m->resume = AppLanguage::text(new QPushButton(this), AppLanguage::source("请求核验并继续批量计划")); m->resume->setObjectName(QStringLiteral("batchResumeButton"));
  m->observe = AppLanguage::text(new QPushButton(this), AppLanguage::source("观察批量队列")); m->observe->setObjectName(QStringLiteral("batchObserveButton"));
  m->cancel = new QPushButton(this); m->cancel->setObjectName(QStringLiteral("batchCancelButton"));
  for (auto *button : {m->observe,m->start,m->resume,m->cancel}) runButtons->addWidget(button); root->addLayout(runButtons);
  connect(addDataset,&QPushButton::clicked,this,[this] {
    const int row=m->datasets->rowCount(); if(row>=256)return fail(AppLanguage::source("批量数据集引用超过 256；未抽样或截断。"));
    m->loading=true; m->datasets->insertRow(row); for(int column=0;column<4;++column)put(m->datasets,row,column,{});
    m->datasets->item(row,0)->setText(QStringLiteral("dataset-")+QUuid::createUuid().toString(QUuid::WithoutBraces));m->loading=false;invalidate();
  });
  connect(chooseDataset,&QPushButton::clicked,this,[this] {
    const int row=m->datasets->currentRow();if(row<0)return;
    const auto path=QFileDialog::getExistingDirectory(this,batchText(AppLanguage::source("选择已导入的相机数据集")),m->datasets->item(row,1)->text());
    if(!path.isEmpty()){m->datasets->item(row,1)->setText(path);m->datasets->item(row,2)->setText({});}
  });
  const auto removeRows=[this](QTableWidget *widget) {
    auto rows=widget->selectionModel()->selectedRows();std::sort(rows.begin(),rows.end(),[](const QModelIndex&a,const QModelIndex&b){return a.row()>b.row();});
    for(const auto&row:rows)widget->removeRow(row.row());invalidate();
  };
  connect(removeDataset,&QPushButton::clicked,this,[removeRows,this]{removeRows(m->datasets);});
  connect(addAxis,&QPushButton::clicked,this,[this]{m->loading=true;int row=m->axes->rowCount();m->axes->insertRow(row);put(m->axes,row,0,{});put(m->axes,row,1,QStringLiteral("[]"));m->loading=false;invalidate();});
  connect(removeAxis,&QPushButton::clicked,this,[removeRows,this]{removeRows(m->axes);});
  connect(m->datasets,&QTableWidget::itemChanged,this,[this]{invalidate();});connect(m->axes,&QTableWidget::itemChanged,this,[this]{invalidate();});
  connect(m->base,&QPlainTextEdit::textChanged,this,[this]{invalidate();});connect(m->failurePolicy,&QComboBox::currentIndexChanged,this,[this]{invalidate();});
  connect(m->preview,&QPushButton::clicked,this,&TrainingBatchDialog::previewPlan);connect(m->create,&QPushButton::clicked,this,&TrainingBatchDialog::createPlan);
  connect(m->start,&QPushButton::clicked,this,&TrainingBatchDialog::startBatch);connect(m->resume,&QPushButton::clicked,this,&TrainingBatchDialog::resumeBatch);
  connect(m->observe,&QPushButton::clicked,this,&TrainingBatchDialog::observeBatch);
  connect(m->refresh,&QPushButton::clicked,this,&TrainingBatchDialog::refreshHistory);
  connect(m->loadPlan,&QPushButton::clicked,this,[this]{const int row=m->history->currentRow();if(row>=0)loadPlan(m->history->item(row,0)->data(Qt::UserRole).toString());});
  connect(m->cancel,&QPushButton::clicked,this,[this]{if(m->busy)cancelOperation();else reject();});
  connect(m->load,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getOpenFileName(this,batchText(AppLanguage::source("导入批量请求…")),m->workspace,batchText(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")));if(!path.isEmpty())loadRequest(path);});
  connect(m->save,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getSaveFileName(this,batchText(AppLanguage::source("保存批量请求…")),QDir(m->workspace).filePath(QStringLiteral("training-batch-request.json")),batchText(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")),nullptr,QFileDialog::DontConfirmOverwrite);if(!path.isEmpty())saveRequest(path);});
  m->process=new QProcess(this);
#ifdef Q_OS_WIN
  m->process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args){args->flags|=CREATE_NO_WINDOW;args->startupInfo->dwFlags|=STARTF_USESHOWWINDOW;args->startupInfo->wShowWindow=SW_HIDE;});
#endif
  connect(m->process,&QProcess::started,this,[this]{if(m->cancelled)m->process->write("cancel\n");});
  connect(m->process,&QProcess::readyReadStandardOutput,this,[this]{readOutput(false);});connect(m->process,&QProcess::readyReadStandardError,this,[this]{readOutput(true);});
  connect(m->process,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status){finishHelper(code,status==QProcess::NormalExit);});
  connect(m->process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){if(error==QProcess::FailedToStart){m->failure=AppLanguage::source("批量计划辅助进程无法启动：%1");m->detail=m->process->errorString();finishHelper(2,false);}});
  m->timeout=new QTimer(this);m->timeout->setSingleShot(true);connect(m->timeout,&QTimer::timeout,this,[this]{m->failure=AppLanguage::source("批量计划核验超时，正在取消辅助进程；未启动训练。");cancelOperation();});
  AppLanguage::onChanged(this,[this]{refreshPresentation();});
  QJsonArray datasets;if(!current.isEmpty())datasets.append(current);
  setRequestDocument({{"schema","gsw.training_batch_plan/1"},{"datasets",datasets},{"matrix",QJsonObject{}},
      {"base",QJsonObject{{"backend","3dgs"},{"quality","full"},{"trainingSeed",0},{"trainOptions",QJsonObject{}}}},
      {"initialization_policy","dataset_declared"},{"evaluation_policy","declared_splits"},{"run_policy",QJsonObject{{"on_failure","stop"}}}});
  refreshHistory();
}
TrainingBatchDialog::~TrainingBatchDialog(){if(m->process->state()!=QProcess::NotRunning){m->process->write("cancel\n");m->process->waitForBytesWritten(100);if(!m->process->waitForFinished(500)){m->process->kill();m->process->waitForFinished(1500);}}}
QJsonObject TrainingBatchDialog::requestDocument() const {
  QJsonObject result=m->original;QJsonArray datasets;QSet<QString> identifiers;
  for(int row=0;row<m->datasets->rowCount();++row){QJsonObject item=m->datasets->item(row,0)->data(Qt::UserRole).toJsonObject();const QString id=m->datasets->item(row,0)->text();if(id.isEmpty()||identifiers.contains(id))return {};identifiers.insert(id);
    item.insert(QStringLiteral("id"),id);item.insert(QStringLiteral("path"),m->datasets->item(row,1)->text());item.insert(QStringLiteral("data_identity"),m->datasets->item(row,2)->text());
    const auto display=m->datasets->item(row,3)->text();if(!display.isEmpty()||item.contains(QStringLiteral("display_name")))item.insert(QStringLiteral("display_name"),display);datasets.append(item);}
  QJsonObject matrix;for(int row=0;row<m->axes->rowCount();++row){const QString path=m->axes->item(row,0)->text();QJsonParseError error;const auto values=QJsonDocument::fromJson(m->axes->item(row,1)->text().toUtf8(),&error);if(path.isEmpty()||matrix.contains(path)||!values.isArray()||error.error!=QJsonParseError::NoError||values.array().isEmpty())return {};matrix.insert(path,values.array());}
  const auto baseBytes=m->base->toPlainText().toUtf8();QJsonParseError error;const auto base=QJsonDocument::fromJson(baseBytes,&error);if(!base.isObject()||error.error!=QJsonParseError::NoError||duplicateKeys(baseBytes))return {};
  result.insert(QStringLiteral("schema"),QStringLiteral("gsw.training_batch_plan/1"));result.insert(QStringLiteral("datasets"),datasets);result.insert(QStringLiteral("matrix"),matrix);result.insert(QStringLiteral("base"),base.object());
  result.insert(QStringLiteral("initialization_policy"),QStringLiteral("dataset_declared"));result.insert(QStringLiteral("evaluation_policy"),QStringLiteral("declared_splits"));
  auto policy=result.value(QStringLiteral("run_policy")).toObject();policy.insert(QStringLiteral("on_failure"),m->failurePolicy->currentData().toString());result.insert(QStringLiteral("run_policy"),policy);return result;
}
void TrainingBatchDialog::setRequestDocument(const QJsonObject &request){
  if(m->busy)return;
  if(request.value(QStringLiteral("schema"))!=QStringLiteral("gsw.training_batch_plan/1")||!request.value(QStringLiteral("datasets")).isArray()||request.value(QStringLiteral("datasets")).toArray().size()>256||
      !request.value(QStringLiteral("matrix")).isObject()||!request.value(QStringLiteral("base")).isObject()||
      request.value(QStringLiteral("initialization_policy"))!=QStringLiteral("dataset_declared")||request.value(QStringLiteral("evaluation_policy"))!=QStringLiteral("declared_splits"))return fail(AppLanguage::source("批量请求结构或初始化／评估策略不受支持；未改动当前参数。"));
  const auto datasets=request.value(QStringLiteral("datasets")).toArray();for(const auto&row:datasets)if(!row.isObject())return fail(AppLanguage::source("批量请求结构或初始化／评估策略不受支持；未改动当前参数。"));
  m->loading=true;m->original=request;m->datasets->setRowCount(0);
  for(const auto&value:datasets){const auto entry=value.toObject();const int row=m->datasets->rowCount();m->datasets->insertRow(row);int column=0;for(const auto&key:{QStringLiteral("id"),QStringLiteral("path"),QStringLiteral("data_identity"),QStringLiteral("display_name")})put(m->datasets,row,column++,entry.value(key).toString());m->datasets->item(row,0)->setData(Qt::UserRole,entry);}
  m->axes->setRowCount(0);const auto matrix=request.value(QStringLiteral("matrix")).toObject();for(auto it=matrix.begin();it!=matrix.end();++it){int row=m->axes->rowCount();m->axes->insertRow(row);put(m->axes,row,0,it.key());put(m->axes,row,1,encoded(it.value()));}
  m->base->setPlainText(QString::fromUtf8(QJsonDocument(request.value(QStringLiteral("base")).toObject()).toJson()));
  m->failurePolicy->setCurrentIndex(m->failurePolicy->findData(request.value(QStringLiteral("run_policy")).toObject().value(QStringLiteral("on_failure")).toString(QStringLiteral("stop"))));m->loading=false;invalidate();
}
QJsonObject TrainingBatchDialog::previewReport()const{return m->report;}
QString TrainingBatchDialog::planPath()const{return m->plan;}
bool TrainingBatchDialog::isRunning()const{return m->busy;}
bool TrainingBatchDialog::loadRequest(const QString&path){if(m->busy)return false;QJsonObject request;if(!readJson(path,&request)){fail(AppLanguage::source("无法读取批量请求，或 JSON 超过 16 MiB。"));return false;}setRequestDocument(request);return !m->failure;}
bool TrainingBatchDialog::saveRequest(const QString&path){if(m->busy)return false;const auto request=requestDocument();if(request.isEmpty()||!freshJson(path,request)){m->failure=AppLanguage::source("无法保存批量请求；请检查参数，已有文件不会覆盖。");refreshPresentation();return false;}m->message=AppLanguage::source("批量请求已保存；尚未启动训练。");refreshPresentation();return true;}
void TrainingBatchDialog::invalidate(){if(m->loading||m->busy)return;m->report={};m->persistentState={};m->plan.clear();m->failure=nullptr;m->message=nullptr;m->cancelled=false;refreshEnabled();refreshPresentation();}
void TrainingBatchDialog::fail(const char*source,const QString&detail){m->failure=source;m->detail=detail;m->report={};m->persistentState={};m->plan.clear();refreshEnabled();refreshPresentation();}
void TrainingBatchDialog::previewPlan(){startHelper(false);}
void TrainingBatchDialog::createPlan(){if(!m->report.isEmpty())startHelper(true);}
void TrainingBatchDialog::startHelper(bool create){
  if(m->busy)return;const auto request=requestDocument();if(request.isEmpty())return fail(AppLanguage::source("请检查数据集 ID、参数轴 JSON 列表与基础配置；空值或重复参数轴不能预览。"));
  const QString script=QDir(m->backend).filePath(QStringLiteral("native/worker/training_batch.py"));if(!QFileInfo(m->python).isFile()||!QFileInfo(script).isFile())return fail(AppLanguage::source("批量计划工具不可用；未启动训练，也不会自动安装。"));
  const QString workspace=QFileInfo(m->workspace).canonicalFilePath();if(workspace.isEmpty())return fail(AppLanguage::source("无法创建当前工程的批量计划临时目录。"));
  QString managed=workspace;for(const auto&part:{QStringLiteral(".gsw"),QStringLiteral("jobs")}){QString next=QDir(managed).filePath(part);QFileInfo info(next);if(!info.exists()&&!info.isSymLink()&&!QDir(managed).mkdir(part))return fail(AppLanguage::source("无法创建当前工程的批量计划临时目录。"));info.refresh();QString canonical=info.canonicalFilePath();if(!info.isDir()||!canonical.startsWith(workspace+QLatin1Char('/'),Qt::CaseInsensitive))return fail(AppLanguage::source("无法创建当前工程的批量计划临时目录。"));managed=canonical;}
  m->scratch=std::make_unique<QTemporaryDir>(QDir(managed).filePath(QStringLiteral("batch-XXXXXX")));if(!m->scratch->isValid()||!freshJson(m->scratch->filePath(QStringLiteral("request.json")),request))return fail(AppLanguage::source("无法创建当前工程的批量计划临时目录。"));
  m->runRequest=request;m->reportPath=m->scratch->filePath(QStringLiteral("report.json"));m->busy=true;m->creating=create;m->cancelled=false;m->failure=nullptr;m->message=nullptr;m->output.clear();m->errors.clear();++m->run;
  auto environment=m->environment;environment.insert(QStringLiteral("PYTHONUTF8"),QStringLiteral("1"));environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"),QStringLiteral("1"));environment.insert(QStringLiteral("TEMP"),m->scratch->path());environment.insert(QStringLiteral("TMP"),m->scratch->path());
  m->process->setProgram(m->python);m->process->setArguments({QStringLiteral("-B"),script,QStringLiteral("--operation"),create?QStringLiteral("create-plan"):QStringLiteral("preview"),QStringLiteral("--request"),m->scratch->filePath(QStringLiteral("request.json")),QStringLiteral("--repository"),m->backend,QStringLiteral("--workspace"),m->workspace,QStringLiteral("--report"),m->reportPath});m->process->setProcessEnvironment(environment);m->process->setWorkingDirectory(m->backend);refreshEnabled();refreshPresentation();m->timeout->start(600000);m->process->start();
}
void TrainingBatchDialog::readOutput(bool standardError){m->process->setReadChannel(standardError?QProcess::StandardError:QProcess::StandardOutput);auto&target=standardError?m->errors:m->output;qint64 budget=MaximumBytes-m->output.size()-m->errors.size();if(budget>=0)target+=m->process->read(budget+1);if(m->output.size()+m->errors.size()>MaximumBytes){m->failure=AppLanguage::source("批量计划输出超过 16 MiB，正在取消；未启动训练。");m->process->closeReadChannel(QProcess::StandardOutput);m->process->closeReadChannel(QProcess::StandardError);cancelOperation();}}
void TrainingBatchDialog::finishHelper(int exitCode,bool normalExit){
  if(!m->busy)return;m->timeout->stop();readOutput(false);readOutput(true);m->busy=false;
  if(m->cancelled)m->report={};else if(!m->failure){QJsonObject response;const auto lines=m->output.split('\n');for(auto it=lines.crbegin();it!=lines.crend();++it){const auto object=QJsonDocument::fromJson(it->trimmed()).object();if(object.contains(QStringLiteral("success"))){response=object;break;}}
    QJsonObject report;const QString returned=response.value(QStringLiteral("reportPath")).toString();const bool validPath=!returned.isEmpty()&&QFileInfo(returned).canonicalFilePath()==QFileInfo(m->reportPath).canonicalFilePath();
    bool valid=normalExit&&exitCode==0&&response.value(QStringLiteral("success")).toBool()&&validPath&&readJson(m->reportPath,&report)&&report.value(QStringLiteral("schema"))==QStringLiteral("gsw.training_batch/1")&&sha(report.value(QStringLiteral("plan_sha256")))&&report.value(QStringLiteral("request")).toObject()==m->runRequest&&report.value(QStringLiteral("datasets")).isArray()&&report.value(QStringLiteral("runs")).isArray()&&!report.value(QStringLiteral("runs")).toArray().isEmpty()&&report.value(QStringLiteral("runs")).toArray().size()<=256;
    auto pinned=m->runRequest;auto rows=pinned.value(QStringLiteral("datasets")).toArray();const auto admitted=report.value(QStringLiteral("datasets")).toArray();QSet<QString>seen;
    valid=valid&&rows.size()==admitted.size();for(int index=0;valid&&index<rows.size();++index){auto row=rows[index].toObject();QJsonObject matched;for(const auto&candidate:admitted)if(candidate.toObject().value(QStringLiteral("id"))==row.value(QStringLiteral("id")))matched=candidate.toObject();const QString id=row.value(QStringLiteral("id")).toString();if(matched.isEmpty()||!sha(matched.value(QStringLiteral("data_identity")))||seen.contains(id)){valid=false;break;}seen.insert(id);row.insert(QStringLiteral("data_identity"),matched.value(QStringLiteral("data_identity")));rows[index]=row;}
    if(valid){pinned.insert(QStringLiteral("datasets"),rows);setRequestDocument(pinned);m->report=report;if(m->creating){const QString path=response.value(QStringLiteral("planPath")).toString();if(!loadPlan(path))valid=false;}}
    if(!valid)fail(AppLanguage::source("批量计划核验失败；未启动训练。技术详情：%1"),response.value(QStringLiteral("code")).toString()+QLatin1Char('\n')+response.value(QStringLiteral("detail")).toString()+QLatin1Char('\n')+QString::fromUtf8(m->errors.left(8192)));
  }
  m->scratch.reset();refreshEnabled();refreshPresentation();emit planningFinished(m->report);if(m->closing)QDialog::reject();
}
void TrainingBatchDialog::cancelOperation(){if(!m->busy||m->cancelled)return;m->cancelled=true;m->report={};m->plan.clear();m->process->write("cancel\n");const quint64 run=m->run;QTimer::singleShot(5000,this,[this,run]{if(m->busy&&m->run==run)m->process->terminate();});QTimer::singleShot(7000,this,[this,run]{if(m->busy&&m->run==run)m->process->kill();});refreshPresentation();}
void TrainingBatchDialog::startBatch(){if(m->busy||m->plan.isEmpty()||m->persistentState.value(QStringLiteral("status"))!=QStringLiteral("ready"))return;const QString path=m->plan;if(!loadPlan(path)||m->persistentState.value(QStringLiteral("status"))!=QStringLiteral("ready"))return;emit runRequested(path,false);}
void TrainingBatchDialog::resumeBatch(){if(m->busy||m->plan.isEmpty())return;const QString path=m->plan;if(!loadPlan(path))return;const QString status=m->persistentState.value(QStringLiteral("status")).toString();if(status!=QStringLiteral("paused")&&status!=QStringLiteral("interrupted"))return;emit runRequested(path,true);}
void TrainingBatchDialog::observeBatch(){if(m->busy||m->plan.isEmpty())return;const QString path=m->plan;if(loadPlan(path))emit observeRequested(path);}
bool TrainingBatchDialog::loadPlan(const QString&path){
  if(m->busy)return false;const QString workspace=QFileInfo(m->workspace).canonicalFilePath();const QString canonical=QFileInfo(path).canonicalFilePath();QJsonObject plan,state;
  if(workspace.isEmpty()||!canonical.startsWith(workspace+QStringLiteral("/.gsw/training-batches/"),Qt::CaseInsensitive)||!readJson(canonical,&plan)||plan.value(QStringLiteral("schema"))!=QStringLiteral("gsw.training_batch/1")||!sha(plan.value(QStringLiteral("plan_sha256")))||!readJson(QDir(QFileInfo(canonical).absolutePath()).filePath(QStringLiteral("state.json")),&state)||state.value(QStringLiteral("schema"))!=QStringLiteral("gsw.training_batch_state/1")||state.value(QStringLiteral("plan_sha256"))!=plan.value(QStringLiteral("plan_sha256"))){fail(AppLanguage::source("持久批量计划或状态无效、不属于当前工程，或身份不匹配；未启动训练。"));return false;}
  setRequestDocument(plan.value(QStringLiteral("request")).toObject());if(m->failure)return false;m->report=plan;m->persistentState=state;m->plan=canonical;refreshEnabled();refreshPresentation();return true;
}
void TrainingBatchDialog::refreshHistory(){if(m->busy)return;m->history->setRowCount(0);const QString root=QDir(m->workspace).filePath(QStringLiteral(".gsw/training-batches"));const auto directories=QDir(root).entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name);for(const auto&directory:directories){QJsonObject plan,state;const QString path=QDir(directory.absoluteFilePath()).filePath(QStringLiteral("plan.json"));if(directory.isSymLink()||!readJson(path,&plan)||!readJson(QDir(directory.absoluteFilePath()).filePath(QStringLiteral("state.json")),&state))continue;const int row=m->history->rowCount();m->history->insertRow(row);put(m->history,row,0,directory.fileName());m->history->item(row,0)->setData(Qt::UserRole,path);put(m->history,row,1,state.value(QStringLiteral("status")).toString());m->history->item(row,1)->setData(Qt::UserRole,state.value(QStringLiteral("status")));put(m->history,row,2,QString::number(plan.value(QStringLiteral("runs")).toArray().size()));put(m->history,row,3,plan.value(QStringLiteral("plan_sha256")).toString());}refreshPresentation();}
void TrainingBatchDialog::refreshEnabled(){m->inputs->setEnabled(!m->busy);for(auto*button:{m->load,m->save,m->preview,m->refresh,m->loadPlan})button->setEnabled(!m->busy);m->observe->setEnabled(!m->busy&&!m->plan.isEmpty());m->create->setEnabled(!m->busy&&!m->report.isEmpty()&&m->plan.isEmpty());const QString status=m->persistentState.value(QStringLiteral("status")).toString();m->start->setEnabled(!m->busy&&!m->plan.isEmpty()&&status==QStringLiteral("ready"));m->resume->setEnabled(!m->busy&&!m->plan.isEmpty()&&(status==QStringLiteral("paused")||status==QStringLiteral("interrupted")));}
void TrainingBatchDialog::refreshPresentation(){
  m->datasets->setHorizontalHeaderLabels({batchText(AppLanguage::source("数据集 ID")),batchText(AppLanguage::source("数据集路径")),batchText(AppLanguage::source("已核验数据身份")),batchText(AppLanguage::source("显示名称"))});
  m->axes->setHorizontalHeaderLabels({batchText(AppLanguage::source("参数路径（原始标识）")),batchText(AppLanguage::source("参数取值 JSON 列表"))});
  m->runs->setHorizontalHeaderLabels({batchText(AppLanguage::source("任务 ID")),batchText(AppLanguage::source("数据集 ID")),batchText(AppLanguage::source("后端")),QStringLiteral("trainingSeed"),batchText(AppLanguage::source("迭代数")),batchText(AppLanguage::source("分辨率倍率")),batchText(AppLanguage::source("完整参数（原始 JSON）"))});
  m->history->setHorizontalHeaderLabels({batchText(AppLanguage::source("计划标识")),batchText(AppLanguage::source("状态")),batchText(AppLanguage::source("任务数")),batchText(AppLanguage::source("计划身份"))});for(int row=0;row<m->history->rowCount();++row)m->history->item(row,1)->setText(stateText(m->history->item(row,1)->data(Qt::UserRole).toString()));
  m->cancel->setText(batchText(m->busy?AppLanguage::source("取消计划核验"):AppLanguage::source("关闭")));
  QString summary;if(m->busy)summary=batchText(m->cancelled?AppLanguage::source("正在取消计划辅助进程；真实训练任务不受影响。"):m->creating?AppLanguage::source("正在核验并保存持久计划；尚未启动训练。"):AppLanguage::source("正在展开参数组合并核验数据身份；尚未启动训练。"));
  else if(m->failure){summary=batchText(m->failure);if(summary.contains(QStringLiteral("%1")))summary=summary.arg(m->detail);}
  else if(m->cancelled)summary=batchText(AppLanguage::source("批量计划核验已取消；未启动或中断真实训练。"));
  else if(m->report.isEmpty())summary=batchText(AppLanguage::source("尚无有效预览；迭代数和分辨率必须在基础配置或参数轴中明确指定。"));
  else summary=batchText(AppLanguage::source("计划含 %1 项串行任务；数据身份已固定。状态：%2。续训仍需后台核验完整检查点，不能从预览或 PLY 推定。"))
      .arg(QString::number(m->report.value(QStringLiteral("runs")).toArray().size()),m->plan.isEmpty()?batchText(AppLanguage::source("仅预览，未保存计划")):stateText(m->persistentState.value(QStringLiteral("status")).toString()));
  if(m->message)summary+=QLatin1Char('\n')+batchText(m->message);m->summary->setText(summary);
  const auto runs=m->report.value(QStringLiteral("runs")).toArray();m->runs->setRowCount(runs.size());int row=0;for(const auto&value:runs){const auto run=value.toObject(),parameters=run.value(QStringLiteral("parameters")).toObject(),config=run.value(QStringLiteral("configuration")).toObject();const auto options=config.value(QStringLiteral("trainOptions")).toObject();put(m->runs,row,0,run.value(QStringLiteral("id")).toString());put(m->runs,row,1,run.value(QStringLiteral("dataset_id")).toString());put(m->runs,row,2,config.value(QStringLiteral("backend")).toString());put(m->runs,row,3,encoded(config.value(QStringLiteral("trainingSeed"))));put(m->runs,row,4,encoded(options.value(QStringLiteral("iterations"))));put(m->runs,row,5,encoded(options.value(QStringLiteral("resolution"))));put(m->runs,row,6,encoded(parameters));++row;}
  const QString details=m->report.isEmpty()?QString():QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("plan"),m->report},{QStringLiteral("state"),m->persistentState}}).toJson());if(m->details->toPlainText()!=details)m->details->setPlainText(details);
}
void TrainingBatchDialog::reject(){if(m->busy){m->closing=true;cancelOperation();return;}QDialog::reject();}
void TrainingBatchDialog::closeEvent(QCloseEvent*event){if(m->busy){m->closing=true;cancelOperation();event->ignore();}else QDialog::closeEvent(event);}
}
