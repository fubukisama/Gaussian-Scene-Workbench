#include "MainWindow.h"

#include "AppTheme.h"
#include "BackendLocator.h"
#include "ColmapSupport.h"
#include "DatasetImportDialog.h"
#include "DatasetImportPlan.h"
#include "DurableArtifactStore.h"
#include "ExternalBackupStore.h"
#include "ImportEnvironmentProbe.h"
#include "MediaProjectBootstrap.h"
#include "ReconstructionDialog.h"
#include "TrainingDialog.h"
#include "TrainingEnvironmentProbe.h"
#include "TrainingOutputLocator.h"
#include "UntitledWorkspaceStorage.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryFile>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <functional>

namespace gsw {

namespace {
constexpr int kDockLayoutStateVersion = 4;

class DockTitleBar final : public QWidget {
public:
  explicit DockTitleBar(QDockWidget *dock) : QWidget(dock), mDock(dock) {
    setObjectName(QStringLiteral("dockTitleBar"));
    setAccessibleName(dock->windowTitle());
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    mLayout = new QHBoxLayout(this);
    mTitleLabel = new QLabel(dock->windowTitle(), this);
    mTitleLabel->setObjectName(QStringLiteral("dockTitleLabel"));
    mTitleLabel->setTextFormat(Qt::PlainText);
    mTitleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mTitleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mTitleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    mLayout->addWidget(mTitleLabel, 1);

    mFloatButton = createButton(QStringLiteral("dockTitleButton"));
    mCloseButton = createButton(QStringLiteral("dockTitleButton"));
    mLayout->addWidget(mFloatButton);
    mLayout->addWidget(mCloseButton);

    connect(dock, &QDockWidget::windowTitleChanged, this,
            [this](const QString &title) {
              setAccessibleName(title);
              mTitleLabel->setText(title);
            });
    connect(dock, &QDockWidget::featuresChanged, this,
            [this]() { updateActions(); });
    connect(dock, &QDockWidget::topLevelChanged, this,
            [this]() { updateActions(); });
    connect(mFloatButton, &QToolButton::clicked, this, [this]() {
      if (mDock->features().testFlag(QDockWidget::DockWidgetFloatable)) {
        mDock->setFloating(!mDock->isFloating());
      }
    });
    connect(mCloseButton, &QToolButton::clicked, mDock, &QWidget::close);
    updateActions();
  }

  void applyScale(const int scalePercent) {
    QFont titleFont = qApp->font();
    titleFont.setPixelSize(AppTheme::dockTitleFontPixelSize(scalePercent));
    titleFont.setWeight(QFont::Normal);
    mTitleLabel->setFont(titleFont);

    const int paddingX = AppTheme::scaled(6, scalePercent);
    const int paddingY = AppTheme::scaled(1, scalePercent);
    const int spacing = AppTheme::scaled(2, scalePercent);
    const int buttonSize = AppTheme::dockTitleButtonSize(scalePercent);
    const int iconSize = AppTheme::scaled(10, scalePercent);
    mLayout->setContentsMargins(paddingX, paddingY, paddingX, paddingY);
    mLayout->setSpacing(spacing);
    for (QToolButton *button : {mFloatButton, mCloseButton}) {
      button->setFixedSize(buttonSize, buttonSize);
      button->setIconSize(QSize(iconSize, iconSize));
    }

    const int contentHeight =
        std::max(QFontMetrics(titleFont).height(), buttonSize);
    setFixedHeight(std::max(AppTheme::dockTitleHeight(scalePercent),
                            contentHeight + paddingY * 2));
    updateActions();
    updateGeometry();
  }

protected:
  void mousePressEvent(QMouseEvent *event) override { event->ignore(); }
  void mouseMoveEvent(QMouseEvent *event) override { event->ignore(); }
  void mouseReleaseEvent(QMouseEvent *event) override { event->ignore(); }
  void mouseDoubleClickEvent(QMouseEvent *event) override { event->ignore(); }

private:
  QToolButton *createButton(const QString &objectName) {
    auto *button = new QToolButton(this);
    button->setObjectName(objectName);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
  }

  void updateActions() {
    const QDockWidget::DockWidgetFeatures features = mDock->features();
    const bool canFloat = features.testFlag(QDockWidget::DockWidgetFloatable);
    const bool canClose = features.testFlag(QDockWidget::DockWidgetClosable);
    const QString floatText = mDock->isFloating() ? QStringLiteral("停靠面板")
                                                  : QStringLiteral("浮动面板");
    mFloatButton->setVisible(canFloat);
    mFloatButton->setEnabled(canFloat);
    mFloatButton->setToolTip(floatText);
    mFloatButton->setAccessibleName(floatText);
    mFloatButton->setIcon(
        mDock->style()->standardIcon(QStyle::SP_TitleBarNormalButton));

    mCloseButton->setVisible(canClose);
    mCloseButton->setEnabled(canClose);
    mCloseButton->setToolTip(QStringLiteral("关闭面板"));
    mCloseButton->setAccessibleName(QStringLiteral("关闭面板"));
    mCloseButton->setIcon(
        mDock->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
  }

  QDockWidget *mDock = nullptr;
  QHBoxLayout *mLayout = nullptr;
  QLabel *mTitleLabel = nullptr;
  QToolButton *mFloatButton = nullptr;
  QToolButton *mCloseButton = nullptr;
};

void installDockTitleBar(QDockWidget *dock, const int scalePercent) {
  auto *titleBar = new DockTitleBar(dock);
  dock->setTitleBarWidget(titleBar);
  titleBar->applyScale(scalePercent);
}

void updateDockTitleBarScale(QDockWidget *dock, const int scalePercent) {
  if (dock == nullptr || dock->titleBarWidget() == nullptr ||
      dock->titleBarWidget()->objectName() != QStringLiteral("dockTitleBar")) {
    return;
  }
  static_cast<DockTitleBar *>(dock->titleBarWidget())->applyScale(scalePercent);
}

QLabel *createValueLabel(QWidget *parent = nullptr) {
  auto *label = new QLabel(QStringLiteral("-"), parent);
  label->setObjectName(QStringLiteral("mutedLabel"));
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setWordWrap(true);
  return label;
}

QLabel *createSectionTitle(const QString &text, QWidget *parent = nullptr) {
  auto *label = new QLabel(text, parent);
  label->setObjectName(QStringLiteral("sectionTitle"));
  return label;
}

QString compactPath(const QString &path) {
  return path.isEmpty() ? QStringLiteral("-") : QDir::toNativeSeparators(path);
}

QString comparablePath(const QString &path) {
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                             : canonical);
}

Qt::CaseSensitivity localPathCaseSensitivity() {
#ifdef Q_OS_WIN
  return Qt::CaseInsensitive;
#else
  return Qt::CaseSensitive;
#endif
}

bool pathsReferToSameLocation(const QString &left, const QString &right) {
  return comparablePath(left).compare(comparablePath(right),
                                      localPathCaseSensitivity()) == 0;
}

QString formatFileSize(const qint64 bytes) {
  if (bytes >= 1024LL * 1024LL * 1024LL) {
    return QStringLiteral("%1 GB").arg(
        static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
  }
  if (bytes >= 1024LL * 1024LL) {
    return QStringLiteral("%1 MB").arg(
        static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
  }
  return QStringLiteral("%1 KB").arg(std::max<qint64>(1, bytes / 1024));
}

QString safeFileName(QString value) {
  value.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*]+)")),
                QStringLiteral("_"));
  value = value.trimmed();
  return value.isEmpty() ? QStringLiteral("gaussian-scene") : value;
}

struct ResolvedTrainingPointCloud final {
  QString path;
  int iteration = 0;
  PlyMetadata metadata;
};

struct ExternalBackupTaskResult final {
  std::optional<ExternalBackupSnapshot> snapshot;
  QString error;
};

std::optional<ResolvedTrainingPointCloud>
resolveTrainingPointCloud(const QString &outputDirectory,
                          const std::optional<int> expectedIteration,
                          QString *errorMessage = nullptr) {
  const QDir pointCloudRoot(
      QDir(outputDirectory).filePath(QStringLiteral("point_cloud")));
  QList<int> iterations;
  if (expectedIteration.has_value()) {
    iterations.append(expectedIteration.value());
  } else if (pointCloudRoot.exists()) {
    static const QRegularExpression iterationPattern(
        QStringLiteral(R"(^iteration_(\d+)$)"));
    const QFileInfoList entries = pointCloudRoot.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo &entry : entries) {
      const QRegularExpressionMatch match =
          iterationPattern.match(entry.fileName());
      if (!match.hasMatch()) {
        continue;
      }
      bool parsed = false;
      const int iteration = match.captured(1).toInt(&parsed);
      if (parsed) {
        iterations.append(iteration);
      }
    }
    std::sort(iterations.begin(), iterations.end(), std::greater<int>());
  }

  QString lastError;
  for (const int iteration : iterations) {
    const QString path = pointCloudRoot.filePath(
        QStringLiteral("iteration_%1/point_cloud.ply").arg(iteration));
    QString inspectionError;
    const PlyMetadata metadata =
        WorkspaceDocument::inspectPly(path, &inspectionError);
    if (metadata.valid && metadata.vertexCount > 0 && metadata.fileSize > 0 &&
        metadata.looksLikeGaussianSplat()) {
      return ResolvedTrainingPointCloud{QFileInfo(path).absoluteFilePath(),
                                        iteration, metadata};
    }
    lastError = inspectionError.isEmpty()
                    ? QStringLiteral("PLY 缺少有效顶点或 3DGS 属性：%1")
                          .arg(QDir::toNativeSeparators(path))
                    : inspectionError;
  }

  if (errorMessage != nullptr) {
    if (!lastError.isEmpty()) {
      *errorMessage = lastError;
    } else if (expectedIteration.has_value()) {
      *errorMessage =
          QStringLiteral("未找到第 %1 次迭代的 point_cloud.ply：%2")
              .arg(expectedIteration.value())
              .arg(QDir::toNativeSeparators(pointCloudRoot.absolutePath()));
    } else {
      *errorMessage =
          QStringLiteral("尚未生成可用的 3DGS point_cloud.ply：%1")
              .arg(QDir::toNativeSeparators(pointCloudRoot.absolutePath()));
    }
  }
  return std::nullopt;
}

std::optional<ResolvedTrainingPointCloud> publishDurableTrainingPointCloud(
    const ResolvedTrainingPointCloud &checkpoint, const QString &projectRoot,
    const QString &outputDirectory, QString *errorMessage = nullptr) {
  const QString group = safeFileName(QFileInfo(outputDirectory).fileName());
  const QString destination =
      QDir(projectRoot)
          .filePath(QStringLiteral(
                        ".gsw/checkpoints/training/%1/iteration_%2/"
                        "point_cloud.ply")
                        .arg(group)
                        .arg(checkpoint.iteration));
  const DurableArtifact artifact = DurableArtifactStore::publish(
      checkpoint.path, destination, errorMessage);
  if (!artifact.isValid()) {
    return std::nullopt;
  }
  return ResolvedTrainingPointCloud{artifact.path, checkpoint.iteration,
                                    checkpoint.metadata};
}

QString workerStageLabel(const QString &stage) {
  static const QHash<QString, QString> labels = {
      {QStringLiteral("queued"), QStringLiteral("排队")},
      {QStringLiteral("preparing"), QStringLiteral("准备")},
      {QStringLiteral("archiving"), QStringLiteral("归档原文件")},
      {QStringLiteral("copying_images"), QStringLiteral("复制图像")},
      {QStringLiteral("extracting_frames"), QStringLiteral("视频抽帧")},
      {QStringLiteral("masks"), QStringLiteral("处理蒙版")},
      {QStringLiteral("finalizing"), QStringLiteral("提交数据集")},
      {QStringLiteral("environment"), QStringLiteral("检查环境")},
      {QStringLiteral("prepare"), QStringLiteral("准备训练")},
      {QStringLiteral("colmap"), QStringLiteral("COLMAP 重建")},
      {QStringLiteral("train"), QStringLiteral("训练")},
      {QStringLiteral("done"), QStringLiteral("完成")},
      {QStringLiteral("failed"), QStringLiteral("失败")},
      {QStringLiteral("cancelled"), QStringLiteral("已取消")},
  };
  return labels.value(stage, stage);
}

bool twoDgsAvailable(const QString &repositoryRoot) {
  QStringList candidates;
  const QString configured = qEnvironmentVariable("TWO_DGS_DIR");
  if (!configured.isEmpty()) {
    candidates.append(configured);
  }
  candidates.append(
      QDir(QDir::homePath()).filePath(QStringLiteral("Documents/2dgs")));
  candidates.append(QDir(repositoryRoot).filePath(QStringLiteral("2dgs")));
  return std::any_of(
      candidates.cbegin(), candidates.cend(), [](const QString &candidate) {
        return QFileInfo::exists(
                   QDir(candidate).filePath(QStringLiteral("train.py"))) &&
               QFileInfo::exists(QDir(candidate).filePath(
                   QStringLiteral(".venv/Scripts/python.exe")));
      });
}

QProcessEnvironment pythonProcessEnvironment(const QString &pythonPath) {
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  const QString prefix = QFileInfo(pythonPath).absolutePath();
  const QStringList pathParts = {
      prefix,
      QDir(prefix).filePath(QStringLiteral("Library/mingw-w64/bin")),
      QDir(prefix).filePath(QStringLiteral("Library/usr/bin")),
      QDir(prefix).filePath(QStringLiteral("Library/bin")),
      QDir(prefix).filePath(QStringLiteral("Scripts")),
      QDir(prefix).filePath(QStringLiteral("bin")),
      environment.value(QStringLiteral("PATH"))};
  environment.insert(QStringLiteral("PATH"),
                     pathParts.join(QDir::listSeparator()));
  environment.insert(QStringLiteral("CONDA_PREFIX"), prefix);
  environment.insert(QStringLiteral("GAUSSIAN_SPLATTING_CONDA_PREFIX"), prefix);
  environment.insert(QStringLiteral("GS_CONDA_PREFIX"), prefix);
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"),
                     QStringLiteral("1"));
  return environment;
}

bool hasImportRecoveryArtifacts(const QString &datasetRoot) {
  const QDir directory(datasetRoot);
  if (!directory.exists()) {
    return false;
  }

  static const QRegularExpression artifactPattern(QStringLiteral(
      R"(^\..+\.(?:import-transaction\.json|import-.+|backup-.+)$)"));
  const QFileInfoList entries = directory.entryInfoList(
      QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  return std::any_of(
      entries.cbegin(), entries.cend(), [](const QFileInfo &entry) {
        return artifactPattern.match(entry.fileName()).hasMatch();
      });
}

QString backendUnavailableMessage(const QString &repositoryRoot,
                                  const QString &workerScript,
                                  const QString &pythonPath) {
  const QString missing = QStringLiteral("<未发现>");
  const QString applicationDirectory =
      QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
  const QString backend = repositoryRoot.isEmpty()
                              ? missing
                              : QDir::toNativeSeparators(repositoryRoot);
  const QString worker = QFileInfo(workerScript).isFile()
                             ? QDir::toNativeSeparators(workerScript)
                             : missing;
  const QString python =
      pythonPath.isEmpty() ? missing : QDir::toNativeSeparators(pythonPath);

  QStringList checkedCandidates;
  const QStringList candidates =
      BackendLocator::gaussianPythonCandidates(repositoryRoot);
  constexpr qsizetype maximumDisplayedCandidates = 12;
  for (qsizetype index = 0;
       index < std::min(candidates.size(), maximumDisplayedCandidates);
       ++index) {
    checkedCandidates.append(
        QStringLiteral("  - %1").arg(candidates.at(index)));
  }
  if (candidates.size() > maximumDisplayedCandidates) {
    checkedCandidates.append(
        QStringLiteral("  - …另有 %1 个候选路径")
            .arg(candidates.size() - maximumDisplayedCandidates));
  }

  return QStringLiteral(
             "未找到可用的原生计算后端或 gaussian_splatting Python 环境。\n\n"
             "应用目录：%1\n"
             "后端目录：%2\n"
             "Worker：%3\n"
             "Python：%4\n\n"
             "已检查的 Python 候选路径：\n%5\n\n"
             "请确认安装包目录完整；如使用自定义位置，可设置 "
             "GSW_BACKEND_ROOT 和 GAUSSIAN_SPLATTING_CONDA_PREFIX。")
      .arg(applicationDirectory, backend, worker, python,
           checkedCandidates.isEmpty()
               ? QStringLiteral("  - <无候选路径>")
               : checkedCandidates.join(QLatin1Char('\n')));
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), mWorkspace(this), mProcessSupervisor(this) {
  const QStringList appArguments = qApp->arguments();
  mDiscardRecoveryOnDestruction =
      std::any_of(appArguments.cbegin(), appArguments.cend(),
                  [](const QString &argument) {
                    return argument.startsWith(QStringLiteral("--smoke-test"));
                  });
  mUiScalePercent = qApp->property("gswUiScalePercent").toInt();
  if (mUiScalePercent <= 0) {
    mUiScalePercent = 100;
  }
  mAutomaticUiScale = qApp->property("gswAutomaticUiScale").toBool();

  mUiAdaptTimer = new QTimer(this);
  mUiAdaptTimer->setSingleShot(true);
  mUiAdaptTimer->setInterval(160);
  connect(mUiAdaptTimer, &QTimer::timeout, this,
          &MainWindow::refreshAutomaticUiScale);

  QString recoveryBaseError;
  const QString recoveryBase =
      defaultUntitledWorkspaceBase(&recoveryBaseError);
  if (!recoveryBase.isEmpty()) {
    mRecoveryStore = std::make_unique<RecoveryStore>(recoveryBase);
    QString scanError;
    mStartupRecovery = mRecoveryStore->recoverableWorkspaces(&scanError);
    if (!scanError.isEmpty()) {
      recoveryBaseError = scanError;
    }
  }

  setObjectName(QStringLiteral("mainWindow"));
  setWindowTitle(QStringLiteral("Native Preview"));
  setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks |
                 QMainWindow::AllowTabbedDocks);
  setMinimumSize(940, 620);

  mViewport = new NativeViewport(this);
  setCentralWidget(mViewport);

  createActions();
  createProjectDock();
  createInspectorDock();
  createTaskDock();
  createMenus();
  createToolBars();
  createStatusBar();
  connectServices();
  restoreWindowState();
  applyUiScale(mUiScalePercent, false);
  scheduleAutomaticUiScale();
  QString untitledError;
  const bool untitledReady =
      beginUntitledProject(QStringLiteral("未命名工程"), &untitledError);
  updateWorkspaceUi();
  appendTaskEvent(QStringLiteral("原生桌面预览版已启动。"));
  if (untitledReady) {
    appendTaskEvent(
        QStringLiteral("已建立未命名工程；可先导入和处理，稍后再保存。"));
  } else {
    appendTaskEvent(
        QStringLiteral("无法建立未命名工程：%1").arg(untitledError));
    QTimer::singleShot(0, this, [this, untitledError]() {
      showError(QStringLiteral("无法准备临时工作区"), untitledError);
    });
  }
  if (!recoveryBaseError.isEmpty()) {
    appendTaskEvent(
        QStringLiteral("恢复存储检查提示：%1").arg(recoveryBaseError));
  }

  mRecoveryCheckpointTimer = new QTimer(this);
  mRecoveryCheckpointTimer->setInterval(30000);
  connect(mRecoveryCheckpointTimer, &QTimer::timeout, this, [this]() {
    checkpointCurrentRecovery();
    snapshotCurrentProject();
  });
  mRecoveryCheckpointTimer->start();
}

MainWindow::~MainWindow() {
  mProcessSupervisor.shutdown();
  if (mDiscardRecoveryOnDestruction) {
    discardCurrentRecovery();
  }
}

void MainWindow::moveEvent(QMoveEvent *event) {
  QMainWindow::moveEvent(event);
  scheduleAutomaticUiScale();
}

void MainWindow::resizeEvent(QResizeEvent *event) {
  QMainWindow::resizeEvent(event);
  scheduleAutomaticUiScale();
}

bool MainWindow::openProjectFile(const QString &filePath) {
  if (mProcessSupervisor.isRunning()) {
    QMessageBox::information(
        this, QStringLiteral("任务仍在运行"),
        QStringLiteral("请先停止或等待当前任务结束，再切换工程。"));
    return false;
  }
  if (!confirmDiscardChanges()) {
    return false;
  }
  mSuppressSnapshots = true;
  QString error;
  if (!mWorkspace.load(filePath, &error)) {
    mSuppressSnapshots = false;
    showError(QStringLiteral("无法打开工程"), error);
    return false;
  }
  if (mCurrentRecovery.has_value() &&
      !pathsReferToSameLocation(mCurrentRecovery->rootPath,
                                mWorkspace.rootPath())) {
    QString discardError;
    if (!discardCurrentRecovery(&discardError)) {
      appendTaskEvent(
          QStringLiteral("旧恢复工作区清理失败：%1").arg(discardError));
    }
  }
  mRecoveryBlocked = true;
  updateWorkspaceUi();
  statusBar()->showMessage(QStringLiteral("正在检查未完成的导入事务…"));
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const bool recovered = recoverInterruptedProjectImports(&error);
  QApplication::restoreOverrideCursor();
  statusBar()->clearMessage();
  if (!recovered) {
    updateWorkspaceUi();
    appendTaskEvent(QStringLiteral("工程已打开，但未完成的导入事务恢复失败：%1")
                        .arg(error));
    showError(QStringLiteral("无法恢复未完成的导入"),
              QStringLiteral(
                  "工程已打开，但数据集导入事务尚未恢复。请勿手动修改 datasets "
                  "目录中的隐藏事务文件；修复后重新打开工程即可重试。\n\n%1")
                  .arg(error));
    mSuppressSnapshots = false;
    return false;
  }
  mRecoveryBlocked = false;
  QString trainingRecoveryError;
  if (!recoverInterruptedTraining(&trainingRecoveryError)) {
    appendTaskEvent(
        QStringLiteral("工程已打开，但未完成训练的检查点恢复失败：%1")
            .arg(trainingRecoveryError));
    QMessageBox::warning(
        this, QStringLiteral("训练检查点恢复待处理"),
        QStringLiteral("工程和训练输出仍保留在磁盘上，但未能自动关联最近的"
                       "完整检查点。\n\n%1")
            .arg(trainingRecoveryError));
  }
  if (mWorkspace.hasPendingDataMigration()) {
    statusBar()->showMessage(QStringLiteral("正在完成上次中断的工程数据迁移…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool migrated = finalizePendingProjectSave(&error);
    QApplication::restoreOverrideCursor();
    statusBar()->clearMessage();
    if (!migrated) {
      appendTaskEvent(
          QStringLiteral("工程已打开，但待迁移数据尚未完成：%1").arg(error));
      showError(QStringLiteral("工程数据迁移待恢复"),
                QStringLiteral("工程清单已安全打开，当前数据仍保留在原工作区。"
                               "可修复目标位置后再次保存以重试迁移。\n\n%1")
                    .arg(error));
    }
  }

  if (mRecoveryStore != nullptr &&
      !mWorkspace.projectFilePath().isEmpty() &&
      !mWorkspace.hasPendingDataMigration()) {
    QString snapshotError;
    const QList<ProjectSnapshot> snapshots =
        mRecoveryStore->projectSnapshots(mWorkspace.rootPath(),
                                         &snapshotError);
    if (!snapshots.isEmpty()) {
      const ProjectSnapshot latest = snapshots.constFirst();
      const QByteArray snapshotJson =
          mRecoveryStore->projectSnapshotJson(latest, &snapshotError);
      const QJsonDocument snapshotDocument =
          QJsonDocument::fromJson(snapshotJson);
      const QJsonDocument currentDocument =
          QJsonDocument::fromJson(mWorkspace.recoveryManifestJson());
      if (snapshotDocument.isObject() && currentDocument.isObject()) {
        QJsonObject snapshotProject = snapshotDocument.object();
        QJsonObject currentProject = currentDocument.object();
        snapshotProject.remove(QStringLiteral("updatedUtc"));
        currentProject.remove(QStringLiteral("updatedUtc"));
        const QString snapshotRoot =
            snapshotProject.value(QStringLiteral("rootPath")).toString();
        const bool sameManagedRoot =
            QDir::isAbsolutePath(snapshotRoot) &&
            pathsReferToSameLocation(snapshotRoot, mWorkspace.rootPath());
        if (sameManagedRoot && snapshotProject != currentProject) {
          const QMessageBox::StandardButton recoverSnapshot =
              QMessageBox::question(
                  this, QStringLiteral("发现较新的自动恢复版本"),
                  QStringLiteral(
                      "检测到异常退出前保存的工程状态，与当前工程文件不同。"
                      "是否将它恢复为一个新工程文件？\n\n快照时间：%1")
                      .arg(latest.createdUtc.toLocalTime().toString(
                          QStringLiteral("yyyy-MM-dd HH:mm:ss"))),
                  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
          if (recoverSnapshot == QMessageBox::Yes) {
            const QFileInfo currentProjectFile(
                mWorkspace.projectFilePath());
            QString stem = currentProjectFile.fileName();
            const QString suffix = QStringLiteral(".gsw.json");
            if (stem.endsWith(suffix, Qt::CaseInsensitive)) {
              stem.chop(suffix.size());
            }
            const QString recoveredPath =
                QDir(currentProjectFile.absolutePath())
                    .filePath(
                        QStringLiteral("%1-autosave-recovered-%2.gsw.json")
                            .arg(stem,
                                 QDateTime::currentDateTime().toString(
                                     QStringLiteral("yyyyMMdd-HHmmss"))));
            if (mRecoveryStore->restoreProjectSnapshot(
                    latest, recoveredPath, &snapshotError) &&
                mWorkspace.load(recoveredPath, &snapshotError)) {
              appendTaskEvent(
                  QStringLiteral("已恢复异常退出前的自动快照：%1")
                      .arg(QDir::toNativeSeparators(recoveredPath)));
            } else {
              showError(QStringLiteral("无法恢复自动快照"),
                        snapshotError);
            }
          }
        }
      }
    }
    if (!snapshotError.isEmpty()) {
      appendTaskEvent(
          QStringLiteral("自动快照检查提示：%1").arg(snapshotError));
    }
  }
  mSuppressSnapshots = false;
  snapshotCurrentProject();
  updateWorkspaceUi();
  appendTaskEvent(
      QStringLiteral("已打开工程：%1").arg(QDir::toNativeSeparators(filePath)));
  return true;
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (mClosePending) {
    if (mProcessSupervisor.isRunning()) {
      event->ignore();
      return;
    }
    mClosePending = false;
    statusBar()->clearMessage();
    if (!confirmDiscardChanges(true)) {
      mExitConfirmed = false;
      event->ignore();
      return;
    }
    saveWindowState();
    QString discardError;
    if (!discardCurrentRecovery(&discardError)) {
      appendTaskEvent(
          QStringLiteral("退出时恢复工作区清理失败，将保留供下次恢复：%1")
              .arg(discardError));
    }
    mProcessSupervisor.shutdown();
    event->accept();
    return;
  }

  if (!confirmExit()) {
    event->ignore();
    return;
  }

  if (mProcessSupervisor.isRunning()) {
    mClosePending = true;
    statusBar()->showMessage(QStringLiteral("正在停止任务，完成后将关闭软件…"));
    mProcessSupervisor.stop();
    event->ignore();
    return;
  }
  if (!confirmDiscardChanges(true)) {
    mExitConfirmed = false;
    event->ignore();
    return;
  }
  saveWindowState();
  QString discardError;
  if (!discardCurrentRecovery(&discardError)) {
    appendTaskEvent(
        QStringLiteral("退出时恢复工作区清理失败，将保留供下次恢复：%1")
            .arg(discardError));
  }
  mProcessSupervisor.shutdown();
  event->accept();
}

void MainWindow::createActions() {
  mNewProjectAction = new QAction(style()->standardIcon(QStyle::SP_FileIcon),
                                  QStringLiteral("新建工程"), this);
  mNewProjectAction->setObjectName(QStringLiteral("newProjectAction"));
  mNewProjectAction->setShortcut(QKeySequence::New);
  mNewProjectAction->setToolTip(QStringLiteral("新建工程"));
  connect(mNewProjectAction, &QAction::triggered, this,
          &MainWindow::newProject);

  mOpenProjectAction =
      new QAction(style()->standardIcon(QStyle::SP_DialogOpenButton),
                  QStringLiteral("打开工程"), this);
  mOpenProjectAction->setShortcut(QKeySequence::Open);
  mOpenProjectAction->setToolTip(QStringLiteral("打开工程"));
  connect(mOpenProjectAction, &QAction::triggered, this, [this]() {
    const QString filePath = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开 Gaussian Scene Workbench 工程"), {},
        QStringLiteral(
            "GSW Project (*.gsw.json);;JSON (*.json);;All files (*.*)"));
    if (!filePath.isEmpty()) {
      openProjectFile(filePath);
    }
  });

  mSaveAction = new QAction(style()->standardIcon(QStyle::SP_DialogSaveButton),
                            QStringLiteral("保存工程"), this);
  mSaveAction->setObjectName(QStringLiteral("saveProjectAction"));
  mSaveAction->setShortcut(QKeySequence::Save);
  mSaveAction->setToolTip(QStringLiteral("保存工程"));
  connect(mSaveAction, &QAction::triggered, this,
          [this]() { saveProject(false); });

  mSaveAsAction = new QAction(QStringLiteral("工程另存为..."), this);
  mSaveAsAction->setObjectName(QStringLiteral("saveProjectAsAction"));
  mSaveAsAction->setShortcut(QKeySequence::SaveAs);
  connect(mSaveAsAction, &QAction::triggered, this,
          [this]() { saveProject(true); });

  mRecoveryCenterAction =
      new QAction(QStringLiteral("恢复中心..."), this);
  mRecoveryCenterAction->setObjectName(
      QStringLiteral("recoveryCenterAction"));
  mRecoveryCenterAction->setToolTip(
      QStringLiteral("恢复异常退出时保留的未命名工程"));
  connect(mRecoveryCenterAction, &QAction::triggered, this,
          [this]() { showRecoveryCenter(false); });

  mSnapshotHistoryAction =
      new QAction(QStringLiteral("版本历史..."), this);
  mSnapshotHistoryAction->setObjectName(
      QStringLiteral("snapshotHistoryAction"));
  mSnapshotHistoryAction->setToolTip(
      QStringLiteral("查看并恢复工程自动快照"));
  connect(mSnapshotHistoryAction, &QAction::triggered, this,
          &MainWindow::showSnapshotHistory);

  mConfigureBackupAction =
      new QAction(QStringLiteral("设置第二存储位置..."), this);
  mConfigureBackupAction->setObjectName(
      QStringLiteral("configureExternalBackupAction"));
  connect(mConfigureBackupAction, &QAction::triggered, this,
          &MainWindow::configureExternalBackup);

  mExternalBackupsAction =
      new QAction(QStringLiteral("外部备份与恢复..."), this);
  mExternalBackupsAction->setObjectName(
      QStringLiteral("externalBackupsAction"));
  connect(mExternalBackupsAction, &QAction::triggered, this,
          &MainWindow::showExternalBackups);

  mImportDatasetAction =
      new QAction(style()->standardIcon(QStyle::SP_DirOpenIcon),
                  QStringLiteral("添加照片/视频..."), this);
  mImportDatasetAction->setObjectName(QStringLiteral("importDatasetAction"));
  mImportDatasetAction->setToolTip(
      QStringLiteral("直接选择照片或视频；视频会自动抽帧"));
  connect(mImportDatasetAction, &QAction::triggered, this,
          &MainWindow::importDataset);

  mImportDatasetDirectoryAction =
      new QAction(style()->standardIcon(QStyle::SP_DirIcon),
                  QStringLiteral("添加媒体目录..."), this);
  mImportDatasetDirectoryAction->setObjectName(
      QStringLiteral("importDatasetDirectoryAction"));
  mImportDatasetDirectoryAction->setToolTip(
      QStringLiteral("递归添加目录中的照片与视频"));
  connect(mImportDatasetDirectoryAction, &QAction::triggered, this,
          &MainWindow::importDatasetDirectory);

  mAttachDatasetAction =
      new QAction(style()->standardIcon(QStyle::SP_DirLinkIcon),
                  QStringLiteral("关联已有数据集..."), this);
  mAttachDatasetAction->setObjectName(QStringLiteral("attachDatasetAction"));
  mAttachDatasetAction->setToolTip(QStringLiteral(
      "直接关联现有 images/input 与 COLMAP sparse 数据，不复制文件"));
  connect(mAttachDatasetAction, &QAction::triggered, this,
          &MainWindow::attachExistingDataset);

  mImportSceneAction =
      new QAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView),
                  QStringLiteral("导入高斯场景"), this);
  mImportSceneAction->setObjectName(QStringLiteral("importSceneAction"));
  mImportSceneAction->setToolTip(QStringLiteral("导入 PLY 高斯场景"));
  connect(mImportSceneAction, &QAction::triggered, this,
          &MainWindow::importScene);

  auto *environmentAction =
      new QAction(style()->standardIcon(QStyle::SP_BrowserReload),
                  QStringLiteral("检查环境"), this);
  environmentAction->setShortcut(QKeySequence(QStringLiteral("F6")));
  environmentAction->setToolTip(QStringLiteral("检查训练与重建环境"));
  connect(environmentAction, &QAction::triggered, this,
          &MainWindow::runEnvironmentCheck);
  environmentAction->setObjectName(QStringLiteral("environmentAction"));

  mReconstructAction =
      new QAction(style()->standardIcon(QStyle::SP_ComputerIcon),
                  QStringLiteral("COLMAP 重建..."), this);
  mReconstructAction->setShortcut(QKeySequence(QStringLiteral("F7")));
  mReconstructAction->setToolTip(QStringLiteral("计算相机位姿与稀疏点云 (F7)"));
  mReconstructAction->setObjectName(QStringLiteral("reconstructAction"));
  connect(mReconstructAction, &QAction::triggered, this,
          &MainWindow::startReconstruction);

  mTrainAction = new QAction(style()->standardIcon(QStyle::SP_MediaPlay),
                             QStringLiteral("开始训练..."), this);
  mTrainAction->setToolTip(QStringLiteral("启动当前工程训练"));
  connect(mTrainAction, &QAction::triggered, this, &MainWindow::startTraining);

  mStopAction = new QAction(style()->standardIcon(QStyle::SP_MediaStop),
                            QStringLiteral("停止任务"), this);
  mStopAction->setToolTip(QStringLiteral("停止当前任务"));
  mStopAction->setEnabled(false);
  connect(mStopAction, &QAction::triggered, &mProcessSupervisor,
          &ProcessSupervisor::stop);

  auto *resetCameraAction =
      new QAction(style()->standardIcon(QStyle::SP_BrowserReload),
                  QStringLiteral("重置视图"), this);
  resetCameraAction->setShortcut(QKeySequence(QStringLiteral("Home")));
  resetCameraAction->setToolTip(QStringLiteral("重置视图"));
  connect(resetCameraAction, &QAction::triggered, mViewport,
          &NativeViewport::resetCamera);
  resetCameraAction->setObjectName(QStringLiteral("resetCameraAction"));

  mRenderModeActionGroup = new QActionGroup(this);
  mRenderModeActionGroup->setExclusive(true);
  mGaussianRenderAction = new QAction(QStringLiteral("高斯"), this);
  mGaussianRenderAction->setObjectName(QStringLiteral("gaussianRenderAction"));
  mGaussianRenderAction->setCheckable(true);
  mGaussianRenderAction->setEnabled(false);
  mGaussianRenderAction->setToolTip(
      QStringLiteral("使用缩放、旋转与透明度显示屏幕空间高斯"));
  mRenderModeActionGroup->addAction(mGaussianRenderAction);
  connect(mGaussianRenderAction, &QAction::triggered, this, [this]() {
    mViewport->setRenderMode(NativeViewport::RenderMode::Gaussians);
  });

  mPointRenderAction = new QAction(QStringLiteral("点"), this);
  mPointRenderAction->setObjectName(QStringLiteral("pointRenderAction"));
  mPointRenderAction->setCheckable(true);
  mPointRenderAction->setChecked(true);
  mPointRenderAction->setToolTip(QStringLiteral("使用固定大小点预览场景"));
  mRenderModeActionGroup->addAction(mPointRenderAction);
  connect(mPointRenderAction, &QAction::triggered, this, [this]() {
    mViewport->setRenderMode(NativeViewport::RenderMode::Points);
  });

  mShowCamerasAction = new QAction(QStringLiteral("相机轨迹"), this);
  mShowCamerasAction->setObjectName(QStringLiteral("showCamerasAction"));
  mShowCamerasAction->setCheckable(true);
  mShowCamerasAction->setChecked(
      QSettings().value(QStringLiteral("view/showCameras"), false).toBool());
  mShowCamerasAction->setEnabled(false);
  mShowCamerasAction->setToolTip(
      QStringLiteral("显示 cameras.json 中的相机视锥和拍摄路径"));
  mViewport->setShowCameras(mShowCamerasAction->isChecked());
  connect(mShowCamerasAction, &QAction::toggled, this,
          [this](const bool enabled) {
            QSettings().setValue(QStringLiteral("view/showCameras"), enabled);
            mViewport->setShowCameras(enabled);
          });

  mEditModeActionGroup = new QActionGroup(this);
  mEditModeActionGroup->setExclusive(true);

  mInspectAction = new QAction(style()->standardIcon(QStyle::SP_ArrowUp),
                               QStringLiteral("查看"), this);
  mInspectAction->setCheckable(true);
  mInspectAction->setChecked(true);
  mInspectAction->setShortcut(QKeySequence(QStringLiteral("V")));
  mInspectAction->setToolTip(QStringLiteral("查看与导航 (V)"));
  mEditModeActionGroup->addAction(mInspectAction);
  connect(mInspectAction, &QAction::triggered, this, [this]() {
    mViewport->setInteractionMode(NativeViewport::InteractionMode::Inspect);
  });

  mRectangleAction =
      new QAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView),
                  QStringLiteral("框选"), this);
  mRectangleAction->setCheckable(true);
  mRectangleAction->setShortcut(QKeySequence(QStringLiteral("R")));
  mRectangleAction->setToolTip(
      QStringLiteral("矩形选择 (R)，Shift 添加，Alt 减去"));
  mEditModeActionGroup->addAction(mRectangleAction);
  connect(mRectangleAction, &QAction::triggered, this, [this]() {
    mViewport->setInteractionMode(NativeViewport::InteractionMode::Rectangle);
  });

  mLassoAction =
      new QAction(style()->standardIcon(QStyle::SP_FileDialogListView),
                  QStringLiteral("套索"), this);
  mLassoAction->setCheckable(true);
  mLassoAction->setShortcut(QKeySequence(QStringLiteral("L")));
  mLassoAction->setToolTip(
      QStringLiteral("套索选择 (L)，Shift 添加，Alt 减去"));
  mEditModeActionGroup->addAction(mLassoAction);
  connect(mLassoAction, &QAction::triggered, this, [this]() {
    mViewport->setInteractionMode(NativeViewport::InteractionMode::Lasso);
  });

  mBrushAction =
      new QAction(style()->standardIcon(QStyle::SP_FileDialogListView),
                  QStringLiteral("笔刷"), this);
  mBrushAction->setCheckable(true);
  mBrushAction->setShortcut(QKeySequence(QStringLiteral("B")));
  mBrushAction->setToolTip(
      QStringLiteral("连续笔刷选择 (B)，Shift 添加，Alt 减去"));
  mEditModeActionGroup->addAction(mBrushAction);
  connect(mBrushAction, &QAction::triggered, this, [this]() {
    mViewport->setInteractionMode(NativeViewport::InteractionMode::Brush);
  });

  mVisibleOnlyAction =
      new QAction(style()->standardIcon(QStyle::SP_DialogApplyButton),
                  QStringLiteral("仅选择可见点"), this);
  mVisibleOnlyAction->setCheckable(true);
  mVisibleOnlyAction->setChecked(true);
  mVisibleOnlyAction->setToolTip(QStringLiteral("仅选择当前视角可见的点"));
  connect(mVisibleOnlyAction, &QAction::toggled, mViewport,
          &NativeViewport::setVisibleOnlySelection);

  mClearSelectionAction =
      new QAction(style()->standardIcon(QStyle::SP_DialogResetButton),
                  QStringLiteral("清除选择"), this);
  mClearSelectionAction->setShortcut(QKeySequence(Qt::Key_Escape));
  mClearSelectionAction->setToolTip(QStringLiteral("清除选择 (Esc)"));
  connect(mClearSelectionAction, &QAction::triggered, mViewport,
          &NativeViewport::clearSelection);

  mInvertSelectionAction =
      new QAction(style()->standardIcon(QStyle::SP_BrowserReload),
                  QStringLiteral("反选"), this);
  mInvertSelectionAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
  mInvertSelectionAction->setToolTip(QStringLiteral("反选未删除的点 (Ctrl+I)"));
  connect(mInvertSelectionAction, &QAction::triggered, mViewport,
          &NativeViewport::invertSelection);

  mDeleteSelectionAction =
      new QAction(style()->standardIcon(QStyle::SP_TrashIcon),
                  QStringLiteral("删除所选"), this);
  mDeleteSelectionAction->setShortcut(QKeySequence::Delete);
  mDeleteSelectionAction->setToolTip(QStringLiteral("删除所选点 (Delete)"));
  connect(mDeleteSelectionAction, &QAction::triggered, mViewport,
          &NativeViewport::deleteSelection);

  mUndoEditAction = new QAction(style()->standardIcon(QStyle::SP_ArrowBack),
                                QStringLiteral("撤销删除"), this);
  mUndoEditAction->setShortcut(QKeySequence::Undo);
  mUndoEditAction->setToolTip(QStringLiteral("撤销删除 (Ctrl+Z)"));
  connect(mUndoEditAction, &QAction::triggered, mViewport,
          &NativeViewport::undoEdit);

  mRedoEditAction = new QAction(style()->standardIcon(QStyle::SP_ArrowForward),
                                QStringLiteral("重做删除"), this);
  mRedoEditAction->setShortcut(QKeySequence::Redo);
  mRedoEditAction->setToolTip(QStringLiteral("重做删除 (Ctrl+Y)"));
  connect(mRedoEditAction, &QAction::triggered, mViewport,
          &NativeViewport::redoEdit);

  mExportCropAction =
      new QAction(style()->standardIcon(QStyle::SP_DialogSaveButton),
                  QStringLiteral("裁剪另存为..."), this);
  mExportCropAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+S")));
  mExportCropAction->setToolTip(
      QStringLiteral("按原始顶点索引无损导出裁剪 PLY"));
  connect(mExportCropAction, &QAction::triggered, this,
          &MainWindow::exportCroppedScene);

  connect(mEditModeActionGroup, &QActionGroup::triggered, this,
          [this]() { updateEditActions(); });

  updateEditActions();

  auto *exitAction = new QAction(QStringLiteral("退出"), this);
  exitAction->setShortcut(QKeySequence::Quit);
  connect(exitAction, &QAction::triggered, this, &QWidget::close);
  exitAction->setObjectName(QStringLiteral("exitAction"));

  auto *resetLayoutAction = new QAction(QStringLiteral("重置工作区布局"), this);
  connect(resetLayoutAction, &QAction::triggered, this,
          &MainWindow::resetDockLayout);
  resetLayoutAction->setObjectName(QStringLiteral("resetLayoutAction"));

  addAction(mNewProjectAction);
  addAction(mOpenProjectAction);
  addAction(mSaveAsAction);
  addAction(environmentAction);
  addAction(resetCameraAction);
  addAction(exitAction);
  addAction(resetLayoutAction);
}

void MainWindow::createMenus() {
  QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
  fileMenu->addAction(actions().at(0));
  fileMenu->addAction(actions().at(1));
  fileMenu->addSeparator();
  fileMenu->addAction(mSaveAction);
  fileMenu->addAction(mSaveAsAction);
  fileMenu->addAction(mRecoveryCenterAction);
  fileMenu->addAction(mSnapshotHistoryAction);
  fileMenu->addSeparator();
  fileMenu->addAction(mConfigureBackupAction);
  fileMenu->addAction(mExternalBackupsAction);
  fileMenu->addSeparator();
  fileMenu->addAction(mImportDatasetAction);
  fileMenu->addAction(mImportDatasetDirectoryAction);
  fileMenu->addAction(mAttachDatasetAction);
  fileMenu->addAction(mImportSceneAction);
  fileMenu->addSeparator();
  fileMenu->addAction(actions().at(5));

  QMenu *editMenu = menuBar()->addMenu(QStringLiteral("编辑"));
  editMenu->addAction(mUndoEditAction);
  editMenu->addAction(mRedoEditAction);
  editMenu->addSeparator();
  editMenu->addAction(mClearSelectionAction);
  editMenu->addAction(mInvertSelectionAction);
  editMenu->addAction(mDeleteSelectionAction);

  QMenu *workflowMenu = menuBar()->addMenu(QStringLiteral("工作流"));
  workflowMenu->addAction(mImportDatasetAction);
  workflowMenu->addAction(mImportDatasetDirectoryAction);
  workflowMenu->addSeparator();
  workflowMenu->addAction(actions().at(3));
  workflowMenu->addSeparator();
  workflowMenu->addAction(mReconstructAction);
  workflowMenu->addAction(mTrainAction);
  workflowMenu->addAction(mStopAction);

  QMenu *sceneMenu = menuBar()->addMenu(QStringLiteral("场景"));
  sceneMenu->addAction(mImportSceneAction);
  sceneMenu->addAction(actions().at(4));
  sceneMenu->addSeparator();
  sceneMenu->addAction(mInspectAction);
  sceneMenu->addAction(mRectangleAction);
  sceneMenu->addAction(mLassoAction);
  sceneMenu->addAction(mBrushAction);
  sceneMenu->addAction(mVisibleOnlyAction);
  sceneMenu->addSeparator();
  sceneMenu->addAction(mExportCropAction);

  QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
  QMenu *renderMenu = viewMenu->addMenu(QStringLiteral("渲染模式"));
  renderMenu->addAction(mGaussianRenderAction);
  renderMenu->addAction(mPointRenderAction);
  viewMenu->addAction(mShowCamerasAction);
  viewMenu->addSeparator();
  viewMenu->addAction(mProjectDock->toggleViewAction());
  viewMenu->addAction(mInspectorDock->toggleViewAction());
  viewMenu->addAction(mTaskDock->toggleViewAction());
  viewMenu->addSeparator();
  viewMenu->addAction(actions().at(6));

  QMenu *displayMenu = viewMenu->addMenu(QStringLiteral("显示与适配"));
  displayMenu->setObjectName(QStringLiteral("displaySettingsMenu"));
  mAutoScaleAction =
      displayMenu->addAction(QStringLiteral("自动适配界面（推荐）"));
  mAutoScaleAction->setObjectName(QStringLiteral("autoUiScaleAction"));
  mAutoScaleAction->setCheckable(true);
  mAutoScaleAction->setChecked(mAutomaticUiScale);
  mAutoScaleAction->setToolTip(
      QStringLiteral("根据当前屏幕和窗口分辨率自动调整文字、控件与图标"));
  connect(mAutoScaleAction, &QAction::triggered, this,
          [this]() { setAutomaticUiScale(true, true); });

  QMenu *scaleMenu = displayMenu->addMenu(QStringLiteral("手动界面比例"));
  mScaleActionGroup = new QActionGroup(this);
  mScaleActionGroup->setExclusive(true);
  const QList<int> scales = {90, 100, 110, 125, 150};
  for (const int scale : scales) {
    auto *action = scaleMenu->addAction(QStringLiteral("%1%").arg(scale));
    action->setCheckable(true);
    action->setData(scale);
    action->setChecked(!mAutomaticUiScale && scale == mUiScalePercent);
    mScaleActionGroup->addAction(action);
  }
  connect(mScaleActionGroup, &QActionGroup::triggered, this,
          [this](QAction *action) {
            mAutomaticUiScale = false;
            AppTheme::saveScaleMode(UiScaleMode::Manual);
            applyUiScale(action->data().toInt(), true);
          });

  displayMenu->addSeparator();
  QMenu *resolutionMenu = displayMenu->addMenu(QStringLiteral("窗口分辨率"));
  auto *fitWindowAction =
      resolutionMenu->addAction(QStringLiteral("适合当前屏幕（自动）"));
  fitWindowAction->setObjectName(QStringLiteral("fitWindowToScreenAction"));
  connect(fitWindowAction, &QAction::triggered, this,
          &MainWindow::fitWindowToScreen);

  resolutionMenu->addSeparator();
  const QList<QSize> resolutions = {QSize(1280, 720), QSize(1600, 900),
                                    QSize(1920, 1080), QSize(2560, 1440)};
  for (const QSize &resolution : resolutions) {
    auto *action = resolutionMenu->addAction(QStringLiteral("%1 × %2")
                                                 .arg(resolution.width())
                                                 .arg(resolution.height()));
    action->setObjectName(QStringLiteral("windowResolution%1x%2Action")
                              .arg(resolution.width())
                              .arg(resolution.height()));
    action->setData(resolution);
    connect(action, &QAction::triggered, this,
            [this, resolution]() { applyWindowResolution(resolution); });
  }

  QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
  auto *aboutAction =
      helpMenu->addAction(QStringLiteral("关于 Gaussian Scene Workbench"));
  connect(aboutAction, &QAction::triggered, this, [this]() {
    QMessageBox::about(
        this, QStringLiteral("关于 Gaussian Scene Workbench"),
        QStringLiteral(
            "<b>Gaussian Scene Workbench 0.3.0 Native Preview</b><br>"
            "高斯场景研究工作台<br><br>"
            "Qt 6 原生桌面架构，构建日期 %1。<br>"
            "当前为原生桌面预览通道；稳定版仍保留在 main。")
            .arg(QStringLiteral(GSW_RELEASE_DATE)));
  });
}

void MainWindow::createToolBars() {
  auto *mainToolbar = addToolBar(QStringLiteral("主工具"));
  mainToolbar->setObjectName(QStringLiteral("mainToolbar"));
  mainToolbar->setMovable(false);
  mainToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  mainToolbar->addAction(actions().at(0));
  mainToolbar->addAction(actions().at(1));
  mainToolbar->addAction(mSaveAction);
  mainToolbar->addSeparator();
  mainToolbar->addAction(mImportDatasetAction);
  mainToolbar->addAction(mImportSceneAction);
  mainToolbar->addSeparator();
  mainToolbar->addAction(mReconstructAction);
  mainToolbar->addAction(mTrainAction);
  mainToolbar->addAction(mStopAction);

  addToolBarBreak(Qt::TopToolBarArea);
  mRenderToolbar = addToolBar(QStringLiteral("渲染模式"));
  mRenderToolbar->setObjectName(QStringLiteral("renderToolbar"));
  mRenderToolbar->setMovable(false);
  mRenderToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  mRenderToolbar->addAction(mGaussianRenderAction);
  mRenderToolbar->addAction(mPointRenderAction);
  mRenderToolbar->addSeparator();
  mRenderToolbar->addAction(mShowCamerasAction);

  mSelectionToolbar = addToolBar(QStringLiteral("选择模式"));
  mSelectionToolbar->setObjectName(QStringLiteral("selectionToolbar"));
  mSelectionToolbar->setMovable(false);
  mSelectionToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  mSelectionToolbar->addAction(mInspectAction);
  mSelectionToolbar->addAction(mRectangleAction);
  mSelectionToolbar->addAction(mLassoAction);
  mSelectionToolbar->addAction(mBrushAction);
  mSelectionToolbar->addAction(mVisibleOnlyAction);
  mSelectionToolbar->addSeparator();
  auto *brushRadiusLabel =
      new QLabel(QStringLiteral("半径"), mSelectionToolbar);
  brushRadiusLabel->setObjectName(QStringLiteral("mutedLabel"));
  mSelectionToolbar->addWidget(brushRadiusLabel);
  mBrushRadiusSpin = new QSpinBox(mSelectionToolbar);
  mBrushRadiusSpin->setObjectName(QStringLiteral("brushRadiusSpin"));
  mBrushRadiusSpin->setAccessibleName(QStringLiteral("笔刷半径"));
  mBrushRadiusSpin->setRange(4, 256);
  mBrushRadiusSpin->setSingleStep(4);
  mBrushRadiusSpin->setSuffix(QStringLiteral(" px"));
  mBrushRadiusSpin->setKeyboardTracking(false);
  mBrushRadiusSpin->setMinimumWidth(86);
  mBrushRadiusSpin->setMaximumWidth(110);
  const int savedBrushRadius = std::clamp(
      QSettings().value(QStringLiteral("selection/brushRadius"), 32).toInt(), 4,
      256);
  mBrushRadiusSpin->setValue(savedBrushRadius);
  mViewport->setBrushRadius(savedBrushRadius);
  connect(mBrushRadiusSpin, &QSpinBox::valueChanged, this,
          [this](const int radius) {
            mViewport->setBrushRadius(radius);
            QSettings().setValue(QStringLiteral("selection/brushRadius"),
                                 radius);
          });
  mSelectionToolbar->addWidget(mBrushRadiusSpin);

  mEditToolbar = addToolBar(QStringLiteral("编辑操作"));
  mEditToolbar->setObjectName(QStringLiteral("editToolbar"));
  mEditToolbar->setMovable(false);
  mEditToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  mEditToolbar->addAction(mClearSelectionAction);
  mEditToolbar->addAction(mInvertSelectionAction);
  mEditToolbar->addAction(mDeleteSelectionAction);
  mEditToolbar->addSeparator();
  mEditToolbar->addAction(mUndoEditAction);
  mEditToolbar->addAction(mRedoEditAction);
  mEditToolbar->addAction(mExportCropAction);

  mRenderToolbar->hide();
  mSelectionToolbar->hide();
  mEditToolbar->hide();
}

void MainWindow::createProjectDock() {
  mProjectDock = new QDockWidget(QStringLiteral("工程"), this);
  mProjectDock->setObjectName(QStringLiteral("projectDock"));
  mProjectDock->setAllowedAreas(Qt::LeftDockWidgetArea |
                                Qt::RightDockWidgetArea);
  installDockTitleBar(mProjectDock, mUiScalePercent);
  mProjectTree = new QTreeWidget(mProjectDock);
  mProjectTree->setHeaderHidden(true);
  mProjectTree->setAlternatingRowColors(false);
  mProjectTree->setUniformRowHeights(true);
  mProjectTree->setSelectionMode(QAbstractItemView::SingleSelection);
  mProjectDock->setWidget(mProjectTree);
  addDockWidget(Qt::LeftDockWidgetArea, mProjectDock);

  connect(mProjectTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
    const QList<QTreeWidgetItem *> selection = mProjectTree->selectedItems();
    if (selection.isEmpty()) {
      return;
    }
    const QString path = selection.first()->data(0, Qt::UserRole).toString();
    if (!path.isEmpty()) {
      statusBar()->showMessage(QDir::toNativeSeparators(path), 5000);
    }
  });
}

void MainWindow::createInspectorDock() {
  mInspectorDock = new QDockWidget(QStringLiteral("属性"), this);
  mInspectorDock->setObjectName(QStringLiteral("inspectorDock"));
  mInspectorDock->setAllowedAreas(Qt::LeftDockWidgetArea |
                                  Qt::RightDockWidgetArea);
  installDockTitleBar(mInspectorDock, mUiScalePercent);

  auto *scrollArea = new QScrollArea(mInspectorDock);
  scrollArea->setWidgetResizable(true);
  scrollArea->setFrameShape(QFrame::NoFrame);
  auto *panel = new QFrame(scrollArea);
  panel->setObjectName(QStringLiteral("inspectorPanel"));
  auto *layout = new QVBoxLayout(panel);
  layout->setContentsMargins(12, 4, 12, 12);
  layout->setSpacing(8);

  layout->addWidget(createSectionTitle(QStringLiteral("工程"), panel));
  auto *projectForm = new QFormLayout();
  projectForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  projectForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
  projectForm->setHorizontalSpacing(12);
  projectForm->setVerticalSpacing(7);
  mProjectNameValue = createValueLabel(panel);
  mProjectNameValue->setObjectName(QStringLiteral("projectNameValue"));
  mProjectRootValue = createValueLabel(panel);
  mProjectRootValue->setObjectName(QStringLiteral("projectRootValue"));
  projectForm->addRow(QStringLiteral("名称"), mProjectNameValue);
  projectForm->addRow(QStringLiteral("路径"), mProjectRootValue);
  layout->addLayout(projectForm);

  layout->addWidget(createSectionTitle(QStringLiteral("数据集"), panel));
  auto *datasetForm = new QFormLayout();
  datasetForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  datasetForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
  datasetForm->setHorizontalSpacing(12);
  datasetForm->setVerticalSpacing(7);
  mDatasetValue = createValueLabel(panel);
  mImageCountValue = createValueLabel(panel);
  datasetForm->addRow(QStringLiteral("目录"), mDatasetValue);
  datasetForm->addRow(QStringLiteral("图像"), mImageCountValue);
  layout->addLayout(datasetForm);

  layout->addWidget(createSectionTitle(QStringLiteral("场景"), panel));
  auto *sceneForm = new QFormLayout();
  sceneForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  sceneForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
  sceneForm->setHorizontalSpacing(12);
  sceneForm->setVerticalSpacing(7);
  mSceneValue = createValueLabel(panel);
  mGaussianCountValue = createValueLabel(panel);
  mPlyFormatValue = createValueLabel(panel);
  mCameraCountValue = createValueLabel(panel);
  sceneForm->addRow(QStringLiteral("文件"), mSceneValue);
  sceneForm->addRow(QStringLiteral("数量"), mGaussianCountValue);
  sceneForm->addRow(QStringLiteral("格式"), mPlyFormatValue);
  sceneForm->addRow(QStringLiteral("相机"), mCameraCountValue);
  layout->addLayout(sceneForm);
  layout->addStretch(1);

  scrollArea->setWidget(panel);
  mInspectorDock->setWidget(scrollArea);
  addDockWidget(Qt::RightDockWidgetArea, mInspectorDock);
}

void MainWindow::createTaskDock() {
  mTaskDock = new QDockWidget(QStringLiteral("任务与日志"), this);
  mTaskDock->setObjectName(QStringLiteral("taskDock"));
  mTaskDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  installDockTitleBar(mTaskDock, mUiScalePercent);

  auto *tabs = new QTabWidget(mTaskDock);
  mTaskTable = new QTableWidget(0, 4, tabs);
  mTaskTable->setHorizontalHeaderLabels(
      {QStringLiteral("状态"), QStringLiteral("任务"),
       QStringLiteral("开始时间"), QStringLiteral("结果")});
  mTaskTable->setAlternatingRowColors(true);
  mTaskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  mTaskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  mTaskTable->verticalHeader()->setVisible(false);
  mTaskTable->horizontalHeader()->setSectionResizeMode(
      0, QHeaderView::ResizeToContents);
  mTaskTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  mTaskTable->horizontalHeader()->setSectionResizeMode(
      2, QHeaderView::ResizeToContents);
  mTaskTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

  mConsole = new QPlainTextEdit(tabs);
  mConsole->setReadOnly(true);
  mConsole->setMaximumBlockCount(10000);
  mConsole->setLineWrapMode(QPlainTextEdit::NoWrap);

  tabs->addTab(mTaskTable, QStringLiteral("任务"));
  tabs->addTab(mConsole, QStringLiteral("日志"));
  mTaskDock->setWidget(tabs);
  addDockWidget(Qt::BottomDockWidgetArea, mTaskDock);
}

void MainWindow::createStatusBar() {
  mProjectStatus = new QLabel(QStringLiteral("未打开工程"), this);
  mProjectStatus->setObjectName(QStringLiteral("mutedLabel"));
  mRendererStatus = new QLabel(QStringLiteral("原生点预览 | 未载入场景"), this);
  mRendererStatus->setObjectName(QStringLiteral("statusWarn"));
  mEditStatus = new QLabel(QStringLiteral("选择 0 | 删除 0"), this);
  mEditStatus->setObjectName(QStringLiteral("mutedLabel"));
  mScaleStatus = new QLabel(this);
  mScaleStatus->setObjectName(QStringLiteral("uiScaleStatus"));
  statusBar()->addWidget(mProjectStatus, 1);
  statusBar()->addPermanentWidget(mRendererStatus);
  statusBar()->addPermanentWidget(mEditStatus);
  statusBar()->addPermanentWidget(mScaleStatus);
}

void MainWindow::connectServices() {
  connect(&mWorkspace, &WorkspaceDocument::changed, this,
          &MainWindow::updateWorkspaceUi);
  connect(&mWorkspace, &WorkspaceDocument::changed, this,
          &MainWindow::checkpointCurrentRecovery);
  connect(&mWorkspace, &WorkspaceDocument::changed, this,
          &MainWindow::snapshotCurrentProject);
  connect(&mWorkspace, &WorkspaceDocument::modifiedChanged, this,
          [this](const bool modified) { setWindowModified(modified); });
  connect(&mProcessSupervisor, &ProcessSupervisor::runningChanged, this,
          [this](const bool) { updateActionAvailability(); });
  connect(&mProcessSupervisor, &ProcessSupervisor::taskStarted, this,
          [this](const QString &taskName) {
            mActiveWorkerState.clear();
            mActiveTaskRow = mTaskTable->rowCount();
            mTaskTable->insertRow(mActiveTaskRow);
            auto *state = new QTableWidgetItem(QStringLiteral("运行中"));
            state->setForeground(QColor(102, 193, 168));
            mTaskTable->setItem(mActiveTaskRow, 0, state);
            mTaskTable->setItem(mActiveTaskRow, 1,
                                new QTableWidgetItem(taskName));
            mTaskTable->setItem(
                mActiveTaskRow, 2,
                new QTableWidgetItem(QDateTime::currentDateTime().toString(
                    QStringLiteral("HH:mm:ss"))));
            mTaskTable->setItem(mActiveTaskRow, 3,
                                new QTableWidgetItem(QStringLiteral("-")));
            appendTaskEvent(QStringLiteral("开始任务：%1").arg(taskName));
          });
  connect(&mProcessSupervisor, &ProcessSupervisor::outputReady, this,
          &MainWindow::appendLog);
  connect(&mProcessSupervisor, &ProcessSupervisor::workerStatusReady, this,
          [this](const WorkerStatus &status) {
            mActiveWorkerState = status.state;
            if (mActiveTaskRow < 0 ||
                mActiveTaskRow >= mTaskTable->rowCount()) {
              return;
            }
            QString detail = workerStageLabel(status.stage);
            if (status.progressPercent.has_value()) {
              detail +=
                  QStringLiteral(" · %1%").arg(status.progressPercent.value());
            }
            mTaskTable->item(mActiveTaskRow, 3)->setText(detail);
          });
  connect(
      &mProcessSupervisor, &ProcessSupervisor::taskFinished, this,
      [this](const QString &taskName, const int exitCode,
             const bool succeeded) {
        const bool processCancelled =
            mActiveWorkerState == QStringLiteral("cancelled") ||
            exitCode == 130 || mProcessSupervisor.wasStopRequested();
        bool effectiveSucceeded = succeeded;
        bool recoveryFailed = false;
        QString completionDetail;

        if (mPendingDatasetImport.has_value() &&
            mPendingDatasetImport->taskName == taskName) {
          const PendingDatasetImport pending = *mPendingDatasetImport;
          mPendingDatasetImport.reset();
          QString recoveryError;
          bool committed = false;
          if (!recoverDatasetImport(pending, &recoveryError, &committed)) {
            recoveryFailed = true;
            effectiveSucceeded = false;
            mClosePending = false;
            mExitConfirmed = false;
            if (mWorkspace.hasProject() &&
                pathsReferToSameLocation(mWorkspace.rootPath(),
                                         pending.projectRoot)) {
              mRecoveryBlocked = true;
              updateWorkspaceUi();
            }
            statusBar()->clearMessage();
            appendTaskEvent(
                QStringLiteral("导入事务恢复失败：%1").arg(recoveryError));
            showError(
                QStringLiteral("无法恢复导入事务"),
                QStringLiteral(
                    "任务已结束，但无法确认数据集事务状态。为避免丢失数据，"
                    "软件不会自动关闭；请修复问题后重新打开工程。\n\n%1")
                    .arg(recoveryError));
          } else {
            effectiveSucceeded = succeeded || committed;
            if (committed && !succeeded) {
              appendTaskEvent(QStringLiteral("导入进程虽已中断，但数据集提交已"
                                             "完成；将保留并关联新数据集。"));
              if (mActiveTaskRow >= 0 &&
                  mActiveTaskRow < mTaskTable->rowCount()) {
                mTaskTable->item(mActiveTaskRow, 3)
                    ->setText(QStringLiteral("提交已确认"));
              }
            } else if (!effectiveSucceeded) {
              appendTaskEvent(QStringLiteral(
                  "已清理未完成的导入事务；提交前的数据已恢复。"));
            }
          }

          if (effectiveSucceeded &&
              !pathsReferToSameLocation(mWorkspace.rootPath(),
                                        pending.projectRoot)) {
            appendTaskEvent(
                QStringLiteral("数据集已导入到原工程，但当前工程已切换，因此未"
                               "自动关联：%1")
                    .arg(QDir::toNativeSeparators(pending.datasetPath)));
          } else if (effectiveSucceeded) {
            QString error;
            if (WorkspaceDocument::countDatasetImages(pending.datasetPath) <=
                    0 ||
                !mWorkspace.setDatasetPath(pending.datasetPath, &error)) {
              showError(
                  QStringLiteral("导入结果无效"),
                  error.isEmpty()
                      ? QStringLiteral(
                            "导入任务完成，但目标数据集中没有可用图像：%1")
                            .arg(QDir::toNativeSeparators(pending.datasetPath))
                      : error);
            } else {
              QString saveError;
              if (!mWorkspace.projectFilePath().isEmpty() &&
                  !mWorkspace.saveManifest({}, &saveError)) {
                appendTaskEvent(
                    QStringLiteral("数据集已导入，但工程自动保存失败：%1")
                        .arg(saveError));
              }
              appendTaskEvent(
                  QStringLiteral("当前数据集已切换到：%1")
                      .arg(QDir::toNativeSeparators(pending.datasetPath)));
            }
          }
        }

        if (mPendingTraining.has_value() &&
            mPendingTraining->taskName == taskName) {
          const PendingTraining pending = *mPendingTraining;
          mPendingTraining.reset();
          if (effectiveSucceeded) {
            QString resultError;
            std::optional<ResolvedTrainingPointCloud> result =
                resolveTrainingPointCloud(pending.outputDirectory,
                                          pending.expectedIterations,
                                          &resultError);
            if (!result.has_value()) {
              effectiveSucceeded = false;
              completionDetail = QStringLiteral("训练输出无效");
              showError(QStringLiteral("训练结果无效"),
                        QStringLiteral(
                            "训练进程已正常退出，但最终模型未通过校验。\n\n%1")
                            .arg(resultError));
            } else {
              QString durableError;
              const std::optional<ResolvedTrainingPointCloud> durable =
                  publishDurableTrainingPointCloud(
                      *result, pending.projectRoot, pending.outputDirectory,
                      &durableError);
              if (durable.has_value()) {
                result = durable;
                appendTaskEvent(
                    QStringLiteral("训练检查点已通过校验并原子发布：%1")
                        .arg(QDir::toNativeSeparators(result->path)));
              } else {
                appendTaskEvent(
                    QStringLiteral("训练结果有效，但保护副本发布失败；"
                                   "仍保留原始输出：%1")
                        .arg(durableError));
              }
            }
            if (result.has_value() &&
                !pathsReferToSameLocation(mWorkspace.rootPath(),
                                                 pending.projectRoot)) {
              completionDetail = QStringLiteral("模型已生成（当前工程已切换）");
              appendTaskEvent(
                  QStringLiteral(
                      "训练模型已生成，但当前工程已切换，未自动关联：%1")
                      .arg(QDir::toNativeSeparators(result->path)));
            } else if (result.has_value()) {
              QString sceneError;
              if (!mWorkspace.setScenePath(result->path, &sceneError)) {
                effectiveSucceeded = false;
                completionDetail = QStringLiteral("模型关联失败");
                showError(QStringLiteral("无法载入训练结果"), sceneError);
              } else {
                completionDetail = QStringLiteral("迭代 %1 · %2 个高斯")
                                       .arg(result->iteration)
                                       .arg(result->metadata.vertexCount);
                QString saveError;
                if (!mWorkspace.projectFilePath().isEmpty() &&
                    !mWorkspace.saveManifest({}, &saveError)) {
                  QMessageBox::warning(
                      this, QStringLiteral("训练结果已载入，但工程未保存"),
                      QStringLiteral(
                          "模型已载入视口，但工程文件自动保存失败。\n\n%1")
                          .arg(saveError));
                  appendTaskEvent(
                      QStringLiteral("训练结果已载入，但工程自动保存失败：%1")
                          .arg(saveError));
                }
                appendTaskEvent(
                    QStringLiteral(
                        "训练结果已校验并载入：迭代 %1，%2 个高斯，%3")
                        .arg(result->iteration)
                        .arg(result->metadata.vertexCount)
                        .arg(QDir::toNativeSeparators(result->path)));
                statusBar()->showMessage(
                    QStringLiteral("%1 训练完成，模型已载入")
                        .arg(pending.backend.toUpper()),
                    8000);
              }
            }
          } else {
            std::optional<ResolvedTrainingPointCloud> partial =
                resolveTrainingPointCloud(pending.outputDirectory,
                                          std::nullopt);
            if (partial.has_value()) {
              QString durableError;
              const std::optional<ResolvedTrainingPointCloud> durable =
                  publishDurableTrainingPointCloud(
                      *partial, pending.projectRoot, pending.outputDirectory,
                      &durableError);
              if (durable.has_value()) {
                partial = durable;
              } else {
                appendTaskEvent(
                    QStringLiteral("最近检查点有效，但保护副本发布失败：%1")
                        .arg(durableError));
              }
              completionDetail =
                  QStringLiteral("保留迭代 %1").arg(partial->iteration);
              appendTaskEvent(
                  QStringLiteral(
                      "训练未完成，已保留最近可用检查点：迭代 %1，%2")
                      .arg(partial->iteration)
                      .arg(QDir::toNativeSeparators(partial->path)));
              if ((mClosePending || mWorkspace.hasPendingDataMigration()) &&
                  pathsReferToSameLocation(mWorkspace.rootPath(),
                                           pending.projectRoot)) {
                QString sceneError;
                if (mWorkspace.setScenePath(partial->path, &sceneError)) {
                  completionDetail = QStringLiteral("保留迭代 %1 · 等待保存")
                                         .arg(partial->iteration);
                  appendTaskEvent(
                      QStringLiteral("退出前已将最近训练检查点关联到当前工程；"
                                     "请在保存确认中选择是否保留。"));
                } else {
                  appendTaskEvent(
                      QStringLiteral("无法将保留的训练检查点关联到工程：%1")
                          .arg(sceneError));
                }
              }
            }
          }
          QString recoveryRecordError;
          if (!clearActiveTrainingJob(pending.projectRoot,
                                      &recoveryRecordError)) {
            appendTaskEvent(
                QStringLiteral("训练已结束，但恢复标记清理失败：%1")
                    .arg(recoveryRecordError));
          }
        }

        const bool cancelled =
            processCancelled && !effectiveSucceeded && !recoveryFailed;
        if (mActiveTaskRow >= 0 && mActiveTaskRow < mTaskTable->rowCount()) {
          auto *state = mTaskTable->item(mActiveTaskRow, 0);
          state->setText(effectiveSucceeded ? QStringLiteral("完成")
                         : cancelled        ? QStringLiteral("已取消")
                                            : QStringLiteral("失败"));
          state->setForeground(effectiveSucceeded ? QColor(102, 193, 168)
                               : cancelled        ? QColor(218, 169, 82)
                                                  : QColor(211, 95, 95));
          if (!effectiveSucceeded) {
            mTaskTable->item(mActiveTaskRow, 3)
                ->setText(!completionDetail.isEmpty() ? completionDetail
                          : recoveryFailed ? QStringLiteral("事务恢复失败")
                          : cancelled
                              ? QStringLiteral("用户取消")
                              : QStringLiteral("退出码 %1").arg(exitCode));
          } else if (!completionDetail.isEmpty()) {
            mTaskTable->item(mActiveTaskRow, 3)->setText(completionDetail);
          }
        }

        const QString outcome = effectiveSucceeded ? QStringLiteral("完成")
                                : cancelled        ? QStringLiteral("取消")
                                                   : QStringLiteral("失败");
        appendTaskEvent(QStringLiteral("任务%1：%2").arg(outcome, taskName));
        if (!recoveryFailed && mWorkspace.hasPendingDataMigration()) {
          QString migrationError;
          statusBar()->showMessage(
              QStringLiteral("任务已结束，正在完成工程数据迁移…"));
          QApplication::setOverrideCursor(Qt::WaitCursor);
          const bool migrated = finalizePendingProjectSave(&migrationError);
          QApplication::restoreOverrideCursor();
          statusBar()->clearMessage();
          if (!migrated) {
            mClosePending = false;
            mExitConfirmed = false;
            completionDetail = QStringLiteral("工程迁移待恢复");
            if (mActiveTaskRow >= 0 &&
                mActiveTaskRow < mTaskTable->rowCount()) {
              mTaskTable->item(mActiveTaskRow, 3)->setText(completionDetail);
            }
            appendTaskEvent(QStringLiteral("任务已结束，但工程数据迁移失败：%1")
                                .arg(migrationError));
            showError(QStringLiteral("无法完成工程数据迁移"),
                      QStringLiteral(
                          "任务结果仍保留在原工作区，工程清单也已保存。"
                          "软件不会自动关闭；请修复目标位置后再次保存。\n\n%1")
                          .arg(migrationError));
          } else {
            snapshotCurrentProject();
            startExternalBackup(false);
          }
        }
        mActiveTaskRow = -1;
        mActiveWorkerState.clear();
        rebuildProjectTree();
        updateInspector();
        if (mClosePending) {
          QTimer::singleShot(0, this, &QWidget::close);
        }
      });
  connect(mViewport, &NativeViewport::frameTimeChanged, this,
          [this](const double milliseconds) {
            if (!mWorkspace.scenePath().isEmpty()) {
              const QString renderer =
                  mRenderMode == NativeViewport::RenderMode::Gaussians
                      ? QStringLiteral("高斯预览")
                      : QStringLiteral("点预览");
              mRendererStatus->setText(QStringLiteral("%1 | CPU 提交 %2 ms")
                                           .arg(renderer)
                                           .arg(milliseconds, 0, 'f', 2));
            }
          });
  connect(mViewport, &NativeViewport::sceneLoadStarted, this,
          [this](const QString &scenePath) {
            mRendererStatus->setText(QStringLiteral("正在读取点云"));
            appendTaskEvent(QStringLiteral("读取场景：%1")
                                .arg(QDir::toNativeSeparators(scenePath)));
          });
  connect(mViewport, &NativeViewport::sceneLoaded, this,
          [this](const qint64 sourceVertexCount,
                 const qsizetype previewVertexCount) {
            const QString renderer =
                mRenderMode == NativeViewport::RenderMode::Gaussians
                    ? QStringLiteral("高斯预览")
                    : QStringLiteral("点预览");
            mRendererStatus->setText(QStringLiteral("%1 | %2 个预览图元")
                                         .arg(renderer)
                                         .arg(previewVertexCount));
            appendTaskEvent(
                QStringLiteral(
                    "场景已载入 GPU 预览：源数据 %1 个图元，显示 %2 个图元。")
                    .arg(sourceVertexCount)
                    .arg(previewVertexCount));
          });
  connect(mViewport, &NativeViewport::sceneLoadFailed, this,
          [this](const QString &, const QString &message) {
            mRendererStatus->setText(QStringLiteral("点云读取失败"));
            appendTaskEvent(QStringLiteral("场景读取失败：%1").arg(message));
          });
  connect(
      mViewport, &NativeViewport::cameraTrajectoryChanged, this,
      [this](const qsizetype cameraCount, const qsizetype invalidCameraCount,
             const bool displayDecimated, const QString &sourcePath,
             const QString &error) {
        mCameraCount = cameraCount;
        mInvalidCameraCount = invalidCameraCount;
        mCameraDisplayDecimated = displayDecimated;
        mCameraSourcePath = sourcePath;
        mCameraTrajectoryError = error;
        mShowCamerasAction->setEnabled(cameraCount > 0);
        updateInspector();
        rebuildProjectTree();

        const QString eventKey = QStringLiteral("%1\n%2\n%3\n%4\n%5")
                                     .arg(QDir::cleanPath(sourcePath))
                                     .arg(cameraCount)
                                     .arg(invalidCameraCount)
                                     .arg(displayDecimated)
                                     .arg(error);
        if (eventKey == mLastCameraTrajectoryEventKey) {
          return;
        }
        mLastCameraTrajectoryEventKey = eventKey;

        if (!error.isEmpty()) {
          const QString location = sourcePath.isEmpty()
                                       ? QStringLiteral("cameras.json")
                                       : QDir::toNativeSeparators(sourcePath);
          const QString message =
              QStringLiteral(
                  "相机轨迹未载入：%1。请检查 %2 是否为有效的标准 3DGS "
                  "cameras.json，修复后重新载入场景。")
                  .arg(error, location);
          appendTaskEvent(message);
          statusBar()->showMessage(message, 10000);
        } else if (cameraCount > 0) {
          QString message =
              QStringLiteral("已载入相机轨迹：%1 个位姿").arg(cameraCount);
          if (invalidCameraCount > 0) {
            message +=
                QStringLiteral("，跳过 %1 个无效条目").arg(invalidCameraCount);
          }
          if (displayDecimated) {
            message += QStringLiteral("，视口已自动抽稀");
          }
          message += QStringLiteral("（%1）。")
                         .arg(QDir::toNativeSeparators(sourcePath));
          appendTaskEvent(message);
          if (invalidCameraCount > 0) {
            statusBar()->showMessage(message, 8000);
          }
        }
      });
  connect(mViewport, &NativeViewport::editStateChanged, this,
          [this](const qsizetype selectedCount, const qsizetype deletedCount,
                 const bool canUndo, const bool canRedo, const bool sceneReady,
                 const bool) {
            mSelectedPointCount = selectedCount;
            mDeletedPointCount = deletedCount;
            mCanUndoEdit = canUndo;
            mCanRedoEdit = canRedo;
            mSceneReady = sceneReady;
            updateEditActions();
          });
  connect(mViewport, &NativeViewport::selectionBusyChanged, this,
          [this](const bool busy) {
            mSelectionBusy = busy;
            updateEditActions();
          });
  connect(mViewport, &NativeViewport::gaussianRenderingAvailabilityChanged,
          this, [this](const bool available) {
            mGaussianRenderAction->setEnabled(available);
            if (!available && mGaussianRenderAction->isChecked()) {
              mPointRenderAction->setChecked(true);
            }
          });
  connect(mViewport, &NativeViewport::renderModeChanged, this,
          [this](const NativeViewport::RenderMode mode) {
            mRenderMode = mode;
            mGaussianRenderAction->setChecked(
                mode == NativeViewport::RenderMode::Gaussians);
            mPointRenderAction->setChecked(mode ==
                                           NativeViewport::RenderMode::Points);
          });
}

void MainWindow::restoreWindowState() {
  QSettings settings;
  const QByteArray geometry =
      settings.value(QStringLiteral("window/geometry")).toByteArray();
  const QByteArray state =
      settings.value(QStringLiteral("window/state")).toByteArray();
  if (!geometry.isEmpty()) {
    restoreGeometry(geometry);
  } else {
    const QRect available =
        QGuiApplication::primaryScreen()->availableGeometry();
    const QSize size(std::min(available.width() * 92 / 100, 1600),
                     std::min(available.height() * 90 / 100, 1000));
    resize(size);
    move(available.center() - rect().center());
  }
  if (state.isEmpty() || !restoreState(state, kDockLayoutStateVersion)) {
    resetDockLayout();
  }
}

void MainWindow::saveWindowState() {
  QSettings settings;
  settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("window/state"),
                    saveState(kDockLayoutStateVersion));
}

void MainWindow::resetDockLayout() {
  addDockWidget(Qt::LeftDockWidgetArea, mProjectDock);
  addDockWidget(Qt::RightDockWidgetArea, mInspectorDock);
  addDockWidget(Qt::BottomDockWidgetArea, mTaskDock);
  mProjectDock->show();
  mInspectorDock->show();
  mTaskDock->show();
  rebalanceDockSizes();
}

void MainWindow::rebalanceDockSizes() {
  updateDockMetrics();
  resizeDocks({mProjectDock, mInspectorDock},
              {std::max(AppTheme::scaled(225, mUiScalePercent),
                        mProjectDock->minimumWidth()),
               std::max(AppTheme::scaled(270, mUiScalePercent),
                        mInspectorDock->minimumWidth())},
              Qt::Horizontal);
  resizeDocks({mTaskDock},
              {std::max(AppTheme::scaled(180, mUiScalePercent),
                        mTaskDock->minimumHeight())},
              Qt::Vertical);
}

void MainWindow::updateDockMetrics() {
  updateDockTitleBarScale(mProjectDock, mUiScalePercent);
  updateDockTitleBarScale(mInspectorDock, mUiScalePercent);
  updateDockTitleBarScale(mTaskDock, mUiScalePercent);
  const int fontHeight = QFontMetrics(qApp->font()).height();
  if (mProjectDock != nullptr) {
    mProjectDock->setMinimumWidth(
        std::max(AppTheme::scaled(190, mUiScalePercent), fontHeight * 10));
  }
  if (mInspectorDock != nullptr) {
    mInspectorDock->setMinimumWidth(
        std::max(AppTheme::scaled(230, mUiScalePercent), fontHeight * 12));
  }
  if (mTaskDock != nullptr) {
    mTaskDock->setMinimumHeight(
        std::max(AppTheme::scaled(150, mUiScalePercent), fontHeight * 7));
  }
}

void MainWindow::applyUiScale(const int scalePercent, const bool persist) {
  const int previousScalePercent = mUiScalePercent;
  mUiScalePercent = std::clamp(scalePercent, 90, 150);
  AppTheme::apply(*qApp, mUiScalePercent, persist);
  updateDockMetrics();
  if (previousScalePercent != mUiScalePercent) {
    resizeDocks({mProjectDock, mInspectorDock},
                {AppTheme::rescaledDockExtent(
                     mProjectDock->width(), previousScalePercent,
                     mUiScalePercent, mProjectDock->minimumWidth()),
                 AppTheme::rescaledDockExtent(
                     mInspectorDock->width(), previousScalePercent,
                     mUiScalePercent, mInspectorDock->minimumWidth())},
                Qt::Horizontal);
    resizeDocks({mTaskDock},
                {AppTheme::rescaledDockExtent(
                    mTaskDock->height(), previousScalePercent, mUiScalePercent,
                    mTaskDock->minimumHeight())},
                Qt::Vertical);
  }
  const QSize iconSize(AppTheme::scaled(20, mUiScalePercent),
                       AppTheme::scaled(20, mUiScalePercent));
  for (QToolBar *toolbar : findChildren<QToolBar *>()) {
    toolbar->setIconSize(iconSize);
  }
  if (mBrushRadiusSpin != nullptr) {
    mBrushRadiusSpin->setMinimumWidth(AppTheme::scaled(86, mUiScalePercent));
    mBrushRadiusSpin->setMaximumWidth(AppTheme::scaled(120, mUiScalePercent));
  }
  updateScaleStatus();
  if (mAutoScaleAction != nullptr) {
    mAutoScaleAction->setChecked(mAutomaticUiScale);
  }
  if (mScaleActionGroup != nullptr) {
    for (QAction *action : mScaleActionGroup->actions()) {
      action->setChecked(!mAutomaticUiScale &&
                         action->data().toInt() == mUiScalePercent);
    }
  }
}

void MainWindow::setAutomaticUiScale(const bool automatic, const bool persist) {
  mAutomaticUiScale = automatic;
  if (persist) {
    AppTheme::saveScaleMode(automatic ? UiScaleMode::Automatic
                                      : UiScaleMode::Manual);
  }
  if (automatic) {
    refreshAutomaticUiScale();
    rebalanceDockSizes();
  } else {
    applyUiScale(mUiScalePercent, false);
  }
}

void MainWindow::refreshAutomaticUiScale() {
  if (!mAutomaticUiScale) {
    updateScaleStatus();
    return;
  }
  QScreen *activeScreen = screen();
  if (activeScreen == nullptr) {
    activeScreen = QGuiApplication::primaryScreen();
  }
  const QSize availableSize = activeScreen == nullptr
                                  ? QSize{}
                                  : activeScreen->availableGeometry().size();
  const int recommended = AppTheme::recommendedScalePercent(
      availableSize, size(),
      activeScreen == nullptr ? 1.0 : activeScreen->devicePixelRatio());
  if (recommended != mUiScalePercent) {
    applyUiScale(recommended, false);
  } else {
    updateScaleStatus();
  }
}

void MainWindow::scheduleAutomaticUiScale() {
  if (mAutomaticUiScale && mUiAdaptTimer != nullptr) {
    mUiAdaptTimer->start();
  } else {
    updateScaleStatus();
  }
}

void MainWindow::updateScaleStatus() {
  if (mScaleStatus == nullptr) {
    return;
  }
  mScaleStatus->setText(QStringLiteral("%1 %2% · %3×%4")
                            .arg(mAutomaticUiScale ? QStringLiteral("自动")
                                                   : QStringLiteral("手动"))
                            .arg(mUiScalePercent)
                            .arg(width())
                            .arg(height()));
  mScaleStatus->setToolTip(
      QStringLiteral("视图 > 显示与适配，可切换自动缩放或窗口分辨率"));
}

void MainWindow::applyWindowResolution(const QSize &requestedSize) {
  QScreen *activeScreen = screen();
  if (activeScreen == nullptr) {
    activeScreen = QGuiApplication::primaryScreen();
  }
  if (activeScreen == nullptr) {
    return;
  }

  const QRect available = activeScreen->availableGeometry();
  const QSize fitted = AppTheme::fitWindowResolution(
      requestedSize, available.size(), minimumSize());
  showNormal();
  resize(fitted);
  move(available.topLeft() +
       QPoint((available.width() - fitted.width()) / 2,
              (available.height() - fitted.height()) / 2));
  scheduleAutomaticUiScale();
  statusBar()->showMessage(QStringLiteral("窗口已调整为 %1 × %2")
                               .arg(fitted.width())
                               .arg(fitted.height()),
                           3500);
}

void MainWindow::fitWindowToScreen() {
  QScreen *activeScreen = screen();
  if (activeScreen == nullptr) {
    activeScreen = QGuiApplication::primaryScreen();
  }
  if (activeScreen == nullptr) {
    return;
  }
  const QSize available = activeScreen->availableGeometry().size();
  const QSize automaticSize(qRound(available.width() * 0.92),
                            qRound(available.height() * 0.90));
  setAutomaticUiScale(true, true);
  applyWindowResolution(automaticSize);
}

void MainWindow::updateEditActions() {
  const bool interactive = mSceneReady && !mSelectionBusy &&
                           !mRecoveryBlocked && !mProcessSupervisor.isRunning();
  if (mInspectAction == nullptr) {
    return;
  }
  if (mRenderToolbar != nullptr) {
    mRenderToolbar->setVisible(mSceneReady);
  }
  if (mSelectionToolbar != nullptr) {
    mSelectionToolbar->setVisible(mSceneReady);
  }
  if (mEditToolbar != nullptr) {
    mEditToolbar->setVisible(mSceneReady);
  }
  mInspectAction->setEnabled(interactive);
  mRectangleAction->setEnabled(interactive);
  mLassoAction->setEnabled(interactive);
  mBrushAction->setEnabled(interactive);
  if (mBrushRadiusSpin != nullptr) {
    mBrushRadiusSpin->setEnabled(interactive && mBrushAction->isChecked());
  }
  mVisibleOnlyAction->setEnabled(interactive);
  mClearSelectionAction->setEnabled(interactive && mSelectedPointCount > 0);
  mInvertSelectionAction->setEnabled(interactive);
  mDeleteSelectionAction->setEnabled(interactive && mSelectedPointCount > 0);
  mUndoEditAction->setEnabled(interactive && mCanUndoEdit);
  mRedoEditAction->setEnabled(interactive && mCanRedoEdit);
  mExportCropAction->setEnabled(interactive && mDeletedPointCount > 0);
  if (mEditStatus != nullptr) {
    mEditStatus->setVisible(mSceneReady || mSelectionBusy);
    mEditStatus->setText(mSelectionBusy ? QStringLiteral("正在计算选择")
                                        : QStringLiteral("选择 %1 | 删除 %2")
                                              .arg(mSelectedPointCount)
                                              .arg(mDeletedPointCount));
  }
}

bool MainWindow::confirmDiscardChanges(const bool exiting) {
  if (mWorkspace.isModified() || isWindowModified()) {
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this,
        exiting ? QStringLiteral("退出前保存进度")
                : QStringLiteral("工程尚未保存"),
        exiting ? QStringLiteral("当前工程包含未保存的修改或最近保留的"
                                 "任务进度。是否在退出前保存？")
                : QStringLiteral("当前工程包含未保存的修改。"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (answer == QMessageBox::Save && !saveProject(false)) {
      return false;
    }
    if (answer == QMessageBox::Cancel) {
      return false;
    }
  }
  return confirmDiscardSceneEdits(exiting);
}

bool MainWindow::confirmExit() {
  if (mExitConfirmed) {
    return true;
  }

  QMessageBox prompt(
      QMessageBox::Question, QStringLiteral("确认退出"),
      QStringLiteral("确定要退出 Gaussian Scene Workbench Native 吗？"),
      QMessageBox::NoButton, this);
  prompt.setObjectName(QStringLiteral("exitConfirmationDialog"));
  prompt.setInformativeText(
      QStringLiteral("退出前如有未保存进度，软件将继续询问是否保存。\n"
                     "退出后会停止当前软件启动的训练、COLMAP、"
                     "导入及其他后台进程。"));
  auto *exitButton =
      prompt.addButton(QStringLiteral("退出"), QMessageBox::AcceptRole);
  auto *cancelButton =
      prompt.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
  prompt.setDefaultButton(cancelButton);
  prompt.setEscapeButton(cancelButton);
  prompt.exec();
  if (prompt.clickedButton() != exitButton) {
    return false;
  }
  mExitConfirmed = true;
  return true;
}

bool MainWindow::confirmDiscardSceneEdits(const bool exiting) {
  if (!mViewport->hasUnsavedSceneEdits()) {
    return true;
  }
  const QMessageBox::StandardButton answer = QMessageBox::warning(
      this,
      exiting ? QStringLiteral("退出前导出裁剪进度")
              : QStringLiteral("裁剪尚未导出"),
      exiting ? QStringLiteral("当前场景包含尚未导出的删除操作。"
                               "是否在退出前导出？")
              : QStringLiteral("当前场景包含尚未导出的删除操作。"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  if (answer == QMessageBox::Save) {
    return exportCroppedScene();
  }
  return answer == QMessageBox::Discard;
}

bool MainWindow::beginUntitledProject(const QString &displayName,
                                      QString *errorMessage) {
  if (mRecoveryStore == nullptr) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("恢复工作区存储不可用。");
    }
    return false;
  }

  if (mCurrentRecovery.has_value() &&
      !discardCurrentRecovery(errorMessage)) {
    return false;
  }

  const std::optional<RecoveryWorkspace> recovery =
      mRecoveryStore->beginWorkspace(displayName, errorMessage);
  if (!recovery.has_value()) {
    return false;
  }
  QString createError;
  if (!mWorkspace.createUntitled(recovery->rootPath, displayName,
                                 &createError)) {
    mRecoveryStore->discardWorkspace(*recovery);
    if (errorMessage != nullptr) {
      *errorMessage = createError;
    }
    return false;
  }
  mCurrentRecovery = recovery;
  checkpointCurrentRecovery();
  mRecoveryBlocked = false;
  return true;
}

void MainWindow::checkpointCurrentRecovery() {
  if (mRecoveryStore == nullptr || !mCurrentRecovery.has_value()) {
    return;
  }
  mCurrentRecovery->displayName = mWorkspace.projectName();
  mCurrentRecovery->projectFilePath = mWorkspace.projectFilePath();
  mCurrentRecovery->datasetPath = mWorkspace.datasetPath();
  mCurrentRecovery->scenePath = mWorkspace.scenePath();
  QString error;
  if (!mRecoveryStore->checkpoint(*mCurrentRecovery, &error)) {
    appendTaskEvent(
        QStringLiteral("自动恢复检查点保存失败：%1").arg(error));
  }
}

void MainWindow::snapshotCurrentProject() {
  if (mSuppressSnapshots || mRecoveryStore == nullptr ||
      mWorkspace.projectFilePath().isEmpty() ||
      !QFileInfo::exists(mWorkspace.projectFilePath()) ||
      mWorkspace.rootPath().isEmpty()) {
    return;
  }
  const int maximumSnapshots =
      std::clamp(QSettings().value(QStringLiteral("recovery/maxSnapshots"), 20)
                     .toInt(),
                 3, 100);
  QString error;
  if (!mRecoveryStore
           ->createProjectSnapshot(
               mWorkspace.recoveryManifestJson(),
               mWorkspace.projectFilePath(), mWorkspace.rootPath(),
               maximumSnapshots, &error)
           .has_value()) {
    appendTaskEvent(
        QStringLiteral("工程版本快照保存失败：%1").arg(error));
  }
}

bool MainWindow::discardCurrentRecovery(QString *errorMessage) {
  if (!mCurrentRecovery.has_value()) {
    return true;
  }
  if (mRecoveryStore == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("恢复工作区存储不可用。");
    }
    return false;
  }
  const RecoveryWorkspace recovery = *mCurrentRecovery;
  if (!mRecoveryStore->discardWorkspace(recovery, errorMessage)) {
    return false;
  }
  mCurrentRecovery.reset();
  return true;
}

bool MainWindow::completeCurrentRecovery(
    const QString &managedProjectRoot, QString *errorMessage) {
  if (!mCurrentRecovery.has_value()) {
    return true;
  }
  if (mRecoveryStore == nullptr ||
      !mRecoveryStore->completeWorkspace(*mCurrentRecovery,
                                         managedProjectRoot,
                                         errorMessage)) {
    return false;
  }
  mCurrentRecovery.reset();
  return true;
}

void MainWindow::offerStartupRecovery() {
  if (mDiscardRecoveryOnDestruction || mStartupRecovery.isEmpty()) {
    return;
  }
  QTimer::singleShot(0, this, [this]() { showRecoveryCenter(true); });
}

void MainWindow::showRecoveryCenter(const bool startupPrompt) {
  if (mRecoveryStore == nullptr) {
    if (!startupPrompt) {
      QMessageBox::information(this, QStringLiteral("恢复中心"),
                               QStringLiteral("恢复存储当前不可用。"));
    }
    return;
  }

  QString scanError;
  QList<RecoveryWorkspace> candidates =
      mRecoveryStore->recoverableWorkspaces(&scanError);
  if (mCurrentRecovery.has_value()) {
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
                       [this](const RecoveryWorkspace &candidate) {
                         return candidate.sessionId ==
                                mCurrentRecovery->sessionId;
                       }),
        candidates.end());
  }
  if (candidates.isEmpty()) {
    if (!startupPrompt) {
      QMessageBox::information(
          this, QStringLiteral("恢复中心"),
          scanError.isEmpty() ? QStringLiteral("没有可恢复的异常退出工程。")
                              : scanError);
    }
    return;
  }

  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("工程恢复中心"));
  dialog.resize(820, 380);
  auto *layout = new QVBoxLayout(&dialog);
  auto *description = new QLabel(
      startupPrompt
          ? QStringLiteral("检测到上次异常退出保留的工程。选择一项恢复，"
                           "或稍后从“文件 → 恢复中心”处理。")
          : QStringLiteral("以下工程由异常退出保护保留。"),
      &dialog);
  description->setWordWrap(true);
  layout->addWidget(description);

  auto *table = new QTableWidget(candidates.size(), 4, &dialog);
  table->setHorizontalHeaderLabels(
      {QStringLiteral("更新时间"), QStringLiteral("名称"),
       QStringLiteral("状态"), QStringLiteral("恢复位置")});
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  for (qsizetype row = 0; row < candidates.size(); ++row) {
    const RecoveryWorkspace &candidate = candidates.at(row);
    table->setItem(
        static_cast<int>(row), 0,
        new QTableWidgetItem(
            candidate.updatedUtc.toLocalTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    table->setItem(static_cast<int>(row), 1,
                   new QTableWidgetItem(candidate.displayName));
    table->setItem(
        static_cast<int>(row), 2,
        new QTableWidgetItem(candidate.projectFilePath.isEmpty()
                                 ? QStringLiteral("未命名工程")
                                 : QStringLiteral("保存迁移待恢复")));
    table->setItem(
        static_cast<int>(row), 3,
        new QTableWidgetItem(QDir::toNativeSeparators(candidate.rootPath)));
  }
  table->selectRow(0);
  layout->addWidget(table, 1);

  auto *buttons = new QDialogButtonBox(&dialog);
  auto *recoverButton =
      buttons->addButton(QStringLiteral("恢复所选工程"),
                         QDialogButtonBox::AcceptRole);
  auto *discardButton =
      buttons->addButton(QStringLiteral("永久删除所选"),
                         QDialogButtonBox::DestructiveRole);
  auto *laterButton =
      buttons->addButton(QStringLiteral("稍后处理"),
                         QDialogButtonBox::RejectRole);
  connect(recoverButton, &QPushButton::clicked, &dialog,
          [&dialog]() { dialog.done(1); });
  connect(discardButton, &QPushButton::clicked, &dialog,
          [&dialog]() { dialog.done(2); });
  connect(laterButton, &QPushButton::clicked, &dialog, &QDialog::reject);
  layout->addWidget(buttons);

  const int result = dialog.exec();
  const int selectedRow = table->currentRow();
  if (selectedRow < 0 || selectedRow >= candidates.size()) {
    return;
  }
  const RecoveryWorkspace selected = candidates.at(selectedRow);
  if (result == 1) {
    restoreRecoveryWorkspace(selected);
    return;
  }
  if (result == 2) {
    const QMessageBox::StandardButton confirmed = QMessageBox::warning(
        this, QStringLiteral("永久删除恢复工程"),
        QStringLiteral("将永久删除以下异常恢复数据：\n%1")
            .arg(QDir::toNativeSeparators(selected.rootPath)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (confirmed != QMessageBox::Yes) {
      return;
    }
    QString discardError;
    if (!mRecoveryStore->discardWorkspace(selected, &discardError)) {
      showError(QStringLiteral("无法删除恢复工程"), discardError);
      return;
    }
    appendTaskEvent(
        QStringLiteral("已永久删除恢复工程：%1").arg(selected.displayName));
    showRecoveryCenter(false);
  }
}

bool MainWindow::restoreRecoveryWorkspace(
    const RecoveryWorkspace &workspace) {
  if (!workspace.projectFilePath.isEmpty() &&
      QFileInfo::exists(workspace.projectFilePath)) {
    if (!openProjectFile(workspace.projectFilePath)) {
      return false;
    }
    if (!pathsReferToSameLocation(workspace.rootPath,
                                  mWorkspace.rootPath())) {
      QString discardError;
      if (!mRecoveryStore->completeWorkspace(
              workspace, mWorkspace.rootPath(), &discardError)) {
        appendTaskEvent(
            QStringLiteral("工程已恢复，但旧恢复目录清理失败：%1")
                .arg(discardError));
      }
    } else {
      mCurrentRecovery = workspace;
      checkpointCurrentRecovery();
    }
    appendTaskEvent(
        QStringLiteral("已从恢复中心恢复工程：%1")
            .arg(workspace.displayName));
    return true;
  }

  if (!confirmDiscardChanges()) {
    return false;
  }
  QString createError;
  if (!mWorkspace.createUntitled(workspace.rootPath, workspace.displayName,
                                 &createError)) {
    showError(QStringLiteral("无法恢复未命名工程"), createError);
    return false;
  }
  QString discardError;
  if (mCurrentRecovery.has_value() &&
      mCurrentRecovery->sessionId != workspace.sessionId &&
      !discardCurrentRecovery(&discardError)) {
    appendTaskEvent(
        QStringLiteral("旧恢复工作区清理失败：%1").arg(discardError));
  }
  mCurrentRecovery = workspace;
  mRecoveryBlocked = true;
  QString importRecoveryError;
  if (!recoverInterruptedProjectImports(&importRecoveryError)) {
    appendTaskEvent(
        QStringLiteral("未命名工程已恢复，但导入事务仍待处理：%1")
            .arg(importRecoveryError));
    QMessageBox::warning(
        this, QStringLiteral("导入恢复待处理"),
        QStringLiteral("工程数据已保留，但未完成的导入事务尚未恢复。"
                       "修复 Python/worker 环境后，可重新启动软件再重试。\n\n%1")
            .arg(importRecoveryError));
  } else {
    mRecoveryBlocked = false;
  }
  QString attachError;
  if (!workspace.datasetPath.isEmpty() &&
      QFileInfo(workspace.datasetPath).isDir() &&
      !mWorkspace.setDatasetPath(workspace.datasetPath, &attachError)) {
    appendTaskEvent(
        QStringLiteral("恢复工程的数据集未能自动关联：%1").arg(attachError));
  }
  if (!workspace.scenePath.isEmpty() &&
      QFileInfo(workspace.scenePath).isFile() &&
      !mWorkspace.setScenePath(workspace.scenePath, &attachError)) {
    appendTaskEvent(
        QStringLiteral("恢复工程的场景未能自动关联：%1").arg(attachError));
  }
  if (!mRecoveryBlocked) {
    QString trainingRecoveryError;
    if (!recoverInterruptedTraining(&trainingRecoveryError)) {
      appendTaskEvent(
          QStringLiteral("未命名工程已恢复，但训练检查点仍待处理：%1")
              .arg(trainingRecoveryError));
      QMessageBox::warning(
          this, QStringLiteral("训练检查点恢复待处理"),
          QStringLiteral("工程和训练输出仍保留在磁盘上，但未能自动关联最近的"
                         "完整检查点。\n\n%1")
              .arg(trainingRecoveryError));
    }
  }
  checkpointCurrentRecovery();
  updateWorkspaceUi();
  appendTaskEvent(
      QStringLiteral("已恢复未命名工程：%1").arg(workspace.displayName));
  return true;
}

void MainWindow::showSnapshotHistory() {
  if (mRecoveryStore == nullptr ||
      mWorkspace.projectFilePath().isEmpty()) {
    QMessageBox::information(this, QStringLiteral("版本历史"),
                             QStringLiteral("请先保存工程。"));
    return;
  }
  QString error;
  const QList<ProjectSnapshot> snapshots =
      mRecoveryStore->projectSnapshots(mWorkspace.rootPath(), &error);
  if (snapshots.isEmpty()) {
    QMessageBox::information(
        this, QStringLiteral("版本历史"),
        error.isEmpty() ? QStringLiteral("当前工程还没有自动快照。")
                        : error);
    return;
  }

  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("工程版本历史"));
  dialog.resize(720, 360);
  auto *layout = new QVBoxLayout(&dialog);
  auto *description = new QLabel(
      QStringLiteral("自动快照保存工程状态并复用已校验的数据。恢复时会"
                     "创建新工程文件，不覆盖当前版本。"),
      &dialog);
  description->setWordWrap(true);
  layout->addWidget(description);
  auto *table = new QTableWidget(snapshots.size(), 2, &dialog);
  table->setHorizontalHeaderLabels(
      {QStringLiteral("快照时间"), QStringLiteral("来源工程")});
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  for (qsizetype row = 0; row < snapshots.size(); ++row) {
    const ProjectSnapshot &snapshot = snapshots.at(row);
    table->setItem(
        static_cast<int>(row), 0,
        new QTableWidgetItem(snapshot.createdUtc.toLocalTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
    table->setItem(
        static_cast<int>(row), 1,
        new QTableWidgetItem(
            QDir::toNativeSeparators(snapshot.sourceProjectFilePath)));
  }
  table->selectRow(0);
  layout->addWidget(table, 1);
  auto *buttons = new QDialogButtonBox(&dialog);
  auto *restoreButton =
      buttons->addButton(QStringLiteral("恢复为新工程"),
                         QDialogButtonBox::AcceptRole);
  auto *cancelButton =
      buttons->addButton(QStringLiteral("取消"),
                         QDialogButtonBox::RejectRole);
  connect(restoreButton, &QPushButton::clicked, &dialog, &QDialog::accept);
  connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted || table->currentRow() < 0) {
    return;
  }

  const ProjectSnapshot selected = snapshots.at(table->currentRow());
  const QFileInfo currentProject(mWorkspace.projectFilePath());
  QString projectStemName = currentProject.fileName();
  const QString suffix = QStringLiteral(".gsw.json");
  if (projectStemName.endsWith(suffix, Qt::CaseInsensitive)) {
    projectStemName.chop(suffix.size());
  }
  const QString restoredPath =
      QDir(currentProject.absolutePath())
          .filePath(QStringLiteral("%1-restored-%2.gsw.json")
                        .arg(projectStemName,
                             QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd-HHmmss"))));
  if (!mRecoveryStore->restoreProjectSnapshot(selected, restoredPath,
                                               &error)) {
    showError(QStringLiteral("无法恢复工程快照"), error);
    return;
  }
  appendTaskEvent(
      QStringLiteral("工程快照已恢复为：%1")
          .arg(QDir::toNativeSeparators(restoredPath)));
  openProjectFile(restoredPath);
}

void MainWindow::configureExternalBackup() {
  const QString current =
      QSettings()
          .value(QStringLiteral("recovery/externalBackupRoot"))
          .toString();
  const QString selected = QFileDialog::getExistingDirectory(
      this, QStringLiteral("选择第二存储位置"), current);
  if (selected.isEmpty()) {
    return;
  }
  if (!mWorkspace.rootPath().isEmpty()) {
    const QStorageInfo projectStorage(mWorkspace.rootPath());
    const QStorageInfo backupStorage(selected);
    if (projectStorage.isValid() && backupStorage.isValid() &&
        pathsReferToSameLocation(projectStorage.rootPath(),
                                 backupStorage.rootPath())) {
      QMessageBox::warning(
          this, QStringLiteral("请选择另一存储设备"),
          QStringLiteral("所选位置与当前工程位于同一卷，无法防护整盘损坏。"
                         "请选择另一块硬盘、移动存储、NAS 或网络共享。"));
      return;
    }
  }
  QSettings().setValue(QStringLiteral("recovery/externalBackupRoot"),
                       QDir::cleanPath(QFileInfo(selected).absoluteFilePath()));
  appendTaskEvent(
      QStringLiteral("第二存储位置已设置为：%1")
          .arg(QDir::toNativeSeparators(selected)));
  startExternalBackup(true);
}

void MainWindow::startExternalBackup(const bool interactive) {
  if (mExternalBackupRunning) {
    if (interactive) {
      QMessageBox::information(this, QStringLiteral("外部备份"),
                               QStringLiteral("外部备份正在运行。"));
    }
    return;
  }
  const QString backupRoot =
      QSettings()
          .value(QStringLiteral("recovery/externalBackupRoot"))
          .toString();
  if (backupRoot.isEmpty()) {
    if (interactive) {
      configureExternalBackup();
    }
    return;
  }
  if (mWorkspace.projectFilePath().isEmpty() ||
      mWorkspace.rootPath().isEmpty() ||
      mWorkspace.hasPendingDataMigration()) {
    if (interactive) {
      QMessageBox::information(
          this, QStringLiteral("外部备份"),
          QStringLiteral("请先完成工程保存和数据迁移。"));
    }
    return;
  }

  const QString projectFile = mWorkspace.projectFilePath();
  const QString projectDataRoot = mWorkspace.rootPath();
  const QString linkedDatasetPath = mWorkspace.datasetPath();
  const QString linkedScenePath = mWorkspace.scenePath();
  mExternalBackupRunning = true;
  statusBar()->showMessage(
      QStringLiteral("正在后台创建去重外部备份…"));
  appendTaskEvent(QStringLiteral("外部备份已开始：%1")
                      .arg(QDir::toNativeSeparators(backupRoot)));

  auto *watcher = new QFutureWatcher<ExternalBackupTaskResult>(this);
  connect(watcher, &QFutureWatcher<ExternalBackupTaskResult>::finished, this,
          [this, watcher, interactive]() {
            const ExternalBackupTaskResult result = watcher->result();
            watcher->deleteLater();
            mExternalBackupRunning = false;
            if (!result.snapshot.has_value()) {
              statusBar()->showMessage(QStringLiteral("外部备份失败"), 8000);
              appendTaskEvent(
                  QStringLiteral("外部备份失败：%1").arg(result.error));
              if (interactive) {
                showError(QStringLiteral("无法完成外部备份"), result.error);
              }
              return;
            }
            statusBar()->showMessage(QStringLiteral("外部备份已完成"), 8000);
            appendTaskEvent(
                QStringLiteral("外部备份已完成：%1 个文件，%2")
                    .arg(result.snapshot->fileCount)
                    .arg(formatFileSize(result.snapshot->totalBytes)));
          });
  watcher->setFuture(QtConcurrent::run(
      [backupRoot, projectFile, projectDataRoot, linkedDatasetPath,
       linkedScenePath]() {
        ExternalBackupTaskResult result;
        ExternalBackupStore store(backupRoot);
        result.snapshot = store.backupProject(
            projectFile, projectDataRoot, &result.error, linkedDatasetPath,
            linkedScenePath);
        return result;
      }));
}

void MainWindow::showExternalBackups() {
  const QString backupRoot =
      QSettings()
          .value(QStringLiteral("recovery/externalBackupRoot"))
          .toString();
  if (backupRoot.isEmpty()) {
    configureExternalBackup();
    return;
  }
  ExternalBackupStore store(backupRoot);
  QString error;
  const QList<ExternalBackupSnapshot> backups = store.snapshots(&error);
  if (backups.isEmpty()) {
    QMessageBox::information(
        this, QStringLiteral("外部备份与恢复"),
        error.isEmpty() ? QStringLiteral("第二存储位置中还没有备份。")
                        : error);
    return;
  }

  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("外部备份与恢复"));
  dialog.resize(780, 380);
  auto *layout = new QVBoxLayout(&dialog);
  auto *table = new QTableWidget(backups.size(), 4, &dialog);
  table->setHorizontalHeaderLabels(
      {QStringLiteral("备份时间"), QStringLiteral("工程"),
       QStringLiteral("文件数"), QStringLiteral("逻辑大小")});
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  for (qsizetype row = 0; row < backups.size(); ++row) {
    const ExternalBackupSnapshot &backup = backups.at(row);
    table->setItem(
        static_cast<int>(row), 0,
        new QTableWidgetItem(backup.createdUtc.toLocalTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    table->setItem(static_cast<int>(row), 1,
                   new QTableWidgetItem(backup.projectName));
    table->setItem(static_cast<int>(row), 2,
                   new QTableWidgetItem(
                       QString::number(backup.fileCount)));
    table->setItem(static_cast<int>(row), 3,
                   new QTableWidgetItem(formatFileSize(backup.totalBytes)));
  }
  table->selectRow(0);
  layout->addWidget(table, 1);
  auto *buttons = new QDialogButtonBox(&dialog);
  auto *restoreButton =
      buttons->addButton(QStringLiteral("恢复所选备份"),
                         QDialogButtonBox::AcceptRole);
  auto *closeButton =
      buttons->addButton(QStringLiteral("关闭"),
                         QDialogButtonBox::RejectRole);
  connect(restoreButton, &QPushButton::clicked, &dialog, &QDialog::accept);
  connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted || table->currentRow() < 0) {
    return;
  }

  const ExternalBackupSnapshot selected = backups.at(table->currentRow());
  const QString destinationParent = QFileDialog::getExistingDirectory(
      this, QStringLiteral("选择备份恢复位置"));
  if (destinationParent.isEmpty()) {
    return;
  }
  const QString destination =
      QDir(destinationParent)
          .filePath(QStringLiteral("%1-restored-%2")
                        .arg(safeFileName(selected.projectName),
                             QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd-HHmmss"))));
  QProgressDialog progress(QStringLiteral("正在校验并恢复外部备份…"),
                           QString(), 0, 0, this);
  progress.setWindowModality(Qt::WindowModal);
  progress.setCancelButton(nullptr);
  QFutureWatcher<QPair<bool, QString>> watcher;
  connect(&watcher, &QFutureWatcher<QPair<bool, QString>>::finished,
          &progress, &QProgressDialog::close);
  watcher.setFuture(QtConcurrent::run(
      [backupRoot, selected, destination]() {
        QString restoreError;
        const bool restored =
            ExternalBackupStore(backupRoot)
                .restore(selected, destination, &restoreError);
        return qMakePair(restored, restoreError);
      }));
  if (!watcher.isFinished()) {
    progress.exec();
  }
  watcher.waitForFinished();
  const QPair<bool, QString> restored = watcher.result();
  if (!restored.first) {
    showError(QStringLiteral("无法恢复外部备份"), restored.second);
    return;
  }
  appendTaskEvent(
      QStringLiteral("外部备份已恢复到：%1")
          .arg(QDir::toNativeSeparators(destination)));
  QMessageBox::information(
      this, QStringLiteral("外部备份已恢复"),
      QStringLiteral("恢复文件已写入：\n%1")
          .arg(QDir::toNativeSeparators(destination)));
}

bool MainWindow::saveProject(const bool forceChoosePath) {
  if (!ensureProjectRecoveryReady()) {
    return false;
  }
  if (!mWorkspace.hasProject()) {
    return false;
  }
  const bool taskRunning = mProcessSupervisor.isRunning();
  if (taskRunning && forceChoosePath && mWorkspace.hasPendingDataMigration()) {
    QMessageBox::information(
        this, QStringLiteral("工程另存正在等待迁移"),
        QStringLiteral("当前任务结束后会自动完成已选择位置的数据迁移。"
                       "迁移完成前可继续使用“保存工程”记录进度。"));
    return false;
  }
  if (!taskRunning && mWorkspace.hasPendingDataMigration()) {
    QString migrationError;
    statusBar()->showMessage(QStringLiteral("正在完成工程数据迁移…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool migrated = finalizePendingProjectSave(&migrationError);
    QApplication::restoreOverrideCursor();
    statusBar()->clearMessage();
    if (!migrated) {
      showError(QStringLiteral("无法完成工程数据迁移"), migrationError);
      return false;
    }
  }
  QString target = mWorkspace.projectFilePath();
  if (forceChoosePath || target.isEmpty()) {
    target = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存工程"), suggestedProjectFilePath(),
        QStringLiteral("GSW Project (*.gsw.json)"));
    if (target.isEmpty()) {
      return false;
    }
    if (!target.endsWith(QStringLiteral(".gsw.json"), Qt::CaseInsensitive)) {
      target += QStringLiteral(".gsw.json");
    }
  }
  QString error;
  statusBar()->showMessage(taskRunning
                               ? QStringLiteral("正在保存工程状态…")
                               : QStringLiteral("正在保存工程与托管数据…"));
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const bool saved = taskRunning ? mWorkspace.saveManifest(target, &error)
                                 : mWorkspace.save(target, &error);
  QApplication::restoreOverrideCursor();
  statusBar()->clearMessage();
  if (!saved) {
    showError(QStringLiteral("无法保存工程"), error);
    return false;
  }
  if (!taskRunning && !mWorkspace.isUntitled() &&
      !mWorkspace.hasPendingDataMigration()) {
    if (mCurrentRecovery.has_value() &&
        !pathsReferToSameLocation(mCurrentRecovery->rootPath,
                                  mWorkspace.rootPath())) {
      QString discardError;
      if (!completeCurrentRecovery(mWorkspace.rootPath(), &discardError)) {
        appendTaskEvent(
            QStringLiteral("已保存工程，但恢复工作区清理失败：%1")
                .arg(discardError));
      }
    }
  }
  QSettings settings;
  settings.setValue(QStringLiteral("project/lastSaveDirectory"),
                    QFileInfo(target).absolutePath());
  snapshotCurrentProject();
  if (!taskRunning && !mWorkspace.hasPendingDataMigration()) {
    startExternalBackup(false);
  }
  if (taskRunning) {
    appendTaskEvent(
        mWorkspace.hasPendingDataMigration()
            ? QStringLiteral("工程状态已保存：%1；后台任务继续运行，"
                             "结束后将自动迁移托管数据。")
                  .arg(QDir::toNativeSeparators(target))
            : QStringLiteral("工程状态已保存：%1；后台任务继续运行。")
                  .arg(QDir::toNativeSeparators(target)));
  } else {
    appendTaskEvent(QStringLiteral("工程已保存：%1（数据：%2）")
                        .arg(QDir::toNativeSeparators(target),
                             QDir::toNativeSeparators(mWorkspace.rootPath())));
  }
  return true;
}

bool MainWindow::finalizePendingProjectSave(QString *errorMessage) {
  if (!mWorkspace.hasPendingDataMigration()) {
    return true;
  }
  const QString oldRoot = mWorkspace.rootPath();
  if (!mWorkspace.finalizeDataMigration(errorMessage)) {
    return false;
  }
  if (mCurrentRecovery.has_value() &&
      pathsReferToSameLocation(mCurrentRecovery->rootPath, oldRoot)) {
    QString discardError;
    if (!completeCurrentRecovery(mWorkspace.rootPath(), &discardError)) {
      appendTaskEvent(
          QStringLiteral("工程迁移完成，但恢复工作区清理失败：%1")
              .arg(discardError));
    }
  }
  appendTaskEvent(QStringLiteral("工程托管数据已迁移到：%1")
                      .arg(QDir::toNativeSeparators(mWorkspace.rootPath())));
  return true;
}

bool MainWindow::recoverInterruptedTraining(QString *errorMessage) {
  QString loadError;
  const ActiveTrainingJob job =
      loadActiveTrainingJob(mWorkspace.rootPath(), &loadError);
  if (!loadError.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = loadError;
    }
    return false;
  }
  if (!job.isValid()) {
    return true;
  }

  QString resultError;
  std::optional<ResolvedTrainingPointCloud> checkpoint =
      resolveTrainingPointCloud(job.outputSceneRoot, std::nullopt,
                                &resultError);
  if (!checkpoint.has_value()) {
    QString clearError;
    if (!clearActiveTrainingJob(mWorkspace.rootPath(), &clearError)) {
      if (errorMessage != nullptr) {
        *errorMessage = clearError;
      }
      return false;
    }
    appendTaskEvent(
        QStringLiteral("检测到上次训练被中断，但尚无完整检查点可恢复。"));
    return true;
  }

  const std::optional<ResolvedTrainingPointCloud> durable =
      publishDurableTrainingPointCloud(
          *checkpoint, mWorkspace.rootPath(), job.outputSceneRoot,
          &resultError);
  if (!durable.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("最近检查点有效，但无法发布保护副本：%1")
              .arg(resultError);
    }
    return false;
  }
  checkpoint = durable;

  QString sceneError;
  if (!mWorkspace.setScenePath(checkpoint->path, &sceneError)) {
    if (errorMessage != nullptr) {
      *errorMessage = sceneError;
    }
    return false;
  }
  if (!mWorkspace.projectFilePath().isEmpty() &&
      !mWorkspace.saveManifest({}, &sceneError)) {
    if (errorMessage != nullptr) {
      *errorMessage = sceneError;
    }
    return false;
  }
  QString clearError;
  if (!clearActiveTrainingJob(mWorkspace.rootPath(), &clearError)) {
    if (errorMessage != nullptr) {
      *errorMessage = clearError;
    }
    return false;
  }

  appendTaskEvent(
      QStringLiteral("已从中断训练恢复最近完整检查点：迭代 %1，%2")
          .arg(checkpoint->iteration)
          .arg(QDir::toNativeSeparators(checkpoint->path)));
  return true;
}

bool MainWindow::exportCroppedScene() {
  if (!ensureProjectRecoveryReady()) {
    return false;
  }
  const QString sourcePath = mWorkspace.scenePath();
  if (sourcePath.isEmpty()) {
    return false;
  }
  const QFileInfo sourceInfo(sourcePath);
  QString target = QDir(sourceInfo.absolutePath())
                       .filePath(sourceInfo.completeBaseName() +
                                 QStringLiteral("-cropped.ply"));
  target = QFileDialog::getSaveFileName(
      this, QStringLiteral("裁剪另存为"), target,
      QStringLiteral("PLY Scene (*.ply);;All files (*.*)"));
  if (target.isEmpty()) {
    return false;
  }
  if (!target.endsWith(QStringLiteral(".ply"), Qt::CaseInsensitive)) {
    target += QStringLiteral(".ply");
  }

  QApplication::setOverrideCursor(Qt::WaitCursor);
  QString error;
  const bool saved = mViewport->saveCroppedScene(target, &error);
  QApplication::restoreOverrideCursor();
  if (!saved) {
    showError(QStringLiteral("无法导出裁剪场景"), error);
    return false;
  }
  appendTaskEvent(QStringLiteral("裁剪场景已导出：%1")
                      .arg(QDir::toNativeSeparators(target)));
  statusBar()->showMessage(QStringLiteral("裁剪场景已导出"), 5000);
  return true;
}

bool MainWindow::ensureProjectRecoveryReady() {
  if (!mRecoveryBlocked) {
    return true;
  }
  QMessageBox::warning(
      this, QStringLiteral("导入恢复待处理"),
      QStringLiteral(
          "工程中的未完成导入事务尚未安全恢复，因此保存、导入、重建、训练和"
          "场景编辑暂时禁用。请修复 Python/worker 环境后重新打开工程。"));
  return false;
}

bool MainWindow::recoverDatasetImport(const PendingDatasetImport &pending,
                                      QString *errorMessage, bool *committed,
                                      QStringList *committedPaths) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  if (committed != nullptr) {
    *committed = false;
  }
  if (committedPaths != nullptr) {
    committedPaths->clear();
  }

  QProcess recovery;
  recovery.setProcessChannelMode(QProcess::MergedChannels);
  recovery.setWorkingDirectory(pending.workingDirectory);
  recovery.setProcessEnvironment(pythonProcessEnvironment(pending.python));
  recovery.start(pending.python,
                 {pending.workerScript, QStringLiteral("--task"),
                  pending.recoveryTask, QStringLiteral("--config"),
                  pending.configurationPath},
                 QIODevice::ReadOnly);
  if (!recovery.waitForStarted(5000)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("无法启动导入恢复进程：%1")
                          .arg(recovery.errorString());
    }
    return false;
  }
  if (!recovery.waitForFinished(30000)) {
    recovery.kill();
    recovery.waitForFinished(2000);
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "导入恢复超时；另一个软件实例可能仍在处理同名数据集。");
    }
    return false;
  }

  const QString output = QString::fromUtf8(recovery.readAllStandardOutput());
  if (!output.trimmed().isEmpty()) {
    appendLog(output.endsWith(QLatin1Char('\n')) ? output
                                                 : output + QLatin1Char('\n'));
  }
  if (recovery.exitStatus() == QProcess::NormalExit &&
      recovery.exitCode() == 0) {
    static const QString recoveryPrefix = QStringLiteral("[worker-recovery]");
    bool receivedRecoveryReport = false;
    const QStringList lines = output.split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &rawLine : lines) {
      const QString line = rawLine.trimmed();
      if (!line.startsWith(recoveryPrefix)) {
        continue;
      }
      QJsonParseError parseError;
      const QJsonDocument event = QJsonDocument::fromJson(
          line.mid(recoveryPrefix.size()).trimmed().toUtf8(), &parseError);
      if (parseError.error == QJsonParseError::NoError && event.isObject() &&
          event.object().value(QStringLiteral("version")).toInt() == 1 &&
          event.object().value(QStringLiteral("committed")).isBool()) {
        const QJsonObject recoveryReport = event.object();
        receivedRecoveryReport = true;
        if (committed != nullptr) {
          *committed =
              recoveryReport.value(QStringLiteral("committed")).toBool();
        }
        if (committedPaths != nullptr &&
            recoveryReport.value(QStringLiteral("committedPaths")).isArray()) {
          for (const QJsonValue &value :
               recoveryReport.value(QStringLiteral("committedPaths"))
                   .toArray()) {
            if (value.isString() && !value.toString().trimmed().isEmpty()) {
              committedPaths->append(comparablePath(value.toString()));
            }
          }
          committedPaths->removeDuplicates();
        }
      }
    }
    if (receivedRecoveryReport) {
      return true;
    }
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("导入恢复进程已退出，但未返回有效的事务状"
                                     "态报告；worker 版本可能不兼容。");
    }
    return false;
  }

  if (errorMessage != nullptr) {
    const QString detail = output.trimmed();
    *errorMessage = detail.isEmpty()
                        ? QStringLiteral("导入恢复失败，退出码 %1。")
                              .arg(recovery.exitCode())
                        : QStringLiteral("导入恢复失败：%1").arg(detail);
  }
  return false;
}

bool MainWindow::recoverInterruptedProjectImports(QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }

  const QString projectRoot = comparablePath(mWorkspace.rootPath());
  const QString datasetRoot =
      QDir(projectRoot).filePath(QStringLiteral("datasets"));
  if (!hasImportRecoveryArtifacts(datasetRoot)) {
    return true;
  }

  const QString repositoryRoot = BackendLocator::findRepositoryRoot(
      QCoreApplication::applicationDirPath(),
      qEnvironmentVariable("GSW_BACKEND_ROOT"));
  const QString workerScript =
      QDir(repositoryRoot)
          .filePath(QStringLiteral("native/worker/gsw_worker.py"));
  const QString python = findTrainingPython(repositoryRoot);
  if (repositoryRoot.isEmpty() || !QFileInfo::exists(workerScript) ||
      python.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("检测到未完成的导入事务。\n\n") +
          backendUnavailableMessage(repositoryRoot, workerScript, python);
    }
    return false;
  }

  QTemporaryFile configuration(
      QDir(QDir::tempPath())
          .filePath(QStringLiteral("gsw-project-recovery-XXXXXX.json")));
  configuration.setAutoRemove(true);
  if (!configuration.open()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("无法创建工程导入恢复配置：%1")
                          .arg(configuration.errorString());
    }
    return false;
  }

  QJsonObject workerConfig;
  workerConfig.insert(QStringLiteral("task"),
                      QStringLiteral("import-project-recovery"));
  workerConfig.insert(QStringLiteral("projectRoot"), projectRoot);
  workerConfig.insert(QStringLiteral("datasetRoot"),
                      QDir::cleanPath(datasetRoot));
  const QByteArray payload =
      QJsonDocument(workerConfig).toJson(QJsonDocument::Compact);
  if (configuration.write(payload) != payload.size() ||
      !configuration.flush()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("无法写入工程导入恢复配置：%1")
                          .arg(configuration.errorString());
    }
    return false;
  }
  configuration.close();

  const PendingDatasetImport pending{QStringLiteral("工程导入事务恢复"),
                                     datasetRoot,
                                     projectRoot,
                                     configuration.fileName(),
                                     python,
                                     workerScript,
                                     repositoryRoot,
                                     QStringLiteral("import-project-recovery")};
  bool committed = false;
  QStringList committedPaths;
  if (!recoverDatasetImport(pending, errorMessage, &committed,
                            &committedPaths)) {
    return false;
  }

  if (!mWorkspace.projectFilePath().isEmpty()) {
    QString reloadError;
    if (!mWorkspace.load(mWorkspace.projectFilePath(), &reloadError)) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("导入事务已恢复，但无法重新载入工程：%1")
                            .arg(reloadError);
      }
      return false;
    }
  }

  if (committed && committedPaths.isEmpty()) {
    appendTaskEvent(QStringLiteral("检测到已提交的数据集，但恢复报告未包含路径"
                                   "；请使用“关联已有数据集”确认结果。"));
  } else if (committedPaths.size() == 1) {
    const QString recoveredDataset =
        comparablePath(committedPaths.constFirst());
    const QFileInfo recoveredInfo(recoveredDataset);
    const bool trustedLocation =
        recoveredInfo.isDir() &&
        pathsReferToSameLocation(recoveredInfo.absolutePath(), datasetRoot);
    const qint64 imageCount =
        trustedLocation
            ? WorkspaceDocument::countDatasetImages(recoveredDataset)
            : 0;
    QString attachError;
    if (trustedLocation && imageCount > 0 &&
        mWorkspace.setDatasetPath(recoveredDataset, &attachError)) {
      QString saveError;
      if (!mWorkspace.projectFilePath().isEmpty() &&
          !mWorkspace.saveManifest({}, &saveError)) {
        appendTaskEvent(
            QStringLiteral("已恢复并关联数据集，但工程自动保存失败：%1")
                .arg(saveError));
      }
      appendTaskEvent(
          QStringLiteral("已自动关联崩溃前完成提交的数据集：%1（%2 张图像）")
              .arg(QDir::toNativeSeparators(recoveredDataset))
              .arg(imageCount));
    } else {
      appendTaskEvent(
          QStringLiteral("已保留提交完成的数据集，但无法自动关联：%1%2")
              .arg(QDir::toNativeSeparators(recoveredDataset),
                   attachError.isEmpty()
                       ? QString()
                       : QStringLiteral("（%1）").arg(attachError)));
    }
  } else if (committedPaths.size() > 1) {
    appendTaskEvent(QStringLiteral("已恢复 %1 "
                                   "个提交完成的数据集；为避免误选，请使用“关联"
                                   "已有数据集”选择当前数据集。")
                        .arg(committedPaths.size()));
  }
  appendTaskEvent(
      QStringLiteral("已恢复工程中断的导入事务，并重新载入数据集状态。"));
  return true;
}

void MainWindow::newProject() {
  if (mProcessSupervisor.isRunning()) {
    QMessageBox::information(
        this, QStringLiteral("任务仍在运行"),
        QStringLiteral("请先停止或等待当前任务结束，再新建工程。"));
    return;
  }
  if (!confirmDiscardChanges()) {
    return;
  }
  QString error;
  if (!beginUntitledProject(QStringLiteral("未命名工程"), &error)) {
    showError(QStringLiteral("无法创建工程"), error);
    return;
  }
  updateWorkspaceUi();
  appendTaskEvent(
      QStringLiteral("已新建未命名工程；无需预先选择目录，可在任意阶段保存。"));
}

bool MainWindow::ensureProjectForDataAction(const QString &actionName) {
  if (mWorkspace.hasProject()) {
    return true;
  }

  QString error;
  if (!beginUntitledProject(QStringLiteral("未命名工程"), &error)) {
    showError(QStringLiteral("无法准备%1").arg(actionName), error);
    return false;
  }
  updateWorkspaceUi();
  return true;
}

void MainWindow::importDataset() {
  if (!ensureProjectRecoveryReady()) {
    return;
  }
  if (mProcessSupervisor.isRunning()) {
    QMessageBox::information(
        this, QStringLiteral("任务繁忙"),
        QStringLiteral("请等待当前任务结束后再导入媒体。"));
    return;
  }

  QStringList sourcePaths =
      qApp->property("gswInitialMediaSources").toStringList();
  if (!sourcePaths.isEmpty()) {
    qApp->setProperty("gswInitialMediaSources", QStringList{});
  } else {
    sourcePaths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("添加照片或视频"), mWorkspace.rootPath(),
        QStringLiteral("照片与视频 (*.jpg *.jpeg *.png *.bmp *.tif *.tiff "
                       "*.webp *.mp4 *.mov *.avi *.mkv *.webm *.m4v);;"
                       "照片 (*.jpg *.jpeg *.png *.bmp *.tif *.tiff *.webp);;"
                       "视频 (*.mp4 *.mov *.avi *.mkv *.webm *.m4v)"));
  }
  if (!sourcePaths.isEmpty()) {
    importDatasetSources(sourcePaths);
  }
}

void MainWindow::importDatasetDirectory() {
  if (!ensureProjectRecoveryReady()) {
    return;
  }
  if (mProcessSupervisor.isRunning()) {
    QMessageBox::information(
        this, QStringLiteral("任务繁忙"),
        QStringLiteral("请等待当前任务结束后再导入媒体。"));
    return;
  }

  const QString sourcePath = QFileDialog::getExistingDirectory(
      this, QStringLiteral("添加包含照片或视频的目录"), mWorkspace.rootPath(),
      QFileDialog::ShowDirsOnly);
  if (!sourcePath.isEmpty()) {
    importDatasetSources({sourcePath});
  }
}

void MainWindow::importDatasetSources(const QStringList &sourcePaths) {
  if (sourcePaths.isEmpty() || !ensureProjectRecoveryReady() ||
      mProcessSupervisor.isRunning()) {
    return;
  }

  QString error;
  if (!ensureProjectForDataAction(QStringLiteral("添加照片与视频"))) {
    return;
  }

  const QFileInfo firstSource(sourcePaths.constFirst());
  const QString initialDirectory = firstSource.isDir()
                                       ? firstSource.absoluteFilePath()
                                       : firstSource.absolutePath();
  DatasetImportDialog dialog(
      initialDirectory, suggestedMediaSceneName(sourcePaths), sourcePaths,
      mWorkspace.rootPath(), mWorkspace.isUntitled(), this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  const std::optional<DatasetImportPlan> &validatedPlan =
      dialog.validatedPlan();
  if (!validatedPlan.has_value()) {
    showError(QStringLiteral("无法准备媒体导入"),
              QStringLiteral("导入对话框未生成有效的媒体计划。"));
    return;
  }
  const DatasetImportPlan &plan = *validatedPlan;
  const DatasetImportRequest request = dialog.request();

  const QString root = BackendLocator::findRepositoryRoot(
      QCoreApplication::applicationDirPath(),
      qEnvironmentVariable("GSW_BACKEND_ROOT"));
  const QString workerScript =
      QDir(root).filePath(QStringLiteral("native/worker/gsw_worker.py"));
  const QString python = findTrainingPython(root);
  if (root.isEmpty() || !QFileInfo::exists(workerScript) || python.isEmpty()) {
    showError(QStringLiteral("导入后端不可用"),
              backendUnavailableMessage(root, workerScript, python));
    return;
  }

  statusBar()->showMessage(QStringLiteral("正在预检媒体导入环境…"));
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const ImportEnvironmentProbeResult preflight = ImportEnvironmentProbe::run(
      python, root, plan.videoCount() > 0, pythonProcessEnvironment(python));
  QApplication::restoreOverrideCursor();
  statusBar()->clearMessage();
  if (!preflight.ready) {
    showError(QStringLiteral("媒体导入环境不可用"),
              QStringLiteral("所选 Python：%1\n\n%2")
                  .arg(preflight.python, preflight.errorMessage));
    return;
  }
  appendTaskEvent(
      preflight.videoBackend.isEmpty()
          ? QStringLiteral("媒体导入环境预检通过：%1").arg(preflight.python)
          : QStringLiteral("媒体导入环境预检通过：%1（视频后端：%2）")
                .arg(preflight.python, preflight.videoBackend));

  const QString activeProjectRoot = comparablePath(mWorkspace.rootPath());
  const QString datasetRoot =
      QDir(activeProjectRoot).filePath(QStringLiteral("datasets"));
  const QString managedDatasetPath = plan.managedDatasetPath(datasetRoot);
  const QString jobsDirectory =
      QDir(activeProjectRoot).filePath(QStringLiteral(".gsw/jobs"));
  const QString configurationPath =
      QDir(jobsDirectory)
          .filePath(
              QStringLiteral("import-%1.json")
                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (!plan.writeWorkerConfiguration(configurationPath, root, datasetRoot,
                                     &error)) {
    showError(QStringLiteral("无法保存导入任务"), error);
    return;
  }

  const qsizetype mediaCount = plan.imageCount() + plan.videoCount();
  const QString taskName = QStringLiteral("媒体导入 | %1 | %2 项")
                               .arg(plan.sceneName())
                               .arg(mediaCount);
  const PendingDatasetImport pending{taskName,
                                     managedDatasetPath,
                                     activeProjectRoot,
                                     configurationPath,
                                     python,
                                     workerScript,
                                     root,
                                     QStringLiteral("import-recovery")};

  statusBar()->showMessage(QStringLiteral("正在检查上次导入事务…"));
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const bool recovered = recoverDatasetImport(pending, &error);
  QApplication::restoreOverrideCursor();
  statusBar()->clearMessage();
  if (!recovered) {
    showError(QStringLiteral("无法恢复导入事务"), error);
    return;
  }

  if (QFileInfo::exists(managedDatasetPath) && !request.overwrite) {
    QFile::remove(configurationPath);
    QMessageBox::information(
        this, QStringLiteral("数据集已存在"),
        QStringLiteral(
            "目标数据集已存在：\n%1\n\n请启用“覆盖同名托管数据集”后重试。")
            .arg(QDir::toNativeSeparators(managedDatasetPath)));
    return;
  }
  if (QFileInfo::exists(managedDatasetPath) && request.overwrite) {
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, QStringLiteral("确认覆盖托管数据集"),
        QStringLiteral("导入成功后将分阶段替换以下数据集；最终提交前的失败或中"
                       "断会恢复旧数据，"
                       "提交完成后会保留新数据：\n%1")
            .arg(QDir::toNativeSeparators(managedDatasetPath)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      QFile::remove(configurationPath);
      return;
    }
  }

  mPendingDatasetImport = pending;
  const bool started = mProcessSupervisor.start(
      taskName, python,
      {workerScript, QStringLiteral("--task"), QStringLiteral("import"),
       QStringLiteral("--config"), configurationPath},
      root, pythonProcessEnvironment(python), false);
  if (!started) {
    mPendingDatasetImport.reset();
    QFile::remove(configurationPath);
    QMessageBox::information(this, QStringLiteral("任务繁忙"),
                             QStringLiteral("请等待当前任务结束后再试。"));
    return;
  }

  appendTaskEvent(QStringLiteral("导入配置已保存：%1")
                      .arg(QDir::toNativeSeparators(configurationPath)));
}

void MainWindow::attachExistingDataset() {
  if (!ensureProjectRecoveryReady()) {
    return;
  }
  if (!ensureProjectForDataAction(QStringLiteral("关联已有数据集"))) {
    return;
  }
  if (mProcessSupervisor.isRunning()) {
    QMessageBox::information(
        this, QStringLiteral("任务繁忙"),
        QStringLiteral("请等待当前任务结束后再关联数据集。"));
    return;
  }

  const QString initialDirectory = mWorkspace.datasetPath().isEmpty()
                                       ? mWorkspace.rootPath()
                                       : mWorkspace.datasetPath();
  const QString directory = QFileDialog::getExistingDirectory(
      this, QStringLiteral("关联已有图像 / COLMAP 数据集"), initialDirectory,
      QFileDialog::ShowDirsOnly);
  if (directory.isEmpty()) {
    return;
  }

  const qint64 imageCount = WorkspaceDocument::countDatasetImages(directory);
  if (imageCount <= 0) {
    showError(QStringLiteral("数据集没有可用图像"),
              QStringLiteral(
                  "未在所选目录或其 images/input 子目录中找到支持的图像。"));
    return;
  }

  QString error;
  if (!mWorkspace.setDatasetPath(directory, &error)) {
    showError(QStringLiteral("无法关联数据集"), error);
    return;
  }
  appendTaskEvent(
      QStringLiteral(
          "已关联现有数据集：%1（%2 张图像，原目录与 COLMAP 数据保持不变）")
          .arg(QDir::toNativeSeparators(directory))
          .arg(imageCount));
}

void MainWindow::importScene() {
  if (!ensureProjectRecoveryReady()) {
    return;
  }
  if (!ensureProjectForDataAction(QStringLiteral("导入高斯场景"))) {
    return;
  }
  if (!confirmDiscardSceneEdits()) {
    return;
  }
  const QString filePath = QFileDialog::getOpenFileName(
      this, QStringLiteral("导入高斯场景"), mWorkspace.rootPath(),
      QStringLiteral("PLY Scene (*.ply);;All files (*.*)"));
  if (filePath.isEmpty()) {
    return;
  }
  QString error;
  if (!mWorkspace.setScenePath(filePath, &error)) {
    showError(QStringLiteral("无法导入场景"), error);
    return;
  }
  const PlyMetadata metadata = mWorkspace.sceneMetadata();
  appendTaskEvent(QStringLiteral("已读取场景元数据：%1 个顶点，%2")
                      .arg(metadata.vertexCount)
                      .arg(metadata.looksLikeGaussianSplat()
                               ? QStringLiteral("Gaussian Splat")
                               : QStringLiteral("PLY")));
}

void MainWindow::runEnvironmentCheck() {
  const QString root = BackendLocator::findRepositoryRoot(
      QCoreApplication::applicationDirPath(),
      qEnvironmentVariable("GSW_BACKEND_ROOT"));
  const QString script =
      QDir(root).filePath(QStringLiteral("scripts/check_3dgs_env.ps1"));
  if (root.isEmpty() || !QFileInfo::exists(script)) {
    showError(QStringLiteral("找不到环境检查脚本"),
              QStringLiteral("请从完整源码目录运行原生应用。"));
    return;
  }
  const bool started = mProcessSupervisor.start(
      QStringLiteral("环境检查"), QStringLiteral("powershell.exe"),
      {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
       QStringLiteral("Bypass"), QStringLiteral("-File"),
       QDir::toNativeSeparators(script)},
      root);
  if (!started) {
    QMessageBox::information(this, QStringLiteral("任务繁忙"),
                             QStringLiteral("请等待当前任务结束后再试。"));
  }
}

void MainWindow::startReconstruction() {
  if (!ensureProjectRecoveryReady()) {
    return;
  }
  if (mWorkspace.datasetPath().isEmpty()) {
    QMessageBox::information(this, QStringLiteral("尚未导入数据集"),
                             QStringLiteral("请先导入包含照片的图像数据集。"));
    return;
  }
  const QString root = BackendLocator::findRepositoryRoot(
      QCoreApplication::applicationDirPath(),
      qEnvironmentVariable("GSW_BACKEND_ROOT"));
  const QString workerScript =
      QDir(root).filePath(QStringLiteral("native/worker/gsw_worker.py"));
  const QString python = findTrainingPython(root);
  if (root.isEmpty() || !QFileInfo::exists(workerScript) || python.isEmpty()) {
    showError(QStringLiteral("重建后端不可用"),
              backendUnavailableMessage(root, workerScript, python));
    return;
  }

  ReconstructionDialog dialog(mWorkspace.datasetPath(), root, this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const ReconstructionConfiguration config = dialog.configuration();

  const QString jobsRoot =
      QDir(mWorkspace.rootPath()).filePath(QStringLiteral(".gsw/jobs"));
  if (!QDir().mkpath(jobsRoot)) {
    showError(QStringLiteral("无法创建任务目录"),
              QDir::toNativeSeparators(jobsRoot));
    return;
  }

  QJsonObject colmapOptions;
  colmapOptions.insert(QStringLiteral("preset"), config.preset);
  colmapOptions.insert(QStringLiteral("matching"), config.matching);
  colmapOptions.insert(QStringLiteral("camera_model"), config.cameraModel);
  colmapOptions.insert(QStringLiteral("single_camera"), config.singleCamera);
  colmapOptions.insert(QStringLiteral("use_gpu"), config.useGpu);
  colmapOptions.insert(QStringLiteral("reset"), config.reset);
  colmapOptions.insert(QStringLiteral("feature_max_image_size"),
                       config.featureMaxImageSize);
  colmapOptions.insert(QStringLiteral("feature_max_num_features"),
                       config.featureMaxNumFeatures);
  colmapOptions.insert(QStringLiteral("matcher_guided"), config.matcherGuided);
  colmapOptions.insert(QStringLiteral("mapper_max_runtime_seconds"),
                       config.mapperMaxRuntimeSeconds);

  QJsonObject workerConfig;
  workerConfig.insert(QStringLiteral("task"), QStringLiteral("colmap"));
  workerConfig.insert(QStringLiteral("scene"),
                      QStringLiteral("native-project"));
  workerConfig.insert(QStringLiteral("repositoryRoot"), QDir::cleanPath(root));
  workerConfig.insert(QStringLiteral("datasetPath"),
                      QDir::cleanPath(mWorkspace.datasetPath()));
  workerConfig.insert(QStringLiteral("colmapExecutable"),
                      QDir::cleanPath(config.colmapExecutable));
  workerConfig.insert(QStringLiteral("colmapOptions"), colmapOptions);

  const QString configPath = QDir(jobsRoot).filePath(
      QStringLiteral("colmap-%1.json")
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QSaveFile configFile(configPath);
  if (!configFile.open(QIODevice::WriteOnly) ||
      configFile.write(
          QJsonDocument(workerConfig).toJson(QJsonDocument::Indented)) < 0 ||
      !configFile.commit()) {
    showError(QStringLiteral("无法保存重建任务"), configFile.errorString());
    return;
  }

  QProcessEnvironment environment = pythonProcessEnvironment(python);
  environment.insert(QStringLiteral("COLMAP_PATH"), config.colmapExecutable);
  environment.insert(QStringLiteral("COLMAP_EXE"), config.colmapExecutable);
  const bool started = mProcessSupervisor.start(
      QStringLiteral("COLMAP | %1 | %2").arg(config.preset, config.matching),
      python,
      {workerScript, QStringLiteral("--task"), QStringLiteral("colmap"),
       QStringLiteral("--config"), configPath},
      root, environment, true);
  if (!started) {
    QMessageBox::information(this, QStringLiteral("任务繁忙"),
                             QStringLiteral("请等待当前任务结束后再试。"));
  } else {
    appendTaskEvent(QStringLiteral("重建配置已保存：%1")
                        .arg(QDir::toNativeSeparators(configPath)));
  }
}

void MainWindow::startTraining() {
  if (!ensureProjectRecoveryReady()) {
    return;
  }
  if (mWorkspace.datasetPath().isEmpty()) {
    QMessageBox::information(
        this, QStringLiteral("尚未导入数据集"),
        QStringLiteral("请先导入包含 COLMAP 数据的图像工程。"));
    return;
  }
  const QString root = BackendLocator::findRepositoryRoot(
      QCoreApplication::applicationDirPath(),
      qEnvironmentVariable("GSW_BACKEND_ROOT"));
  const QString workerScript =
      QDir(root).filePath(QStringLiteral("native/worker/gsw_worker.py"));
  const QString python = findTrainingPython(root);
  if (root.isEmpty() || !QFileInfo::exists(workerScript) || python.isEmpty()) {
    showError(QStringLiteral("训练后端不可用"),
              backendUnavailableMessage(root, workerScript, python));
    return;
  }

  const QString defaultOutputRoot =
      QDir(mWorkspace.rootPath()).filePath(QStringLiteral("output"));
  TrainingDialog dialog(mWorkspace.datasetPath(), mWorkspace.projectName(),
                        defaultOutputRoot,
                        hasRecognizedTrainingScene(mWorkspace.datasetPath()),
                        twoDgsAvailable(root), this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const TrainingConfiguration config = dialog.configuration();

  if (!confirmDiscardSceneEdits()) {
    return;
  }

  statusBar()->showMessage(
      QStringLiteral("正在预检 %1 训练环境…").arg(config.backend.toUpper()));
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const TrainingEnvironmentProbeResult preflight =
      TrainingEnvironmentProbe::run(python, root, mWorkspace.datasetPath(),
                                    config.backend, config.runColmap,
                                    pythonProcessEnvironment(python));
  QApplication::restoreOverrideCursor();
  statusBar()->clearMessage();
  if (!preflight.ready) {
    const QString guidance =
        preflight.policyBlocked &&
                !preflight.errorMessage.contains(QStringLiteral("系统管理员"))
            ? QStringLiteral("\n\n这是系统级企业应用控制限制，需要管理员允许或"
                             "签名被拦截的运行库；"
                             "桌面端不会绕过安全策略。")
            : QString();
    showError(QStringLiteral("%1 训练环境不可用").arg(config.backend.toUpper()),
              QStringLiteral("所选 Python：%1\n\n%2%3")
                  .arg(preflight.python, preflight.errorMessage, guidance));
    return;
  }
  appendTaskEvent(QStringLiteral("训练预检通过：%1 张图像，%2，CUDA：%3")
                      .arg(preflight.imageCount)
                      .arg(preflight.hasReconstruction
                               ? QStringLiteral("已有稀疏重建")
                               : QStringLiteral("将运行 COLMAP"))
                      .arg(preflight.cudaDevice));

  const QString jobsRoot =
      QDir(mWorkspace.rootPath()).filePath(QStringLiteral(".gsw/jobs"));
  const QString trainingJobStore =
      QDir(jobsRoot).filePath(QStringLiteral("training"));
  if (!QDir().mkpath(trainingJobStore)) {
    showError(QStringLiteral("无法创建任务目录"),
              QDir::toNativeSeparators(trainingJobStore));
    return;
  }

  QJsonObject trainOptions;
  trainOptions.insert(QStringLiteral("iterations"), config.iterations);
  trainOptions.insert(QStringLiteral("resolution"), config.resolution);
  QJsonObject workerConfig;
  workerConfig.insert(QStringLiteral("repositoryRoot"), QDir::cleanPath(root));
  workerConfig.insert(QStringLiteral("datasetPath"),
                      QDir::cleanPath(mWorkspace.datasetPath()));
  workerConfig.insert(QStringLiteral("outputRoot"),
                      QDir::cleanPath(config.outputRoot));
  workerConfig.insert(QStringLiteral("outputScene"), config.outputScene);
  workerConfig.insert(QStringLiteral("scene"),
                      QStringLiteral("native-project"));
  workerConfig.insert(QStringLiteral("backend"), config.backend);
  workerConfig.insert(QStringLiteral("quality"), config.quality);
  workerConfig.insert(QStringLiteral("runColmap"), config.runColmap);
  workerConfig.insert(QStringLiteral("overwrite"), config.overwrite);
  workerConfig.insert(QStringLiteral("jobStore"),
                      QDir::cleanPath(trainingJobStore));
  workerConfig.insert(QStringLiteral("colmapOptions"), QJsonObject());
  workerConfig.insert(QStringLiteral("trainOptions"), trainOptions);

  const QString configPath = QDir(jobsRoot).filePath(
      QStringLiteral("training-%1.json")
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QSaveFile configFile(configPath);
  if (!configFile.open(QIODevice::WriteOnly) ||
      configFile.write(
          QJsonDocument(workerConfig).toJson(QJsonDocument::Indented)) < 0 ||
      !configFile.commit()) {
    showError(QStringLiteral("无法保存训练任务"), configFile.errorString());
    return;
  }

  const QString taskName = QStringLiteral("%1 训练 | %2 | %3 次迭代")
                               .arg(config.backend.toUpper(), config.quality)
                               .arg(config.iterations);
  const QString outputDirectory =
      QDir(config.outputRoot).filePath(config.outputScene);
  QString recoveryRecordError;
  if (!saveActiveTrainingJob(
          mWorkspace.rootPath(),
          ActiveTrainingJob{configPath, outputDirectory},
          &recoveryRecordError)) {
    showError(QStringLiteral("无法保存训练恢复信息"), recoveryRecordError);
    return;
  }
  mPendingTraining =
      PendingTraining{taskName, comparablePath(mWorkspace.rootPath()),
                      outputDirectory, config.backend, config.iterations};
  const bool started = mProcessSupervisor.start(
      taskName, python, {workerScript, QStringLiteral("--config"), configPath},
      root, pythonProcessEnvironment(python), true);
  if (!started) {
    mPendingTraining.reset();
    QString clearError;
    if (!clearActiveTrainingJob(mWorkspace.rootPath(), &clearError)) {
      appendTaskEvent(
          QStringLiteral("训练未启动，恢复标记清理失败：%1").arg(clearError));
    }
    QMessageBox::information(this, QStringLiteral("任务繁忙"),
                             QStringLiteral("请等待当前任务结束后再试。"));
  } else {
    appendTaskEvent(QStringLiteral("训练配置已保存：%1")
                        .arg(QDir::toNativeSeparators(configPath)));
  }
}

void MainWindow::updateWorkspaceUi() {
  rebuildProjectTree();
  updateInspector();
  const QString projectName = mWorkspace.hasProject()
                                  ? mWorkspace.projectName()
                                  : QStringLiteral("未打开工程");
  mViewport->setProjectLabel(projectName);
  mViewport->setScene(mWorkspace.scenePath(),
                      mWorkspace.sceneMetadata().vertexCount);
  mProjectStatus->setText(
      projectName +
      (mWorkspace.isModified() ? QStringLiteral(" *") : QString()) +
      (mWorkspace.hasPendingDataMigration()
           ? QStringLiteral(" · 数据迁移待完成")
           : QString()) +
      (mRecoveryBlocked ? QStringLiteral(" · 导入恢复待处理") : QString()));
  setWindowTitle(QStringLiteral("%1[*]").arg(projectName));
  updateActionAvailability();
}

void MainWindow::updateActionAvailability() {
  const bool running = mProcessSupervisor.isRunning();
  const bool hasProject = mWorkspace.hasProject();
  const bool workspaceReady = hasProject && !mRecoveryBlocked;
  const bool dataEntryReady = !running && (!hasProject || !mRecoveryBlocked);

  mNewProjectAction->setEnabled(!running);
  mOpenProjectAction->setEnabled(!running);
  mStopAction->setEnabled(running);
  mSaveAction->setEnabled(workspaceReady);
  mSaveAsAction->setEnabled(
      workspaceReady && (!running || !mWorkspace.hasPendingDataMigration()));
  mSnapshotHistoryAction->setEnabled(
      workspaceReady && !mWorkspace.projectFilePath().isEmpty());
  mImportDatasetAction->setEnabled(dataEntryReady);
  mImportDatasetDirectoryAction->setEnabled(dataEntryReady);
  mAttachDatasetAction->setEnabled(dataEntryReady);
  mImportSceneAction->setEnabled(dataEntryReady);
  mReconstructAction->setEnabled(!running && workspaceReady &&
                                 !mWorkspace.datasetPath().isEmpty());
  mTrainAction->setEnabled(!running && workspaceReady &&
                           !mWorkspace.datasetPath().isEmpty());
  updateEditActions();
}

void MainWindow::rebuildProjectTree() {
  mProjectTree->clear();
  const QString projectName = mWorkspace.hasProject()
                                  ? mWorkspace.projectName()
                                  : QStringLiteral("未打开工程");
  auto *root = new QTreeWidgetItem(mProjectTree, {projectName});
  root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
  root->setData(0, Qt::UserRole, mWorkspace.rootPath());

  auto *dataset = new QTreeWidgetItem(root, {QStringLiteral("数据集")});
  dataset->setIcon(0, style()->standardIcon(QStyle::SP_DirOpenIcon));
  dataset->setData(0, Qt::UserRole, mWorkspace.datasetPath());
  new QTreeWidgetItem(
      dataset, {mWorkspace.datasetPath().isEmpty()
                    ? QStringLiteral("未导入")
                    : QStringLiteral("图像 %1").arg(mWorkspace.imageCount())});

  auto *reconstruction = new QTreeWidgetItem(root, {QStringLiteral("重建")});
  reconstruction->setIcon(
      0, style()->standardIcon(QStyle::SP_FileDialogContentsView));
  const bool hasSparse = hasRecognizedColmapScene(mWorkspace.datasetPath());
  new QTreeWidgetItem(reconstruction,
                      {hasSparse ? QStringLiteral("COLMAP sparse")
                                 : QStringLiteral("未检测到稀疏重建")});

  auto *scene = new QTreeWidgetItem(root, {QStringLiteral("场景")});
  scene->setIcon(0, style()->standardIcon(QStyle::SP_FileDialogDetailedView));
  scene->setData(0, Qt::UserRole, mWorkspace.scenePath());
  new QTreeWidgetItem(scene,
                      {mWorkspace.scenePath().isEmpty()
                           ? QStringLiteral("未导入")
                           : QFileInfo(mWorkspace.scenePath()).fileName()});
  if (mCameraCount > 0) {
    auto *cameras = new QTreeWidgetItem(
        scene,
        {QStringLiteral("相机位姿 %1%2")
             .arg(mCameraCount)
             .arg(mCameraDisplayDecimated ? QStringLiteral("（抽稀显示）")
                                          : QString())});
    cameras->setData(0, Qt::UserRole, mCameraSourcePath);
    if (mInvalidCameraCount > 0) {
      cameras->setToolTip(
          0,
          QStringLiteral("已跳过 %1 个无效相机条目").arg(mInvalidCameraCount));
    }
  } else if (!mCameraTrajectoryError.isEmpty()) {
    auto *cameras =
        new QTreeWidgetItem(scene, {QStringLiteral("相机位姿不可用")});
    cameras->setData(0, Qt::UserRole, mCameraSourcePath);
    cameras->setToolTip(0, mCameraTrajectoryError);
  }

  auto *jobs = new QTreeWidgetItem(root, {QStringLiteral("任务")});
  jobs->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
  new QTreeWidgetItem(jobs, {mProcessSupervisor.isRunning()
                                 ? mProcessSupervisor.activeTask()
                                 : QStringLiteral("空闲")});

  root->setExpanded(true);
  dataset->setExpanded(true);
  scene->setExpanded(true);
}

void MainWindow::updateInspector() {
  mProjectNameValue->setText(mWorkspace.hasProject() ? mWorkspace.projectName()
                                                     : QStringLiteral("-"));
  mProjectRootValue->setText(
      mWorkspace.isUntitled() ? QStringLiteral("尚未保存（首次保存时选择位置）")
                              : compactPath(mWorkspace.rootPath()));
  mDatasetValue->setText(compactPath(mWorkspace.datasetPath()));
  mImageCountValue->setText(
      mWorkspace.datasetPath().isEmpty()
          ? QStringLiteral("-")
          : QStringLiteral("%1 张").arg(mWorkspace.imageCount()));
  mSceneValue->setText(compactPath(mWorkspace.scenePath()));
  const PlyMetadata metadata = mWorkspace.sceneMetadata();
  mGaussianCountValue->setText(
      metadata.valid ? QStringLiteral("%1").arg(metadata.vertexCount)
                     : QStringLiteral("-"));
  mPlyFormatValue->setText(metadata.valid
                               ? QStringLiteral("%1 | %2 | %3")
                                     .arg(metadata.format,
                                          metadata.looksLikeGaussianSplat()
                                              ? QStringLiteral("Gaussian Splat")
                                              : QStringLiteral("PLY"),
                                          formatFileSize(metadata.fileSize))
                               : QStringLiteral("-"));
  if (mCameraCount > 0) {
    const QString sourceName = mCameraSourcePath.isEmpty()
                                   ? QStringLiteral("cameras.json")
                                   : QFileInfo(mCameraSourcePath).fileName();
    QStringList details;
    details.append(QStringLiteral("%1 个").arg(mCameraCount));
    if (mInvalidCameraCount > 0) {
      details.append(QStringLiteral("跳过 %1").arg(mInvalidCameraCount));
    }
    if (mCameraDisplayDecimated) {
      details.append(QStringLiteral("抽稀显示"));
    }
    details.append(sourceName);
    mCameraCountValue->setText(details.join(QStringLiteral(" | ")));
    QString tooltip = QDir::toNativeSeparators(mCameraSourcePath);
    if (mInvalidCameraCount > 0) {
      tooltip +=
          QStringLiteral("\n已跳过 %1 个无效相机条目").arg(mInvalidCameraCount);
    }
    mCameraCountValue->setToolTip(tooltip);
  } else if (!mCameraTrajectoryError.isEmpty()) {
    mCameraCountValue->setText(QStringLiteral("不可用 | 查看日志"));
    mCameraCountValue->setToolTip(mCameraTrajectoryError);
  } else {
    mCameraCountValue->setText(QStringLiteral("-"));
    mCameraCountValue->setToolTip(QString());
  }
}

void MainWindow::appendLog(const QString &text) {
  mConsole->moveCursor(QTextCursor::End);
  mConsole->insertPlainText(text);
  if (!text.endsWith(QLatin1Char('\n'))) {
    mConsole->insertPlainText(QStringLiteral("\n"));
  }
  mConsole->moveCursor(QTextCursor::End);
}

void MainWindow::appendTaskEvent(const QString &text) {
  appendLog(QStringLiteral("[%1] %2\n")
                .arg(QDateTime::currentDateTime().toString(
                         QStringLiteral("HH:mm:ss")),
                     text));
}

void MainWindow::showError(const QString &title, const QString &message) {
  QMessageBox::critical(this, title, message);
  appendTaskEvent(QStringLiteral("错误：%1").arg(message));
}

QString MainWindow::findTrainingPython(const QString &repositoryRoot) const {
  return BackendLocator::findGaussianPython(repositoryRoot);
}

QString MainWindow::suggestedProjectFilePath() const {
  if (!mWorkspace.projectFilePath().isEmpty()) {
    return mWorkspace.projectFilePath();
  }

  QSettings settings;
  QString directory =
      settings.value(QStringLiteral("project/lastSaveDirectory")).toString();
  if (!QFileInfo(directory).isDir()) {
    const QStorageInfo storage(mWorkspace.rootPath());
    directory = storage.isValid() && storage.isReady()
                    ? storage.rootPath()
                    : QFileInfo(mWorkspace.rootPath()).absolutePath();
  }
  return QDir(directory).filePath(safeFileName(mWorkspace.projectName()) +
                                  QStringLiteral(".gsw.json"));
}

} // namespace gsw
