#pragma once

#include "NativeViewport.h"
#include "ProcessSupervisor.h"
#include "WorkspaceDocument.h"

#include <QMainWindow>
#include <QStringList>

#include <optional>

class QAction;
class QActionGroup;
class QCloseEvent;
class QDockWidget;
class QLabel;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
class QTreeWidget;

namespace gsw {

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  bool openProjectFile(const QString &filePath);

protected:
  void closeEvent(QCloseEvent *event) override;

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
  void applyUiScale(int scalePercent, bool persist);
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
  QActionGroup *mEditModeActionGroup = nullptr;
  QActionGroup *mRenderModeActionGroup = nullptr;
  QActionGroup *mScaleActionGroup = nullptr;
  QSpinBox *mBrushRadiusSpin = nullptr;

  int mUiScalePercent = 90;
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
  NativeViewport::RenderMode mRenderMode = NativeViewport::RenderMode::Points;
};

} // namespace gsw
