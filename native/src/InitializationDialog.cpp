#include "InitializationDialog.h"
#include "InitializationEditor.h"
#include "InitializationPreview.h"
#include "AppLanguage.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
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
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
namespace gsw {
namespace {
constexpr qint64 maximumBytes = 16 * 1024 * 1024;
QString initText(const char *source) { return QCoreApplication::translate("Workbench", source); }
bool writeNew(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  if (bytes.size() > maximumBytes || !file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
  const bool ok = file.write(bytes) == bytes.size() && file.flush();
  file.close(); if (!ok) file.remove(); return ok;
}
QJsonObject readObject(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > maximumBytes) return {};
  return QJsonDocument::fromJson(file.readAll()).object();
}
QJsonValue number(const QString &value) {
  bool ok = false; const double n = QLocale::c().toDouble(value.trimmed(), &ok);
  return ok && std::isfinite(n) ? QJsonValue(n) : QJsonValue(value);
}
bool positiveJsonNumber(const QJsonValue &value) {
  return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0.;
}
bool representableBounds(const QJsonValue &value) {
  if (value.isUndefined() || value.isNull()) return true;
  if (!value.isArray() || value.toArray().size() != 2) return false;
  const auto bounds = value.toArray();
  for (const auto &side : bounds) {
    if (!side.isArray() || side.toArray().size() != 3) return false;
    for (const auto &component : side.toArray())
      if (!component.isDouble() || !std::isfinite(component.toDouble())) return false;
  }
  for (int axis = 0; axis < 3; ++axis)
    if (bounds[0].toArray()[axis].toDouble() >= bounds[1].toArray()[axis].toDouble()) return false;
  return true;
}
bool representableSuggestion(const QJsonValue &value) {
  if (value.isUndefined()) return true;
  if (!value.isObject()) return false;
  const auto suggestion = value.toObject();
  const QString method = suggestion.value(QStringLiteral("method")).toString();
  QSet<QString> allowed{QStringLiteral("method")};
  if (method == QStringLiteral("optical_axis_convergence"))
    allowed.unite({QStringLiteral("view_ids"), QStringLiteral("half_extents")});
  else if (method == QStringLiteral("user_depth_interval"))
    allowed.unite({QStringLiteral("view_id"), QStringLiteral("near"), QStringLiteral("far")});
  else return false;
  for (auto it = suggestion.constBegin(); it != suggestion.constEnd(); ++it)
    if (!allowed.contains(it.key())) return false;
  if (suggestion.contains(QStringLiteral("half_extents"))) {
    const auto half = suggestion.value(QStringLiteral("half_extents"));
    if (!half.isArray() || half.toArray().size() != 3) return false;
    for (const auto &component : half.toArray()) if (!positiveJsonNumber(component)) return false;
  }
  if (suggestion.contains(QStringLiteral("view_ids"))) {
    const auto ids = suggestion.value(QStringLiteral("view_ids"));
    if (!ids.isArray() || ids.toArray().isEmpty()) return false;
    QSet<QString> seen;
    for (const auto &id : ids.toArray()) {
      if (!id.isString() || id.toString().trimmed().isEmpty() || seen.contains(id.toString())) return false;
      seen.insert(id.toString());
    }
  }
  if (suggestion.contains(QStringLiteral("view_id"))) {
    const auto id = suggestion.value(QStringLiteral("view_id"));
    if (!id.isString() || id.toString().trimmed().isEmpty()) return false;
  }
  for (const auto &key : {QStringLiteral("near"), QStringLiteral("far")})
    if (suggestion.contains(key) && !positiveJsonNumber(suggestion.value(key))) return false;
  return !(suggestion.contains(QStringLiteral("near")) && suggestion.contains(QStringLiteral("far")) &&
      suggestion.value(QStringLiteral("near")).toDouble() >= suggestion.value(QStringLiteral("far")).toDouble());
}
QString diagnosis(const QString &code) {
  if (code == QStringLiteral("initialization_bounds_required"))
    return initText(AppLanguage::source("请明确输入粗范围，或提供需要确认的建议参数；相机位置本身不确定物体深度。"));
  if (code == QStringLiteral("initialization_backend_near_excludes_all_points"))
    return initText(AppLanguage::source("某些训练相机的全部初始化点被实际后端近裁剪排除；不能仅修改显示用近裁剪参数。"));
  if (code == QStringLiteral("initialization_no_points_inside_training_image"))
    return initText(AppLanguage::source("某些训练相机没有处于有效图像内的初始化点；请检查框架、范围和相机朝向。"));
  if (code == QStringLiteral("camera_trajectory_does_not_determine_object_depth_or_extent"))
    return initText(AppLanguage::source("相机轨迹不能唯一确定物体深度或范围；未把轨迹包围盒当作对象包围盒。"));
  if (code == QStringLiteral("initialization_materialization_invalidated_by_recipe_change"))
    return initText(AppLanguage::source("初始化配方已改变：旧冻结点集已失效，将按当前种子、点数、框架和范围重新生成。"));
  if (code == QStringLiteral("initialization_parallel_or_rank_deficient_axes"))
    return initText(AppLanguage::source("光轴平行或约束退化，无法确定唯一会聚中心；请增加有效训练视角或明确深度区间。"));
  if (code == QStringLiteral("initialization_half_extents_required") || code == QStringLiteral("object_extent_is_user_supplied_not_observed"))
    return initText(AppLanguage::source("对象尺寸需要明确声明；光轴会聚不能观测对象边界。"));
  if (code == QStringLiteral("optical_axes_are_only_a_target_center_hypothesis"))
    return initText(AppLanguage::source("光轴会聚中心只是观察目标的假设，不是已测表面点。"));
  if (code == QStringLiteral("selected_view_frustum_aabb_is_not_object_bounds") || code == QStringLiteral("depth_interval_is_explicit_user_input_not_observed_depth"))
    return initText(AppLanguage::source("深度来自用户声明；选定视锥的外接范围不是已知对象边界。"));
  return initText(AppLanguage::source("技术诊断（原始标识保留）：%1")).arg(code);
}
}
class InitializationDialog::State {
public:
  QString python, backendRoot, workspace, dataset, identity, operation, reportPath, outputPath, versionPath;
  QProcessEnvironment environment; QJsonObject original, report, runRequest; QByteArray output, errors;
  InitializationEditor *editor = nullptr; InitializationPreview *preview = nullptr; QWidget *inputs = nullptr;
  QComboBox *backend = nullptr, *resolution = nullptr, *method = nullptr, *depthView = nullptr, *projection = nullptr, *view = nullptr;
  std::array<QLineEdit *, 3> extent{}; QLineEdit *nearDepth = nullptr, *farDepth = nullptr;
  QLabel *summary = nullptr; QTableWidget *coverage = nullptr; QPlainTextEdit *details = nullptr;
  QPushButton *inspect = nullptr, *suggest = nullptr, *apply = nullptr, *create = nullptr, *save = nullptr, *cancel = nullptr;
  QPushButton *loadRequest = nullptr, *saveRequest = nullptr;
  QProcess *process = nullptr; QTimer *timeout = nullptr; std::unique_ptr<QTemporaryDir> scratch;
  const char *failure = nullptr; QString failureDetail;
  bool busy = false, loading = false, cancelled = false, closePending = false, suggestionConfigured = false; quint64 generation = 0;
};
InitializationDialog::InitializationDialog(const QString &python, const QString &backendRoot, const QString &workspaceRoot,
    const QString &datasetPath, const QString &identity, const QJsonObject &initialization,
    const QProcessEnvironment &environment, QWidget *parent) : QDialog(parent), m(std::make_unique<State>()) {
  m->python = python; m->backendRoot = backendRoot; m->workspace = workspaceRoot;
  m->dataset = datasetPath; m->identity = identity; m->environment = environment;
  setObjectName(QStringLiteral("initializationDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("初始化设置与检查"));
  setMinimumSize(560, 360); resize(1100, 760);
  auto *layout = new QVBoxLayout(this);
  // The request/preview/report body can be taller than a narrow screen. Keep
  // every control reachable by scrolling, rather than growing the top-level
  // window beyond the desktop or shrinking the user's font. Actions stay out.
  auto *bodyScroll = new QScrollArea(this);
  bodyScroll->setObjectName(QStringLiteral("initializationBodyScroll"));
  bodyScroll->setWidgetResizable(true);
  bodyScroll->setFrameShape(QFrame::NoFrame);
  bodyScroll->setMinimumSize(0, 0);
  bodyScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  auto *body = new QWidget(bodyScroll);
  body->setObjectName(QStringLiteral("dialogBody"));
  body->setProperty("gswLayoutContainer", true);
  auto *bodyLayout = new QVBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyScroll->setWidget(body);
  layout->addWidget(bodyScroll, 1);
  auto *source = new QLabel(QDir::toNativeSeparators(datasetPath) + QLatin1Char('\n') + identity, body);
  source->setTextFormat(Qt::PlainText); source->setWordWrap(true); bodyLayout->addWidget(source);
  auto *split = new QSplitter(Qt::Horizontal, body); split->setChildrenCollapsible(false); bodyLayout->addWidget(split, 1);
  auto *scroll = new QScrollArea(split); scroll->setWidgetResizable(true);
  m->inputs = new QWidget(scroll); m->inputs->setProperty("gswLayoutContainer", true); scroll->setWidget(m->inputs);
  auto *left = new QVBoxLayout(m->inputs); m->editor = new InitializationEditor(false, m->inputs);
  const QFileInfo input(datasetPath);
  m->editor->setDatasetDirectory(input.isDir() ? input.absoluteFilePath() : input.absolutePath()); left->addWidget(m->editor);
  auto *options = new QFormLayout; options->setRowWrapPolicy(QFormLayout::WrapLongRows); left->addLayout(options);
  const auto row = [&](const char *caption, QWidget *widget) { options->addRow(AppLanguage::text(new QLabel(m->inputs), caption), widget); };
  m->backend = new QComboBox(m->inputs); m->backend->setObjectName(QStringLiteral("initializationBackend"));
  for (const QString &id : {QStringLiteral("3dgs"), QStringLiteral("2dgs")}) m->backend->addItem(id.toUpper(), QVariant(id));
  row(AppLanguage::source("训练后端"), m->backend);
  m->resolution = new QComboBox(m->inputs); m->resolution->setObjectName(QStringLiteral("initializationResolution"));
  for (int r : {1, 2, 4, 8}) m->resolution->addItem(QStringLiteral("1/%1").arg(r), r);
  row(AppLanguage::source("训练分辨率"), m->resolution);
  m->method = new QComboBox(m->inputs); m->method->setObjectName(QStringLiteral("initializationSuggestionMethod"));
  m->method->addItem(QString(), QVariant(QStringLiteral("optical_axis_convergence")));
  m->method->addItem(QString(), QVariant(QStringLiteral("user_depth_interval")));
  AppLanguage::bindComboItem(m->method, 0, AppLanguage::source("光轴会聚建议（不是表面测量）"));
  AppLanguage::bindComboItem(m->method, 1, AppLanguage::source("显式相机 Z 深度区间建议"));
  row(AppLanguage::source("建议方法"), m->method);
  auto *extents = new QWidget(m->inputs); extents->setProperty("gswLayoutContainer", true);
  auto *extentLayout = new QHBoxLayout(extents); extentLayout->setContentsMargins(0, 0, 0, 0);
  for (int i = 0; i < 3; ++i) {
    m->extent[i] = new QLineEdit(extents); m->extent[i]->setObjectName(QStringLiteral("initializationSuggestionHalfExtent%1").arg(i));
    m->extent[i]->setPlaceholderText(QString::fromLatin1("XYZ" + i, 1)); extentLayout->addWidget(m->extent[i]);
  }
  row(AppLanguage::source("声明框架中的半尺寸（可选）"), extents);
  m->depthView = new QComboBox(m->inputs); m->depthView->setEditable(true); m->depthView->setObjectName(QStringLiteral("initializationDepthView"));
  row(AppLanguage::source("训练视图 ID（深度区间）"), m->depthView);
  m->nearDepth = new QLineEdit(m->inputs); m->nearDepth->setObjectName(QStringLiteral("initializationSuggestionNear"));
  m->farDepth = new QLineEdit(m->inputs); m->farDepth->setObjectName(QStringLiteral("initializationSuggestionFar"));
  row(AppLanguage::source("近端 Z 深度（声明框架单位）"), m->nearDepth);
  row(AppLanguage::source("远端 Z 深度（声明框架单位）"), m->farDepth);
  auto *suggestionHint = AppLanguage::text(new QLabel(m->inputs), AppLanguage::source("建议必须明确采用后再检查。光轴会聚可能退化，深度区间来自用户声明；截头视锥的外接范围不是已知对象边界。"));
  suggestionHint->setWordWrap(true); left->addWidget(suggestionHint); left->addStretch(1);
  auto *right = new QWidget(split); auto *rightLayout = new QVBoxLayout(right);
  auto *viewOptions = new QHBoxLayout; rightLayout->addLayout(viewOptions);
  m->projection = new QComboBox(right);
  m->projection->addItem(QString(), QVariant(QStringLiteral("isometric")));
  AppLanguage::bindComboItem(m->projection, 0, AppLanguage::source("三维关系示意"));
  for (const auto &id : {QStringLiteral("xy"), QStringLiteral("xz"), QStringLiteral("yz")}) m->projection->addItem(id.toUpper(), QVariant(id));
  viewOptions->addWidget(m->projection); m->view = new QComboBox(right);
  m->view->setObjectName(QStringLiteral("initializationSelectedView")); viewOptions->addWidget(m->view, 1);
  m->preview = new InitializationPreview(right); rightLayout->addWidget(m->preview, 1);
  auto *legend = AppLanguage::text(new QLabel(right), AppLanguage::source("蓝：训练；橙：验证；紫：测试。相机视锥长度仅为关系示意，不代表物体深度；不读取验证或测试图像。"));
  legend->setWordWrap(true); rightLayout->addWidget(legend);
  m->coverage = new QTableWidget(right); m->coverage->setObjectName(QStringLiteral("initializationCoverageTable"));
  m->coverage->setColumnCount(6); m->coverage->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m->coverage->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  m->coverage->horizontalHeader()->setStretchLastSection(true); m->coverage->setMinimumHeight(130); rightLayout->addWidget(m->coverage);
  split->setStretchFactor(0, 0); split->setStretchFactor(1, 1); split->setSizes({420, 650});
  m->summary = new QLabel(body); m->summary->setObjectName(QStringLiteral("initializationSummary"));
  m->summary->setTextFormat(Qt::PlainText); m->summary->setWordWrap(true); bodyLayout->addWidget(m->summary);
  m->details = new QPlainTextEdit(body); m->details->setObjectName(QStringLiteral("initializationRawReport"));
  m->details->setReadOnly(true); m->details->setMaximumHeight(150); bodyLayout->addWidget(m->details);
  auto *buttons = new QGridLayout; layout->addLayout(buttons); int buttonIndex = 0;
  const auto button = [&](const char *caption, const QString &name) {
    auto *b = AppLanguage::text(new QPushButton(this), caption); b->setObjectName(name);
    buttons->addWidget(b, buttonIndex / 2, buttonIndex % 2); ++buttonIndex; return b;
  };
  m->inspect = button(AppLanguage::source("检查范围与可见性"), QStringLiteral("initializationInspect"));
  m->suggest = button(AppLanguage::source("计算建议（不自动采用）"), QStringLiteral("initializationSuggest"));
  m->apply = button(AppLanguage::source("采用此建议并重新检查"), QStringLiteral("initializationApplySuggestion"));
  m->create = button(AppLanguage::source("创建并采用新初始化版本"), QStringLiteral("initializationCreateVersion"));
  m->loadRequest = button(AppLanguage::source("载入初始化配置…"), QStringLiteral("initializationLoadRequest"));
  m->saveRequest = button(AppLanguage::source("保存初始化配置…"), QStringLiteral("initializationSaveRequest"));
  m->save = button(AppLanguage::source("保存初始化报告…"), QStringLiteral("initializationSaveReport"));
  m->cancel = button(AppLanguage::source("关闭"), QStringLiteral("initializationCancel"));
  connect(m->editor, &InitializationEditor::initializationChanged, this, &InitializationDialog::invalidate);
  for (auto *combo : {m->backend, m->resolution}) connect(combo, &QComboBox::currentIndexChanged, this, &InitializationDialog::invalidate);
  const auto suggestionChanged = [this] { if (!m->loading) m->suggestionConfigured = true; invalidate(); };
  connect(m->method, &QComboBox::currentIndexChanged, this, suggestionChanged);
  for (auto *edit : m->extent) connect(edit, &QLineEdit::textChanged, this, suggestionChanged);
  connect(m->nearDepth, &QLineEdit::textChanged, this, suggestionChanged);
  connect(m->farDepth, &QLineEdit::textChanged, this, suggestionChanged);
  connect(m->depthView, &QComboBox::currentTextChanged, this, suggestionChanged);
  connect(m->projection, &QComboBox::currentIndexChanged, this, [this] { m->preview->setProjection(m->projection->currentData().toString()); });
  connect(m->view, &QComboBox::currentIndexChanged, this, [this] { m->preview->setSelectedView(m->view->currentData().toString()); });
  connect(m->inspect, &QPushButton::clicked, this, &InitializationDialog::startInspection);
  connect(m->suggest, &QPushButton::clicked, this, &InitializationDialog::requestSuggestion);
  connect(m->apply, &QPushButton::clicked, this, &InitializationDialog::applySuggestion);
  connect(m->create, &QPushButton::clicked, this, &InitializationDialog::createVersion);
  connect(m->cancel, &QPushButton::clicked, this, [this] { if (m->busy) cancelOperation(); else reject(); });
  connect(m->loadRequest, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, initText(AppLanguage::source("载入初始化配置…")), m->workspace,
        initText(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")));
    if (!path.isEmpty() && !loadRequest(path)) fail(AppLanguage::source("无法读取或应用初始化配置；原输入保持不变。"));
  });
  connect(m->saveRequest, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, initText(AppLanguage::source("保存初始化配置…")),
        QDir(m->workspace).filePath(QStringLiteral("initialization-request.json")), initText(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")), nullptr, QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty() && !saveRequest(path)) fail(AppLanguage::source("无法另存初始化配置；不会覆盖已有文件。"));
  });
  connect(m->save, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, initText(AppLanguage::source("保存初始化报告…")),
        QDir(m->workspace).filePath(QStringLiteral("initialization-report.json")), initText(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")), nullptr, QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty() && !saveReport(path)) fail(AppLanguage::source("无法另存初始化报告；不会覆盖已有文件。"));
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
    if (error == QProcess::FailedToStart) { m->failure = AppLanguage::source("初始化辅助进程无法启动：%1"); m->failureDetail = m->process->errorString(); finishProcess(2, false); }
  });
  m->timeout = new QTimer(this); m->timeout->setSingleShot(true);
  connect(m->timeout, &QTimer::timeout, this, [this] {
    m->failure = AppLanguage::source("初始化检查超时，正在取消本窗口的辅助进程；训练与原数据未改动。"); cancelOperation();
  });
  AppLanguage::onChanged(this, [this] { refresh(); });
  setRequestDocument({{"schema", "gsw.initialization_request/1"}, {"input_data_identity", identity},
      {"backend", "3dgs"}, {"resolution", 1}, {"initialization", initialization}});
}
InitializationDialog::~InitializationDialog() {
  if (m->process->state() != QProcess::NotRunning) {
    m->process->write("cancel\n"); m->process->waitForBytesWritten(100);
    if (!m->process->waitForFinished(500)) { m->process->kill(); m->process->waitForFinished(1500); }
  }
}
QJsonObject InitializationDialog::requestDocument() const {
  auto value = m->original;
  value.insert(QStringLiteral("schema"), QStringLiteral("gsw.initialization_request/1"));
  value.insert(QStringLiteral("input_data_identity"), m->identity);
  value.insert(QStringLiteral("backend"), m->backend->currentData().toString());
  value.insert(QStringLiteral("resolution"), m->resolution->currentData().toInt());
  value.insert(QStringLiteral("initialization"), m->editor->initialization());
  if (!m->suggestionConfigured) value.remove(QStringLiteral("suggestion"));
  else {
    auto suggestion = m->original.value(QStringLiteral("suggestion")).toObject();
    const QString method = m->method->currentData().toString();
    suggestion.insert(QStringLiteral("method"), method);
    if (method == QStringLiteral("optical_axis_convergence")) {
      suggestion.remove(QStringLiteral("view_id")); suggestion.remove(QStringLiteral("near")); suggestion.remove(QStringLiteral("far"));
      QJsonArray half; bool provided = false;
      for (auto *edit : m->extent) { provided |= !edit->text().trimmed().isEmpty(); half.append(number(edit->text())); }
      if (provided) suggestion.insert(QStringLiteral("half_extents"), half); else suggestion.remove(QStringLiteral("half_extents"));
    } else {
      suggestion.remove(QStringLiteral("view_ids")); suggestion.remove(QStringLiteral("half_extents"));
      for (const auto &entry : {std::pair{QStringLiteral("view_id"), m->depthView->currentText().trimmed()},
                               std::pair{QStringLiteral("near"), m->nearDepth->text().trimmed()},
                               std::pair{QStringLiteral("far"), m->farDepth->text().trimmed()}}) {
        if (entry.second.isEmpty()) suggestion.remove(entry.first);
        else suggestion.insert(entry.first, entry.first == QStringLiteral("view_id") ? QJsonValue(entry.second) : number(entry.second));
      }
    }
    value.insert(QStringLiteral("suggestion"), suggestion);
  }
  return value;
}
bool InitializationDialog::setRequestDocument(const QJsonObject &value) {
  if (m->busy) return false;
  if (!representableBounds(value.value(QStringLiteral("initialization")).toObject().value(QStringLiteral("bounds"))) ||
      !representableSuggestion(value.value(QStringLiteral("suggestion")))) {
    fail(AppLanguage::source("初始化请求包含无法无损编辑的范围或建议参数；原输入保持不变。")); return false;
  }
  if (value.value(QStringLiteral("schema")) != QStringLiteral("gsw.initialization_request/1") ||
      value.value(QStringLiteral("input_data_identity")) != m->identity || !value.value(QStringLiteral("initialization")).isObject() ||
      m->backend->findData(value.value(QStringLiteral("backend")).toString()) < 0 ||
      !value.value(QStringLiteral("resolution")).isDouble() ||
      value.value(QStringLiteral("resolution")).toDouble() != value.value(QStringLiteral("resolution")).toInt() ||
      m->resolution->findData(value.value(QStringLiteral("resolution")).toInt()) < 0 ||
      (value.contains(QStringLiteral("suggestion")) && (!value.value(QStringLiteral("suggestion")).isObject() ||
       m->method->findData(value.value(QStringLiteral("suggestion")).toObject().value(QStringLiteral("method")).toString()) < 0))) {
    fail(AppLanguage::source("初始化请求与当前数据身份、后端或分辨率不匹配；原输入保持不变。")); return false;
  }
  m->loading = true; m->original = value;
  m->backend->setCurrentIndex(m->backend->findData(value.value(QStringLiteral("backend")).toString()));
  m->resolution->setCurrentIndex(m->resolution->findData(value.value(QStringLiteral("resolution")).toInt()));
  m->editor->setInitialization(value.value(QStringLiteral("initialization")).toObject());
  const auto suggestion = value.value(QStringLiteral("suggestion")).toObject();
  m->suggestionConfigured = value.contains(QStringLiteral("suggestion"));
  m->method->setCurrentIndex(m->suggestionConfigured ? m->method->findData(suggestion.value(QStringLiteral("method")).toString()) : 0);
  const auto half = suggestion.value(QStringLiteral("half_extents")).toArray();
  const auto display = [](const QJsonValue &v) { return v.isDouble() ? QString::number(v.toDouble(), 'g', 17) : v.toString(); };
  for (int i = 0; i < 3; ++i) m->extent[i]->setText(display(i < half.size() ? half[i] : QJsonValue()));
  m->depthView->setEditText(suggestion.value(QStringLiteral("view_id")).toString());
  m->nearDepth->setText(display(suggestion.value(QStringLiteral("near")))); m->farDepth->setText(display(suggestion.value(QStringLiteral("far"))));
  m->loading = false; invalidate(); return true;
}
QJsonObject InitializationDialog::report() const { return m->report; }
bool InitializationDialog::isRunning() const { return m->busy; }
bool InitializationDialog::loadRequest(const QString &path) { const auto value = readObject(path); return !value.isEmpty() && setRequestDocument(value); }
bool InitializationDialog::saveRequest(const QString &path) { return writeNew(path, QJsonDocument(requestDocument()).toJson(QJsonDocument::Indented)); }
bool InitializationDialog::saveReport(const QString &path) { return !m->report.isEmpty() && writeNew(path, QJsonDocument(m->report).toJson(QJsonDocument::Indented)); }
void InitializationDialog::invalidate() {
  if (m->loading) return;
  m->report = {}; m->versionPath.clear(); m->failure = nullptr; m->failureDetail.clear();
  m->coverage->setRowCount(0); m->preview->setReport({}); refresh();
}
void InitializationDialog::fail(const char *source, const QString &detail) { m->failure = source; m->failureDetail = detail; refresh(); }
void InitializationDialog::startInspection() { startOperation(QStringLiteral("inspect")); }
void InitializationDialog::requestSuggestion() { startOperation(QStringLiteral("suggest")); }
void InitializationDialog::applySuggestion() {
  if (m->busy || m->report.value(QStringLiteral("status")) != QStringLiteral("suggestion_requires_confirmation")) return;
  const auto suggested = m->report.value(QStringLiteral("suggested_initialization")).toObject();
  if (suggested.isEmpty()) return;
  m->suggestionConfigured = false; m->original.remove(QStringLiteral("suggestion"));
  m->editor->setInitialization(suggested); invalidate(); startInspection();
}
void InitializationDialog::createVersion() {
  if (m->busy || m->report.value(QStringLiteral("status")) != QStringLiteral("ready") || !m->versionPath.isEmpty()) return;
  startOperation(QStringLiteral("create-version"));
}
void InitializationDialog::startOperation(const QString &operation) {
  if (m->busy) return;
  const QString invalid = m->editor->validationError();
  if (!invalid.isEmpty()) return fail(AppLanguage::source("初始化声明尚未通过输入检查：%1"), invalid);
  const QString script = QDir(m->backendRoot).filePath(QStringLiteral("native/worker/initialization.py"));
  if (!QFileInfo(m->python).isFile() || !QFileInfo(script).isFile())
    return fail(AppLanguage::source("初始化工具不可用；不会自动安装运行库、重求相机或开始训练。"));
  const QString workspace = QFileInfo(m->workspace).canonicalFilePath();
  if (workspace.isEmpty()) return fail(AppLanguage::source("无法创建工程内受控初始化目录。"));
  QString managed = workspace;
  for (const auto &part : {QStringLiteral(".gsw"), QStringLiteral("i")}) {
    const QString next = QDir(managed).filePath(part); QFileInfo info(next);
    if (!info.exists() && !QDir(managed).mkdir(part)) return fail(AppLanguage::source("无法创建工程内受控初始化目录。"));
    info.refresh(); const QString real = info.canonicalFilePath();
    if (!info.isDir() || info.isSymLink() || real.isEmpty() || !real.startsWith(workspace + QLatin1Char('/'), Qt::CaseInsensitive))
      return fail(AppLanguage::source("无法创建工程内受控初始化目录。"));
    managed = real;
  }
  if (operation == QStringLiteral("suggest")) m->suggestionConfigured = true;
  auto request = requestDocument();
  m->scratch = std::make_unique<QTemporaryDir>(QDir(managed).filePath(QStringLiteral("request-XXXXXX")));
  if (!m->scratch->isValid()) return fail(AppLanguage::source("无法创建工程内受控初始化目录。"));
  const QString requestPath = m->scratch->filePath(QStringLiteral("request.json"));
  if (!writeNew(requestPath, QJsonDocument(request).toJson())) return fail(AppLanguage::source("无法写入受控初始化请求；原数据未改动。"));
  m->reportPath = m->scratch->filePath(QStringLiteral("report.json"));
  m->outputPath = operation == QStringLiteral("create-version")
      ? QDir(managed).filePath(QStringLiteral("v") + QUuid::createUuid().toString(QUuid::Id128)) : QString();
  QStringList arguments{QStringLiteral("-B"), script, QStringLiteral("--operation"), operation,
      QStringLiteral("--dataset"), m->dataset, QStringLiteral("--request"), requestPath, QStringLiteral("--report"), m->reportPath,
      QStringLiteral("--expected-identity"), m->identity};
  if (!m->outputPath.isEmpty()) arguments << QStringLiteral("--output-directory") << m->outputPath;
  m->operation = operation; m->runRequest = request; m->report = {}; m->output.clear(); m->errors.clear();
  m->coverage->setRowCount(0); m->preview->setReport({});
  m->failure = nullptr; m->failureDetail.clear(); m->cancelled = false; m->busy = true; ++m->generation;
  auto env = m->environment; env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  env.insert(QStringLiteral("TEMP"), m->scratch->path()); env.insert(QStringLiteral("TMP"), m->scratch->path());
  m->process->setProcessEnvironment(env); m->process->setWorkingDirectory(m->backendRoot);
  m->process->setProgram(m->python); m->process->setArguments(arguments);
  refresh(); m->timeout->start(operation == QStringLiteral("create-version") ? 600000 : 180000); m->process->start();
}
void InitializationDialog::readOutput(const bool standardError) {
  m->process->setReadChannel(standardError ? QProcess::StandardError : QProcess::StandardOutput);
  auto &output = standardError ? m->errors : m->output;
  const qint64 remaining = maximumBytes - m->output.size() - m->errors.size();
  if (remaining >= 0) output += m->process->read(remaining + 1);
  if (m->output.size() + m->errors.size() > maximumBytes) {
    m->failure = AppLanguage::source("初始化辅助进程输出超出限制，正在取消；未采用任何结果。");
    m->process->closeReadChannel(QProcess::StandardOutput); m->process->closeReadChannel(QProcess::StandardError); cancelOperation();
  }
}
void InitializationDialog::finishProcess(const int exitCode, const bool normalExit) {
  if (!m->busy) return;
  m->timeout->stop(); readOutput(false); readOutput(true); m->busy = false; QJsonObject response;
  for (const auto &output : {m->output, m->errors}) {
    const auto lines = output.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
      const auto object = QJsonDocument::fromJson(it->trimmed()).object();
      if (object.contains(QStringLiteral("success"))) { response = object; break; }
    }
    if (!response.isEmpty()) break;
  }
  if (!m->cancelled && !m->failure) {
    const QString returned = response.value(QStringLiteral("reportPath")).toString();
    const bool same = !returned.isEmpty() && QFileInfo(returned).canonicalFilePath() == QFileInfo(m->reportPath).canonicalFilePath();
    const auto result = same ? readObject(m->reportPath) : QJsonObject{};
    const QString status = result.value(QStringLiteral("status")).toString();
    const bool valid = result.value(QStringLiteral("schema")) == QStringLiteral("gsw.initialization_report/1") &&
        result.value(QStringLiteral("input_data_identity")) == m->identity && result.value(QStringLiteral("request")) == m->runRequest &&
        result.value(QStringLiteral("backend")) == m->runRequest.value(QStringLiteral("backend")) &&
        result.value(QStringLiteral("resolution")) == m->runRequest.value(QStringLiteral("resolution")) &&
        (status == QStringLiteral("ready") || status == QStringLiteral("invalid") || status == QStringLiteral("needs_input") ||
         status == QStringLiteral("suggestion_requires_confirmation"));
    if (normalExit && exitCode == 0 && response.value(QStringLiteral("success")).toBool() && valid) {
      m->report = result; m->preview->setReport(result); m->coverage->setRowCount(0);
      for (const auto &entry : result.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("per_view")).toArray()) {
        if (m->coverage->rowCount() >= 4096) break;
        const auto item = entry.toObject(); const int row = m->coverage->rowCount(); m->coverage->insertRow(row); int column = 0;
        for (const auto &key : {QStringLiteral("view_id"), QStringLiteral("initial_points"), QStringLiteral("positive_depth"),
            QStringLiteral("forward_past_backend_near"), QStringLiteral("inside_image")}) {
          auto *cell = new QTableWidgetItem(item.value(key).isDouble() ? QString::number(item.value(key).toInteger()) : item.value(key).toString());
          cell->setFlags(cell->flags() & ~Qt::ItemIsEditable); m->coverage->setItem(row, column++, cell);
        }
        const double total = item.value(QStringLiteral("initial_points")).toDouble();
        m->coverage->setItem(row, 5, new QTableWidgetItem(total > 0
            ? QLocale().toString(100. * item.value(QStringLiteral("inside_image")).toDouble() / total, 'f', 2) + QStringLiteral(" %")
            : initText(AppLanguage::source("未提供"))));
      }
      {
        const QSignalBlocker viewBlocked(m->view), depthBlocked(m->depthView);
        const QString depth = m->depthView->currentText(); m->view->clear();
        m->view->addItem(QString(), QVariant(QString())); AppLanguage::bindComboItem(m->view, 0, AppLanguage::source("全部相机"));
        m->depthView->clear();
        for (const auto &entry : result.value(QStringLiteral("preview")).toObject().value(QStringLiteral("views")).toArray()) {
          const auto view = entry.toObject(); const QString id = view.value(QStringLiteral("view_id")).toString();
          m->view->addItem(id, QVariant(id));
          if (view.value(QStringLiteral("split")) == QStringLiteral("train")) m->depthView->addItem(id, QVariant(id));
        }
        if (!depth.isEmpty()) m->depthView->setEditText(depth);
      }
      if (m->operation == QStringLiteral("create-version")) {
        const QString path = QFileInfo(response.value(QStringLiteral("datasetPath")).toString()).canonicalFilePath();
        const QString expected = QFileInfo(m->outputPath).canonicalFilePath();
        const auto summary = response.value(QStringLiteral("summary")).toObject();
        if (status != QStringLiteral("ready") || path.isEmpty() || path != expected ||
            summary.value(QStringLiteral("blocked")).toBool(true) || summary.value(QStringLiteral("posePolicy")) != QStringLiteral("locked") ||
            summary.value(QStringLiteral("dataIdentity")).toString().size() != 64)
          fail(AppLanguage::source("新初始化版本未通过身份和锁定策略核验；未采用任何数据集。"));
        else { m->versionPath = path; emit versionCreated(path, summary); }
      }
    } else fail(AppLanguage::source("初始化操作未返回有效的绑定报告。技术详情：%1"),
        response.value(QStringLiteral("code")).toString() + QLatin1Char('\n') + response.value(QStringLiteral("error")).toString() +
        QLatin1Char('\n') + QString::fromUtf8(m->errors.left(8192)));
  }
  if (m->cancelled) m->report = {};
  m->scratch.reset(); refresh(); emit inspectionFinished(m->report);
  if (m->closePending) QDialog::reject();
}
void InitializationDialog::cancelOperation() {
  if (!m->busy || m->cancelled) return;
  m->cancelled = true; m->report = {}; const quint64 generation = m->generation; m->process->write("cancel\n");
  QTimer::singleShot(5000, this, [this, generation] { if (m->busy && m->generation == generation && m->process->state() != QProcess::NotRunning) m->process->terminate(); });
  QTimer::singleShot(7000, this, [this, generation] { if (m->busy && m->generation == generation && m->process->state() != QProcess::NotRunning) m->process->kill(); });
  refresh();
}
void InitializationDialog::reject() { if (m->busy) { m->closePending = true; cancelOperation(); return; } QDialog::reject(); }
void InitializationDialog::closeEvent(QCloseEvent *event) { if (m->busy) { m->closePending = true; cancelOperation(); event->ignore(); } else QDialog::closeEvent(event); }
void InitializationDialog::refresh() {
  m->inputs->setEnabled(!m->busy); m->inspect->setEnabled(!m->busy); m->suggest->setEnabled(!m->busy);
  m->loadRequest->setEnabled(!m->busy); m->saveRequest->setEnabled(!m->busy);
  const QString status = m->report.value(QStringLiteral("status")).toString();
  m->apply->setEnabled(!m->busy && status == QStringLiteral("suggestion_requires_confirmation") && m->report.value(QStringLiteral("suggested_initialization")).isObject());
  m->create->setEnabled(!m->busy && !m->failure && status == QStringLiteral("ready") && m->versionPath.isEmpty());
  m->save->setEnabled(!m->busy && !m->report.isEmpty());
  m->cancel->setText(initText(m->busy ? AppLanguage::source("取消初始化操作") : AppLanguage::source("关闭")));
  m->coverage->setHorizontalHeaderLabels({initText(AppLanguage::source("视图 ID")), initText(AppLanguage::source("初始化点数")),
      initText(AppLanguage::source("正深度点数")), initText(AppLanguage::source("通过后端近裁剪")),
      initText(AppLanguage::source("图像内点数")), initText(AppLanguage::source("图像内比例"))});
  QString summary;
  if (m->busy) summary = initText(m->cancelled ? AppLanguage::source("正在取消本窗口的初始化辅助进程；原数据和训练任务不受影响。")
      : AppLanguage::source("正在后台检查实际初始化与相机关系；不会开始训练或重求解相机。"));
  else if (m->failure) { summary = initText(m->failure); if (summary.contains(QStringLiteral("%1"))) summary = summary.arg(m->failureDetail); }
  else if (m->cancelled) summary = initText(AppLanguage::source("初始化操作已取消；未切换数据集。"));
  else if (m->report.isEmpty()) summary = initText(AppLanguage::source("尚未检查初始化。请明确范围、框架和先验；仅有相机位置不能确定物体深度。"));
  else {
    summary = status == QStringLiteral("ready") ? initText(AppLanguage::source("初始化几何可见性检查通过；这不证明包含真实表面，也不是模型精度。"))
        : status == QStringLiteral("suggestion_requires_confirmation") ? initText(AppLanguage::source("建议尚未采用；请检查假设并明确确认，然后重新检查。"))
        : status == QStringLiteral("needs_input") ? initText(AppLanguage::source("初始化需要补充输入；未猜测对象深度或范围。"))
        : initText(AppLanguage::source("初始化未通过实际后端可见性检查；请调整声明。"));
    const auto frames = m->report.value(QStringLiteral("frames")).toObject();
    summary += QLatin1Char('\n') + initText(AppLanguage::source("实际点数：%1；源单位：%2；规范单位：%3。尺度来源及状态见原始报告，不推断物理精度。"))
        .arg(QLocale().toString(m->report.value(QStringLiteral("point_count")).toInteger()),
             frames.value(QStringLiteral("source_unit")).toString(), frames.value(QStringLiteral("canonical_unit")).toString());
    for (const auto &item : m->report.value(QStringLiteral("assumptions")).toArray()) summary += QLatin1Char('\n') + diagnosis(item.toString());
    for (const auto &item : m->report.value(QStringLiteral("errors")).toArray()) summary += QLatin1Char('\n') + diagnosis(item.toString());
    for (const auto &item : m->report.value(QStringLiteral("warnings")).toArray())
      summary += QLatin1Char('\n') + diagnosis(item.isObject() ? item.toObject().value(QStringLiteral("code")).toString() : item.toString());
    if (m->coverage->rowCount() >= 4096) summary += QLatin1Char('\n') + initText(AppLanguage::source("表格最多显示 4096 个视图；完整逐视图统计保留在原始报告。"));
    if (!m->versionPath.isEmpty()) summary += QLatin1Char('\n') + initText(AppLanguage::source("已创建独立初始化版本：%1；旧数据和检查点保持不变。")).arg(QDir::toNativeSeparators(m->versionPath));
  }
  m->summary->setText(summary);
  const QString raw = m->report.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(m->report).toJson(QJsonDocument::Indented));
  if (m->details->toPlainText() != raw) m->details->setPlainText(raw);
}
}
