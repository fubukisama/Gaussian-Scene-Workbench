#pragma once

#include "NativeViewport.h"
#include "ProcessSupervisor.h"
#include "RecoveryStore.h"
#include "WorkspaceDocument.h"

#include <QMainWindow>
#include <QSize>
#include <QStringList>

#include <memory>
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
class QTabWidget;
class QTimer;
class QToolBar;
class QTreeWidget;

namespace gsw {

class TrainingMonitorWidget;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;
  bool openProjectFile(const QString &filePath);
  bool importSceneFile(const QString &filePath);
  void offerStartupRecovery();

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

  struct PendingTraining {
    QString taskName;
    QString projectRoot;
    QString datasetPath;
    QString outputDirectory;
    QString backend;
    int expectedIterations = 0;
  };

  struct PendingReconstruction {
    QString taskName;
    QString projectRoot;
    QString datasetPath;
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
  void updateEditStatus();

  bool confirmDiscardChanges(bool exiting = false);
  bool confirmDiscardSceneEdits(bool exiting = false);
  bool confirmExit();
  bool beginUntitledProject(const QString &displayName,
                            QString *errorMessage = nullptr);
  void checkpointCurrentRecovery();
  void snapshotCurrentProject();
  bool discardCurrentRecovery(QString *errorMessage = nullptr);
  bool completeCurrentRecovery(const QString &managedProjectRoot,
                               QString *errorMessage = nullptr);
  void showRecoveryCenter(bool startupPrompt);
  void showSnapshotHistory();
  void configureExternalBackup();
  void startExternalBackup(bool interactive);
  void showExternalBackups();
  bool restoreRecoveryWorkspace(const RecoveryWorkspace &workspace);
  bool ensureProjectForDataAction(const QString &actionName);
  bool saveProject(bool forceChoosePath = false);
  bool finalizePendingProjectSave(QString *errorMessage = nullptr);
  bool exportCroppedScene();
  bool exportCoordinateReport();
  bool ensureProjectRecoveryReady();
  bool recoverDatasetImport(const PendingDatasetImport &pending,
                            QString *errorMessage = nullptr,
                            bool *committed = nullptr,
                            QStringList *committedPaths = nullptr);
  bool recoverInterruptedProjectImports(QString *errorMessage = nullptr);
  bool recoverInterruptedTraining(QString *errorMessage = nullptr);
  void newProject();
  void importDataset();
  void importDatasetDirectory();
  void importDatasetSources(const QStringList &sourcePaths);
  void attachExistingDataset();
  void importScene();
  void clearDatasetImport();
  void clearReconstructionImport();
  void clearSceneImport();
  void clearTaskHistory();
  void runEnvironmentCheck();
  void startReconstruction();
  void startTraining();
  void updateWorkspaceUi();
  void retranslateUi();
  void updateTaskLabels();
  void rebuildProjectTree();
  void syncProjectTreeSelection();
  void updateInspector();
  [[nodiscard]] QVector3D workspaceTranslationForViewport() const;
  [[nodiscard]] QVector3D viewportTranslationForWorkspace(
      const QVector3D &translation) const;
  void appendLog(const QString &text);
  void appendTaskEvent(const QString &text);
  void showError(const QString &title, const QString &message);

  [[nodiscard]] QString findTrainingPython(const QString &repositoryRoot) const;
  [[nodiscard]] QString suggestedProjectFilePath() const;

  WorkspaceDocument mWorkspace;
  std::unique_ptr<RecoveryStore> mRecoveryStore;
  std::optional<RecoveryWorkspace> mCurrentRecovery;
  QList<RecoveryWorkspace> mStartupRecovery;
  ProcessSupervisor mProcessSupervisor;
  NativeViewport *mViewport = nullptr;
  QDockWidget *mProjectDock = nullptr;
  QDockWidget *mInspectorDock = nullptr;
  QDockWidget *mTaskDock = nullptr;
  QTreeWidget *mProjectTree = nullptr;
  QTabWidget *mTaskTabs = nullptr;
  QTableWidget *mTaskTable = nullptr;
  QPlainTextEdit *mConsole = nullptr;
  TrainingMonitorWidget *mTrainingMonitor = nullptr;
  QToolBar *mRenderToolbar = nullptr;
  QToolBar *mSelectionToolbar = nullptr;
  QToolBar *mEditToolbar = nullptr;
  QTimer *mUiAdaptTimer = nullptr;
  QTimer *mRecoveryCheckpointTimer = nullptr;

  QLabel *mProjectNameValue = nullptr;
  QLabel *mProjectRootValue = nullptr;
  QLabel *mDatasetValue = nullptr;
  QLabel *mDatasetNameValue = nullptr;
  QLabel *mImageCountValue = nullptr;
  QLabel *mSceneValue = nullptr;
  QLabel *mGaussianCountValue = nullptr;
  QLabel *mPlyFormatValue = nullptr;
  QLabel *mCameraCountValue = nullptr;
  QLabel *mCoordinateSystemValue = nullptr;
  QLabel *mSceneUnitValue = nullptr;
  QLabel *mSceneCenterValue = nullptr;
  QLabel *mSceneSizeValue = nullptr;
  QLabel *mSceneBoundsValue = nullptr;
  QLabel *mDisplayShiftValue = nullptr;
  QLabel *mReferencePlaneValue = nullptr;
  QLabel *mSceneTransformValue = nullptr;
  QLabel *mProjectStatus = nullptr;
  QLabel *mRendererStatus = nullptr;
  QLabel *mEditStatus = nullptr;
  QLabel *mScaleStatus = nullptr;

  QAction *mNewProjectAction = nullptr;
  QAction *mOpenProjectAction = nullptr;
  QAction *mSaveAction = nullptr;
  QAction *mSaveAsAction = nullptr;
  QAction *mRecoveryCenterAction = nullptr;
  QAction *mSnapshotHistoryAction = nullptr;
  QAction *mConfigureBackupAction = nullptr;
  QAction *mExternalBackupsAction = nullptr;
  QAction *mImportDatasetAction = nullptr;
  QAction *mImportDatasetDirectoryAction = nullptr;
  QAction *mAttachDatasetAction = nullptr;
  QAction *mImportSceneAction = nullptr;
  QAction *mClearDatasetAction = nullptr;
  QAction *mClearReconstructionAction = nullptr;
  QAction *mClearSceneAction = nullptr;
  QAction *mClearTasksAction = nullptr;
  QAction *mReconstructAction = nullptr;
  QAction *mTrainAction = nullptr;
  QAction *mStopAction = nullptr;
  QAction *mGaussianRenderAction = nullptr;
  QAction *mMeshRenderAction = nullptr;
  QAction *mPointRenderAction = nullptr;
  QAction *mShowCamerasAction = nullptr;
  QAction *mInspectAction = nullptr;
  QAction *mFindModelAction = nullptr;
  QAction *mMoveModelAction = nullptr;
  QAction *mRotateModelAction = nullptr;
  QAction *mScaleModelAction = nullptr;
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
  QAction *mExportCoordinateReportAction = nullptr;
  QAction *mModelBasePlaneAction = nullptr;
  QAction *mWorldZeroPlaneAction = nullptr;
  QAction *mAutoScaleAction = nullptr;
  QActionGroup *mEditModeActionGroup = nullptr;
  QActionGroup *mRenderModeActionGroup = nullptr;
  QActionGroup *mScaleActionGroup = nullptr;
  QActionGroup *mReferencePlaneActionGroup = nullptr;
  QSpinBox *mBrushRadiusSpin = nullptr;

  int mUiScalePercent = 100;
  int mActiveTaskRow = -1;
  std::optional<PendingDatasetImport> mPendingDatasetImport;
  std::optional<PendingTraining> mPendingTraining;
  std::optional<PendingReconstruction> mPendingReconstruction;
  QString mActiveWorkerState;
  std::optional<WorkerStatus> mLastWorkerStatus;
  qsizetype mSelectedPointCount = 0;
  qsizetype mDeletedPointCount = 0;
  qsizetype mCameraCount = 0;
  qsizetype mInvalidCameraCount = 0;
  bool mCameraDisplayDecimated = false;
  QString mCameraSourcePath;
  QString mCameraTrajectoryError;
  QString mLastCameraTrajectoryEventKey;
  QString mLiveTrainingPreviewPath;
  qint64 mLiveTrainingGaussianCount = 0;
  int mLastTrainingPreviewIteration = -1;
  QString mLiveReconstructionPreviewPath;
  QString mLiveReconstructionDatasetPath;
  qint64 mLiveReconstructionPointCount = 0;
  int mLastReconstructionPreviewIteration = -1;
  bool mSceneReady = false;
  bool mModelReady = false;
  bool mModelSelected = false;
  bool mPreserveProjectTree = false;
  bool mSelectionBusy = false;
  bool mCanUndoEdit = false;
  bool mCanRedoEdit = false;
  bool mClosePending = false;
  bool mExitConfirmed = false;
  bool mRecoveryBlocked = false;
  bool mDiscardRecoveryOnDestruction = false;
  bool mExternalBackupRunning = false;
  bool mSuppressSnapshots = false;
  bool mAutomaticUiScale = true;
  NativeViewport::RenderMode mRenderMode = NativeViewport::RenderMode::Points;
};

} // namespace gsw
