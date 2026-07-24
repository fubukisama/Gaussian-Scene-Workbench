#include "AppTheme.h"
#include "MainWindow.h"
#include "NativeViewport.h"

#include <QAbstractButton>
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
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWheelEvent>
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
  QCommandLineOption exitConfirmationSmokeTestOption(
      QStringLiteral("smoke-test-exit-confirmation"),
      QStringLiteral("Verify that closing requires explicit exit confirmation."));
  parser.addOption(exitConfirmationSmokeTestOption);
  QCommandLineOption infiniteGridSmokeTestOption(
      QStringLiteral("smoke-test-infinite-grid"),
      QStringLiteral("Verify the world-fixed infinite reference grid."));
  parser.addOption(infiniteGridSmokeTestOption);
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
  const bool exitConfirmationSmokeTest =
      parser.isSet(exitConfirmationSmokeTestOption);
  const bool infiniteGridSmokeTest =
      parser.isSet(infiniteGridSmokeTestOption);
  const bool smokeTest = parser.isSet(smokeTestOption) ||
                         importDialogSmokeTest || displayLayoutSmokeTest ||
                         exitConfirmationSmokeTest || infiniteGridSmokeTest;
  if (projectPath.isEmpty() && !parser.positionalArguments().isEmpty()) {
    projectPath = parser.positionalArguments().first();
  }
  if (!projectPath.isEmpty()) {
    window.openProjectFile(QFileInfo(projectPath).absoluteFilePath());
  }
  window.show();
  if (projectPath.isEmpty() && !smokeTest) {
    window.offerStartupRecovery();
  }
  bool smokeTestCompleted = !smokeTest;
  int smokeTestFailureCode = 2;
  if (infiniteGridSmokeTest) {
    QTimer::singleShot(
        450, &application, [&window]() {
          auto *viewport =
              qobject_cast<gsw::NativeViewport *>(window.centralWidget());
          if (viewport == nullptr) {
            return;
          }
          const QPointF start(viewport->width() * 0.5,
                              viewport->height() * 0.5);
          const QPointF moved = start + QPointF(5000.0, 0.0);
          const QPointF globalStart(
              viewport->mapToGlobal(start.toPoint()));
          const QPointF globalMoved(
              viewport->mapToGlobal(moved.toPoint()));
          QMouseEvent press(QEvent::MouseButtonPress, start, start,
                            globalStart,
                            Qt::MiddleButton, Qt::MiddleButton,
                            Qt::NoModifier);
          QMouseEvent move(QEvent::MouseMove, moved, moved, globalMoved,
                           Qt::NoButton,
                           Qt::MiddleButton, Qt::NoModifier);
          QMouseEvent release(QEvent::MouseButtonRelease, moved, moved,
                              globalMoved,
                              Qt::MiddleButton, Qt::NoButton,
                              Qt::NoModifier);
          QCoreApplication::sendEvent(viewport, &press);
          QCoreApplication::sendEvent(viewport, &move);
          QCoreApplication::sendEvent(viewport, &release);
          QWheelEvent zoomOut(
              start, globalStart, QPoint(), QPoint(0, -24 * 120),
              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
          QCoreApplication::sendEvent(viewport, &zoomOut);
        });
    QTimer::singleShot(
        950, &application,
        [&application, &window, &smokeTestCompleted,
         &smokeTestFailureCode]() {
          auto *viewport =
              qobject_cast<gsw::NativeViewport *>(window.centralWidget());
          int visibleGridPixels = 0;
          int distantGridPixels = 0;
          bool gridRenderingAvailable = false;
          bool gridOriginFixed = false;
          QRect sampleRect;
          QRect distantSampleRect;
          QImage frame;
          if (viewport != nullptr) {
            frame = viewport->grabFramebuffer();
            sampleRect = QRect(
                qRound(frame.width() * 0.30),
                qRound(frame.height() * 0.45),
                qRound(frame.width() * 0.45),
                qRound(frame.height() * 0.30));
            for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y) {
              for (int x = sampleRect.left(); x <= sampleRect.right(); ++x) {
                const QColor pixel = frame.pixelColor(x, y);
                if ((std::max)({pixel.red(), pixel.green(), pixel.blue()}) >
                    30) {
                  ++visibleGridPixels;
                }
              }
            }
            distantSampleRect = QRect(
                qRound(frame.width() * 0.46),
                qRound(frame.height() * 0.20),
                qRound(frame.width() * 0.10),
                qRound(frame.height() * 0.18));
            for (int y = distantSampleRect.top();
                 y <= distantSampleRect.bottom(); ++y) {
              for (int x = distantSampleRect.left();
                   x <= distantSampleRect.right(); ++x) {
                const QColor pixel = frame.pixelColor(x, y);
                const int brightest =
                    (std::max)({pixel.red(), pixel.green(), pixel.blue()});
                const int darkest =
                    (std::min)({pixel.red(), pixel.green(), pixel.blue()});
                if (brightest > 22 && brightest - darkest <= 12) {
                  ++distantGridPixels;
                }
              }
            }
            gridRenderingAvailable =
                viewport->infiniteGridRenderingAvailable();
            gridOriginFixed =
                gsw::NativeViewport::referenceGridOrigin() ==
                QVector3D(0.0F, 0.0F, 0.0F);
            smokeTestCompleted =
                gridRenderingAvailable && gridOriginFixed &&
                visibleGridPixels > sampleRect.width() &&
                distantGridPixels > distantSampleRect.width() / 2;
          }
          int exitCode = 0;
          if (viewport == nullptr) {
            exitCode = 2;
          } else if (!gridRenderingAvailable) {
            exitCode = 3;
          } else if (!gridOriginFixed) {
            exitCode = 4;
          } else if (visibleGridPixels <= sampleRect.width()) {
            exitCode = 5;
          } else if (distantGridPixels <= distantSampleRect.width() / 2) {
            exitCode = 6;
          }
          if (!frame.isNull()) {
            frame.save(QDir::temp().filePath(
                QStringLiteral("gsw-infinite-grid-smoke.png")));
          }
          qInfo() << "Infinite-grid smoke:" << "viewport" << (viewport != nullptr)
                  << "shader" << gridRenderingAvailable << "fixed-origin"
                  << gridOriginFixed << "visible-pixels" << visibleGridPixels
                  << "required" << sampleRect.width()
                  << "distant-pixels" << distantGridPixels
                  << "distant-required" << distantSampleRect.width() / 2;
          smokeTestFailureCode = exitCode;
          application.exit(exitCode);
        });
  } else if (exitConfirmationSmokeTest) {
    bool exitPromptFound = false;
    bool savePromptFound = false;
    bool exitPromptReset = false;
    QTimer::singleShot(100, &window, [&window]() { window.close(); });
    QTimer::singleShot(
        350, &application,
        [&exitPromptFound]() {
          for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *prompt = qobject_cast<QMessageBox *>(widget);
            if (prompt == nullptr || !prompt->isVisible() ||
                prompt->windowTitle() != QStringLiteral("确认退出") ||
                !prompt->text().contains(QStringLiteral("退出"))) {
              continue;
            }
            const QPushButton *defaultButton = prompt->defaultButton();
            QAbstractButton *escapeButton = prompt->escapeButton();
            exitPromptFound = defaultButton != nullptr &&
                              defaultButton->text() == QStringLiteral("取消") &&
                              escapeButton != nullptr &&
                              escapeButton->text() == QStringLiteral("取消");
            prompt->reject();
            return;
          }
        });
    QTimer::singleShot(650, &window, [&window]() {
      window.setWindowModified(true);
      window.close();
    });
    QTimer::singleShot(
        900, &application,
        []() {
          for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *prompt = qobject_cast<QMessageBox *>(widget);
            if (prompt == nullptr || !prompt->isVisible() ||
                prompt->windowTitle() != QStringLiteral("确认退出")) {
              continue;
            }
            for (QAbstractButton *button : prompt->buttons()) {
              if (button->text() == QStringLiteral("退出")) {
                button->click();
                return;
              }
            }
          }
        });
    QTimer::singleShot(
        1150, &application,
        [&savePromptFound]() {
          for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *prompt = qobject_cast<QMessageBox *>(widget);
            if (prompt == nullptr || !prompt->isVisible() ||
                prompt->windowTitle() != QStringLiteral("退出前保存进度")) {
              continue;
            }
            QAbstractButton *cancelButton =
                prompt->button(QMessageBox::Cancel);
            savePromptFound =
                cancelButton != nullptr && prompt->defaultButton() != nullptr &&
                prompt->standardButton(prompt->defaultButton()) ==
                    QMessageBox::Save;
            if (cancelButton != nullptr) {
              cancelButton->click();
            }
            return;
          }
        });
    QTimer::singleShot(1450, &window, [&window]() { window.close(); });
    QTimer::singleShot(
        1700, &application,
        [&exitPromptReset]() {
          for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *prompt = qobject_cast<QMessageBox *>(widget);
            if (prompt == nullptr || !prompt->isVisible() ||
                prompt->windowTitle() != QStringLiteral("确认退出")) {
              continue;
            }
            exitPromptReset = true;
            prompt->reject();
            return;
          }
        });
    QTimer::singleShot(
        2050, &application,
        [&application, &window, &smokeTestCompleted, &exitPromptFound,
         &savePromptFound, &exitPromptReset]() {
          smokeTestCompleted = exitPromptFound && savePromptFound &&
                               exitPromptReset && window.isVisible();
          application.exit(smokeTestCompleted ? 0 : 2);
        });
  } else if (displayLayoutSmokeTest) {
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
          QStringLiteral("recoveryCenterAction"),
          QStringLiteral("configureExternalBackupAction"),
          QStringLiteral("externalBackupsAction"),
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
  return smokeTestCompleted ? exitCode : smokeTestFailureCode;
}
