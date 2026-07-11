#include "MainWindow.h"

#include "AppTheme.h"
#include "TrainingDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace gsw {

namespace {
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

QString formatFileSize(const qint64 bytes) {
  if (bytes >= 1024LL * 1024LL * 1024LL) {
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
  }
  if (bytes >= 1024LL * 1024LL) {
    return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
  }
  return QStringLiteral("%1 KB").arg(std::max<qint64>(1, bytes / 1024));
}

QString safeFileName(QString value) {
  value.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*]+)")), QStringLiteral("_"));
  value = value.trimmed();
  return value.isEmpty() ? QStringLiteral("gaussian-scene") : value;
}

bool hasRecognizedSparseScene(const QString &datasetPath) {
  if (datasetPath.isEmpty()) {
    return false;
  }
  const QDir sparse(QDir(datasetPath).filePath(QStringLiteral("sparse/0")));
  const bool binary = QFileInfo::exists(sparse.filePath(QStringLiteral("cameras.bin"))) &&
                      QFileInfo::exists(sparse.filePath(QStringLiteral("images.bin"))) &&
                      QFileInfo::exists(sparse.filePath(QStringLiteral("points3D.bin")));
  const bool text = QFileInfo::exists(sparse.filePath(QStringLiteral("cameras.txt"))) &&
                    QFileInfo::exists(sparse.filePath(QStringLiteral("images.txt"))) &&
                    QFileInfo::exists(sparse.filePath(QStringLiteral("points3D.txt")));
  return binary || text || QFileInfo::exists(QDir(datasetPath).filePath(QStringLiteral("transforms_train.json")));
}

bool twoDgsAvailable(const QString &repositoryRoot) {
  QStringList candidates;
  const QString configured = qEnvironmentVariable("TWO_DGS_DIR");
  if (!configured.isEmpty()) {
    candidates.append(configured);
  }
  candidates.append(QDir(QDir::homePath()).filePath(QStringLiteral("Documents/2dgs")));
  candidates.append(QDir(repositoryRoot).filePath(QStringLiteral("2dgs")));
  return std::any_of(candidates.cbegin(), candidates.cend(), [](const QString &candidate) {
    return QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("train.py"))) &&
           QFileInfo::exists(QDir(candidate).filePath(QStringLiteral(".venv/Scripts/python.exe")));
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
  environment.insert(QStringLiteral("PATH"), pathParts.join(QDir::listSeparator()));
  environment.insert(QStringLiteral("CONDA_PREFIX"), prefix);
  environment.insert(QStringLiteral("GAUSSIAN_SPLATTING_CONDA_PREFIX"), prefix);
  environment.insert(QStringLiteral("GS_CONDA_PREFIX"), prefix);
  environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  return environment;
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), mWorkspace(this), mProcessSupervisor(this) {
  mUiScalePercent = qApp->property("gswUiScalePercent").toInt();
  if (mUiScalePercent <= 0) {
    mUiScalePercent = 90;
  }

  setObjectName(QStringLiteral("mainWindow"));
  setWindowTitle(QStringLiteral("Gaussian Scene Workbench - Native Preview"));
  setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
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
  updateWorkspaceUi();
  appendTaskEvent(QStringLiteral("原生桌面预览版已启动。"));
}

bool MainWindow::openProjectFile(const QString &filePath) {
  if (!confirmDiscardChanges()) {
    return false;
  }
  QString error;
  if (!mWorkspace.load(filePath, &error)) {
    showError(QStringLiteral("无法打开工程"), error);
    return false;
  }
  appendTaskEvent(QStringLiteral("已打开工程：%1").arg(QDir::toNativeSeparators(filePath)));
  return true;
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (!confirmDiscardChanges()) {
    event->ignore();
    return;
  }
  if (mProcessSupervisor.isRunning()) {
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("任务仍在运行"), QStringLiteral("关闭软件将停止当前任务。是否继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      event->ignore();
      return;
    }
    mProcessSupervisor.stop();
  }
  saveWindowState();
  event->accept();
}

void MainWindow::createActions() {
  auto *newAction = new QAction(style()->standardIcon(QStyle::SP_FileIcon), QStringLiteral("新建工程"), this);
  newAction->setShortcut(QKeySequence::New);
  newAction->setToolTip(QStringLiteral("新建工程"));
  connect(newAction, &QAction::triggered, this, &MainWindow::newProject);

  auto *openAction = new QAction(style()->standardIcon(QStyle::SP_DialogOpenButton), QStringLiteral("打开工程"), this);
  openAction->setShortcut(QKeySequence::Open);
  openAction->setToolTip(QStringLiteral("打开工程"));
  connect(openAction, &QAction::triggered, this, [this]() {
    const QString filePath = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开 Gaussian Scene Workbench 工程"), {},
        QStringLiteral("GSW Project (*.gsw.json);;JSON (*.json);;All files (*.*)"));
    if (!filePath.isEmpty()) {
      openProjectFile(filePath);
    }
  });

  mSaveAction = new QAction(style()->standardIcon(QStyle::SP_DialogSaveButton), QStringLiteral("保存工程"), this);
  mSaveAction->setShortcut(QKeySequence::Save);
  mSaveAction->setToolTip(QStringLiteral("保存工程"));
  connect(mSaveAction, &QAction::triggered, this, [this]() { saveProject(false); });

  auto *saveAsAction = new QAction(QStringLiteral("工程另存为..."), this);
  saveAsAction->setShortcut(QKeySequence::SaveAs);
  connect(saveAsAction, &QAction::triggered, this, [this]() { saveProject(true); });

  mImportDatasetAction = new QAction(style()->standardIcon(QStyle::SP_DirOpenIcon), QStringLiteral("导入数据集"), this);
  mImportDatasetAction->setToolTip(QStringLiteral("导入图像数据集"));
  connect(mImportDatasetAction, &QAction::triggered, this, &MainWindow::importDataset);

  mImportSceneAction = new QAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView), QStringLiteral("导入高斯场景"), this);
  mImportSceneAction->setToolTip(QStringLiteral("导入 PLY 高斯场景"));
  connect(mImportSceneAction, &QAction::triggered, this, &MainWindow::importScene);

  auto *environmentAction = new QAction(style()->standardIcon(QStyle::SP_BrowserReload), QStringLiteral("检查环境"), this);
  environmentAction->setShortcut(QKeySequence(QStringLiteral("F6")));
  environmentAction->setToolTip(QStringLiteral("检查训练与重建环境"));
  connect(environmentAction, &QAction::triggered, this, &MainWindow::runEnvironmentCheck);
  environmentAction->setObjectName(QStringLiteral("environmentAction"));

  mTrainAction = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("开始训练..."), this);
  mTrainAction->setToolTip(QStringLiteral("启动当前工程训练"));
  connect(mTrainAction, &QAction::triggered, this, &MainWindow::startTraining);

  mStopAction = new QAction(style()->standardIcon(QStyle::SP_MediaStop), QStringLiteral("停止任务"), this);
  mStopAction->setToolTip(QStringLiteral("停止当前任务"));
  mStopAction->setEnabled(false);
  connect(mStopAction, &QAction::triggered, &mProcessSupervisor, &ProcessSupervisor::stop);

  auto *resetCameraAction = new QAction(style()->standardIcon(QStyle::SP_BrowserReload), QStringLiteral("重置视图"), this);
  resetCameraAction->setShortcut(QKeySequence(QStringLiteral("Home")));
  resetCameraAction->setToolTip(QStringLiteral("重置视图"));
  connect(resetCameraAction, &QAction::triggered, mViewport, &NativeViewport::resetCamera);
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

  mRectangleAction = new QAction(
      style()->standardIcon(QStyle::SP_FileDialogDetailedView),
      QStringLiteral("框选"), this);
  mRectangleAction->setCheckable(true);
  mRectangleAction->setShortcut(QKeySequence(QStringLiteral("R")));
  mRectangleAction->setToolTip(
      QStringLiteral("矩形选择 (R)，Shift 添加，Alt 减去"));
  mEditModeActionGroup->addAction(mRectangleAction);
  connect(mRectangleAction, &QAction::triggered, this, [this]() {
    mViewport->setInteractionMode(NativeViewport::InteractionMode::Rectangle);
  });

  mLassoAction = new QAction(
      style()->standardIcon(QStyle::SP_FileDialogListView),
      QStringLiteral("套索"), this);
  mLassoAction->setCheckable(true);
  mLassoAction->setShortcut(QKeySequence(QStringLiteral("L")));
  mLassoAction->setToolTip(
      QStringLiteral("套索选择 (L)，Shift 添加，Alt 减去"));
  mEditModeActionGroup->addAction(mLassoAction);
  connect(mLassoAction, &QAction::triggered, this, [this]() {
    mViewport->setInteractionMode(NativeViewport::InteractionMode::Lasso);
  });

  mBrushAction = new QAction(
      style()->standardIcon(QStyle::SP_FileDialogListView),
      QStringLiteral("笔刷"), this);
  mBrushAction->setCheckable(true);
  mBrushAction->setShortcut(QKeySequence(QStringLiteral("B")));
  mBrushAction->setToolTip(
      QStringLiteral("连续笔刷选择 (B)，Shift 添加，Alt 减去"));
  mEditModeActionGroup->addAction(mBrushAction);
  connect(mBrushAction, &QAction::triggered, this, [this]() {
    mViewport->setInteractionMode(NativeViewport::InteractionMode::Brush);
  });

  mVisibleOnlyAction = new QAction(
      style()->standardIcon(QStyle::SP_DialogApplyButton),
      QStringLiteral("仅选择可见点"), this);
  mVisibleOnlyAction->setCheckable(true);
  mVisibleOnlyAction->setChecked(true);
  mVisibleOnlyAction->setToolTip(QStringLiteral("仅选择当前视角可见的点"));
  connect(mVisibleOnlyAction, &QAction::toggled, mViewport,
          &NativeViewport::setVisibleOnlySelection);

  mClearSelectionAction = new QAction(
      style()->standardIcon(QStyle::SP_DialogResetButton),
      QStringLiteral("清除选择"), this);
  mClearSelectionAction->setShortcut(QKeySequence(Qt::Key_Escape));
  mClearSelectionAction->setToolTip(QStringLiteral("清除选择 (Esc)"));
  connect(mClearSelectionAction, &QAction::triggered, mViewport,
          &NativeViewport::clearSelection);

  mInvertSelectionAction = new QAction(
      style()->standardIcon(QStyle::SP_BrowserReload),
      QStringLiteral("反选"), this);
  mInvertSelectionAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
  mInvertSelectionAction->setToolTip(QStringLiteral("反选未删除的点 (Ctrl+I)"));
  connect(mInvertSelectionAction, &QAction::triggered, mViewport,
          &NativeViewport::invertSelection);

  mDeleteSelectionAction = new QAction(
      style()->standardIcon(QStyle::SP_TrashIcon), QStringLiteral("删除所选"),
      this);
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

  mExportCropAction = new QAction(
      style()->standardIcon(QStyle::SP_DialogSaveButton),
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
  connect(resetLayoutAction, &QAction::triggered, this, &MainWindow::resetDockLayout);
  resetLayoutAction->setObjectName(QStringLiteral("resetLayoutAction"));

  addAction(newAction);
  addAction(openAction);
  addAction(saveAsAction);
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
  fileMenu->addAction(actions().at(2));
  fileMenu->addSeparator();
  fileMenu->addAction(mImportDatasetAction);
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
  workflowMenu->addAction(actions().at(3));
  workflowMenu->addSeparator();
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
  viewMenu->addSeparator();
  viewMenu->addAction(mProjectDock->toggleViewAction());
  viewMenu->addAction(mInspectorDock->toggleViewAction());
  viewMenu->addAction(mTaskDock->toggleViewAction());
  viewMenu->addSeparator();
  viewMenu->addAction(actions().at(6));

  QMenu *scaleMenu = viewMenu->addMenu(QStringLiteral("界面缩放"));
  mScaleActionGroup = new QActionGroup(this);
  mScaleActionGroup->setExclusive(true);
  const QList<int> scales = {75, 80, 85, 90, 100, 110, 125};
  for (const int scale : scales) {
    auto *action = scaleMenu->addAction(QStringLiteral("%1%").arg(scale));
    action->setCheckable(true);
    action->setData(scale);
    action->setChecked(scale == mUiScalePercent);
    mScaleActionGroup->addAction(action);
  }
  connect(mScaleActionGroup, &QActionGroup::triggered, this,
          [this](QAction *action) { applyUiScale(action->data().toInt(), true); });

  QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
  auto *aboutAction = helpMenu->addAction(QStringLiteral("关于 Gaussian Scene Workbench"));
  connect(aboutAction, &QAction::triggered, this, [this]() {
    QMessageBox::about(
        this, QStringLiteral("关于 Gaussian Scene Workbench"),
        QStringLiteral("<b>Gaussian Scene Workbench 0.3.0 Native Preview</b><br>"
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
  mainToolbar->addAction(actions().at(3));
  mainToolbar->addAction(mTrainAction);
  mainToolbar->addAction(mStopAction);
  mainToolbar->addSeparator();
  mainToolbar->addAction(actions().at(4));

  auto *renderToolbar = addToolBar(QStringLiteral("渲染模式"));
  renderToolbar->setObjectName(QStringLiteral("renderToolbar"));
  renderToolbar->setMovable(false);
  renderToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  renderToolbar->addAction(mGaussianRenderAction);
  renderToolbar->addAction(mPointRenderAction);

  auto *editToolbar = addToolBar(QStringLiteral("场景编辑"));
  editToolbar->setObjectName(QStringLiteral("editToolbar"));
  editToolbar->setMovable(false);
  editToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  editToolbar->addAction(mInspectAction);
  editToolbar->addAction(mRectangleAction);
  editToolbar->addAction(mLassoAction);
  editToolbar->addAction(mBrushAction);
  editToolbar->addAction(mVisibleOnlyAction);
  editToolbar->addSeparator();
  auto *brushRadiusLabel = new QLabel(QStringLiteral("半径"), editToolbar);
  brushRadiusLabel->setObjectName(QStringLiteral("mutedLabel"));
  editToolbar->addWidget(brushRadiusLabel);
  mBrushRadiusSpin = new QSpinBox(editToolbar);
  mBrushRadiusSpin->setObjectName(QStringLiteral("brushRadiusSpin"));
  mBrushRadiusSpin->setAccessibleName(QStringLiteral("笔刷半径"));
  mBrushRadiusSpin->setRange(4, 256);
  mBrushRadiusSpin->setSingleStep(4);
  mBrushRadiusSpin->setSuffix(QStringLiteral(" px"));
  mBrushRadiusSpin->setKeyboardTracking(false);
  mBrushRadiusSpin->setMinimumWidth(86);
  mBrushRadiusSpin->setMaximumWidth(110);
  const int savedBrushRadius =
      std::clamp(QSettings().value(QStringLiteral("selection/brushRadius"), 32)
                     .toInt(),
                 4, 256);
  mBrushRadiusSpin->setValue(savedBrushRadius);
  mViewport->setBrushRadius(savedBrushRadius);
  connect(mBrushRadiusSpin, &QSpinBox::valueChanged, this,
          [this](const int radius) {
            mViewport->setBrushRadius(radius);
            QSettings().setValue(QStringLiteral("selection/brushRadius"),
                                 radius);
          });
  editToolbar->addWidget(mBrushRadiusSpin);
  editToolbar->addSeparator();
  editToolbar->addAction(mClearSelectionAction);
  editToolbar->addAction(mInvertSelectionAction);
  editToolbar->addAction(mDeleteSelectionAction);
  editToolbar->addSeparator();
  editToolbar->addAction(mUndoEditAction);
  editToolbar->addAction(mRedoEditAction);
  editToolbar->addAction(mExportCropAction);
}

void MainWindow::createProjectDock() {
  mProjectDock = new QDockWidget(QStringLiteral("工程"), this);
  mProjectDock->setObjectName(QStringLiteral("projectDock"));
  mProjectDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
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
  mInspectorDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

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
  mProjectRootValue = createValueLabel(panel);
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
  sceneForm->addRow(QStringLiteral("文件"), mSceneValue);
  sceneForm->addRow(QStringLiteral("数量"), mGaussianCountValue);
  sceneForm->addRow(QStringLiteral("格式"), mPlyFormatValue);
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

  auto *tabs = new QTabWidget(mTaskDock);
  mTaskTable = new QTableWidget(0, 4, tabs);
  mTaskTable->setHorizontalHeaderLabels(
      {QStringLiteral("状态"), QStringLiteral("任务"), QStringLiteral("开始时间"), QStringLiteral("结果")});
  mTaskTable->setAlternatingRowColors(true);
  mTaskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  mTaskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  mTaskTable->verticalHeader()->setVisible(false);
  mTaskTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  mTaskTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  mTaskTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
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
  mScaleStatus->setObjectName(QStringLiteral("mutedLabel"));
  statusBar()->addWidget(mProjectStatus, 1);
  statusBar()->addPermanentWidget(mRendererStatus);
  statusBar()->addPermanentWidget(mEditStatus);
  statusBar()->addPermanentWidget(mScaleStatus);
}

void MainWindow::connectServices() {
  connect(&mWorkspace, &WorkspaceDocument::changed, this, &MainWindow::updateWorkspaceUi);
  connect(&mWorkspace, &WorkspaceDocument::modifiedChanged, this, [this](const bool modified) {
    setWindowModified(modified);
  });
  connect(&mProcessSupervisor, &ProcessSupervisor::runningChanged, this, [this](const bool running) {
    mStopAction->setEnabled(running);
    mTrainAction->setEnabled(!running && mWorkspace.hasProject() && !mWorkspace.datasetPath().isEmpty());
  });
  connect(&mProcessSupervisor, &ProcessSupervisor::taskStarted, this, [this](const QString &taskName) {
    mActiveTaskRow = mTaskTable->rowCount();
    mTaskTable->insertRow(mActiveTaskRow);
    auto *state = new QTableWidgetItem(QStringLiteral("运行中"));
    state->setForeground(QColor(102, 193, 168));
    mTaskTable->setItem(mActiveTaskRow, 0, state);
    mTaskTable->setItem(mActiveTaskRow, 1, new QTableWidgetItem(taskName));
    mTaskTable->setItem(mActiveTaskRow, 2,
                        new QTableWidgetItem(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    mTaskTable->setItem(mActiveTaskRow, 3, new QTableWidgetItem(QStringLiteral("-")));
    appendTaskEvent(QStringLiteral("开始任务：%1").arg(taskName));
  });
  connect(&mProcessSupervisor, &ProcessSupervisor::outputReady, this, &MainWindow::appendLog);
  connect(&mProcessSupervisor, &ProcessSupervisor::taskFinished, this,
          [this](const QString &taskName, const int exitCode, const bool succeeded) {
            if (mActiveTaskRow >= 0 && mActiveTaskRow < mTaskTable->rowCount()) {
              auto *state = mTaskTable->item(mActiveTaskRow, 0);
              state->setText(succeeded ? QStringLiteral("完成") : QStringLiteral("失败"));
              state->setForeground(succeeded ? QColor(102, 193, 168) : QColor(211, 95, 95));
              mTaskTable->item(mActiveTaskRow, 3)->setText(
                  succeeded ? QStringLiteral("退出码 0") : QStringLiteral("退出码 %1").arg(exitCode));
            }
            appendTaskEvent(QStringLiteral("任务%1：%2").arg(succeeded ? QStringLiteral("完成") : QStringLiteral("失败"), taskName));
            mActiveTaskRow = -1;
          });
  connect(mViewport, &NativeViewport::frameTimeChanged, this, [this](const double milliseconds) {
    if (!mWorkspace.scenePath().isEmpty()) {
      const QString renderer =
          mRenderMode == NativeViewport::RenderMode::Gaussians
              ? QStringLiteral("高斯预览")
              : QStringLiteral("点预览");
      mRendererStatus->setText(
          QStringLiteral("%1 | CPU 提交 %2 ms")
              .arg(renderer)
              .arg(milliseconds, 0, 'f', 2));
    }
  });
  connect(mViewport, &NativeViewport::sceneLoadStarted, this, [this](const QString &scenePath) {
    mRendererStatus->setText(QStringLiteral("正在读取点云"));
    appendTaskEvent(QStringLiteral("读取场景：%1").arg(QDir::toNativeSeparators(scenePath)));
  });
  connect(mViewport, &NativeViewport::sceneLoaded, this,
          [this](const qint64 sourceVertexCount, const qsizetype previewVertexCount) {
            const QString renderer =
                mRenderMode == NativeViewport::RenderMode::Gaussians
                    ? QStringLiteral("高斯预览")
                    : QStringLiteral("点预览");
            mRendererStatus->setText(
                QStringLiteral("%1 | %2 个预览图元")
                    .arg(renderer)
                    .arg(previewVertexCount));
            appendTaskEvent(QStringLiteral("场景已载入 GPU 预览：源数据 %1 个图元，显示 %2 个图元。")
                                .arg(sourceVertexCount)
                                .arg(previewVertexCount));
          });
  connect(mViewport, &NativeViewport::sceneLoadFailed, this,
          [this](const QString &, const QString &message) {
            mRendererStatus->setText(QStringLiteral("点云读取失败"));
            appendTaskEvent(QStringLiteral("场景读取失败：%1").arg(message));
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
  connect(mViewport,
          &NativeViewport::gaussianRenderingAvailabilityChanged, this,
          [this](const bool available) {
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
            mPointRenderAction->setChecked(
                mode == NativeViewport::RenderMode::Points);
          });
}

void MainWindow::restoreWindowState() {
  QSettings settings;
  const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
  const QByteArray state = settings.value(QStringLiteral("window/state")).toByteArray();
  if (!geometry.isEmpty()) {
    restoreGeometry(geometry);
  } else {
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    const QSize size(std::min(available.width() * 92 / 100, 1600),
                     std::min(available.height() * 90 / 100, 1000));
    resize(size);
    move(available.center() - rect().center());
  }
  if (!state.isEmpty()) {
    restoreState(state, 1);
  } else {
    resetDockLayout();
  }
}

void MainWindow::saveWindowState() {
  QSettings settings;
  settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("window/state"), saveState(1));
}

void MainWindow::resetDockLayout() {
  addDockWidget(Qt::LeftDockWidgetArea, mProjectDock);
  addDockWidget(Qt::RightDockWidgetArea, mInspectorDock);
  addDockWidget(Qt::BottomDockWidgetArea, mTaskDock);
  mProjectDock->show();
  mInspectorDock->show();
  mTaskDock->show();
  resizeDocks({mProjectDock, mInspectorDock},
              {AppTheme::scaled(250, mUiScalePercent), AppTheme::scaled(300, mUiScalePercent)}, Qt::Horizontal);
  resizeDocks({mTaskDock}, {AppTheme::scaled(210, mUiScalePercent)}, Qt::Vertical);
}

void MainWindow::applyUiScale(const int scalePercent, const bool persist) {
  mUiScalePercent = scalePercent;
  AppTheme::apply(*qApp, mUiScalePercent, persist);
  const QSize iconSize(AppTheme::scaled(19, mUiScalePercent), AppTheme::scaled(19, mUiScalePercent));
  for (QToolBar *toolbar : findChildren<QToolBar *>()) {
    toolbar->setIconSize(iconSize);
  }
  mScaleStatus->setText(QStringLiteral("UI %1%").arg(mUiScalePercent));
  if (mScaleActionGroup != nullptr) {
    for (QAction *action : mScaleActionGroup->actions()) {
      action->setChecked(action->data().toInt() == mUiScalePercent);
    }
  }
}

void MainWindow::updateEditActions() {
  const bool interactive = mSceneReady && !mSelectionBusy;
  if (mInspectAction == nullptr) {
    return;
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
    mEditStatus->setText(
        mSelectionBusy
            ? QStringLiteral("正在计算选择")
            : QStringLiteral("选择 %1 | 删除 %2")
                  .arg(mSelectedPointCount)
                  .arg(mDeletedPointCount));
  }
}

bool MainWindow::confirmDiscardChanges() {
  if (mWorkspace.isModified()) {
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, QStringLiteral("工程尚未保存"),
        QStringLiteral("当前工程包含未保存的修改。"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (answer == QMessageBox::Save && !saveProject(false)) {
      return false;
    }
    if (answer == QMessageBox::Cancel) {
      return false;
    }
  }
  return confirmDiscardSceneEdits();
}

bool MainWindow::confirmDiscardSceneEdits() {
  if (!mViewport->hasUnsavedSceneEdits()) {
    return true;
  }
  const QMessageBox::StandardButton answer = QMessageBox::warning(
      this, QStringLiteral("裁剪尚未导出"),
      QStringLiteral("当前场景包含尚未导出的删除操作。"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  if (answer == QMessageBox::Save) {
    return exportCroppedScene();
  }
  return answer == QMessageBox::Discard;
}

bool MainWindow::saveProject(const bool forceChoosePath) {
  if (!mWorkspace.hasProject()) {
    return false;
  }
  QString target = mWorkspace.projectFilePath();
  if (forceChoosePath || target.isEmpty()) {
    target = QFileDialog::getSaveFileName(this, QStringLiteral("保存工程"), suggestedProjectFilePath(),
                                          QStringLiteral("GSW Project (*.gsw.json)"));
    if (target.isEmpty()) {
      return false;
    }
    if (!target.endsWith(QStringLiteral(".gsw.json"), Qt::CaseInsensitive)) {
      target += QStringLiteral(".gsw.json");
    }
  }
  QString error;
  if (!mWorkspace.save(target, &error)) {
    showError(QStringLiteral("无法保存工程"), error);
    return false;
  }
  appendTaskEvent(QStringLiteral("工程已保存：%1").arg(QDir::toNativeSeparators(target)));
  return true;
}

bool MainWindow::exportCroppedScene() {
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

void MainWindow::newProject() {
  if (!confirmDiscardChanges()) {
    return;
  }
  const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择工程目录"));
  if (directory.isEmpty()) {
    return;
  }
  QString error;
  if (!mWorkspace.create(directory, &error)) {
    showError(QStringLiteral("无法创建工程"), error);
    return;
  }
  saveProject(false);
}

void MainWindow::importDataset() {
  if (!mWorkspace.hasProject()) {
    QMessageBox::information(this, QStringLiteral("请先创建工程"), QStringLiteral("导入数据前需要创建或打开工程。"));
    return;
  }
  const QString directory = QFileDialog::getExistingDirectory(
      this, QStringLiteral("选择图像数据集"), mWorkspace.rootPath());
  if (directory.isEmpty()) {
    return;
  }
  QString error;
  if (!mWorkspace.setDatasetPath(directory, &error)) {
    showError(QStringLiteral("无法导入数据集"), error);
    return;
  }
  appendTaskEvent(QStringLiteral("已导入数据集：%1 张图像").arg(mWorkspace.imageCount()));
}

void MainWindow::importScene() {
  if (!mWorkspace.hasProject()) {
    QMessageBox::information(this, QStringLiteral("请先创建工程"), QStringLiteral("导入场景前需要创建或打开工程。"));
    return;
  }
  if (!confirmDiscardSceneEdits()) {
    return;
  }
  const QString filePath = QFileDialog::getOpenFileName(
      this, QStringLiteral("导入高斯场景"), mWorkspace.rootPath(), QStringLiteral("PLY Scene (*.ply);;All files (*.*)"));
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
                      .arg(metadata.looksLikeGaussianSplat() ? QStringLiteral("Gaussian Splat") : QStringLiteral("PLY")));
}

void MainWindow::runEnvironmentCheck() {
  const QString root = findRepositoryRoot();
  const QString script = QDir(root).filePath(QStringLiteral("scripts/check_3dgs_env.ps1"));
  if (root.isEmpty() || !QFileInfo::exists(script)) {
    showError(QStringLiteral("找不到环境检查脚本"), QStringLiteral("请从完整源码目录运行原生应用。"));
    return;
  }
  const bool started = mProcessSupervisor.start(
      QStringLiteral("环境检查"), QStringLiteral("powershell.exe"),
      {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
       QStringLiteral("-File"), QDir::toNativeSeparators(script)},
      root);
  if (!started) {
    QMessageBox::information(this, QStringLiteral("任务繁忙"), QStringLiteral("请等待当前任务结束后再试。"));
  }
}

void MainWindow::startTraining() {
  if (mWorkspace.datasetPath().isEmpty()) {
    QMessageBox::information(this, QStringLiteral("尚未导入数据集"), QStringLiteral("请先导入包含 COLMAP 数据的图像工程。"));
    return;
  }
  const QString root = findRepositoryRoot();
  const QString workerScript = QDir(root).filePath(QStringLiteral("native/worker/gsw_worker.py"));
  const QString python = findTrainingPython(root);
  if (root.isEmpty() || !QFileInfo::exists(workerScript) || python.isEmpty()) {
    showError(QStringLiteral("训练后端不可用"),
              QStringLiteral("未找到原生训练 worker、gaussian-splatting 源码或 gaussian_splatting Python 环境。"));
    return;
  }

  const QString defaultOutputRoot = QDir(mWorkspace.rootPath()).filePath(QStringLiteral("output"));
  TrainingDialog dialog(mWorkspace.datasetPath(), mWorkspace.projectName(), defaultOutputRoot,
                        hasRecognizedSparseScene(mWorkspace.datasetPath()), twoDgsAvailable(root), this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const TrainingConfiguration config = dialog.configuration();

  const QString jobsRoot = QDir(mWorkspace.rootPath()).filePath(QStringLiteral(".gsw/jobs"));
  const QString trainingJobStore = QDir(jobsRoot).filePath(QStringLiteral("training"));
  if (!QDir().mkpath(trainingJobStore)) {
    showError(QStringLiteral("无法创建任务目录"), QDir::toNativeSeparators(trainingJobStore));
    return;
  }

  QJsonObject trainOptions;
  trainOptions.insert(QStringLiteral("iterations"), config.iterations);
  trainOptions.insert(QStringLiteral("resolution"), config.resolution);
  QJsonObject workerConfig;
  workerConfig.insert(QStringLiteral("repositoryRoot"), QDir::cleanPath(root));
  workerConfig.insert(QStringLiteral("datasetPath"), QDir::cleanPath(mWorkspace.datasetPath()));
  workerConfig.insert(QStringLiteral("outputRoot"), QDir::cleanPath(config.outputRoot));
  workerConfig.insert(QStringLiteral("outputScene"), config.outputScene);
  workerConfig.insert(QStringLiteral("scene"), QStringLiteral("native-project"));
  workerConfig.insert(QStringLiteral("backend"), config.backend);
  workerConfig.insert(QStringLiteral("quality"), config.quality);
  workerConfig.insert(QStringLiteral("runColmap"), config.runColmap);
  workerConfig.insert(QStringLiteral("overwrite"), config.overwrite);
  workerConfig.insert(QStringLiteral("jobStore"), QDir::cleanPath(trainingJobStore));
  workerConfig.insert(QStringLiteral("colmapOptions"), QJsonObject());
  workerConfig.insert(QStringLiteral("trainOptions"), trainOptions);

  const QString configPath = QDir(jobsRoot).filePath(
      QStringLiteral("training-%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QSaveFile configFile(configPath);
  if (!configFile.open(QIODevice::WriteOnly) ||
      configFile.write(QJsonDocument(workerConfig).toJson(QJsonDocument::Indented)) < 0 ||
      !configFile.commit()) {
    showError(QStringLiteral("无法保存训练任务"), configFile.errorString());
    return;
  }

  const bool started = mProcessSupervisor.start(
      QStringLiteral("%1 训练 | %2 | %3 次迭代")
          .arg(config.backend.toUpper(), config.quality)
          .arg(config.iterations),
      python, {workerScript, QStringLiteral("--config"), configPath}, root,
      pythonProcessEnvironment(python), true);
  if (!started) {
    QMessageBox::information(this, QStringLiteral("任务繁忙"), QStringLiteral("请等待当前任务结束后再试。"));
  } else {
    appendTaskEvent(QStringLiteral("训练配置已保存：%1").arg(QDir::toNativeSeparators(configPath)));
  }
}

void MainWindow::updateWorkspaceUi() {
  rebuildProjectTree();
  updateInspector();
  const QString projectName = mWorkspace.hasProject() ? mWorkspace.projectName() : QStringLiteral("未打开工程");
  mViewport->setProjectLabel(projectName);
  mViewport->setScene(mWorkspace.scenePath(), mWorkspace.sceneMetadata().vertexCount);
  mProjectStatus->setText(projectName + (mWorkspace.isModified() ? QStringLiteral(" *") : QString()));
  setWindowTitle(QStringLiteral("Gaussian Scene Workbench - %1[*]").arg(projectName));
  mSaveAction->setEnabled(mWorkspace.hasProject());
  mImportDatasetAction->setEnabled(mWorkspace.hasProject());
  mImportSceneAction->setEnabled(mWorkspace.hasProject());
  mTrainAction->setEnabled(!mProcessSupervisor.isRunning() && mWorkspace.hasProject() &&
                           !mWorkspace.datasetPath().isEmpty());
  updateEditActions();
}

void MainWindow::rebuildProjectTree() {
  mProjectTree->clear();
  const QString projectName = mWorkspace.hasProject() ? mWorkspace.projectName() : QStringLiteral("未打开工程");
  auto *root = new QTreeWidgetItem(mProjectTree, {projectName});
  root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
  root->setData(0, Qt::UserRole, mWorkspace.rootPath());

  auto *dataset = new QTreeWidgetItem(root, {QStringLiteral("数据集")});
  dataset->setIcon(0, style()->standardIcon(QStyle::SP_DirOpenIcon));
  dataset->setData(0, Qt::UserRole, mWorkspace.datasetPath());
  new QTreeWidgetItem(dataset, {mWorkspace.datasetPath().isEmpty()
                                    ? QStringLiteral("未导入")
                                    : QStringLiteral("图像 %1").arg(mWorkspace.imageCount())});

  auto *reconstruction = new QTreeWidgetItem(root, {QStringLiteral("重建")});
  reconstruction->setIcon(0, style()->standardIcon(QStyle::SP_FileDialogContentsView));
  const bool hasSparse = hasRecognizedSparseScene(mWorkspace.datasetPath());
  new QTreeWidgetItem(reconstruction, {hasSparse ? QStringLiteral("COLMAP sparse") : QStringLiteral("未检测到稀疏重建")});

  auto *scene = new QTreeWidgetItem(root, {QStringLiteral("场景")});
  scene->setIcon(0, style()->standardIcon(QStyle::SP_FileDialogDetailedView));
  scene->setData(0, Qt::UserRole, mWorkspace.scenePath());
  new QTreeWidgetItem(scene, {mWorkspace.scenePath().isEmpty()
                                  ? QStringLiteral("未导入")
                                  : QFileInfo(mWorkspace.scenePath()).fileName()});

  auto *jobs = new QTreeWidgetItem(root, {QStringLiteral("任务")});
  jobs->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
  new QTreeWidgetItem(jobs, {mProcessSupervisor.isRunning() ? mProcessSupervisor.activeTask() : QStringLiteral("空闲")});

  root->setExpanded(true);
  dataset->setExpanded(true);
  scene->setExpanded(true);
}

void MainWindow::updateInspector() {
  mProjectNameValue->setText(mWorkspace.hasProject() ? mWorkspace.projectName() : QStringLiteral("-"));
  mProjectRootValue->setText(compactPath(mWorkspace.rootPath()));
  mDatasetValue->setText(compactPath(mWorkspace.datasetPath()));
  mImageCountValue->setText(mWorkspace.datasetPath().isEmpty()
                                ? QStringLiteral("-")
                                : QStringLiteral("%1 张").arg(mWorkspace.imageCount()));
  mSceneValue->setText(compactPath(mWorkspace.scenePath()));
  const PlyMetadata metadata = mWorkspace.sceneMetadata();
  mGaussianCountValue->setText(metadata.valid ? QStringLiteral("%1").arg(metadata.vertexCount) : QStringLiteral("-"));
  mPlyFormatValue->setText(metadata.valid
                              ? QStringLiteral("%1 | %2 | %3")
                                    .arg(metadata.format,
                                         metadata.looksLikeGaussianSplat() ? QStringLiteral("Gaussian Splat") : QStringLiteral("PLY"),
                                         formatFileSize(metadata.fileSize))
                              : QStringLiteral("-"));
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
  appendLog(QStringLiteral("[%1] %2\n").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text));
}

void MainWindow::showError(const QString &title, const QString &message) {
  QMessageBox::critical(this, title, message);
  appendTaskEvent(QStringLiteral("错误：%1").arg(message));
}

QString MainWindow::findRepositoryRoot() const {
  QStringList candidates = {QCoreApplication::applicationDirPath(), QDir::currentPath(), mWorkspace.rootPath()};
  for (QString candidate : candidates) {
    if (candidate.isEmpty()) {
      continue;
    }
    QDir directory(candidate);
    for (int depth = 0; depth < 10; ++depth) {
      const bool hasEnvironmentScript = QFileInfo::exists(directory.filePath(QStringLiteral("scripts/check_3dgs_env.ps1")));
      const bool hasTrainingScript = QFileInfo::exists(directory.filePath(QStringLiteral("gaussian-splatting/train.py")));
      if (hasEnvironmentScript && hasTrainingScript) {
        return QDir::cleanPath(directory.absolutePath());
      }
      if (!directory.cdUp()) {
        break;
      }
    }
  }
  return {};
}

QString MainWindow::findTrainingPython(const QString &repositoryRoot) const {
  const QString configured = qEnvironmentVariable("GAUSSIAN_SPLATTING_CONDA_PREFIX");
  QStringList candidates;
  if (!configured.isEmpty()) {
    candidates.append(QDir(configured).filePath(QStringLiteral("python.exe")));
  }
  const QString driveRoot = QDir(repositoryRoot).rootPath();
  candidates.append(QDir(driveRoot).filePath(QStringLiteral("conda/envs/gaussian_splatting/python.exe")));
  candidates.append(QDir(driveRoot).filePath(QStringLiteral("miniforge3/envs/gaussian_splatting/python.exe")));
  candidates.append(QDir(QDir::homePath()).filePath(QStringLiteral("miniforge3/envs/gaussian_splatting/python.exe")));

  for (const QString &candidate : candidates) {
    if (QFileInfo::exists(candidate)) {
      return QDir::toNativeSeparators(candidate);
    }
  }
  return QStandardPaths::findExecutable(QStringLiteral("python.exe"));
}

QString MainWindow::suggestedProjectFilePath() const {
  return QDir(mWorkspace.rootPath()).filePath(safeFileName(mWorkspace.projectName()) + QStringLiteral(".gsw.json"));
}

} // namespace gsw
