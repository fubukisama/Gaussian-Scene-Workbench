#pragma once

#include "NativeViewport.h"
#include "ProcessSupervisor.h"
#include "WorkspaceDocument.h"

#include <QMainWindow>

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
  void updateEditActions();

  bool confirmDiscardChanges();
  bool confirmDiscardSceneEdits();
  bool saveProject(bool forceChoosePath = false);
  bool exportCroppedScene();
  void newProject();
  void importDataset();
  void importScene();
  void runEnvironmentCheck();
  void startTraining();
  void updateWorkspaceUi();
  void rebuildProjectTree();
  void updateInspector();
  void appendLog(const QString &text);
  void appendTaskEvent(const QString &text);
  void showError(const QString &title, const QString &message);

  [[nodiscard]] QString findRepositoryRoot() const;
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
  QLabel *mProjectStatus = nullptr;
  QLabel *mRendererStatus = nullptr;
  QLabel *mEditStatus = nullptr;
  QLabel *mScaleStatus = nullptr;

  QAction *mSaveAction = nullptr;
  QAction *mImportDatasetAction = nullptr;
  QAction *mImportSceneAction = nullptr;
  QAction *mTrainAction = nullptr;
  QAction *mStopAction = nullptr;
  QAction *mGaussianRenderAction = nullptr;
  QAction *mPointRenderAction = nullptr;
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
  qsizetype mSelectedPointCount = 0;
  qsizetype mDeletedPointCount = 0;
  bool mSceneReady = false;
  bool mSelectionBusy = false;
  bool mCanUndoEdit = false;
  bool mCanRedoEdit = false;
  NativeViewport::RenderMode mRenderMode = NativeViewport::RenderMode::Points;
};

} // namespace gsw
