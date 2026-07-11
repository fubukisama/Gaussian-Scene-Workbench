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

  bool confirmDiscardChanges();
  bool saveProject(bool forceChoosePath = false);
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
  QLabel *mScaleStatus = nullptr;

  QAction *mSaveAction = nullptr;
  QAction *mImportDatasetAction = nullptr;
  QAction *mImportSceneAction = nullptr;
  QAction *mTrainAction = nullptr;
  QAction *mStopAction = nullptr;
  QActionGroup *mScaleActionGroup = nullptr;

  int mUiScalePercent = 90;
  int mActiveTaskRow = -1;
};

} // namespace gsw
