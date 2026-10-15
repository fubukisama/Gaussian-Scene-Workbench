#include "QualityMeasurementDialog.h"
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
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {
constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
QString text(const char *source) { return QCoreApplication::translate("Workbench", source); }
QString ownedDirectory(const QString &root, const QStringList &parts) {
  const QString workspace = QFileInfo(root).canonicalFilePath();
  if (workspace.isEmpty() || !QFileInfo(workspace).isDir()) return {};
  QString result = workspace;
  for (const QString &part : parts) {
    QFileInfo next(QDir(result).filePath(part));
    if (!next.exists() && !next.isSymLink()) {
      if (!QDir(result).mkdir(part)) return {};
      next.refresh();
    }
    const QString canonical = next.canonicalFilePath();
    if (!next.isDir() || canonical.isEmpty() ||
        !canonical.startsWith(workspace + QLatin1Char('/'), Qt::CaseInsensitive)) return {};
    result = canonical;
  }
  return result;
}
QString metricValue(const QJsonValue &value) {
  if (value.isDouble()) return QLocale().toString(value.toDouble(), 'g', 10);
  if (value.isObject() && value.toObject().value(QStringLiteral("special")) == QStringLiteral("positive_infinity"))
    return QString::fromUtf8("+∞");
  return text(AppLanguage::source("未提供"));
}
void put(QTableWidget *table, const int row, const int column, const QString &value) {
  auto *item = new QTableWidgetItem(value.left(1024));
  item->setToolTip(value.left(8192));
  table->setItem(row, column, item);
}
class RetainedTableState {
public:
  RetainedTableState(QTableWidget *table, const bool retain)
      : table(table), retain(retain), selection(table->selectionModel()->selection()),
        current(table->currentIndex()), vertical(table->verticalScrollBar()->value()),
        horizontal(table->horizontalScrollBar()->value()) {}
  ~RetainedTableState() {
    if (!retain) { table->clearSelection(); return; }
    table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    table->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    table->verticalScrollBar()->setValue(vertical);
    table->horizontalScrollBar()->setValue(horizontal);
  }
private:
  QTableWidget *table;
  bool retain;
  QItemSelection selection;
  QModelIndex current;
  int vertical, horizontal;
};
} // namespace

class QualityMeasurementDialog::State {
public:
  QString python, backendRoot, workspaceRoot;
  QProcessEnvironment environment;
  QLineEdit *manifest = nullptr;
  QLineEdit *referenceFolder = nullptr, *renderFolder = nullptr, *maskFolder = nullptr;
  QComboBox *mode = nullptr;
  QWidget *folderInputs = nullptr, *manifestInputs = nullptr;
  QPushButton *browse = nullptr, *start = nullptr, *cancel = nullptr, *refresh = nullptr;
  QPushButton *baseline = nullptr, *candidate = nullptr;
  QLabel *summary = nullptr, *saved = nullptr, *pairSummary = nullptr;
  QProgressBar *progress = nullptr;
  QTableWidget *views = nullptr;
  QTableWidget *pairsTable = nullptr;
  QProcess *process = nullptr;
  QTimer *timeout = nullptr;
  std::unique_ptr<QTemporaryDir> scratch;
  QJsonObject report;
  QJsonArray pairs;
  QJsonArray shownPairs;
  QJsonObject shownReport;
  QString tableLanguage;
  QByteArray output, errors, progressBuffer;
  QString publishedPath, detail;
  const char *failure = nullptr;
  const char *pairFailure = nullptr;
  QString pairDetail;
  bool busy = false, cancelled = false, closePending = false;
  int completed = 0, total = 0;
  quint64 runId = 0;
};

QualityMeasurementDialog::QualityMeasurementDialog(const QString &python, const QString &backendRoot,
    const QString &workspaceRoot, const QProcessEnvironment &environment, QWidget *parent)
    : QDialog(parent), m(std::make_unique<State>()) {
  m->python = python;
  m->backendRoot = backendRoot;
  m->workspaceRoot = workspaceRoot;
  m->environment = environment;
  setObjectName(QStringLiteral("qualityMeasurementDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("图像质量实测"));
  setMinimumSize(600, 340);
  resize(900, 600);
  auto *root = new QVBoxLayout(this);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *body = new QWidget(scroll);
  auto *layout = new QVBoxLayout(body);
  auto *notice = AppLanguage::text(new QLabel(body), AppLanguage::source(
      "仅在 CPU 上比较已有参考图与渲染图的 PSNR、SSIM，不启动训练或渲染。配对清单需明确视图标识、图像路径和可选遮罩；尺寸必须一致，不自动缩放。没有留出集来源证明时保持未验证，图像指标不能证明几何精度。"));
  notice->setTextFormat(Qt::PlainText);
  notice->setWordWrap(true);
  layout->addWidget(notice);
  auto *protocol = AppLanguage::text(new QLabel(body), AppLanguage::source(
      "指标协议：RGB8 编码值 · PSNR 汇总 MSE · SSIM 11×11 高斯有效窗口"));
  protocol->setObjectName(QStringLiteral("qualityMeasurementProtocol"));
  protocol->setWordWrap(true);
  protocol->setTextFormat(Qt::PlainText);
  AppLanguage::bind(protocol, "toolTip", AppLanguage::source(
      "RGB8 编码值除以 255，不做颜色变换或线性化。PSNR 按全部有效通道的均方误差汇总；SSIM 使用 11×11、σ=1.5 高斯有效窗口并按窗口数加权。它不等于训练损失 SSIM，也不验证留出集来源或几何精度。"));
  layout->addWidget(protocol);
  m->mode = new QComboBox(body);
  m->mode->setObjectName(QStringLiteral("qualityPairingMode"));
  m->mode->addItem(QString());
  m->mode->addItem(QString());
  AppLanguage::bindComboItem(m->mode, 0, AppLanguage::source("按文件夹配对（同名文件）"));
  AppLanguage::bindComboItem(m->mode, 1, AppLanguage::source("高级：使用 JSON 配对清单"));
  auto *modeActions = new QHBoxLayout;
  modeActions->addWidget(m->mode, 1);
  m->refresh = AppLanguage::text(new QPushButton(body), AppLanguage::source("刷新配对"));
  m->refresh->setObjectName(QStringLiteral("qualityRefreshPairs"));
  modeActions->addWidget(m->refresh);
  layout->addLayout(modeActions);
  connect(m->refresh, &QPushButton::clicked, this, &QualityMeasurementDialog::refreshFolderPairs);
  m->folderInputs = new QWidget(body);
  m->folderInputs->setProperty("gswLayoutContainer", true);
  auto *folderLayout = new QFormLayout(m->folderInputs);
  folderLayout->setContentsMargins(0, 0, 0, 0);
  folderLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
  const auto addFolder = [&](const char *caption, const QString &name, QLineEdit **field) {
    auto *container = new QWidget(m->folderInputs);
    container->setProperty("gswLayoutContainer", true);
    auto *line = new QHBoxLayout(container);
    line->setContentsMargins(0, 0, 0, 0);
    *field = new QLineEdit(container);
    (*field)->setObjectName(name);
    auto *browse = AppLanguage::text(new QPushButton(container), AppLanguage::source("浏览..."));
    line->addWidget(*field, 1);
    line->addWidget(browse);
    folderLayout->addRow(text(caption), container);
    AppLanguage::text(qobject_cast<QLabel *>(folderLayout->labelForField(container)), caption);
    connect(browse, &QPushButton::clicked, this, [this, target = *field] {
      const QString path = QFileDialog::getExistingDirectory(this,
          text(AppLanguage::source("选择图像文件夹")), target->text());
      if (!path.isEmpty()) target->setText(QDir::toNativeSeparators(path));
    });
    connect(*field, &QLineEdit::textChanged, this, &QualityMeasurementDialog::inputsChanged);
  };
  addFolder(AppLanguage::source("参考图文件夹"), QStringLiteral("qualityReferenceFolder"), &m->referenceFolder);
  addFolder(AppLanguage::source("渲染图文件夹"), QStringLiteral("qualityRenderFolder"), &m->renderFolder);
  addFolder(AppLanguage::source("遮罩文件夹（可选）"), QStringLiteral("qualityMaskFolder"), &m->maskFolder);
  auto *folderNotice = AppLanguage::text(new QLabel(m->folderInputs), AppLanguage::source(
      "只读取所选目录第一层的非隐藏 PNG/JPEG，不递归或跟随链接。每张参考图必须有文件名完全相同（含大小写和扩展名）的渲染图；指定遮罩目录时也必须逐张同名。不同名称或 JPEG 对应 PNG 遮罩请使用高级清单，不自动猜测或抽样。"));
  folderNotice->setWordWrap(true);
  folderNotice->setTextFormat(Qt::PlainText);
  folderLayout->addRow(folderNotice);
  layout->addWidget(m->folderInputs);
  m->manifestInputs = new QWidget(body);
  m->manifestInputs->setProperty("gswLayoutContainer", true);
  auto *paths = new QFormLayout(m->manifestInputs);
  paths->setContentsMargins(0, 0, 0, 0);
  paths->setRowWrapPolicy(QFormLayout::WrapLongRows);
  auto *row = new QWidget(m->manifestInputs);
  row->setProperty("gswLayoutContainer", true);
  auto *horizontal = new QHBoxLayout(row);
  horizontal->setContentsMargins(0, 0, 0, 0);
  m->manifest = new QLineEdit(row);
  m->manifest->setObjectName(QStringLiteral("qualityPairManifest"));
  m->browse = AppLanguage::text(new QPushButton(row), AppLanguage::source("浏览..."));
  horizontal->addWidget(m->manifest, 1);
  horizontal->addWidget(m->browse);
  paths->addRow(text(AppLanguage::source("图像配对清单（JSON）")), row);
  AppLanguage::text(qobject_cast<QLabel *>(paths->labelForField(row)), AppLanguage::source("图像配对清单（JSON）"));
  layout->addWidget(m->manifestInputs);
  m->pairSummary = new QLabel(body);
  m->pairSummary->setObjectName(QStringLiteral("qualityPairSummary"));
  m->pairSummary->setTextFormat(Qt::PlainText);
  m->pairSummary->setWordWrap(true);
  layout->addWidget(m->pairSummary);
  m->pairsTable = new QTableWidget(0, 4, body);
  m->pairsTable->setObjectName(QStringLiteral("qualityImagePairs"));
  m->pairsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m->pairsTable->verticalHeader()->hide();
  m->pairsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  m->pairsTable->setMinimumHeight(120);
  layout->addWidget(m->pairsTable);
  m->summary = new QLabel(body);
  m->summary->setObjectName(QStringLiteral("qualityMeasurementSummary"));
  m->summary->setTextFormat(Qt::PlainText);
  m->summary->setWordWrap(true);
  m->summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(m->summary);
  m->progress = new QProgressBar(body);
  m->progress->setObjectName(QStringLiteral("qualityMeasurementProgress"));
  m->progress->hide();
  layout->addWidget(m->progress);
  m->views = new QTableWidget(0, 5, body);
  m->views->setObjectName(QStringLiteral("qualityMeasuredViews"));
  m->views->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m->views->verticalHeader()->hide();
  m->views->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  m->views->setMinimumHeight(180);
  layout->addWidget(m->views, 1);
  m->saved = new QLabel(body);
  m->saved->setWordWrap(true);
  m->saved->setTextFormat(Qt::PlainText);
  m->saved->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(m->saved);
  scroll->setWidget(body);
  root->addWidget(scroll, 1);
  auto *buttons = new QHBoxLayout;
  m->start = AppLanguage::text(new QPushButton(this), AppLanguage::source("开始图像实测"));
  m->start->setObjectName(QStringLiteral("qualityMeasureStart"));
  m->baseline = AppLanguage::text(new QPushButton(this), AppLanguage::source("用作基准"));
  m->baseline->setObjectName(QStringLiteral("qualityUseAsBaseline"));
  m->candidate = AppLanguage::text(new QPushButton(this), AppLanguage::source("用作候选"));
  m->candidate->setObjectName(QStringLiteral("qualityUseAsCandidate"));
  m->cancel = new QPushButton(this);
  m->cancel->setObjectName(QStringLiteral("qualityMeasureCancel"));
  buttons->addWidget(m->start);
  buttons->addStretch();
  buttons->addWidget(m->baseline);
  buttons->addWidget(m->candidate);
  buttons->addWidget(m->cancel);
  root->addLayout(buttons);
  connect(m->browse, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, text(AppLanguage::source("选择图像配对清单")),
        m->manifest->text(), text(AppLanguage::source("JSON 文件 (*.json);;所有文件 (*)")));
    if (!path.isEmpty()) m->manifest->setText(QDir::toNativeSeparators(path));
  });
  connect(m->manifest, &QLineEdit::textChanged, this, &QualityMeasurementDialog::inputsChanged);
  connect(m->mode, &QComboBox::currentIndexChanged, this, &QualityMeasurementDialog::inputsChanged);
  connect(m->start, &QPushButton::clicked, this, &QualityMeasurementDialog::startMeasurement);
  connect(m->baseline, &QPushButton::clicked, this, [this] { selectReport(true); });
  connect(m->candidate, &QPushButton::clicked, this, [this] { selectReport(false); });
  connect(m->cancel, &QPushButton::clicked, this, [this] { if (m->busy) cancelMeasurement(); else reject(); });
  m->process = new QProcess(this);
#ifdef Q_OS_WIN
  m->process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
    arguments->flags |= CREATE_NO_WINDOW;
    arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
    arguments->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
  connect(m->process, &QProcess::started, this, [this] { if (m->cancelled || m->failure) m->process->kill(); });
  connect(m->process, &QProcess::readyReadStandardOutput, this, [this] { readOutput(false); });
  connect(m->process, &QProcess::readyReadStandardError, this, [this] { readOutput(true); });
  connect(m->process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus state) {
    finishProcess(code, state == QProcess::NormalExit);
  });
  connect(m->process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      if (!m->cancelled) {
        m->failure = AppLanguage::source("无法启动图像实测：%1");
        m->detail = m->process->errorString();
      }
      finishProcess(2, false);
    }
  });
  m->timeout = new QTimer(this);
  m->timeout->setSingleShot(true);
  connect(m->timeout, &QTimer::timeout, this, [this] {
    m->failure = AppLanguage::source("图像实测超过 10 分钟，已停止本窗口的辅助进程。");
    m->process->kill();
  });
  AppLanguage::onChanged(this, [this] { refreshPresentation(); });
  refreshPairs();
  refreshEnabled();
  refreshPresentation();
}

QualityMeasurementDialog::~QualityMeasurementDialog() {
  m->timeout->stop();
  // Destruction is a last-resort fallback. Normal close waits asynchronously.
  // Prevent synchronous finished/ready callbacks from re-entering a dialog
  // whose State and temporary files are about to be destroyed.
  disconnect(m->process, nullptr, this, nullptr);
  if (m->process->state() != QProcess::NotRunning) {
    m->process->kill();
    if (!m->process->waitForFinished(1000) && m->scratch)
      m->scratch->setAutoRemove(false); // Never remove files beneath a still-live owned helper.
  }
}
void QualityMeasurementDialog::setManifestPath(const QString &path) {
  if (m->busy) return;
  m->mode->setCurrentIndex(1);
  m->manifest->setText(path);
}
void QualityMeasurementDialog::setPairedFolders(const QString &reference, const QString &render, const QString &mask) {
  if (m->busy) return;
  m->mode->setCurrentIndex(0);
  m->referenceFolder->setText(reference);
  m->renderFolder->setText(render);
  m->maskFolder->setText(mask);
  inputsChanged(); // Rescan on an explicit repeated choice, even when its text did not change.
}
void QualityMeasurementDialog::inputsChanged() {
  if (m->busy) return;
  m->report = {};
  m->publishedPath.clear();
  m->scratch.reset();
  m->failure = nullptr;
  m->cancelled = false;
  refreshPairs();
  refreshEnabled();
  refreshPresentation();
}
void QualityMeasurementDialog::refreshFolderPairs() {
  if (!m->busy && m->mode->currentIndex() == 0) inputsChanged();
}
void QualityMeasurementDialog::refreshPairs() {
  m->pairs = {};
  m->pairFailure = nullptr;
  m->pairDetail.clear();
  if (m->mode->currentIndex() != 0) return;
  const auto reject = [&](const char *source, const QString &detail = {}) {
    m->pairFailure = source;
    m->pairDetail = detail;
    m->pairs = {};
  };
  if (m->referenceFolder->text().isEmpty() || m->renderFolder->text().isEmpty()) {
    reject(AppLanguage::source("请先选择参考图与渲染图文件夹。"));
    return;
  }
  const auto files = [&](const QString &folder, QMap<QString, QFileInfo> *images) {
    const QFileInfo directory(folder);
    if (!directory.isDir() || !directory.isReadable() || directory.isSymLink()) {
      reject(AppLanguage::source("图像文件夹无法读取或是链接：%1"), folder);
      return false;
    }
    for (const auto &file : QDir(folder).entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
      const QString extension = file.suffix().toLower();
      if (extension != QStringLiteral("png") && extension != QStringLiteral("jpg") && extension != QStringLiteral("jpeg")) continue;
      if (file.isSymLink() || !file.isFile() || !file.isReadable()) {
        reject(AppLanguage::source("图像文件不可读取或是链接：%1"), file.absoluteFilePath());
        return false;
      }
      if (images->contains(file.fileName())) {
        reject(AppLanguage::source("发现有歧义的同名图像：%1"), file.fileName());
        return false;
      }
      images->insert(file.fileName(), file);
    }
    return true;
  };
  QMap<QString, QFileInfo> references, renders, masks;
  if (!files(m->referenceFolder->text(), &references) || !files(m->renderFolder->text(), &renders) ||
      (!m->maskFolder->text().isEmpty() && !files(m->maskFolder->text(), &masks))) return;
  if (references.isEmpty()) {
    reject(AppLanguage::source("参考图文件夹第一层没有可配对的非隐藏 PNG/JPEG 图像。"));
    return;
  }
  if (references.size() > 4096) {
    reject(AppLanguage::source("图像实测最多接受 4096 对图像；未自动抽样。"));
    return;
  }
  for (auto iterator = references.begin(); iterator != references.end(); ++iterator) {
    if (!renders.contains(iterator.key())) {
      reject(AppLanguage::source("参考图缺少完全同名的渲染图：%1"), iterator.key());
      return;
    }
    if (!m->maskFolder->text().isEmpty() && !masks.contains(iterator.key())) {
      reject(AppLanguage::source("参考图缺少完全同名的遮罩：%1"), iterator.key());
      return;
    }
    QJsonObject pair{{QStringLiteral("id"), iterator.key()},
        {QStringLiteral("reference"), iterator.value().absoluteFilePath()},
        {QStringLiteral("render"), renders.value(iterator.key()).absoluteFilePath()}};
    if (!m->maskFolder->text().isEmpty()) pair.insert(QStringLiteral("mask"), masks.value(iterator.key()).absoluteFilePath());
    m->pairs.append(pair);
  }
}
QString QualityMeasurementDialog::manifestPath() const { return m->manifest->text(); }
QString QualityMeasurementDialog::reportPath() const {
  if (!m->publishedPath.isEmpty()) return m->publishedPath;
  return m->report.isEmpty() || !m->scratch ? QString() : m->scratch->filePath(QStringLiteral("report.json"));
}
QJsonObject QualityMeasurementDialog::measurementReport() const { return m->report; }
bool QualityMeasurementDialog::isRunning() const { return m->busy; }
void QualityMeasurementDialog::refreshEnabled() {
  const bool folderMode = m->mode->currentIndex() == 0;
  m->mode->setEnabled(!m->busy);
  m->refresh->setEnabled(!m->busy);
  m->refresh->setVisible(folderMode);
  m->folderInputs->setVisible(folderMode);
  m->manifestInputs->setVisible(!folderMode);
  m->folderInputs->setEnabled(!m->busy);
  m->pairSummary->setVisible(folderMode);
  m->pairsTable->setVisible(folderMode);
  m->manifest->setEnabled(!m->busy);
  m->browse->setEnabled(!m->busy);
  m->start->setEnabled(!m->busy && (!folderMode || !m->pairs.isEmpty()));
  m->baseline->setEnabled(!m->busy && !m->report.isEmpty());
  m->candidate->setEnabled(!m->busy && !m->report.isEmpty());
}
void QualityMeasurementDialog::fail(const char *source, const QString &detail) {
  m->failure = source;
  m->detail = detail;
  m->busy = false;
  m->report = {};
  m->scratch.reset();
  refreshEnabled();
  refreshPresentation();
  emit measurementFinished(m->report);
}
void QualityMeasurementDialog::startMeasurement() {
  if (m->busy) return;
  m->report = {};
  m->scratch.reset();
  m->publishedPath.clear();
  m->failure = nullptr;
  m->detail.clear();
  m->cancelled = false;
  m->completed = m->total = 0;
  const QString script = QDir(m->backendRoot).filePath(QStringLiteral("scripts/measure_image_quality.py"));
  if (!QFileInfo(m->python).isFile() || !QFileInfo(script).isFile())
    return fail(AppLanguage::source("图像实测工具不可用，请检查 Python、NumPy、Pillow 和已安装脚本；不会自动安装。"));
  QByteArray contents;
  QString baseDirectory;
  if (m->mode->currentIndex() == 0) {
    refreshPairs(); // Refuse a directory changed since its displayed pairing was prepared.
    if (m->pairs.isEmpty()) return fail(m->pairFailure, m->pairDetail);
    contents = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("views"), m->pairs}}).toJson(QJsonDocument::Compact);
    baseDirectory = m->workspaceRoot; // Generated pairs use explicit absolute paths.
  } else {
    const QFileInfo inputInfo(m->manifest->text());
    if (!inputInfo.isFile() || inputInfo.size() < 0 || inputInfo.size() > MaximumBytes)
      return fail(AppLanguage::source("无法读取图像配对清单，或文件超过 16 MiB；原文件未修改。"));
    QFile input(m->manifest->text());
    if (!input.open(QIODevice::ReadOnly))
      return fail(AppLanguage::source("无法读取图像配对清单，或文件超过 16 MiB；原文件未修改。"));
    contents = input.read(MaximumBytes + 1);
    if (!input.atEnd()) return fail(AppLanguage::source("无法读取图像配对清单，或文件超过 16 MiB；原文件未修改。"));
    baseDirectory = inputInfo.absoluteDir().absolutePath();
  }
  if (contents.size() > MaximumBytes)
    return fail(AppLanguage::source("无法读取图像配对清单，或文件超过 16 MiB；原文件未修改。"));
  const QString scratchRoot = ownedDirectory(m->workspaceRoot, {QStringLiteral(".gsw"), QStringLiteral("jobs")});
  if (scratchRoot.isEmpty()) return fail(AppLanguage::source("无法在当前工作区创建图像实测临时目录。"));
  m->scratch = std::make_unique<QTemporaryDir>(QDir(scratchRoot).filePath(QStringLiteral("measure-XXXXXX")));
  if (!m->scratch->isValid()) return fail(AppLanguage::source("无法在当前工作区创建图像实测临时目录。"));
  const QString snapshot = m->scratch->filePath(QStringLiteral("pairs.json"));
  QFile copy(snapshot);
  if (!copy.open(QIODevice::WriteOnly | QIODevice::NewOnly) || copy.write(contents) != contents.size()) {
    copy.close();
    return fail(AppLanguage::source("无法在当前工作区创建图像实测临时目录。"));
  }
  copy.close();
  m->output.clear();
  m->errors.clear();
  m->progressBuffer.clear();
  m->busy = true;
  ++m->runId;
  m->process->setProgram(m->python);
  m->process->setArguments({QStringLiteral("-B"), script, QStringLiteral("--manifest"), snapshot,
      QStringLiteral("--base-dir"), baseDirectory,
      QStringLiteral("--output"), m->scratch->filePath(QStringLiteral("report.json"))});
  auto environment = m->environment;
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  environment.insert(QStringLiteral("TEMP"), m->scratch->path());
  environment.insert(QStringLiteral("TMP"), m->scratch->path());
  environment.insert(QStringLiteral("TMPDIR"), m->scratch->path());
  m->process->setProcessEnvironment(environment);
  m->process->setWorkingDirectory(m->backendRoot);
  refreshEnabled();
  refreshPresentation();
  m->timeout->start(600000);
  m->process->start();
}
void QualityMeasurementDialog::readOutput(const bool standardError) {
  m->process->setReadChannel(standardError ? QProcess::StandardError : QProcess::StandardOutput);
  const qint64 budget = MaximumBytes - m->output.size() - m->errors.size();
  const QByteArray bytes = budget >= 0 ? m->process->read(budget + 1) : QByteArray();
  (standardError ? m->errors : m->output) += bytes;
  if (m->output.size() + m->errors.size() > MaximumBytes) {
    m->failure = AppLanguage::source("图像实测输出超过 16 MiB，已停止本窗口的辅助进程。");
    m->process->closeReadChannel(QProcess::StandardOutput);
    m->process->closeReadChannel(QProcess::StandardError);
    m->process->kill();
    return;
  }
  if (!standardError) return;
  m->progressBuffer += bytes;
  while (m->progressBuffer.contains('\n')) {
    const qsizetype end = m->progressBuffer.indexOf('\n');
    const auto event = QJsonDocument::fromJson(m->progressBuffer.left(end)).object();
    m->progressBuffer.remove(0, end + 1);
    const double completed = event.value(QStringLiteral("completed")).toDouble(-1);
    const double total = event.value(QStringLiteral("total")).toDouble(-1);
    if (event.value(QStringLiteral("event")) == QStringLiteral("progress") &&
        total > 0 && total <= 1000000 && completed >= 0 && completed <= total &&
        std::floor(total) == total && std::floor(completed) == completed) {
      m->completed = int(completed);
      m->total = int(total);
      refreshProgress();
    }
  }
}
void QualityMeasurementDialog::finishProcess(const int exitCode, const bool normalExit) {
  if (!m->busy) return;
  m->timeout->stop();
  readOutput(false);
  readOutput(true);
  m->busy = false;
  if (!m->cancelled && !m->failure) {
    QJsonParseError error;
    const auto parsed = QJsonDocument::fromJson(m->output, &error);
    const auto report = parsed.object();
    const auto evidence = report.value(QStringLiteral("evidence")).toObject();
    const auto split = report.value(QStringLiteral("split")).toString();
    QFile file(m->scratch->filePath(QStringLiteral("report.json")));
    QJsonObject saved;
    QByteArray savedBytes;
    if (file.size() <= MaximumBytes && file.open(QIODevice::ReadOnly)) {
      savedBytes = file.read(MaximumBytes + 1);
      saved = QJsonDocument::fromJson(savedBytes).object();
      file.close();
    }
    if (normalExit && exitCode == 0 && parsed.isObject() && report.value(QStringLiteral("version")) == 1 &&
        (split == QStringLiteral("unverified") || split == QStringLiteral("training_fit")) &&
        report.value(QStringLiteral("metrics")).isObject() && !report.value(QStringLiteral("identity")).toObject().isEmpty() &&
        evidence.value(QStringLiteral("producer")) == QStringLiteral("gsw-image-quality-v1") &&
        evidence.value(QStringLiteral("held_out_verified")).isBool() && !evidence.value(QStringLiteral("held_out_verified")).toBool() &&
        evidence.value(QStringLiteral("views")).isArray() && !evidence.value(QStringLiteral("views")).toArray().isEmpty() &&
        saved == report && savedBytes == m->output) {
      m->report = report;
    } else {
      m->failure = AppLanguage::source("图像实测未返回有效结果。请确认 NumPy/Pillow 可用，图像为同尺寸的 8 位 RGB PNG/JPEG；遮罩为同尺寸 8 位灰度，至少保留一个完整的 11×11 有效窗口。技术详情（原始日志）：%1");
      m->detail = QString::fromUtf8(m->errors.left(8192)).trimmed();
      if (m->detail.isEmpty()) m->detail = error.errorString();
    }
  }
  if (m->cancelled || m->failure) { m->report = {}; m->scratch.reset(); }
  refreshEnabled();
  refreshPresentation();
  emit measurementFinished(m->report);
  if (m->closePending) QDialog::reject();
}
void QualityMeasurementDialog::selectReport(const bool useAsBaseline) {
  if (m->busy || m->report.isEmpty() || !m->scratch) return;
  const auto failed = [this] {
    m->failure = AppLanguage::source("无法保存图像实测报告；未覆盖已有文件，也未填入质量对照。");
    refreshPresentation();
  };
  const QString directory = ownedDirectory(m->workspaceRoot,
      {QStringLiteral(".gsw"), QStringLiteral("quality"), QStringLiteral("measurements")});
  const auto trusted = [this](const QString &path, const QString &parent) {
    const QFileInfo info(path);
    if (parent.isEmpty() || !info.isFile() || info.isSymLink() || info.size() > MaximumBytes ||
        info.canonicalPath().compare(parent, Qt::CaseInsensitive) != 0 ||
        !info.canonicalFilePath().startsWith(parent + QLatin1Char('/'), Qt::CaseInsensitive)) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.read(MaximumBytes + 1);
    return !bytes.isEmpty() && bytes.size() <= MaximumBytes && file.atEnd() &&
        bytes == m->output && QJsonDocument::fromJson(bytes).object() == m->report;
  };
  if (!m->publishedPath.isEmpty() && !trusted(m->publishedPath, directory))
    m->publishedPath.clear(); // Preserve the user's external edit; explicitly adopt a new immutable file instead.
  if (m->publishedPath.isEmpty()) {
    const QString workspace = QFileInfo(m->workspaceRoot).canonicalFilePath();
    const QString scratch = QFileInfo(m->scratch->path()).canonicalFilePath();
    if (directory.isEmpty() || workspace.isEmpty() ||
        !scratch.startsWith(workspace + QLatin1Char('/'), Qt::CaseInsensitive) ||
        !trusted(m->scratch->filePath(QStringLiteral("report.json")), scratch)) return failed();
    const QString path = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return failed();
    if (output.write(m->output) != m->output.size() || !output.flush()) {
      output.close();
      output.remove();
      return failed();
    }
    output.close();
    m->publishedPath = path;
  }
  // Recheck even freshly written reports immediately before exposing a path.
  if (!trusted(m->publishedPath, directory)) {
    m->publishedPath.clear();
    return failed();
  }
  m->failure = nullptr;
  refreshPresentation();
  emit reportChosen(m->publishedPath, useAsBaseline);
}
void QualityMeasurementDialog::cancelMeasurement() {
  if (!m->busy) return;
  m->cancelled = true;
  m->report = {};
  m->process->kill();
  const quint64 runId = m->runId;
  QTimer::singleShot(1500, this, [this, runId] {
    if (m->runId == runId && m->busy && m->cancelled && m->process->state() != QProcess::NotRunning)
      m->process->kill();
  });
}
void QualityMeasurementDialog::reject() {
  if (m->busy) { m->closePending = true; cancelMeasurement(); return; }
  QDialog::reject();
}
void QualityMeasurementDialog::closeEvent(QCloseEvent *event) {
  if (m->busy) { m->closePending = true; cancelMeasurement(); event->ignore(); }
  else QDialog::closeEvent(event);
}
void QualityMeasurementDialog::refreshProgress() {
  m->cancel->setText(text(m->busy ? AppLanguage::source("取消实测") : AppLanguage::source("关闭")));
  m->progress->setVisible(m->busy);
  if (m->total > 0) { m->progress->setRange(0, m->total); m->progress->setValue(m->completed); }
  else m->progress->setRange(0, 0);
  if (m->busy) m->summary->setText(text(AppLanguage::source("正在 CPU 上实测图像：%1 / %2；不会启动训练或渲染。"))
      .arg(QLocale().toString(m->completed), m->total > 0 ? QLocale().toString(m->total) : QStringLiteral("—")));
}
void QualityMeasurementDialog::refreshPresentation() {
  refreshProgress();
  if (m->busy) { /* Progress presentation above never rebuilds a table. */ }
  else if (m->failure) {
    const QString message = text(m->failure);
    m->summary->setText(message.contains(QStringLiteral("%1")) ? message.arg(m->detail) : message);
  } else if (m->cancelled) m->summary->setText(text(AppLanguage::source("图像实测已取消；没有采用新的报告，训练任务不受影响。")));
  else if (m->report.isEmpty()) m->summary->setText(text(AppLanguage::source("请选择明确配对的图像清单；实测完成后再选择用作基准或候选。")));
  else {
    const auto metrics = m->report.value(QStringLiteral("metrics")).toObject();
    const auto evidence = m->report.value(QStringLiteral("evidence")).toObject();
    m->summary->setText(text(AppLanguage::source("实测图像 %1 幅 · PSNR %2 dB · SSIM %3。独立验证：未提供；不代表几何精度或最终模型质量。"))
        .arg(QLocale().toString(evidence.value(QStringLiteral("views")).toArray().size()),
             metricValue(metrics.value(QStringLiteral("psnr"))), metricValue(metrics.value(QStringLiteral("ssim")))));
    if (m->publishedPath.isEmpty()) m->summary->setText(m->summary->text() + QLatin1Char('\n') +
        text(AppLanguage::source("报告尚未采用；只有点击用作基准或候选，才持久保存到当前工作区并填入对照，不自动关联实验。")));
  }
  m->saved->setText(m->publishedPath.isEmpty() ? QString() :
      text(AppLanguage::source("已保存实测报告：%1")).arg(QDir::toNativeSeparators(m->publishedPath)));
  m->saved->setVisible(!m->publishedPath.isEmpty());
  if (m->pairFailure) {
    const QString message = text(m->pairFailure);
    m->pairSummary->setText(message.contains(QStringLiteral("%1")) ? message.arg(m->pairDetail) : message);
  } else m->pairSummary->setText(text(AppLanguage::source("已完整配对 %1 幅参考图；实测将处理全部配对，不抽样。"))
      .arg(QLocale().toString(m->pairs.size())));
  const bool languageChanged = m->tableLanguage != AppLanguage::current();
  const bool samePairs = m->shownPairs == m->pairs;
  const int shownPairs = int(qMin(m->pairs.size(), qsizetype(512)));
  if (!samePairs || languageChanged) {
    RetainedTableState retained(m->pairsTable, samePairs);
    m->pairsTable->setHorizontalHeaderLabels({text(AppLanguage::source("视图标识（原始标识）")),
        text(AppLanguage::source("参考图")), text(AppLanguage::source("渲染图")), text(AppLanguage::source("遮罩"))});
    m->pairsTable->setRowCount(shownPairs);
    for (int row = 0; row < shownPairs; ++row) {
      const auto pair = m->pairs[row].toObject();
      put(m->pairsTable, row, 0, pair.value(QStringLiteral("id")).toString());
      put(m->pairsTable, row, 1, QDir::toNativeSeparators(pair.value(QStringLiteral("reference")).toString()));
      put(m->pairsTable, row, 2, QDir::toNativeSeparators(pair.value(QStringLiteral("render")).toString()));
      put(m->pairsTable, row, 3, pair.contains(QStringLiteral("mask"))
          ? QDir::toNativeSeparators(pair.value(QStringLiteral("mask")).toString()) : text(AppLanguage::source("未提供")));
    }
    m->pairsTable->resizeRowsToContents();
    m->shownPairs = m->pairs;
  }
  if (m->pairs.size() > shownPairs) m->pairSummary->setText(m->pairSummary->text() + QLatin1Char('\n') +
      text(AppLanguage::source("配对表仅显示前 512 项；生成清单与实测仍包含全部 %1 对图像。"))
          .arg(QLocale().toString(m->pairs.size())));
  const bool sameReport = m->shownReport == m->report;
  const auto views = m->report.value(QStringLiteral("evidence")).toObject().value(QStringLiteral("views")).toArray();
  const int visible = int(qMin(views.size(), qsizetype(512)));
  m->views->setVisible(!m->report.isEmpty());
  if (!sameReport || languageChanged) {
    RetainedTableState retained(m->views, sameReport);
    m->views->setHorizontalHeaderLabels({text(AppLanguage::source("视图标识（原始标识）")),
        text(AppLanguage::source("图像尺寸")), QStringLiteral("PSNR (dB)"), QStringLiteral("SSIM"), text(AppLanguage::source("有效像素"))});
    m->views->setRowCount(visible);
    for (int row = 0; row < visible; ++row) {
      const auto view = views[row].toObject();
      const auto metrics = view.value(QStringLiteral("metrics")).toObject();
      put(m->views, row, 0, view.value(QStringLiteral("id")).toString());
      put(m->views, row, 1, QStringLiteral("%1 × %2 px").arg(QLocale().toString(view.value(QStringLiteral("width")).toInt()),
          QLocale().toString(view.value(QStringLiteral("height")).toInt())));
      put(m->views, row, 2, metricValue(metrics.value(QStringLiteral("psnr"))));
      put(m->views, row, 3, metricValue(metrics.value(QStringLiteral("ssim"))));
      put(m->views, row, 4, QLocale().toString(view.value(QStringLiteral("valid_pixels")).toDouble(), 'f', 0));
    }
    m->views->resizeRowsToContents();
    m->shownReport = m->report;
  }
  if (views.size() > visible) m->summary->setText(m->summary->text() + QLatin1Char('\n') +
      text(AppLanguage::source("表格仅显示前 512 幅图像；保存的实测报告保留全部逐视图结果。")));
  m->tableLanguage = AppLanguage::current();
}
} // namespace gsw
