#include "AppTheme.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QSurfaceFormat>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

#include <algorithm>

#ifdef _WIN32
#include <shobjidl.h>

#include <cwchar>

#define GSW_WIDEN_IMPL(value) L##value
#define GSW_WIDEN(value) GSW_WIDEN_IMPL(value)

namespace {
bool configureWindowsApplicationIdentity() {
  constexpr const wchar_t *expectedId = GSW_WIDEN(GSW_APP_USER_MODEL_ID);
  if (FAILED(SetCurrentProcessExplicitAppUserModelID(expectedId))) {
    return false;
  }

  PWSTR actualId = nullptr;
  const HRESULT result = GetCurrentProcessExplicitAppUserModelID(&actualId);
  const bool matches = SUCCEEDED(result) && actualId != nullptr &&
                       std::wcscmp(actualId, expectedId) == 0;
  CoTaskMemFree(actualId);
  return matches;
}
} // namespace

#undef GSW_WIDEN
#undef GSW_WIDEN_IMPL
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
  if (!configureWindowsApplicationIdentity()) {
    return 4;
  }
#endif

  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  if (qEnvironmentVariableIntValue("GSW_USE_NON_NATIVE_DIALOGS") != 0) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  }

  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  format.setSamples(4);
  format.setSwapInterval(1);
  QSurfaceFormat::setDefaultFormat(format);

  QApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("GaussianSceneWorkbench"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("github.com/fubukisama"));
  QCoreApplication::setApplicationName(QStringLiteral("Gaussian Scene Workbench"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("Gaussian Scene Workbench Native"));
  QCoreApplication::setApplicationVersion(QStringLiteral(GSW_VERSION));
  const QIcon applicationIcon(QStringLiteral(":/icons/gsw-app-icon.png"));
  if (applicationIcon.isNull()) {
    qCritical() << "Failed to load the embedded application icon.";
    return 3;
  }
  application.setWindowIcon(applicationIcon);

  const gsw::UiScaleMode scaleMode = gsw::AppTheme::loadScaleMode();
  const int scalePercent =
      scaleMode == gsw::UiScaleMode::Automatic
          ? gsw::AppTheme::recommendedScalePercent(
                QGuiApplication::primaryScreen())
          : gsw::AppTheme::loadScalePercent(
                QGuiApplication::primaryScreen());
  gsw::AppTheme::apply(application, scalePercent, false);
  application.setProperty("gswAutomaticUiScale",
                          scaleMode == gsw::UiScaleMode::Automatic);

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("Gaussian Scene Workbench native desktop application"));
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption projectOption(
      {QStringLiteral("p"), QStringLiteral("project")},
      QStringLiteral("Open a .gsw.json project file."), QStringLiteral("file"));
  parser.addOption(projectOption);
  QCommandLineOption smokeTestOption(
      QStringLiteral("smoke-test"),
      QStringLiteral("Launch the main window briefly, then exit successfully."));
  parser.addOption(smokeTestOption);
  QCommandLineOption importDialogSmokeTestOption(
      QStringLiteral("smoke-test-import-dialog"),
      QStringLiteral("Verify that adding media from an empty workspace opens the import dialog."));
  parser.addOption(importDialogSmokeTestOption);
  QCommandLineOption displayLayoutSmokeTestOption(
      QStringLiteral("smoke-test-display-layout"),
      QStringLiteral("Verify adaptive display controls and the simplified empty-workspace toolbars."));
  parser.addOption(displayLayoutSmokeTestOption);
  QCommandLineOption mediaSourceOption(
      QStringLiteral("media-source"),
      QStringLiteral("Pre-populate the media import dialog with a file or directory. "
                     "May be specified more than once."),
      QStringLiteral("path"));
  parser.addOption(mediaSourceOption);
  parser.addPositionalArgument(QStringLiteral("project"), QStringLiteral("Project file to open."), QStringLiteral("[project]"));
  parser.process(application);

  application.setProperty("gswInitialMediaSources",
                          parser.values(mediaSourceOption));
  gsw::MainWindow window;
  QString projectPath = parser.value(projectOption);
  const bool importDialogSmokeTest = parser.isSet(importDialogSmokeTestOption);
  const bool displayLayoutSmokeTest =
      parser.isSet(displayLayoutSmokeTestOption);
  const bool smokeTest = parser.isSet(smokeTestOption) ||
                         importDialogSmokeTest || displayLayoutSmokeTest;
  if (projectPath.isEmpty() && !parser.positionalArguments().isEmpty()) {
    projectPath = parser.positionalArguments().first();
  }
  if (!projectPath.isEmpty()) {
    window.openProjectFile(QFileInfo(projectPath).absoluteFilePath());
  }
  window.show();
  bool smokeTestCompleted = !smokeTest;
  if (displayLayoutSmokeTest) {
    QTimer::singleShot(
        350, &application,
        [&application, &window, &smokeTestCompleted]() {
          QAction *autoScale = window.findChild<QAction *>(
              QStringLiteral("autoUiScaleAction"));
          QAction *fitWindow = window.findChild<QAction *>(
              QStringLiteral("fitWindowToScreenAction"));
          QAction *resolution = window.findChild<QAction *>(
              QStringLiteral("windowResolution1600x900Action"));
          QAction *environment = window.findChild<QAction *>(
              QStringLiteral("environmentAction"));
          QAction *resetCamera = window.findChild<QAction *>(
              QStringLiteral("resetCameraAction"));
          const QToolBar *mainToolbar = window.findChild<QToolBar *>(
              QStringLiteral("mainToolbar"));
          const QToolBar *renderToolbar = window.findChild<QToolBar *>(
              QStringLiteral("renderToolbar"));
          const QToolBar *selectionToolbar = window.findChild<QToolBar *>(
              QStringLiteral("selectionToolbar"));
          const QToolBar *editToolbar = window.findChild<QToolBar *>(
              QStringLiteral("editToolbar"));
          const QDockWidget *projectDock = window.findChild<QDockWidget *>(
              QStringLiteral("projectDock"));
          const QDockWidget *inspectorDock = window.findChild<QDockWidget *>(
              QStringLiteral("inspectorDock"));
          const QDockWidget *taskDock = window.findChild<QDockWidget *>(
              QStringLiteral("taskDock"));
          const QLabel *scaleStatus = window.findChild<QLabel *>(
              QStringLiteral("uiScaleStatus"));
          const int fontHeight = QFontMetrics(window.font()).height();
          const int uiScalePercent =
              application.property("gswUiScalePercent").toInt();
          const auto hasCompactDockTitle =
              [uiScalePercent](const QDockWidget *dock) {
                if (dock == nullptr || dock->titleBarWidget() == nullptr) {
                  return false;
                }
                const QWidget *titleBar = dock->titleBarWidget();
                const QLabel *titleLabel = titleBar->findChild<QLabel *>(
                    QStringLiteral("dockTitleLabel"));
                const QList<QToolButton *> buttons =
                    titleBar->findChildren<QToolButton *>(
                        QStringLiteral("dockTitleButton"));
                if (titleBar->objectName() != QStringLiteral("dockTitleBar") ||
                    titleLabel == nullptr || buttons.size() != 2 ||
                    titleLabel->text() != dock->windowTitle() ||
                    titleLabel->font().pixelSize() !=
                        gsw::AppTheme::dockTitleFontPixelSize(
                            uiScalePercent)) {
                  return false;
                }

                const int paddingY =
                    gsw::AppTheme::scaled(1, uiScalePercent);
                const int buttonSize =
                    gsw::AppTheme::dockTitleButtonSize(uiScalePercent);
                const int contentHeight = (std::max)(
                    QFontMetrics(titleLabel->font()).height(), buttonSize);
                const int expectedHeight = (std::max)(
                    gsw::AppTheme::dockTitleHeight(uiScalePercent),
                    contentHeight + paddingY * 2);
                if (titleBar->height() != expectedHeight ||
                    titleBar->height() >
                        gsw::AppTheme::dockTitleHeight(uiScalePercent) +
                            gsw::AppTheme::scaled(2, uiScalePercent) ||
                    !titleBar->rect().contains(titleLabel->geometry())) {
                  return false;
                }
                for (const QToolButton *button : buttons) {
                  if (button->size() != QSize(buttonSize, buttonSize) ||
                      button->accessibleName().isEmpty() ||
                      !titleBar->rect().contains(button->geometry())) {
                    return false;
                  }
                }
                return true;
              };

          smokeTestCompleted =
              window.isVisible() && autoScale != nullptr &&
              fitWindow != nullptr && resolution != nullptr &&
              mainToolbar != nullptr && renderToolbar != nullptr &&
              selectionToolbar != nullptr && editToolbar != nullptr &&
              !renderToolbar->isVisible() &&
              !selectionToolbar->isVisible() && !editToolbar->isVisible() &&
              environment != nullptr && resetCamera != nullptr &&
              !mainToolbar->actions().contains(environment) &&
              !mainToolbar->actions().contains(resetCamera) &&
              projectDock != nullptr && inspectorDock != nullptr &&
              taskDock != nullptr && hasCompactDockTitle(projectDock) &&
              hasCompactDockTitle(inspectorDock) &&
              hasCompactDockTitle(taskDock) &&
              inspectorDock->minimumWidth() >= fontHeight * 12 &&
              taskDock->minimumHeight() >= fontHeight * 7 &&
              scaleStatus != nullptr && scaleStatus->text().contains('%') &&
              scaleStatus->text().contains(QChar(0x00D7));
          application.exit(smokeTestCompleted ? 0 : 2);
        });
  } else if (importDialogSmokeTest) {
    QTimer::singleShot(100, &window, [&window]() {
      QAction *action = window.findChild<QAction *>(
          QStringLiteral("importDatasetAction"));
      if (action != nullptr && action->isEnabled()) {
        action->trigger();
      }
    });
    QTimer::singleShot(
        500, &application,
        [&application, &window, &smokeTestCompleted]() {
          QWidget *dialog = window.findChild<QWidget *>(
              QStringLiteral("datasetImportDialog"));
          const QLabel *introduction =
              dialog == nullptr
                  ? nullptr
                  : dialog->findChild<QLabel *>(QStringLiteral(
                        "datasetImportIntroductionLabel"));
          const QLabel *projectPath =
              dialog == nullptr
                  ? nullptr
                  : dialog->findChild<QLabel *>(QStringLiteral(
                        "datasetImportProjectPathLabel"));
          smokeTestCompleted = window.isVisible() && dialog != nullptr &&
                               dialog->isVisible() && introduction != nullptr &&
                               introduction->text().contains(
                                   QStringLiteral("稍后")) &&
                               projectPath != nullptr &&
                               projectPath->text().contains(
                                   QStringLiteral("尚未保存")) &&
                               !projectPath->text().contains(
                                   QStringLiteral(".Gaussian-Scene-Workbench"));
          const auto topLevels = QApplication::topLevelWidgets();
          for (QWidget *widget : topLevels) {
            if (widget != &window && widget->isVisible()) {
              widget->close();
            }
          }
          application.exit(smokeTestCompleted ? 0 : 2);
        });
  } else if (smokeTest) {
    QTimer::singleShot(250, &application, [&application, &window, &smokeTestCompleted]() {
      const QStringList requiredEntryActions = {
          QStringLiteral("newProjectAction"),
          QStringLiteral("saveProjectAction"),
          QStringLiteral("saveProjectAsAction"),
          QStringLiteral("importDatasetAction"),
          QStringLiteral("importDatasetDirectoryAction"),
          QStringLiteral("attachDatasetAction"),
          QStringLiteral("importSceneAction"),
      };
      smokeTestCompleted = window.isVisible();
      for (const QString &objectName : requiredEntryActions) {
        const QAction *action = window.findChild<QAction *>(objectName);
        smokeTestCompleted = smokeTestCompleted && action != nullptr && action->isEnabled();
      }
      const QLabel *projectName = window.findChild<QLabel *>(
          QStringLiteral("projectNameValue"));
      const QLabel *projectRoot = window.findChild<QLabel *>(
          QStringLiteral("projectRootValue"));
      smokeTestCompleted =
          smokeTestCompleted && projectName != nullptr &&
          projectName->text() == QStringLiteral("未命名工程") &&
          projectRoot != nullptr &&
          projectRoot->text().contains(QStringLiteral("首次保存"));
      application.exit(smokeTestCompleted ? 0 : 2);
    });
  }
  const int exitCode = application.exec();
  return smokeTestCompleted ? exitCode : 2;
}
