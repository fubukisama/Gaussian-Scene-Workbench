#include "QualityComparisonDialog.h"
#include "QualityMeasurementDialog.h"
#include "AppLanguage.h"

#include <QCheckBox>
#include <QCloseEvent>
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
#include <QLineEdit>
#include <QLocale>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {
constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
const std::array<QString, 3> LimitKeys{QStringLiteral("max_regression"),
    QStringLiteral("absolute_min"), QStringLiteral("absolute_max")};
QString text(const char *source) { return QCoreApplication::translate("Workbench", source); }
QString statusText(const QString &status) {
  if (status == QStringLiteral("pass")) return text(AppLanguage::source("按所选门限通过"));
  if (status == QStringLiteral("regressed")) return text(AppLanguage::source("超出质量门限"));
  if (status == QStringLiteral("not_comparable")) return text(AppLanguage::source("不具可比性"));
  return text(AppLanguage::source("未验证"));
}
QString jsonText(const QJsonValue &value) {
  const QByteArray encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}
QString valueText(const QJsonValue &value, const QString &kind = {}) {
  if (kind == QStringLiteral("positive_infinity") ||
      (value.isObject() && value.toObject().value(QStringLiteral("special")) == QStringLiteral("positive_infinity")))
    return QString::fromUtf8("+∞");
  if (kind == QStringLiteral("negative_infinity")) return QString::fromUtf8("−∞");
  if (value.isNull() || value.isUndefined()) return text(AppLanguage::source("未提供"));
  if (value.isDouble()) return QLocale().toString(value.toDouble(), 'g', 12);
  return jsonText(value);
}
QString reasonText(const QString &reason) {
  if (reason == QStringLiteral("below_absolute_min")) return text(AppLanguage::source("低于绝对下限"));
  if (reason == QStringLiteral("above_absolute_max")) return text(AppLanguage::source("高于绝对上限"));
  if (reason == QStringLiteral("exceeds_max_regression")) return text(AppLanguage::source("超出允许退化"));
  if (reason == QStringLiteral("missing_measurement")) return text(AppLanguage::source("缺少实测结果"));
  if (reason == QStringLiteral("user_threshold_missing")) return text(AppLanguage::source("未设置质量门限"));
  if (reason == QStringLiteral("independent_validation_missing")) return text(AppLanguage::source("缺少独立验证"));
  if (reason == QStringLiteral("measurement_identity_mismatch")) return text(AppLanguage::source("评估条件不一致"));
  return text(AppLanguage::source("未识别的原因（原始标识）：%1")).arg(reason);
}
void configureTable(QTableWidget *table) {
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setWordWrap(true);
  table->setMinimumHeight(140);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}
void put(QTableWidget *table, int row, int column, const QString &value) {
  const QString preview = value.size() > 1024 ? value.left(1024) + QString::fromUtf8("…") : value;
  auto *item = new QTableWidgetItem(preview);
  item->setToolTip(value.left(8192));
  table->setItem(row, column, item);
}
} // namespace

QualityComparisonDialog::QualityComparisonDialog(const QString &python, const QString &backendRoot,
    const QString &workspaceRoot, const QProcessEnvironment &environment, QWidget *parent)
    : QDialog(parent), mPython(python), mBackendRoot(backendRoot),
      mWorkspaceRoot(workspaceRoot), mEnvironment(environment) {
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("质量对照"));
  setMinimumSize(640, 360);
  resize(1000, 760);
  auto *root = new QVBoxLayout(this);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *body = new QWidget(scroll);
  auto *layout = new QVBoxLayout(body);
  auto *notice = AppLanguage::text(new QLabel(body), AppLanguage::source(
      "只比较已提供的 JSON 实测结果，不启动训练、渲染或评估。未设置门限、缺少数据或只有训练拟合结果时，不会显示质量通过。几何数值按报告的指标协议解释，不自动猜测或换算单位。"));
  notice->setWordWrap(true);
  notice->setTextFormat(Qt::PlainText);
  layout->addWidget(notice);
  mInputs = new QWidget(body);
  auto *inputLayout = new QVBoxLayout(mInputs);
  inputLayout->setContentsMargins(0, 0, 0, 0);
  auto *paths = new QFormLayout;
  paths->setRowWrapPolicy(QFormLayout::WrapLongRows);
  const auto addPath = [&](const char *caption, const QString &name, QLineEdit **field) {
    auto *row = new QWidget(mInputs);
    row->setProperty("gswLayoutContainer", true);
    auto *horizontal = new QHBoxLayout(row);
    horizontal->setContentsMargins(0, 0, 0, 0);
    *field = new QLineEdit(row);
    (*field)->setObjectName(name);
    auto *browse = AppLanguage::text(new QPushButton(row), AppLanguage::source("浏览..."));
    horizontal->addWidget(*field, 1);
    horizontal->addWidget(browse);
    paths->addRow(text(caption), row);
    AppLanguage::text(qobject_cast<QLabel *>(paths->labelForField(row)), caption);
    connect(browse, &QPushButton::clicked, this, [this, target = *field] { chooseReport(target); });
  };
  addPath(AppLanguage::source("基准实测报告"), QStringLiteral("qualityBaselinePath"), &mBaseline);
  addPath(AppLanguage::source("候选实测报告"), QStringLiteral("qualityCandidatePath"), &mCandidate);
  inputLayout->addLayout(paths);
  auto *domains = new QHBoxLayout;
  mImageRequired = AppLanguage::text(new QCheckBox(mInputs), AppLanguage::source("要求图像质量证据"));
  mGeometryRequired = AppLanguage::text(new QCheckBox(mInputs), AppLanguage::source("同时要求几何质量证据"));
  mImageRequired->setChecked(true);
  domains->addWidget(mImageRequired);
  domains->addWidget(mGeometryRequired);
  domains->addStretch();
  inputLayout->addLayout(domains);
  mMetrics = {{{QStringLiteral("psnr"), AppLanguage::source("PSNR"), {}},
      {QStringLiteral("ssim"), AppLanguage::source("SSIM"), {}},
      {QStringLiteral("lpips"), AppLanguage::source("LPIPS"), {}},
      {QStringLiteral("depth_rmse"), AppLanguage::source("深度 RMSE"), {}},
      {QStringLiteral("normal_mae"), AppLanguage::source("法线 MAE"), {}},
      {QStringLiteral("mesh_distance"), AppLanguage::source("网格距离"), {}}}};
  mLimits = new QTableWidget(int(mMetrics.size()), 4, mInputs);
  mLimits->setObjectName(QStringLiteral("qualityContractTable"));
  AppLanguage::bind(mLimits, "toolTip", AppLanguage::source(
      "勾选后才启用门限；PSNR/SSIM 下降与 LPIPS/几何误差上升计为退化。数字使用句点，支持科学计数法。"));
  configureTable(mLimits);
  mLimits->setMinimumHeight(230);
  const auto edited = [this] {
    if (mBusy) return;
    mReport = {};
    mFailure = nullptr;
    mCancelled = false;
    mSaveMessage = nullptr;
    mProtocolIdentifier.clear();
    refreshEnabled();
    refreshPresentation();
  };
  for (int row = 0; row < int(mMetrics.size()); ++row) {
    for (int column = 0; column < 3; ++column) {
      auto &limit = mMetrics[row].limits[column];
      auto *cell = new QWidget(mLimits);
      cell->setProperty("gswLayoutContainer", true);
      auto *horizontal = new QHBoxLayout(cell);
      horizontal->setContentsMargins(4, 1, 4, 1);
      limit.enabled = new QCheckBox(cell);
      limit.enabled->setObjectName(QStringLiteral("quality_%1_%2_enabled").arg(mMetrics[row].identifier, LimitKeys[column]));
      limit.value = new QLineEdit(QStringLiteral("0"), cell);
      limit.value->setObjectName(QStringLiteral("quality_%1_%2").arg(mMetrics[row].identifier, LimitKeys[column]));
      limit.value->setMinimumWidth(55);
      horizontal->addWidget(limit.enabled);
      horizontal->addWidget(limit.value, 1);
      mLimits->setCellWidget(row, column + 1, cell);
      connect(limit.enabled, &QCheckBox::toggled, this, edited);
      connect(limit.value, &QLineEdit::textChanged, this, edited);
    }
  }
  connect(mBaseline, &QLineEdit::textChanged, this, edited);
  connect(mCandidate, &QLineEdit::textChanged, this, edited);
  connect(mImageRequired, &QCheckBox::toggled, this, edited);
  connect(mGeometryRequired, &QCheckBox::toggled, this, edited);
  inputLayout->addWidget(mLimits);
  layout->addWidget(mInputs);
  mSummary = new QLabel(body);
  mSummary->setObjectName(QStringLiteral("qualityComparisonSummary"));
  mSummary->setTextFormat(Qt::PlainText);
  mSummary->setWordWrap(true);
  mSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(mSummary);
  mProtocol = new QLabel(body);
  mProtocol->setTextFormat(Qt::PlainText);
  mProtocol->setWordWrap(true);
  layout->addWidget(mProtocol);
  mResults = new QTableWidget(0, 7, body);
  mResults->setObjectName(QStringLiteral("qualityMetricResults"));
  configureTable(mResults);
  layout->addWidget(mResults);
  mChanges = new QTableWidget(0, 3, body);
  mChanges->setObjectName(QStringLiteral("qualitySettingsChanges"));
  configureTable(mChanges);
  layout->addWidget(mChanges);
  scroll->setWidget(body);
  root->addWidget(scroll, 1);
  auto *buttons = new QHBoxLayout;
  mMeasure = AppLanguage::text(new QPushButton(this), AppLanguage::source("图像实测…"));
  mMeasure->setObjectName(QStringLiteral("qualityMeasureImagesButton"));
  AppLanguage::bind(mMeasure, "toolTip", AppLanguage::source("主动测量已有参考图与渲染图；不启动训练，完成后由你选择是否用作基准或候选。"));
  buttons->addWidget(mMeasure);
  buttons->addStretch();
  mCompare = AppLanguage::text(new QPushButton(this), AppLanguage::source("开始比较"));
  mCompare->setObjectName(QStringLiteral("qualityCompareButton"));
  mSave = AppLanguage::text(new QPushButton(this), AppLanguage::source("保存对照报告…"));
  mSave->setObjectName(QStringLiteral("qualitySaveButton"));
  mCancel = new QPushButton(this);
  mCancel->setObjectName(QStringLiteral("qualityCancelButton"));
  buttons->addWidget(mCompare);
  buttons->addWidget(mSave);
  buttons->addWidget(mCancel);
  root->addLayout(buttons);
  connect(mCompare, &QPushButton::clicked, this, &QualityComparisonDialog::startComparison);
  connect(mMeasure, &QPushButton::clicked, this, &QualityComparisonDialog::showMeasurement);
  connect(mSave, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getSaveFileName(this,
        text(AppLanguage::source("保存对照报告…")),
        QDir(mWorkspaceRoot).filePath(QStringLiteral("quality-comparison.json")),
        text(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty()) saveComparisonReport(path);
  });
  connect(mCancel, &QPushButton::clicked, this, [this] { if (mBusy) cancelComparison(); else reject(); });
  mProcess = new QProcess(this);
#ifdef Q_OS_WIN
  mProcess->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
    arguments->flags |= CREATE_NO_WINDOW;
    arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
    arguments->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  connect(mProcess, &QProcess::started, this, [this] { if (mCancelled) mProcess->kill(); });
  connect(mProcess, &QProcess::readyReadStandardOutput, this, [this] { readOutput(false); });
  connect(mProcess, &QProcess::readyReadStandardError, this, [this] { readOutput(true); });
  connect(mProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
    finishProcess(code, status == QProcess::NormalExit);
  });
  connect(mProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      mFailure = AppLanguage::source("无法启动质量对照：%1");
      mFailureDetail = mProcess->errorString();
      finishProcess(2, false);
    }
  });
  mTimeout = new QTimer(this);
  mTimeout->setSingleShot(true);
  connect(mTimeout, &QTimer::timeout, this, [this] {
    mFailure = AppLanguage::source("质量对照超时，已停止比较辅助进程。");
    mProcess->kill();
  });
  AppLanguage::onChanged(this, [this] { refreshPresentation(); });
  refreshEnabled();
  refreshPresentation();
}

QualityComparisonDialog::~QualityComparisonDialog() {
  if (mProcess->state() != QProcess::NotRunning) {
    mProcess->kill();
    mProcess->waitForFinished(1000); // Last-resort parent destruction, not normal UI close.
  }
}
void QualityComparisonDialog::setReportPaths(const QString &baseline, const QString &candidate) {
  if (mBusy) return;
  mBaseline->setText(baseline);
  mCandidate->setText(candidate);
}
void QualityComparisonDialog::setContract(const QJsonObject &contract) {
  if (mBusy) return;
  const auto domains = contract.value(QStringLiteral("required_domains")).toArray();
  mImageRequired->setChecked(domains.contains(QStringLiteral("image")));
  mGeometryRequired->setChecked(domains.contains(QStringLiteral("geometry")));
  const auto metrics = contract.value(QStringLiteral("metrics")).toObject();
  for (auto &metric : mMetrics) {
    const auto limits = metrics.value(metric.identifier).toObject();
    for (int column = 0; column < 3; ++column) {
      auto &limit = metric.limits[column];
      const auto value = limits.value(LimitKeys[column]);
      limit.enabled->setChecked(value.isDouble());
      if (value.isDouble()) limit.value->setText(jsonText(value));
    }
  }
  mReport = {};
  mFailure = nullptr;
  mCancelled = false;
  mSaveMessage = nullptr;
  mProtocolIdentifier.clear();
  refreshEnabled();
  refreshPresentation();
}
QJsonObject QualityComparisonDialog::buildContract(bool *valid) const {
  *valid = true;
  QLocale numericLocale = QLocale::c();
  numericLocale.setNumberOptions(QLocale::RejectGroupSeparator);
  QJsonArray domains;
  if (mImageRequired->isChecked()) domains.append(QStringLiteral("image"));
  if (mGeometryRequired->isChecked()) domains.append(QStringLiteral("geometry"));
  QJsonObject metrics;
  for (const auto &metric : mMetrics) {
    QJsonObject limits;
    for (int column = 0; column < 3; ++column) {
      const auto &limit = metric.limits[column];
      if (!limit.enabled->isChecked()) continue;
      bool converted = false;
      const double value = numericLocale.toDouble(limit.value->text(), &converted);
      *valid = *valid && converted && std::isfinite(value);
      limits[LimitKeys[column]] = value;
    }
    if (!limits.isEmpty()) metrics[metric.identifier] = limits;
  }
  return {{QStringLiteral("version"), 1}, {QStringLiteral("required_domains"), domains},
          {QStringLiteral("metrics"), metrics}};
}
QJsonObject QualityComparisonDialog::contract() const { bool valid; return buildContract(&valid); }
QJsonObject QualityComparisonDialog::comparisonReport() const { return mReport; }
bool QualityComparisonDialog::isRunning() const { return mBusy; }
bool QualityComparisonDialog::saveComparisonReport(const QString &path) {
  const auto failed = [&](const QString &detail) {
    mSaveMessage = AppLanguage::source("无法保存对照报告：%1。已有文件与输入报告均未覆盖。");
    mSaveDetail = detail;
    refreshPresentation();
    return false;
  };
  if (mBusy || mReport.isEmpty()) {
    mSaveMessage = AppLanguage::source("尚无可保存的有效对照报告。");
    mSaveDetail.clear();
    refreshPresentation();
    return false;
  }
  const QFileInfo destination(path);
  const QString absolute = QDir::cleanPath(destination.absoluteFilePath());
  for (const QString &input : {mBaseline->text(), mCandidate->text()}) {
    if (absolute.compare(QDir::cleanPath(QFileInfo(input).absoluteFilePath()), Qt::CaseInsensitive) == 0)
      return failed(path);
  }
  if (path.isEmpty() || destination.exists() || destination.isSymLink()) return failed(path);
  bool valid = false;
  const QJsonObject selectedContract = buildContract(&valid);
  if (!valid) return failed(path);
  const QByteArray contents = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
      {QStringLiteral("comparison"), mReport}, {QStringLiteral("contract"), selectedContract}})
          .toJson(QJsonDocument::Compact);
  if (contents.size() > MaximumBytes) return failed(QStringLiteral("16 MiB"));
  QFile output(path);
  if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return failed(output.errorString());
  if (output.write(contents) != contents.size() || !output.flush()) {
    const QString error = output.errorString();
    output.close();
    output.remove(); // Only the fresh file exclusively created above belongs to this call.
    return failed(error);
  }
  output.close();
  mSaveMessage = AppLanguage::source("对照报告已保存：%1");
  mSaveDetail = QDir::toNativeSeparators(path);
  refreshPresentation();
  return true;
}
void QualityComparisonDialog::refreshEnabled() {
  mInputs->setEnabled(!mBusy);
  mCompare->setEnabled(!mBusy);
  mMeasure->setEnabled(!mBusy);
  mSave->setEnabled(!mBusy && !mReport.isEmpty());
  for (const auto &metric : mMetrics)
    for (const auto &limit : metric.limits) limit.value->setEnabled(limit.enabled->isChecked());
}
void QualityComparisonDialog::showMeasurement() {
  if (mBusy || mMeasurementDialog) return; // Do not raise an existing window.
  mMeasurementDialog = new QualityMeasurementDialog(mPython, mBackendRoot, mWorkspaceRoot, mEnvironment, this);
  mMeasurementDialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(mMeasurementDialog, &QualityMeasurementDialog::reportChosen, this,
      [this](const QString &path, const bool useAsBaseline) {
    if (!mBusy) (useAsBaseline ? mBaseline : mCandidate)->setText(QDir::toNativeSeparators(path));
  });
  connect(mMeasurementDialog, &QualityMeasurementDialog::measurementFinished, this, [this] {
    if (mClosePending && !mBusy) QTimer::singleShot(0, this, [this] { QDialog::reject(); });
  });
  mMeasurementDialog->show(); // Only the explicit measurement action opens this dialog.
}
void QualityComparisonDialog::chooseReport(QLineEdit *target) {
  const QString path = QFileDialog::getOpenFileName(this, text(AppLanguage::source("选择质量实测报告")),
      target->text(), text(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")));
  if (!path.isEmpty()) target->setText(QDir::toNativeSeparators(path));
}
void QualityComparisonDialog::fail(const char *message, const QString &detail) {
  mFailure = message;
  mFailureDetail = detail;
  mBusy = false;
  mReport = {};
  mScratch.reset();
  refreshEnabled();
  refreshPresentation();
  emit comparisonFinished(mReport);
}
void QualityComparisonDialog::startComparison() {
  if (mBusy) return;
  mReport = {};
  mFailure = nullptr;
  mFailureDetail.clear();
  mCancelled = false;
  mSaveMessage = nullptr;
  mProtocolIdentifier.clear();
  bool valid = false;
  const QJsonObject selectedContract = buildContract(&valid);
  if (!valid) return fail(AppLanguage::source("质量门限必须是有限数字；小数使用句点。"));
  const QString script = QDir(mBackendRoot).filePath(QStringLiteral("scripts/compare_training_quality.py"));
  if (!QFileInfo(mPython).isFile() || !QFileInfo(script).isFile())
    return fail(AppLanguage::source("质量对照工具不可用，请检查 Python 和已安装的比较脚本；不会自动安装。"));
  const QString workspace = QFileInfo(mWorkspaceRoot).canonicalFilePath();
  if (workspace.isEmpty())
    return fail(AppLanguage::source("无法在当前工作区创建质量对照临时目录。"));
  QString scratchRoot = workspace;
  for (const QString &part : {QStringLiteral(".gsw"), QStringLiteral("jobs")}) {
    const QString next = QDir(scratchRoot).filePath(part);
    QFileInfo location(next);
    // Resolve an existing junction before creating any child beneath it. New
    // levels are created one at a time from the already-verified real parent.
    if (!location.exists() && !location.isSymLink()) {
      if (!QDir(scratchRoot).mkdir(part))
        return fail(AppLanguage::source("无法在当前工作区创建质量对照临时目录。"));
      location.refresh();
    }
    const QString canonical = location.canonicalFilePath();
    if (!location.isDir() || canonical.isEmpty() ||
        !canonical.startsWith(workspace + QLatin1Char('/'), Qt::CaseInsensitive))
      return fail(AppLanguage::source("无法在当前工作区创建质量对照临时目录。"));
    scratchRoot = canonical;
  }
  mScratch = std::make_unique<QTemporaryDir>(QDir(scratchRoot).filePath(QStringLiteral("quality-XXXXXX")));
  if (!mScratch->isValid()) return fail(AppLanguage::source("无法在当前工作区创建质量对照临时目录。"));
  const auto snapshot = [&](const QString &source, const QString &name) {
    const QFileInfo sourceInfo(source);
    if (!sourceInfo.isFile() || sourceInfo.size() < 0 || sourceInfo.size() > MaximumBytes) return false;
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) return false;
    const QByteArray contents = input.read(MaximumBytes + 1);
    if (contents.size() > MaximumBytes || !input.atEnd()) return false;
    QFile output(mScratch->filePath(name));
    return output.open(QIODevice::WriteOnly | QIODevice::NewOnly) && output.write(contents) == contents.size();
  };
  if (!snapshot(mBaseline->text(), QStringLiteral("baseline.json")) ||
      !snapshot(mCandidate->text(), QStringLiteral("candidate.json")))
    return fail(AppLanguage::source("无法读取实测报告，或文件超过 16 MiB；原文件未修改。"));
  QFile contractFile(mScratch->filePath(QStringLiteral("contract.json")));
  const QByteArray contractBytes = QJsonDocument(selectedContract).toJson();
  if (!contractFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      contractFile.write(contractBytes) != contractBytes.size()) {
    contractFile.close();
    return fail(AppLanguage::source("无法写入质量门限临时文件。"));
  }
  contractFile.close();
  mOutput.clear();
  mErrors.clear();
  mBusy = true;
  ++mRunId;
  mProcess->setProgram(mPython);
  mProcess->setArguments({QStringLiteral("-B"), script, QStringLiteral("--baseline"),
      mScratch->filePath(QStringLiteral("baseline.json")), QStringLiteral("--candidate"),
      mScratch->filePath(QStringLiteral("candidate.json")), QStringLiteral("--contract"),
      mScratch->filePath(QStringLiteral("contract.json"))});
  auto environment = mEnvironment;
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  environment.insert(QStringLiteral("TEMP"), mScratch->path());
  environment.insert(QStringLiteral("TMP"), mScratch->path());
  mProcess->setProcessEnvironment(environment);
  mProcess->setWorkingDirectory(mBackendRoot);
  refreshEnabled();
  refreshPresentation();
  mTimeout->start(30000);
  mProcess->start();
}
void QualityComparisonDialog::readOutput(const bool standardError) {
  mProcess->setReadChannel(standardError ? QProcess::StandardError : QProcess::StandardOutput);
  QByteArray &target = standardError ? mErrors : mOutput;
  const qint64 budget = MaximumBytes - mOutput.size() - mErrors.size();
  if (budget >= 0) target += mProcess->read(budget + 1);
  if (mOutput.size() + mErrors.size() > MaximumBytes) {
    mFailure = AppLanguage::source("质量对照输出超过 16 MiB，已停止比较辅助进程。");
    mProcess->closeReadChannel(QProcess::StandardOutput);
    mProcess->closeReadChannel(QProcess::StandardError);
    mProcess->kill();
  }
}
void QualityComparisonDialog::finishProcess(const int exitCode, const bool normalExit) {
  if (!mBusy) return;
  mTimeout->stop();
  readOutput(false);
  readOutput(true);
  mBusy = false;
  if (mCancelled) {
    mReport = {};
  } else if (!mFailure) {
    QJsonParseError parseError;
    const auto parsed = QJsonDocument::fromJson(mOutput, &parseError);
    const QJsonObject result = parsed.object();
    const QString state = result.value(QStringLiteral("status")).toString();
    const int expectedExit = state == QStringLiteral("pass") ? 0 : state == QStringLiteral("regressed") ? 1
        : state == QStringLiteral("unverified") ? 3 : state == QStringLiteral("not_comparable") ? 4 : -1;
    if (normalExit && parsed.isObject() && expectedExit >= 0 && exitCode == expectedExit &&
        result.value(QStringLiteral("version")) == 1 && result.value(QStringLiteral("metrics")).isObject() &&
        result.value(QStringLiteral("settings_changes")).isArray()) {
      mReport = result;
      QFile baseline(mScratch->filePath(QStringLiteral("baseline.json")));
      if (baseline.open(QIODevice::ReadOnly))
        mProtocolIdentifier = QJsonDocument::fromJson(baseline.read(MaximumBytes)).object()
            .value(QStringLiteral("identity")).toObject().value(QStringLiteral("metric_protocol_id")).toString();
    } else {
      mFailure = AppLanguage::source("质量对照未返回有效结果，请检查实测报告与门限。详情：%1");
      mFailureDetail = QString::fromUtf8(mErrors.left(8192)).trimmed();
      if (mFailureDetail.isEmpty()) mFailureDetail = parseError.errorString();
    }
  }
  mScratch.reset();
  refreshEnabled();
  refreshPresentation();
  emit comparisonFinished(mReport);
  if (mClosePending && (!mMeasurementDialog || !mMeasurementDialog->isRunning())) QDialog::reject();
}
void QualityComparisonDialog::cancelComparison() {
  if (!mBusy) return;
  mCancelled = true;
  mReport = {};
  mProcess->kill();
  const quint64 runId = mRunId;
  QTimer::singleShot(1500, this, [this, runId] {
    if (mRunId == runId && mBusy && mCancelled && mProcess->state() != QProcess::NotRunning)
      mProcess->kill();
  });
}
void QualityComparisonDialog::reject() {
  if (mBusy || (mMeasurementDialog && mMeasurementDialog->isRunning())) {
    mClosePending = true;
    if (mMeasurementDialog) mMeasurementDialog->close();
    if (mBusy) cancelComparison();
    return;
  }
  QDialog::reject();
}
void QualityComparisonDialog::closeEvent(QCloseEvent *event) {
  if (mBusy || (mMeasurementDialog && mMeasurementDialog->isRunning())) {
    mClosePending = true;
    if (mMeasurementDialog) mMeasurementDialog->close();
    if (mBusy) cancelComparison();
    event->ignore();
  }
  else QDialog::closeEvent(event);
}
void QualityComparisonDialog::refreshPresentation() {
  mCancel->setText(text(mBusy ? AppLanguage::source("取消比较") : AppLanguage::source("关闭")));
  const QStringList limitHeaders{text(AppLanguage::source("指标")), text(AppLanguage::source("允许退化")),
      text(AppLanguage::source("绝对下限")), text(AppLanguage::source("绝对上限"))};
  mLimits->setHorizontalHeaderLabels(limitHeaders);
  for (int row = 0; row < int(mMetrics.size()); ++row) {
    put(mLimits, row, 0, text(mMetrics[row].caption));
    for (int column = 0; column < 3; ++column) {
      const QString name = text(mMetrics[row].caption) + QStringLiteral(" / ") + limitHeaders[column + 1];
      mMetrics[row].limits[column].enabled->setAccessibleName(name);
      mMetrics[row].limits[column].value->setAccessibleName(name);
    }
  }
  if (mBusy) mSummary->setText(text(AppLanguage::source("正在比较实测结果；不会启动训练、渲染或评估。")));
  else if (mFailure) {
    const QString failure = text(mFailure);
    mSummary->setText(failure.contains(QStringLiteral("%1")) ? failure.arg(mFailureDetail) : failure);
  }
  else if (mCancelled) mSummary->setText(text(AppLanguage::source("比较已取消；没有生成质量判定，训练任务不受影响。")));
  else if (mReport.isEmpty()) mSummary->setText(text(AppLanguage::source("尚未比较：请选择两个实测报告并明确设置质量门限。")));
  else {
    mSummary->setText(text(AppLanguage::source("结果：%1 · 独立验证：%2。仅适用于当前报告与所选门限，不代表所有视角或最终模型质量。"))
        .arg(statusText(mReport.value(QStringLiteral("status")).toString()),
             text(mReport.value(QStringLiteral("independent_validation")).toBool()
                 ? AppLanguage::source("已提供") : AppLanguage::source("未提供"))));
    const auto domains = mReport.value(QStringLiteral("domains")).toObject();
    mSummary->setText(mSummary->text() + QLatin1Char('\n') +
        text(AppLanguage::source("图像证据：%1 · 几何证据：%2")).arg(
            statusText(domains.value(QStringLiteral("image")).toObject().value(QStringLiteral("status")).toString()),
            statusText(domains.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("status")).toString())));
    const auto mismatches = mReport.value(QStringLiteral("identity_mismatches")).toArray();
    if (!mismatches.isEmpty()) mSummary->setText(mSummary->text() + QLatin1Char('\n') +
        text(AppLanguage::source("评估条件不一致（原始标识）：%1")).arg(jsonText(mismatches)));
  }
  mProtocol->setText(mProtocolIdentifier.isEmpty() ? QString()
      : text(AppLanguage::source("指标协议（原始标识）：%1")).arg(mProtocolIdentifier.left(1024)));
  mProtocol->setVisible(!mProtocolIdentifier.isEmpty());
  mResults->setHorizontalHeaderLabels({text(AppLanguage::source("指标")), text(AppLanguage::source("基准")),
      text(AppLanguage::source("候选")), text(AppLanguage::source("变化")), text(AppLanguage::source("门限判定")),
      text(AppLanguage::source("质量判定")), text(AppLanguage::source("原因"))});
  const auto results = mReport.value(QStringLiteral("metrics")).toObject();
  mResults->setRowCount(results.size());
  int row = 0;
  for (auto iterator = results.begin(); iterator != results.end(); ++iterator, ++row) {
    const auto result = iterator.value().toObject();
    QString caption = iterator.key();
    for (const auto &metric : mMetrics) if (metric.identifier == iterator.key()) caption = text(metric.caption);
    put(mResults, row, 0, caption);
    put(mResults, row, 1, valueText(result.value(QStringLiteral("baseline"))));
    put(mResults, row, 2, valueText(result.value(QStringLiteral("candidate"))));
    put(mResults, row, 3, valueText(result.value(QStringLiteral("delta")), result.value(QStringLiteral("delta_kind")).toString()));
    put(mResults, row, 4, statusText(result.value(QStringLiteral("threshold_status")).toString()));
    put(mResults, row, 5, statusText(result.value(QStringLiteral("status")).toString()));
    QStringList reasons;
    for (const auto &reason : result.value(QStringLiteral("reasons")).toArray()) reasons.append(reasonText(reason.toString()));
    put(mResults, row, 6, reasons.join(QLatin1Char('\n')));
  }
  mChanges->setHorizontalHeaderLabels({text(AppLanguage::source("参数路径（原始标识）")),
      text(AppLanguage::source("基准")), text(AppLanguage::source("候选"))});
  const auto changes = mReport.value(QStringLiteral("settings_changes")).toArray();
  const int visibleChanges = int(qMin(changes.size(), qsizetype(512)));
  mChanges->setRowCount(visibleChanges);
  if (changes.size() > visibleChanges) mSummary->setText(mSummary->text() + QLatin1Char('\n') +
      text(AppLanguage::source("表格仅显示前 512 项参数差异（共 %1 项）；长值为摘要，原始报告和机器结果保持完整。"))
          .arg(QLocale().toString(changes.size())));
  if (mSaveMessage) {
    const QString message = text(mSaveMessage);
    mSummary->setText(mSummary->text() + QLatin1Char('\n') +
        (message.contains(QStringLiteral("%1")) ? message.arg(mSaveDetail) : message));
  }
  for (int index = 0; index < visibleChanges; ++index) {
    const auto change = changes[index].toObject();
    put(mChanges, index, 0, jsonText(change.value(QStringLiteral("path"))));
    put(mChanges, index, 1, change.value(QStringLiteral("before_present")).toBool()
        ? jsonText(change.value(QStringLiteral("before"))) : text(AppLanguage::source("未记录")));
    put(mChanges, index, 2, change.value(QStringLiteral("after_present")).toBool()
        ? jsonText(change.value(QStringLiteral("after"))) : text(AppLanguage::source("未记录")));
  }
  mLimits->resizeRowsToContents();
  mResults->resizeRowsToContents();
  mChanges->resizeRowsToContents();
}
} // namespace gsw
