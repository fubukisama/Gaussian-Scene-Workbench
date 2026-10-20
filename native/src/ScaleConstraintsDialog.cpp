#include "ScaleConstraintsDialog.h"
#include "AppLanguage.h"
#include "CameraDatasetInfo.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSet>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {
constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
constexpr int MaximumRows = 4096;
QString scaleText(const char *source) { return QCoreApplication::translate("Workbench", source); }
QString compact(const QJsonValue &value) {
  const auto bytes = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(bytes.mid(1, bytes.size() - 2));
}
QJsonValue numeric(const QString &input) {
  if (input.trimmed().isEmpty()) return QJsonValue(QJsonValue::Undefined);
  QLocale locale = QLocale::c();
  locale.setNumberOptions(QLocale::RejectGroupSeparator);
  bool ok = false;
  const double value = locale.toDouble(input.trimmed(), &ok);
  return ok && std::isfinite(value) ? QJsonValue(value) : QJsonValue(input);
}
QJsonValue vectorValue(const QString &input) {
  QJsonParseError error;
  const auto json = QJsonDocument::fromJson(input.trimmed().toUtf8(), &error);
  return error.error == QJsonParseError::NoError && json.isArray()
      ? QJsonValue(json.array()) : QJsonValue(input);
}
QString checkStatus(const QString &status) {
  if (status == QStringLiteral("pass")) return scaleText(AppLanguage::source("独立检核通过（仅限所列参考约束）"));
  if (status == QStringLiteral("fail")) return scaleText(AppLanguage::source("独立检核失败；禁止创建尺度版本"));
  if (status == QStringLiteral("not_provided")) return scaleText(AppLanguage::source("未提供独立检核"));
  return scaleText(AppLanguage::source("独立检核未验证"));
}
bool writeFresh(const QString &path, const QByteArray &bytes, QString *error) {
  const QFileInfo info(path);
  if (path.isEmpty() || info.exists() || info.isSymLink()) { *error = path; return false; }
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { *error = file.errorString(); return false; }
  if (file.write(bytes) != bytes.size() || !file.flush()) {
    *error = file.errorString(); file.close(); file.remove(); return false;
  }
  file.close();
  return true;
}
bool readBounded(const QString &path, QByteArray *bytes) {
  QFile file(path);
  if (!QFileInfo(file).isFile() || file.size() < 0 || file.size() > MaximumBytes || !file.open(QIODevice::ReadOnly)) return false;
  *bytes = file.read(MaximumBytes + 1);
  return bytes->size() <= MaximumBytes && file.atEnd();
}
bool duplicateObjectKey(const QByteArray &bytes) {
  // QJsonDocument handles syntax; this second pass only rejects duplicate keys
  // before any original source record can be normalized away by Qt.
  QList<QSet<QString>> objects;
  for (qsizetype index = 0; index < bytes.size(); ++index) {
    const char value = bytes[index];
    if (value == '{') objects.append(QSet<QString>{});
    else if (value == '}') { if (!objects.isEmpty()) objects.removeLast(); }
    else if (value == '"') {
      const qsizetype begin = index;
      bool escape = false;
      for (++index; index < bytes.size(); ++index) {
        if (!escape && bytes[index] == '"') break;
        if (!escape && bytes[index] == '\\') escape = true; else escape = false;
      }
      qsizetype next = index + 1;
      while (next < bytes.size() && (bytes[next] == ' ' || bytes[next] == '\t' || bytes[next] == '\n' || bytes[next] == '\r')) ++next;
      if (next < bytes.size() && bytes[next] == ':' && !objects.isEmpty()) {
        const auto array = QJsonDocument::fromJson("[" + bytes.mid(begin, index - begin + 1) + "]").array();
        const QString key = array.at(0).toString();
        if (objects.last().contains(key)) return true;
        objects.last().insert(key);
      }
    }
  }
  return false;
}
const std::array<const char *, 12> CommonHeaders{{
    AppLanguage::source("约束 ID"), AppLanguage::source("角色"), AppLanguage::source("参考 ID"),
    AppLanguage::source("参考单位"), AppLanguage::source("相对权重"), AppLanguage::source("参考标准不确定度"),
    AppLanguage::source("参考来源"), AppLanguage::source("测量 ID"), AppLanguage::source("不确定度说明"),
    AppLanguage::source("声明独立于拟合"), AppLanguage::source("检核容差 (m)"), AppLanguage::source("原始来源记录 (JSON)")}};
}

class ScaleConstraintsDialog::State {
public:
  struct Table { QString kind; QTableWidget *widget = nullptr; QStringList fields; QList<const char *> captions; };
  QString python, backendRoot, workspaceRoot, datasetPath, expectedIdentity, sourceKind, requestedIdentity;
  QProcessEnvironment environment;
  QJsonObject original, report, resultInfo, runRequest;
  QString resultPath, versionPath, operationOutput, reportPath;
  QComboBox *mode = nullptr;
  QLineEdit *fitLimit = nullptr;
  QWidget *inputs = nullptr;
  QTabWidget *tabs = nullptr;
  std::array<Table, 3> tables;
  QLabel *summary = nullptr, *sourceLabel = nullptr, *modelNotice = nullptr;
  QPlainTextEdit *details = nullptr;
  QPushButton *estimate = nullptr, *create = nullptr, *saveRequest = nullptr, *loadRequest = nullptr,
      *saveResult = nullptr, *cancel = nullptr, *inspect = nullptr;
  QProcess *process = nullptr;
  QTimer *timeout = nullptr;
  std::unique_ptr<QTemporaryDir> scratch;
  QByteArray output, errors;
  const char *failure = nullptr;
  QString failureDetail;
  const char *message = nullptr;
  QString messageDetail;
  bool busy = false, creating = false, loading = false, cancelled = false, closePending = false,
      inspecting = false, inspectionReady = false;
  quint64 run = 0;
};

ScaleConstraintsDialog::ScaleConstraintsDialog(const QString &python, const QString &backendRoot,
    const QString &workspaceRoot, const QString &sourceDatasetPath, const QString &expectedIdentity,
    const QProcessEnvironment &environment, QWidget *parent)
    : ScaleConstraintsDialog(python, backendRoot, workspaceRoot,
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("dataset")}, {QStringLiteral("path"), sourceDatasetPath},
          {QStringLiteral("identity"), expectedIdentity}, {QStringLiteral("frame"), QStringLiteral("source")}}, environment, parent) {}

ScaleConstraintsDialog::ScaleConstraintsDialog(const QString &python, const QString &backendRoot,
    const QString &workspaceRoot, const QJsonObject &descriptor, const QProcessEnvironment &environment,
    QWidget *parent) : QDialog(parent), m(std::make_unique<State>()) {
  m->python = python; m->backendRoot = backendRoot; m->workspaceRoot = workspaceRoot;
  m->datasetPath = descriptor.value(QStringLiteral("path")).toString();
  m->sourceKind = descriptor.value(QStringLiteral("kind")).toString();
  m->requestedIdentity = descriptor.value(QStringLiteral("identity")).toString();
  m->inspectionReady = m->sourceKind == QStringLiteral("dataset");
  m->expectedIdentity = m->inspectionReady ? m->requestedIdentity : QString();
  m->environment = environment;
  setObjectName(QStringLiteral("scaleConstraintsDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("定尺度与对齐"));
  resize(1180, 780);
  setMinimumSize(660, 420);
  auto *layout = new QVBoxLayout(this);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
  auto *body = new QWidget(scroll);
  auto *bodyLayout = new QVBoxLayout(body);
  auto *notice = AppLanguage::text(new QLabel(body), AppLanguage::source(
      "拟合残差不是模型精度。定尺度只约束比例；完整对齐另需非退化控制点。独立检核不参与拟合、阈值选择或重新估计；参考数据的不确定度仍须单独记录。不会启动训练或重新求解相机。"));
  notice->setWordWrap(true); notice->setTextFormat(Qt::PlainText);
  bodyLayout->addWidget(notice);
  m->sourceLabel = new QLabel(body);
  m->sourceLabel->setObjectName(QStringLiteral("scaleSourceIdentity"));
  m->sourceLabel->setTextFormat(Qt::PlainText); m->sourceLabel->setWordWrap(true); m->sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  bodyLayout->addWidget(m->sourceLabel);
  m->modelNotice = AppLanguage::text(new QLabel(body), AppLanguage::source(
      "模型来源不包含相机，不能使用相机基线。完整旋转涉及方向相关球谐时，仅保存变换清单与原模型副本，不冒称已烘焙模型。"));
  m->modelNotice->setWordWrap(true); m->modelNotice->setTextFormat(Qt::PlainText);
  m->modelNotice->setVisible(m->sourceKind == QStringLiteral("model")); bodyLayout->addWidget(m->modelNotice);
  m->inputs = new QWidget(body);
  auto *inputsLayout = new QVBoxLayout(m->inputs); inputsLayout->setContentsMargins(0, 0, 0, 0);
  auto *form = new QFormLayout;
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  m->mode = new QComboBox(m->inputs); m->mode->setObjectName(QStringLiteral("scaleConstraintMode"));
  m->mode->addItem(QString(), QVariant(QStringLiteral("scale_only")));
  m->mode->addItem(QString(), QVariant(QStringLiteral("sim3")));
  AppLanguage::bindComboItem(m->mode, 0, AppLanguage::source("仅定尺度（不确定方向与位置）"));
  AppLanguage::bindComboItem(m->mode, 1, AppLanguage::source("相似变换对齐（尺度、旋转、平移）"));
  form->addRow(scaleText(AppLanguage::source("估计模式")), m->mode);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(m->mode)), AppLanguage::source("估计模式"));
  m->fitLimit = new QLineEdit(m->inputs); m->fitLimit->setObjectName(QStringLiteral("scaleFitTolerance"));
  AppLanguage::bind(m->fitLimit, "placeholderText", AppLanguage::source("必须明确输入；非负有限数字，不代表模型精度"));
  form->addRow(scaleText(AppLanguage::source("拟合冲突容差 (m)")), m->fitLimit);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(m->fitLimit)), AppLanguage::source("拟合冲突容差 (m)"));
  inputsLayout->addLayout(form);
  auto *numericNotice = AppLanguage::text(new QLabel(m->inputs), AppLanguage::source(
      "坐标使用原始来源框架；向量输入 [x,y,z]，数字使用句点并支持科学计数法。参考标准不确定度留空表示未知，不是零误差；检核需明确独立来源、测量 ID 及容差。"));
  numericNotice->setWordWrap(true); numericNotice->setTextFormat(Qt::PlainText); inputsLayout->addWidget(numericNotice);
  m->tabs = new QTabWidget(m->inputs); m->tabs->setObjectName(QStringLiteral("scaleConstraintTabs"));
  m->tables = {{
      {QStringLiteral("distance"), nullptr, {QStringLiteral("source_a"), QStringLiteral("source_b"), QStringLiteral("reference_distance")},
          {AppLanguage::source("源端点 A [x,y,z]"), AppLanguage::source("源端点 B [x,y,z]"), AppLanguage::source("已知距离")}},
      {QStringLiteral("baseline"), nullptr, {QStringLiteral("view_a"), QStringLiteral("view_b"), QStringLiteral("reference_distance")},
          {AppLanguage::source("相机 view_id A"), AppLanguage::source("相机 view_id B"), AppLanguage::source("已知基线")}},
      {QStringLiteral("control"), nullptr, {QStringLiteral("source_xyz"), QStringLiteral("reference_xyz")},
          {AppLanguage::source("源控制点 [x,y,z]"), AppLanguage::source("参考控制点 [x,y,z]")}}}};
  for (auto &entry : m->tables) {
    auto *page = new QWidget(m->tabs); auto *pageLayout = new QVBoxLayout(page);
    entry.widget = new QTableWidget(0, 12 + entry.fields.size(), page);
    auto *table = entry.widget;
    table->setObjectName(entry.kind == QStringLiteral("distance") ? QStringLiteral("scaleDistanceTable")
        : entry.kind == QStringLiteral("baseline") ? QStringLiteral("scaleBaselineTable") : QStringLiteral("scaleControlTable"));
    table->setMinimumHeight(260); table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection); table->setWordWrap(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true); table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    for (int column = 0; column < table->columnCount(); ++column) table->setColumnWidth(column, column >= 12 ? 190 : 145);
    pageLayout->addWidget(table, 1);
    auto *buttons = new QHBoxLayout;
    auto *add = AppLanguage::text(new QPushButton(page), AppLanguage::source("添加约束"));
    auto *remove = AppLanguage::text(new QPushButton(page), AppLanguage::source("移除所选约束"));
    add->setObjectName(QStringLiteral("scaleAdd_%1").arg(entry.kind));
    remove->setObjectName(QStringLiteral("scaleRemove_%1").arg(entry.kind));
    buttons->addWidget(add); buttons->addWidget(remove); buttons->addStretch(); pageLayout->addLayout(buttons);
    const QString kind = entry.kind;
    connect(add, &QPushButton::clicked, this, [this, kind] {
      if (m->busy) return;
      auto request = constraintDocument(); auto rows = request.value(QStringLiteral("constraints")).toArray();
      if (rows.size() >= MaximumRows) return fail(AppLanguage::source("约束数量超过 4096；未截断或估计。"));
      QJsonObject row{{QStringLiteral("id"), kind + QLatin1Char('-') + QUuid::createUuid().toString(QUuid::WithoutBraces)},
          {QStringLiteral("kind"), kind}, {QStringLiteral("role"), QStringLiteral("fit")},
          {QStringLiteral("reference_unit"), QStringLiteral("m")}};
      rows.append(row); request.insert(QStringLiteral("constraints"), rows); setConstraintDocument(request);
    });
    connect(remove, &QPushButton::clicked, this, [this, table] {
      if (m->busy) return;
      auto selected = table->selectionModel()->selectedRows();
      std::sort(selected.begin(), selected.end(), [](const QModelIndex &a, const QModelIndex &b) { return a.row() > b.row(); });
      for (const auto &index : selected) table->removeRow(index.row());
      invalidate();
    });
    connect(table, &QTableWidget::itemChanged, this, [this] { invalidate(); });
    m->tabs->addTab(page, {});
  }
  inputsLayout->addWidget(m->tabs);
  bodyLayout->addWidget(m->inputs);
  m->summary = new QLabel(body); m->summary->setObjectName(QStringLiteral("scaleConstraintSummary"));
  m->summary->setWordWrap(true); m->summary->setTextFormat(Qt::PlainText); m->summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  bodyLayout->addWidget(m->summary);
  m->details = new QPlainTextEdit(body); m->details->setObjectName(QStringLiteral("scaleConstraintReport"));
  m->details->setReadOnly(true); m->details->setMinimumHeight(160); bodyLayout->addWidget(m->details);
  scroll->setWidget(body); layout->addWidget(scroll, 1);
  auto *buttons = new QHBoxLayout;
  m->loadRequest = AppLanguage::text(new QPushButton(this), AppLanguage::source("导入约束 JSON…"));
  m->loadRequest->setObjectName(QStringLiteral("scaleLoadConstraintsButton"));
  m->saveRequest = AppLanguage::text(new QPushButton(this), AppLanguage::source("保存约束…"));
  m->saveRequest->setObjectName(QStringLiteral("scaleSaveConstraintsButton"));
  m->saveResult = AppLanguage::text(new QPushButton(this), AppLanguage::source("保存定尺度报告…"));
  m->saveResult->setObjectName(QStringLiteral("scaleSaveReportButton"));
  m->inspect = AppLanguage::text(new QPushButton(this), AppLanguage::source("重新检查模型来源"));
  m->inspect->setObjectName(QStringLiteral("scaleInspectSourceButton"));
  m->inspect->setVisible(m->sourceKind == QStringLiteral("model"));
  m->estimate = AppLanguage::text(new QPushButton(this), AppLanguage::source("估计并独立检核"));
  m->estimate->setObjectName(QStringLiteral("scaleEstimateButton"));
  m->create = AppLanguage::text(new QPushButton(this), m->sourceKind == QStringLiteral("model")
      ? AppLanguage::source("创建新模型版本／变换记录") : AppLanguage::source("创建并使用新数据版本"));
  m->create->setObjectName(QStringLiteral("scaleCreateVersionButton"));
  m->cancel = new QPushButton(this); m->cancel->setObjectName(QStringLiteral("scaleCancelButton"));
  for (auto *button : {m->loadRequest, m->saveRequest, m->saveResult, m->inspect}) buttons->addWidget(button);
  buttons->addStretch();
  layout->addLayout(buttons);
  auto *operationButtons = new QHBoxLayout;
  operationButtons->addStretch();
  for (auto *button : {m->estimate, m->create, m->cancel}) operationButtons->addWidget(button);
  layout->addLayout(operationButtons);
  connect(m->mode, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
  connect(m->fitLimit, &QLineEdit::textChanged, this, [this] { invalidate(); });
  connect(m->estimate, &QPushButton::clicked, this, &ScaleConstraintsDialog::startEstimate);
  connect(m->create, &QPushButton::clicked, this, &ScaleConstraintsDialog::createDatasetVersion);
  connect(m->inspect, &QPushButton::clicked, this, &ScaleConstraintsDialog::inspectSource);
  connect(m->cancel, &QPushButton::clicked, this, [this] { if (m->busy) cancelOperation(); else reject(); });
  connect(m->loadRequest, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getOpenFileName(this, scaleText(AppLanguage::source("导入约束 JSON…")), m->workspaceRoot,
        scaleText(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")));
    if (!path.isEmpty()) loadConstraints(path);
  });
  connect(m->saveRequest, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getSaveFileName(this, scaleText(AppLanguage::source("保存约束…")),
        QDir(m->workspaceRoot).filePath(QStringLiteral("scale-constraints.json")),
        scaleText(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")), nullptr, QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty()) saveConstraints(path);
  });
  connect(m->saveResult, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getSaveFileName(this, scaleText(AppLanguage::source("保存定尺度报告…")),
        QDir(m->workspaceRoot).filePath(QStringLiteral("scale-solution.json")),
        scaleText(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")), nullptr, QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty()) saveReport(path);
  });
  m->process = new QProcess(this);
#ifdef Q_OS_WIN
  m->process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
    args->flags |= CREATE_NO_WINDOW; args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW; args->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  connect(m->process, &QProcess::started, this, [this] { if (m->cancelled) m->process->write("cancel\n"); });
  connect(m->process, &QProcess::readyReadStandardOutput, this, [this] { readOutput(false); });
  connect(m->process, &QProcess::readyReadStandardError, this, [this] { readOutput(true); });
  connect(m->process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) { finishProcess(code, status == QProcess::NormalExit); });
  connect(m->process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) { m->failure = AppLanguage::source("定尺度辅助进程无法启动：%1"); m->failureDetail = m->process->errorString(); finishProcess(2, false); }
  });
  m->timeout = new QTimer(this); m->timeout->setSingleShot(true);
  connect(m->timeout, &QTimer::timeout, this, [this] {
    m->failure = AppLanguage::source("定尺度操作超时，正在取消本窗口的辅助进程。原数据未改动。"); cancelOperation();
  });
  AppLanguage::onChanged(this, [this] { refreshPresentation(); });
  setConstraintDocument({{QStringLiteral("schema"), QStringLiteral("gsw.scale_constraints/1")},
      {QStringLiteral("input_data_identity"), m->expectedIdentity}, {QStringLiteral("input_frame"), QStringLiteral("source")},
      {QStringLiteral("mode"), QStringLiteral("scale_only")}, {QStringLiteral("constraints"), QJsonArray{}}});
  if ((m->sourceKind != QStringLiteral("model") && m->sourceKind != QStringLiteral("dataset")) ||
      descriptor.value(QStringLiteral("frame")) != QStringLiteral("source"))
    fail(AppLanguage::source("约束数据身份、坐标框架或版本不匹配；未改动当前输入。"));
  else if (m->sourceKind == QStringLiteral("model")) QTimer::singleShot(0, this, &ScaleConstraintsDialog::inspectSource);
}

ScaleConstraintsDialog::~ScaleConstraintsDialog() {
  if (m->process->state() != QProcess::NotRunning) {
    m->process->write("cancel\n"); m->process->waitForBytesWritten(100);
    if (!m->process->waitForFinished(500)) { m->process->kill(); m->process->waitForFinished(1500); }
  }
}

QJsonObject ScaleConstraintsDialog::constraintDocument() const {
  QJsonObject result = m->original;
  result.insert(QStringLiteral("schema"), QStringLiteral("gsw.scale_constraints/1"));
  result.insert(QStringLiteral("input_data_identity"), m->expectedIdentity);
  result.insert(QStringLiteral("input_frame"), QStringLiteral("source"));
  result.insert(QStringLiteral("mode"), m->mode->currentData().toString());
  const auto tolerance = numeric(m->fitLimit->text());
  if (tolerance.isUndefined()) result.remove(QStringLiteral("fit_tolerance_m")); else result.insert(QStringLiteral("fit_tolerance_m"), tolerance);
  struct OrderedRow { int order; QJsonObject value; };
  QList<OrderedRow> ordered;
  const QStringList common{QStringLiteral("id"), QStringLiteral("role"), QStringLiteral("reference_id"), QStringLiteral("reference_unit"),
      QStringLiteral("weight"), QStringLiteral("reference_sigma"), QStringLiteral("source"), QStringLiteral("measurement_id"),
      QStringLiteral("uncertainty_notes"), QStringLiteral("independent_of_fit"), QStringLiteral("tolerance_m"), QStringLiteral("provenance")};
  for (const auto &entry : m->tables) for (int row = 0; row < entry.widget->rowCount(); ++row) {
    auto *table = entry.widget;
    QJsonObject item = table->item(row, 0)->data(Qt::UserRole).toJsonObject();
    const int order = table->item(row, 0)->data(Qt::UserRole + 1).toInt();
    auto provenance = item.value(QStringLiteral("provenance")).toObject();
    const auto rawProvenance = QJsonDocument::fromJson(table->item(row, 11)->text().toUtf8());
    if (rawProvenance.isObject()) provenance = rawProvenance.object();
    for (int column = 0; column < table->columnCount(); ++column) {
      if (column == 11) continue;
      const QString field = column < common.size() ? common[column] : entry.fields[column - common.size()];
      const auto *combo = qobject_cast<QComboBox *>(table->cellWidget(row, column));
      const QString value = combo ? combo->currentData().toString() : table->item(row, column)->text();
      if (column >= 6 && column <= 8) {
        if (value.isEmpty()) provenance.remove(field); else provenance.insert(field, value);
      } else if (column == 9) {
        if (value.isEmpty()) item.remove(field); else item.insert(field, value == QStringLiteral("true"));
      } else if (column == 4 || column == 5 || column == 10 || field == QStringLiteral("reference_distance")) {
        const auto number = numeric(value);
        if (number.isUndefined()) item.remove(field); else item.insert(field, number);
      } else if (field == QStringLiteral("source_a") || field == QStringLiteral("source_b") ||
          field == QStringLiteral("source_xyz") || field == QStringLiteral("reference_xyz")) {
        item.insert(field, vectorValue(value));
      } else item.insert(field, value);
    }
    if (provenance.isEmpty() && !item.contains(QStringLiteral("provenance"))) item.remove(QStringLiteral("provenance"));
    else item.insert(QStringLiteral("provenance"), provenance);
    item.insert(QStringLiteral("kind"), entry.kind); ordered.append({order, item});
  }
  std::stable_sort(ordered.begin(), ordered.end(), [](const OrderedRow &a, const OrderedRow &b) { return a.order < b.order; });
  QJsonArray rows; for (const auto &row : ordered) rows.append(row.value);
  result.insert(QStringLiteral("constraints"), rows);
  return result;
}

void ScaleConstraintsDialog::setConstraintDocument(const QJsonObject &document) {
  if (m->busy) return;
  if (!document.value(QStringLiteral("constraints")).isArray())
    return fail(AppLanguage::source("约束 JSON 无效：%1"), QStringLiteral("constraints_must_be_array"));
  const auto constraints = document.value(QStringLiteral("constraints")).toArray();
  if (constraints.size() > MaximumRows) return fail(AppLanguage::source("约束数量超过 4096；未截断或估计。"));
  for (const auto &row : constraints) if (!row.isObject() ||
      (row.toObject().value(QStringLiteral("kind")) != QStringLiteral("distance") &&
       row.toObject().value(QStringLiteral("kind")) != QStringLiteral("baseline") &&
       row.toObject().value(QStringLiteral("kind")) != QStringLiteral("control")))
    return fail(AppLanguage::source("约束 JSON 含未知类型；未截断、替换或估计。"));
  if (m->sourceKind == QStringLiteral("model")) for (const auto &row : constraints)
    if (row.toObject().value(QStringLiteral("kind")) == QStringLiteral("baseline"))
      return fail(AppLanguage::source("所选模型没有相机来源，不能使用或导入相机基线约束；原约束未改动。"));
  const QString identity = document.value(QStringLiteral("input_data_identity")).toString();
  if ((!identity.isEmpty() && identity != m->expectedIdentity) ||
      document.value(QStringLiteral("input_frame")).toString(QStringLiteral("source")) != QStringLiteral("source") ||
      document.value(QStringLiteral("schema")).toString() != QStringLiteral("gsw.scale_constraints/1"))
    return fail(AppLanguage::source("约束数据身份、坐标框架或版本不匹配；未改动当前输入。"));
  m->loading = true; m->original = document;
  const int modeIndex = m->mode->findData(document.value(QStringLiteral("mode")).toString());
  m->mode->setCurrentIndex(modeIndex);
  const auto tolerance = document.value(QStringLiteral("fit_tolerance_m"));
  m->fitLimit->setText(tolerance.isUndefined() ? QString() : tolerance.isDouble() ? compact(tolerance) : tolerance.toString());
  for (auto &entry : m->tables) entry.widget->setRowCount(0);
  int sequence = 0;
  for (const auto &value : constraints) {
    const auto item = value.toObject();
    auto &entry = *std::find_if(m->tables.begin(), m->tables.end(), [&](const State::Table &table) { return table.kind == item.value(QStringLiteral("kind")); });
    auto *table = entry.widget; const int row = table->rowCount(); table->insertRow(row);
    const auto provenance = item.value(QStringLiteral("provenance")).toObject();
    QStringList cells{item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("role")).toString(),
        item.value(QStringLiteral("reference_id")).toString(), item.value(QStringLiteral("reference_unit")).toString(),
        item.contains(QStringLiteral("weight")) ? compact(item.value(QStringLiteral("weight"))) : QString(),
        item.contains(QStringLiteral("reference_sigma")) ? compact(item.value(QStringLiteral("reference_sigma"))) : QString(),
        provenance.value(QStringLiteral("source")).toString(), provenance.value(QStringLiteral("measurement_id")).toString(),
        provenance.value(QStringLiteral("uncertainty_notes")).toString(),
        item.contains(QStringLiteral("independent_of_fit")) ? item.value(QStringLiteral("independent_of_fit")).toBool() ? QStringLiteral("true") : QStringLiteral("false") : QString(),
        item.contains(QStringLiteral("tolerance_m")) ? compact(item.value(QStringLiteral("tolerance_m"))) : QString(),
        QString::fromUtf8(QJsonDocument(provenance).toJson(QJsonDocument::Compact))};
    for (const auto &field : entry.fields) {
      const auto number = item.value(field);
      cells.append(number.isString() ? number.toString() : number.isUndefined() ? QString() : compact(number));
    }
    for (int column = 0; column < cells.size(); ++column) {
      auto *cell = new QTableWidgetItem(cells[column]); table->setItem(row, column, cell);
      if (column == 11) cell->setFlags(cell->flags() & ~Qt::ItemIsEditable);
      if (column == 0) { cell->setData(Qt::UserRole, item); cell->setData(Qt::UserRole + 1, sequence++); }
      if (column == 1 || column == 3 || column == 9) {
        auto *combo = new QComboBox(table);
        if (column == 1) {
          combo->addItem(QString(), QVariant(QStringLiteral("fit")));
          combo->addItem(QString(), QVariant(QStringLiteral("check")));
          AppLanguage::bindComboItem(combo, 0, AppLanguage::source("参与拟合"));
          AppLanguage::bindComboItem(combo, 1, AppLanguage::source("仅作独立检核"));
        } else if (column == 3) for (const auto &unit : {QStringLiteral("m"), QStringLiteral("cm"), QStringLiteral("mm")}) combo->addItem(unit, unit);
        else {
          combo->addItem(QString(), QVariant(QString()));
          combo->addItem(QString(), QVariant(QStringLiteral("false")));
          combo->addItem(QString(), QVariant(QStringLiteral("true")));
          AppLanguage::bindComboItem(combo, 0, AppLanguage::source("未声明"));
          AppLanguage::bindComboItem(combo, 1, AppLanguage::source("否")); AppLanguage::bindComboItem(combo, 2, AppLanguage::source("是"));
        }
        combo->setCurrentIndex(combo->findData(cells[column])); table->setCellWidget(row, column, combo);
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
      }
    }
  }
  m->loading = false; invalidate();
}

QJsonObject ScaleConstraintsDialog::solutionReport() const { return m->report; }
QString ScaleConstraintsDialog::resultDatasetPath() const { return m->resultPath; }
QJsonObject ScaleConstraintsDialog::resultDatasetInfo() const { return m->resultInfo; }
bool ScaleConstraintsDialog::isRunning() const { return m->busy; }
QJsonObject ScaleConstraintsDialog::sourceDescriptor() const {
  return {{QStringLiteral("kind"), m->sourceKind}, {QStringLiteral("path"), m->datasetPath},
      {QStringLiteral("identity"), m->expectedIdentity}, {QStringLiteral("frame"), QStringLiteral("source")},
      {QStringLiteral("inspected"), m->inspectionReady}};
}
QString ScaleConstraintsDialog::resultVersionPath() const { return m->versionPath; }
bool ScaleConstraintsDialog::loadConstraints(const QString &path) {
  if (m->busy) return false;
  QByteArray bytes;
  QJsonParseError error;
  if (!readBounded(path, &bytes)) { fail(AppLanguage::source("无法读取约束 JSON，或内容超过 16 MiB。原文件未改动。")); return false; }
  const auto document = QJsonDocument::fromJson(bytes, &error);
  const bool duplicate = document.isObject() && error.error == QJsonParseError::NoError && duplicateObjectKey(bytes);
  if (!document.isObject() || error.error != QJsonParseError::NoError || duplicate) {
    fail(AppLanguage::source("约束 JSON 无效：%1"), duplicate ? QStringLiteral("duplicate_object_key") : error.errorString()); return false;
  }
  const QJsonObject before = m->original;
  setConstraintDocument(document.object());
  return !m->failure && (m->original != before || m->original == document.object());
}
bool ScaleConstraintsDialog::saveConstraints(const QString &path) {
  if (m->busy) return false;
  const auto bytes = QJsonDocument(constraintDocument()).toJson();
  QString error;
  if (bytes.size() > MaximumBytes || !writeFresh(path, bytes, &error)) {
    m->message = AppLanguage::source("无法保存尺度记录：%1；已有文件未覆盖。"); m->messageDetail = error; refreshPresentation(); return false;
  }
  m->message = AppLanguage::source("尺度记录已保存：%1"); m->messageDetail = QDir::toNativeSeparators(path); refreshPresentation(); return true;
}
bool ScaleConstraintsDialog::saveReport(const QString &path) {
  if (m->busy || m->report.isEmpty()) return false;
  const auto bytes = QJsonDocument(m->report).toJson();
  QString error;
  if (bytes.size() > MaximumBytes || !writeFresh(path, bytes, &error)) {
    m->message = AppLanguage::source("无法保存尺度记录：%1；已有文件未覆盖。"); m->messageDetail = error; refreshPresentation(); return false;
  }
  m->message = AppLanguage::source("尺度记录已保存：%1"); m->messageDetail = QDir::toNativeSeparators(path); refreshPresentation(); return true;
}
void ScaleConstraintsDialog::invalidate() {
  if (m->loading || m->busy) return;
  m->report = {}; m->resultInfo = {}; m->resultPath.clear(); m->versionPath.clear(); m->failure = nullptr; m->failureDetail.clear(); m->message = nullptr;
  m->cancelled = false; refreshEnabled(); refreshPresentation();
}
void ScaleConstraintsDialog::fail(const char *source, const QString &detail) {
  m->failure = source; m->failureDetail = detail; m->report = {}; m->resultInfo = {}; m->resultPath.clear(); m->versionPath.clear();
  refreshEnabled(); refreshPresentation();
}
void ScaleConstraintsDialog::startEstimate() { startOperation(false); }
void ScaleConstraintsDialog::inspectSource() { if (m->sourceKind == QStringLiteral("model")) startOperation(false, true); }
void ScaleConstraintsDialog::createDatasetVersion() {
  if (m->report.isEmpty() || m->report.value(QStringLiteral("independent_checks")).toObject().value(QStringLiteral("status")) == QStringLiteral("fail")) return;
  startOperation(true);
}
void ScaleConstraintsDialog::startOperation(const bool createVersion, const bool inspect) {
  if (m->busy) return;
  const auto request = constraintDocument();
  const auto tolerance = request.value(QStringLiteral("fit_tolerance_m"));
  if (!inspect && (!m->inspectionReady || m->expectedIdentity.isEmpty()))
    return fail(AppLanguage::source("模型来源尚未核验；请先完成后台来源检查，不能根据预览猜测尺度。"));
  if (!inspect && (!tolerance.isDouble() || tolerance.toDouble() < 0))
    return fail(AppLanguage::source("请明确输入非负有限的拟合冲突容差；它不是模型精度。"));
  const QString script = QDir(m->backendRoot).filePath(QStringLiteral("native/worker/scale_constraints.py"));
  if (!QFileInfo(m->python).isFile() || !QFileInfo(script).isFile())
    return fail(AppLanguage::source("定尺度工具不可用；请检查已安装的 Python 与辅助脚本，不会自动安装或更换相机策略。"));
  const QString root = QFileInfo(m->workspaceRoot).canonicalFilePath();
  if (root.isEmpty()) return fail(AppLanguage::source("无法在当前工程创建受控尺度工作目录。"));
  QString managed = root;
  for (const auto &part : {QStringLiteral(".gsw"), QStringLiteral("scale")}) {
    const QString next = QDir(managed).filePath(part); QFileInfo info(next);
    if (!info.exists() && !info.isSymLink() && !QDir(managed).mkdir(part))
      return fail(AppLanguage::source("无法在当前工程创建受控尺度工作目录。"));
    info.refresh(); const QString canonical = info.canonicalFilePath();
    if (!info.isDir() || canonical.isEmpty() || !canonical.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
      return fail(AppLanguage::source("无法在当前工程创建受控尺度工作目录。"));
    managed = canonical;
  }
  m->scratch = std::make_unique<QTemporaryDir>(QDir(managed).filePath(QStringLiteral("request-XXXXXX")));
  if (!m->scratch->isValid()) return fail(AppLanguage::source("无法在当前工程创建受控尺度工作目录。"));
  const auto bytes = QJsonDocument(request).toJson(); QString error;
  if (!inspect && (bytes.size() > MaximumBytes || !writeFresh(m->scratch->filePath(QStringLiteral("request.json")), bytes, &error)))
    return fail(AppLanguage::source("无法写入尺度请求：%1"), error);
  m->reportPath = m->scratch->filePath(QStringLiteral("solution.json"));
  m->operationOutput = createVersion ? QDir(managed).filePath(QStringLiteral("dataset-") + QUuid::createUuid().toString(QUuid::WithoutBraces)) : QString();
  QStringList args{QStringLiteral("-B"), script, QStringLiteral("--operation"), inspect ? QStringLiteral("inspect")
      : createVersion ? QStringLiteral("create-version") : QStringLiteral("solve"),
      m->sourceKind == QStringLiteral("model") ? QStringLiteral("--model") : QStringLiteral("--dataset"), m->datasetPath,
      QStringLiteral("--report"), m->reportPath};
  if (!inspect) args << QStringLiteral("--request") << m->scratch->filePath(QStringLiteral("request.json"))
      << QStringLiteral("--expected-identity") << m->expectedIdentity;
  if (createVersion) args << QStringLiteral("--output-directory") << m->operationOutput;
  m->failure = nullptr; m->message = nullptr; m->cancelled = false; m->busy = true; m->creating = createVersion; m->inspecting = inspect;
  if (inspect) { m->inspectionReady = false; m->report = {}; }
  m->runRequest = request;
  m->resultInfo = {}; m->resultPath.clear(); m->versionPath.clear(); m->output.clear(); m->errors.clear(); ++m->run;
  auto environment = m->environment;
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  environment.insert(QStringLiteral("TEMP"), m->scratch->path()); environment.insert(QStringLiteral("TMP"), m->scratch->path());
  m->process->setProgram(m->python); m->process->setArguments(args); m->process->setProcessEnvironment(environment);
  m->process->setWorkingDirectory(m->backendRoot);
  refreshEnabled(); refreshPresentation(); m->timeout->start(createVersion || inspect ? 600000 : 60000); m->process->start();
}
void ScaleConstraintsDialog::readOutput(const bool standardError) {
  m->process->setReadChannel(standardError ? QProcess::StandardError : QProcess::StandardOutput);
  QByteArray &target = standardError ? m->errors : m->output;
  const qint64 budget = MaximumBytes - m->output.size() - m->errors.size();
  if (budget >= 0) target += m->process->read(budget + 1);
  if (m->output.size() + m->errors.size() > MaximumBytes) {
    m->failure = AppLanguage::source("定尺度辅助进程输出超过 16 MiB，正在取消。");
    m->process->closeReadChannel(QProcess::StandardOutput); m->process->closeReadChannel(QProcess::StandardError); cancelOperation();
  }
}
void ScaleConstraintsDialog::finishProcess(const int exitCode, const bool normalExit) {
  if (!m->busy) return;
  m->timeout->stop(); readOutput(false); readOutput(true); m->busy = false;
  if (m->cancelled) { m->report = {}; }
  else if (!m->failure) {
    QJsonObject response;
    const auto lines = m->output.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
      const auto parsed = QJsonDocument::fromJson(it->trimmed());
      if (parsed.isObject() && parsed.object().contains(QStringLiteral("success"))) { response = parsed.object(); break; }
    }
    QByteArray bytes;
    const auto path = response.value(QStringLiteral("reportPath")).toString();
    const bool sameReport = QFileInfo(path).canonicalFilePath() == QFileInfo(m->reportPath).canonicalFilePath() && !path.isEmpty();
    const bool readable = sameReport && readBounded(m->reportPath, &bytes);
    const auto report = readable ? QJsonDocument::fromJson(bytes).object() : QJsonObject{};
    const auto status = report.value(QStringLiteral("independent_checks")).toObject().value(QStringLiteral("status")).toString();
    const bool reportValid = report.value(QStringLiteral("schema")) == QStringLiteral("gsw.scale_solution/1") &&
        report.value(QStringLiteral("input_data_identity")) == m->expectedIdentity &&
        report.value(QStringLiteral("model_geometry_accuracy")) == QStringLiteral("not_measured") &&
        report.value(QStringLiteral("scope")) == QStringLiteral("scale_and_reference_constraint_agreement") &&
        report.value(QStringLiteral("request")).toObject() == m->runRequest &&
        report.value(QStringLiteral("fit")).toObject().value(QStringLiteral("accepted")).toBool() &&
        report.value(QStringLiteral("source_to_metric")).isObject() && report.value(QStringLiteral("fit")).isObject() &&
        (status == QStringLiteral("pass") || status == QStringLiteral("fail") || status == QStringLiteral("unverified") || status == QStringLiteral("not_provided"));
    const QString inspectionIdentity = response.value(QStringLiteral("inputDataIdentity")).toString();
    const bool inspectionValid = m->inspecting && report.value(QStringLiteral("schema")) == QStringLiteral("gsw.scale_input/1") &&
        report.value(QStringLiteral("kind")) == QStringLiteral("model") && report.value(QStringLiteral("views")).isArray() &&
        report.value(QStringLiteral("views")).toArray().isEmpty() && report.value(QStringLiteral("data_identity")) == inspectionIdentity &&
        QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(inspectionIdentity).hasMatch() &&
        response.value(QStringLiteral("summary")).toObject().value(QStringLiteral("sourceKind")) == QStringLiteral("model");
    if (normalExit && exitCode == 0 && response.value(QStringLiteral("success")).toBool() && inspectionValid) {
      const QString pinnedIdentity = m->requestedIdentity.isEmpty() ? m->expectedIdentity : m->requestedIdentity;
      if (!pinnedIdentity.isEmpty() && pinnedIdentity != inspectionIdentity) {
        fail(AppLanguage::source("模型来源身份已改变；旧约束未自动移植，请重新确认来源。"));
      } else {
        m->expectedIdentity = inspectionIdentity; m->original.insert(QStringLiteral("input_data_identity"), inspectionIdentity);
        m->inspectionReady = true; m->report = {}; m->message = AppLanguage::source("模型来源已在后台核验；这不是尺度估计或模型精度验证。"); m->messageDetail.clear();
      }
    } else if (normalExit && exitCode == 0 && response.value(QStringLiteral("success")).toBool() && reportValid && !m->inspecting) {
      m->report = report;
      if (m->creating) {
        const auto returnedPath = QFileInfo(response.value(m->sourceKind == QStringLiteral("model")
            ? QStringLiteral("versionPath") : QStringLiteral("datasetPath")).toString()).canonicalFilePath();
        const auto expectedPath = QFileInfo(m->operationOutput).canonicalFilePath();
        const auto summary = response.value(QStringLiteral("summary")).toObject();
        if (status == QStringLiteral("fail") || returnedPath.isEmpty() || returnedPath != expectedPath)
          fail(AppLanguage::source("新尺度版本未通过身份与锁定策略核验；未切换当前数据集。"));
        else if (m->sourceKind == QStringLiteral("model")) {
          const QString representation = summary.value(QStringLiteral("representation")).toString();
          const QString modelPath = response.value(QStringLiteral("modelPath")).toString();
          const QString canonicalModel = QFileInfo(modelPath).canonicalFilePath();
          const bool baked = representation == QStringLiteral("baked_model") && !canonicalModel.isEmpty() &&
              QFileInfo(modelPath).isFile() && canonicalModel.startsWith(returnedPath + QLatin1Char('/'), Qt::CaseInsensitive);
          const bool manifest = representation == QStringLiteral("transform_manifest") && modelPath.isEmpty();
          if (summary.value(QStringLiteral("sourceKind")) != QStringLiteral("model") || (!baked && !manifest))
            fail(AppLanguage::source("新模型版本没有有效的烘焙产物或明确的变换清单；未采用任何模型。"));
          else {
            m->versionPath = returnedPath; m->resultInfo = summary;
            m->resultInfo.insert(QStringLiteral("modelPath"), baked ? canonicalModel : QString());
            m->resultInfo.insert(QStringLiteral("inputDataIdentity"), m->expectedIdentity);
            m->resultInfo.insert(QStringLiteral("sourcePath"), m->datasetPath);
          }
        } else if (summary.value(QStringLiteral("blocked")).toBool(true) || summary.value(QStringLiteral("posePolicy")) != QStringLiteral("locked") ||
            summary.value(QStringLiteral("dataIdentity")).toString().isEmpty() ||
            summary.value(QStringLiteral("scaleDerivation")).toObject().value(QStringLiteral("solutionSha256")) != report.value(QStringLiteral("solution_sha256")))
          fail(AppLanguage::source("新尺度版本未通过身份与锁定策略核验；未切换当前数据集。"));
        else { m->resultPath = returnedPath; m->versionPath = returnedPath; m->resultInfo = summary; }
      }
    } else {
      const QString code = response.value(QStringLiteral("code")).toString();
      if (m->sourceKind == QStringLiteral("model") && code.contains(QStringLiteral("identity"))) m->inspectionReady = false;
      const QString detail = response.value(QStringLiteral("detail")).toVariant().toString();
      const QString diagnostics = response.value(QStringLiteral("diagnostics")).isObject()
          ? QString::fromUtf8(QJsonDocument(response.value(QStringLiteral("diagnostics")).toObject()).toJson(QJsonDocument::Indented)) : QString();
      fail(AppLanguage::source("定尺度未返回有效结果。技术详情：%1"), code + QLatin1Char('\n') + detail + QLatin1Char('\n') +
          diagnostics.left(16384) + QLatin1Char('\n') + QString::fromUtf8(m->errors.left(8192)));
    }
  }
  m->scratch.reset(); refreshEnabled(); refreshPresentation();
  emit estimateFinished(m->report);
  if (!m->resultPath.isEmpty()) emit datasetVersionCreated(m->resultPath, m->resultInfo);
  if (!m->versionPath.isEmpty()) emit versionCreated(m->sourceKind, m->versionPath, m->resultInfo);
  if (m->closePending) QDialog::reject();
}
void ScaleConstraintsDialog::cancelOperation() {
  if (!m->busy || m->cancelled) return;
  m->cancelled = true; m->report = {}; m->resultInfo = {}; m->resultPath.clear(); m->versionPath.clear();
  m->process->write("cancel\n");
  const quint64 run = m->run;
  QTimer::singleShot(5000, this, [this, run] {
    if (m->busy && m->run == run && m->process->state() != QProcess::NotRunning) m->process->terminate();
  });
  QTimer::singleShot(7000, this, [this, run] {
    if (m->busy && m->run == run && m->process->state() != QProcess::NotRunning) m->process->kill();
  });
  refreshPresentation();
}
void ScaleConstraintsDialog::reject() {
  if (m->busy) { m->closePending = true; cancelOperation(); return; }
  QDialog::reject();
}
void ScaleConstraintsDialog::closeEvent(QCloseEvent *event) {
  if (m->busy) { m->closePending = true; cancelOperation(); event->ignore(); }
  else QDialog::closeEvent(event);
}
void ScaleConstraintsDialog::refreshEnabled() {
  m->inputs->setEnabled(!m->busy); m->loadRequest->setEnabled(!m->busy && m->inspectionReady); m->saveRequest->setEnabled(!m->busy);
  m->estimate->setEnabled(!m->busy && m->inspectionReady); m->saveResult->setEnabled(!m->busy && !m->report.isEmpty());
  m->inspect->setEnabled(!m->busy); m->tabs->setTabEnabled(1, m->sourceKind != QStringLiteral("model"));
  const QString status = m->report.value(QStringLiteral("independent_checks")).toObject().value(QStringLiteral("status")).toString();
  m->create->setEnabled(!m->busy && !m->report.isEmpty() && status != QStringLiteral("fail") && m->versionPath.isEmpty());
}
void ScaleConstraintsDialog::refreshPresentation() {
  m->tabs->setTabText(0, scaleText(AppLanguage::source("已知距离")));
  m->tabs->setTabText(1, scaleText(AppLanguage::source("相机基线")));
  m->tabs->setTabText(2, scaleText(AppLanguage::source("控制点")));
  m->sourceLabel->setText(QDir::toNativeSeparators(m->datasetPath) + QLatin1Char('\n') +
      (m->inspectionReady ? m->expectedIdentity : scaleText(AppLanguage::source("来源身份待后台核验"))));
  for (auto &entry : m->tables) {
    QStringList headers; for (const auto *caption : CommonHeaders) headers.append(scaleText(caption));
    for (const auto *caption : entry.captions) headers.append(scaleText(caption));
    entry.widget->setHorizontalHeaderLabels(headers);
  }
  m->cancel->setText(scaleText(m->busy ? AppLanguage::source("取消定尺度操作") : AppLanguage::source("关闭")));
  QString summary;
  if (m->busy) summary = scaleText(m->cancelled ? AppLanguage::source("正在取消；等待本窗口辅助进程清理，训练任务不受影响。")
      : m->inspecting ? AppLanguage::source("正在后台核验模型与关联资源的来源身份；未估计尺度，不改变原模型。")
      : m->creating ? AppLanguage::source("正在创建独立数据版本；原相机、图像与检查点不改动。")
      : AppLanguage::source("正在估计与检核；检核约束不会参与拟合。"));
  else if (m->failure) { summary = scaleText(m->failure); if (summary.contains(QStringLiteral("%1"))) summary = summary.arg(m->failureDetail); }
  else if (m->cancelled) summary = scaleText(AppLanguage::source("定尺度已取消；没有切换数据集或更改训练任务。"));
  else if (m->report.isEmpty()) summary = scaleText(AppLanguage::source("尚未估计。请分别填写拟合与检核约束，并明确拟合冲突容差。"));
  else {
    const auto checks = m->report.value(QStringLiteral("independent_checks")).toObject();
    const auto transform = m->report.value(QStringLiteral("source_to_metric")).toObject();
    summary = scaleText(AppLanguage::source("尺度估计：s=%1；%2。模型几何精度未测量。"))
        .arg(QLocale().toString(transform.value(QStringLiteral("s")).toDouble(), 'g', 17), checkStatus(checks.value(QStringLiteral("status")).toString()));
    const auto fit = m->report.value(QStringLiteral("fit")).toObject();
    const auto fitValue = [&fit](const QString &key) {
      return fit.value(key).isDouble() ? QLocale().toString(fit.value(key).toDouble(), 'g', 17)
          : scaleText(AppLanguage::source("未提供"));
    };
    summary += QLatin1Char('\n') + scaleText(AppLanguage::source("拟合约束一致性（不是模型精度）：加权 RMSE %1 m；最大残差 %2 m。"))
        .arg(fitValue(QStringLiteral("weighted_rmse_m")), fitValue(QStringLiteral("max_residual_m")));
    if (checks.value(QStringLiteral("status")) != QStringLiteral("pass")) summary += QLatin1Char('\n') +
        scaleText(AppLanguage::source("仅可标记为约束求得；缺少独立检核不等于检核通过。"));
    if (!m->resultPath.isEmpty()) summary += QLatin1Char('\n') + scaleText(AppLanguage::source("新数据版本：%1；旧版本与原始文件保持不变。"))
        .arg(QDir::toNativeSeparators(m->resultPath));
    if (!m->versionPath.isEmpty() && m->sourceKind == QStringLiteral("model")) summary += QLatin1Char('\n') +
        scaleText(m->resultInfo.value(QStringLiteral("representation")) == QStringLiteral("baked_model")
            ? AppLanguage::source("新模型版本已生成：%1。变换已应用到新文件，原模型保持不变。")
            : AppLanguage::source("仅生成变换清单与原模型副本：%1。方向相关球谐尚未烘焙；不会把原模型当作已对齐产物采用。"))
            .arg(QDir::toNativeSeparators(m->versionPath));
  }
  if (m->message) summary += QLatin1Char('\n') + scaleText(m->message).arg(m->messageDetail);
  m->summary->setText(summary);
  const QString details = m->report.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(m->report).toJson(QJsonDocument::Indented));
  if (m->details->toPlainText() != details) m->details->setPlainText(details);
  AppLanguage::bind(m->details, "toolTip", AppLanguage::source("原始机器报告：拟合统计、独立检核、参考不确定度、变换及逆变换分别保留；这不是模型精度报告。"));
}
} // namespace gsw
