#pragma once

#include "NativeViewport.h"
#include "ProcessSupervisor.h"
#include "WorkspaceDocument.h"

#include <QMainWindow>
#include <QSize>
#include <QStringList>

#include <optional>

class QAction;
class QActionGroup;
class QCloseEvent;
class QDockWidget;
class QLabel;
class QMoveEvent;
class QPlainTextEdit;
class QResizeEvent;
class QSpinBox;
class QTableWidget;
class QTimer;
class QToolBar;
class QTreeWidget;

namespace gsw {

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  bool openProjectFile(const QString &filePath);

protected:
  void closeEvent(QCloseEvent *event) override;
  void moveEvent(QMoveEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

private:
  struct PendingDatasetImport {
    QString taskName;
    QString datasetPath;
    QString projectRoot;
    QString configurationPath;
    QString python;
    QString workerScript;
    QString workingDirectory;
    QString recoveryTask;
  };

  struct PendingTrainingResult {
    QString taskName;
    QString outputSceneRoot;
    QString projectRoot;
  };

  void createActions();
  void createMenus();
  void createToolBars();
  void createProjectDock();
  void createInspectorDock();
  void createTaskDock();
  void createStatusBar();
  void connectServices();
  void restoreWindowState();
  void saveWindowState();
  void resetDockLayout();
  void updateDockMetrics();
  void rebalanceDockSizes();
  void applyUiScale(int scalePercent, bool persist);
  void setAutomaticUiScale(bool automatic, bool persist);
  void refreshAutomaticUiScale();
  void scheduleAutomaticUiScale();
  void updateScaleStatus();
  void applyWindowResolution(const QSize &requestedSize);
  void fitWindowToScreen();
  void updateActionAvailability();
  void updateEditActions();

  bool confirmDiscardChanges();
  bool confirmDiscardSceneEdits();
  bool ensureProjectForDataAction(const QString &actionName);
  bool saveProject(bool forceChoosePath = false);
  bool exportCroppedScene();
  bool ensureProjectRecoveryReady();
  bool recoverDatasetImport(const PendingDatasetImport &pending,
                            QString *errorMessage = nullptr,
                            bool *committed = nullptr,
                            QStringList *committedPaths = nullptr);
  bool recoverInterruptedProjectImports(QString *errorMessage = nullptr);
  void newProject();
  void importDataset();
  void importDatasetDirectory();
  void importDatasetSources(const QStringList &sourcePaths);
  void attachExistingDataset();
  void importScene();
  void runEnvironmentCheck();
  void startReconstruction();
  void startTraining();
  void updateWorkspaceUi();
  void rebuildProjectTree();
  void updateInspector();
  void appendLog(const QString &text);
  void appendTaskEvent(const QString &text);
  void showError(const QString &title, const QString &message);

  [[nodiscard]] QString findTrainingPython(const QString &repositoryRoot) const;
  [[nodiscard]] QString suggestedProjectFilePath() const;

  WorkspaceDocument mWorkspace;
  ProcessSupervisor mProcessSupervisor;
  NativeViewport *mViewport = nullptr;
  QDockWidget *mProjectDock = nullptr;
  QDockWidget *mInspectorDock = nullptr;
  QDockWidget *mTaskDock = nullptr;
  QTreeWidget *mProjectTree = nullptr;
  QTableWidget *mTaskTable = nullptr;
  QPlainTextEdit *mConsole = nullptr;
  QToolBar *mRenderToolbar = nullptr;
  QToolBar *mSelectionToolbar = nullptr;
  QToolBar *mEditToolbar = nullptr;
  QTimer *mUiAdaptTimer = nullptr;

  QLabel *mProjectNameValue = nullptr;
  QLabel *mProjectRootValue = nullptr;
  QLabel *mDatasetValue = nullptr;
  QLabel *mImageCountValue = nullptr;
  QLabel *mSceneValue = nullptr;
  QLabel *mGaussianCountValue = nullptr;
  QLabel *mPlyFormatValue = nullptr;
  QLabel *mCameraCountValue = nullptr;
  QLabel *mProjectStatus = nullptr;
  QLabel *mRendererStatus = nullptr;
  QLabel *mEditStatus = nullptr;
  QLabel *mScaleStatus = nullptr;

  QAction *mNewProjectAction = nullptr;
  QAction *mOpenProjectAction = nullptr;
  QAction *mSaveAction = nullptr;
  QAction *mImportDatasetAction = nullptr;
  QAction *mImportDatasetDirectoryAction = nullptr;
  QAction *mAttachDatasetAction = nullptr;
  QAction *mImportSceneAction = nullptr;
  QAction *mReconstructAction = nullptr;
  QAction *mTrainAction = nullptr;
  QAction *mStopAction = nullptr;
  QAction *mGaussianRenderAction = nullptr;
  QAction *mPointRenderAction = nullptr;
  QAction *mShowCamerasAction = nullptr;
  QAction *mInspectAction = nullptr;
  QAction *mRectangleAction = nullptr;
  QAction *mLassoAction = nullptr;
  QAction *mBrushAction = nullptr;
  QAction *mVisibleOnlyAction = nullptr;
  QAction *mClearSelectionAction = nullptr;
  QAction *mInvertSelectionAction = nullptr;
  QAction *mDeleteSelectionAction = nullptr;
  QAction *mUndoEditAction = nullptr;
  QAction *mRedoEditAction = nullptr;
  QAction *mExportCropAction = nullptr;
  QAction *mAutoScaleAction = nullptr;
  QActionGroup *mEditModeActionGroup = nullptr;
  QActionGroup *mRenderModeActionGroup = nullptr;
  QActionGroup *mScaleActionGroup = nullptr;
  QSpinBox *mBrushRadiusSpin = nullptr;

  int mUiScalePercent = 100;
  int mActiveTaskRow = -1;
  std::optional<PendingDatasetImport> mPendingDatasetImport;
  std::optional<PendingTrainingResult> mPendingTrainingResult;
  QString mActiveWorkerState;
  qsizetype mSelectedPointCount = 0;
  qsizetype mDeletedPointCount = 0;
  qsizetype mCameraCount = 0;
  qsizetype mInvalidCameraCount = 0;
  bool mCameraDisplayDecimated = false;
  QString mCameraSourcePath;
  QString mCameraTrajectoryError;
  QString mLastCameraTrajectoryEventKey;
  bool mSceneReady = false;
  bool mSelectionBusy = false;
  bool mCanUndoEdit = false;
  bool mCanRedoEdit = false;
  bool mClosePending = false;
  bool mRecoveryBlocked = false;
  bool mAutomaticUiScale = true;
  NativeViewport::RenderMode mRenderMode = NativeViewport::RenderMode::Points;
};

} // namespace gsw
