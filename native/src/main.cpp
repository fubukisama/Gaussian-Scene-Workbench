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
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>

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
  // Dense point clouds make scan-out tearing especially visible while the
  // camera is moving. Synchronize presentation by default; benchmarks can
  // explicitly opt out with GSW_SWAP_INTERVAL=0.
  bool swapIntervalOverrideValid = false;
  const int swapIntervalOverride =
      qEnvironmentVariableIntValue("GSW_SWAP_INTERVAL",
                                   &swapIntervalOverrideValid);
  const int requestedSwapInterval =
      swapIntervalOverrideValid && swapIntervalOverride >= 0 &&
              swapIntervalOverride <= 1
          ? swapIntervalOverride
          : 1;
  format.setSwapInterval(requestedSwapInterval);
  QSurfaceFormat::setDefaultFormat(format);

  QApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("GaussianSceneWorkbench"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("github.com/fubukisama"));
  QCoreApplication::setApplicationName(QStringLiteral("Gaussian Scene Workbench"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("Gaussian Scene Workbench Native"));
  QCoreApplication::setApplicationVersion(QStringLiteral(GSW_VERSION));
  if (application.arguments().contains(
          QStringLiteral("--smoke-test-display-layout"))) {
    // The layout smoke test validates the default dock arrangement. Isolate
    // it from the interactive app's persisted geometry and scale settings.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope,
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("test-settings/%1")
                          .arg(QCoreApplication::applicationPid())));
  }
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
      QStringLiteral("Verify the adaptive all-axis infinite reference grid."));
  parser.addOption(infiniteGridSmokeTestOption);
  QCommandLineOption referenceAxesSmokeTestOption(
      QStringLiteral("smoke-test-reference-axes"),
      QStringLiteral("Verify reference axes remain visible after scene load."));
  parser.addOption(referenceAxesSmokeTestOption);
  QCommandLineOption smokeSceneOption(
      QStringLiteral("smoke-scene"),
      QStringLiteral("Scene PLY used by viewport smoke tests."),
      QStringLiteral("file"));
  parser.addOption(smokeSceneOption);
  QCommandLineOption gpuPreviewInteropProbeOption(
      QStringLiteral("probe-gpu-preview-interop"),
      QStringLiteral("Probe CUDA VMM / OpenGL Win32 external-memory support."));
  parser.addOption(gpuPreviewInteropProbeOption);
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
  const bool referenceAxesSmokeTest =
      parser.isSet(referenceAxesSmokeTestOption);
  const bool gpuPreviewInteropProbe =
      parser.isSet(gpuPreviewInteropProbeOption);
  const bool smokeTest = parser.isSet(smokeTestOption) ||
                         importDialogSmokeTest || displayLayoutSmokeTest ||
                         exitConfirmationSmokeTest || infiniteGridSmokeTest ||
                         referenceAxesSmokeTest || gpuPreviewInteropProbe;
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
  if (gpuPreviewInteropProbe) {
    QTimer::singleShot(
        650, &application,
        [&application, &window, &smokeTestCompleted,
         &smokeTestFailureCode]() {
          auto *viewport =
              qobject_cast<gsw::NativeViewport *>(window.centralWidget());
          if (viewport == nullptr) {
            smokeTestFailureCode = 2;
            application.exit(smokeTestFailureCode);
            return;
          }
          const gsw::TrainingGpuPreviewCapability capability =
              viewport->trainingGpuPreviewCapability();
          qInfo().noquote()
              << QStringLiteral("GPU_PREVIEW_INTEROP available=%1 renderer=%2 detail=%3")
                     .arg(capability.available ? QStringLiteral("true")
                                               : QStringLiteral("false"),
                          capability.renderer, capability.detail);
          smokeTestCompleted = capability.available;
          smokeTestFailureCode = capability.available ? 0 : 3;
          application.exit(smokeTestFailureCode);
        });
  } else if (referenceAxesSmokeTest) {
    auto *viewport =
        qobject_cast<gsw::NativeViewport *>(window.centralWidget());
    const QString smokeScenePath =
        QFileInfo(parser.value(smokeSceneOption)).absoluteFilePath();
    if (viewport != nullptr && QFileInfo::exists(smokeScenePath)) {
      viewport->setReferencePlaneMode(
          gsw::NativeViewport::ReferencePlaneMode::ModelBase);
      QObject::connect(
          viewport, &gsw::NativeViewport::sceneLoaded, &application,
          [&application, &window, viewport, smokeScenePath,
           &smokeTestCompleted,
           &smokeTestFailureCode](const qint64 sourceVertexCount,
                                 const qsizetype, const qint64 sourceFaceCount,
                                 const qsizetype) {
            const int settleMilliseconds =
                sourceVertexCount > 5'000'000
                    ? 5000
                    : qEnvironmentVariableIsSet(
                          "GSW_MESH_RESIDENT_VERTEX_LIMIT")
                          ? 1500
                          : 300;
            QTimer::singleShot(
                settleMilliseconds, &application,
                [&application, &window, viewport, smokeScenePath,
                 sourceVertexCount, sourceFaceCount,
                 &smokeTestCompleted, &smokeTestFailureCode]() {
                  const QImage frame = viewport->grabFramebuffer();
                  const QRect sampleRect(
                      qRound(frame.width() * 0.25),
                      qRound(frame.height() * 0.10),
                      qRound(frame.width() * 0.50),
                      qRound(frame.height() * 0.70));
                  int greenAxisPixels = 0;
                  for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y) {
                    for (int x = sampleRect.left(); x <= sampleRect.right();
                         ++x) {
                      const QColor pixel = frame.pixelColor(x, y);
                      if (pixel.green() > 70 &&
                          pixel.green() - pixel.red() > 20 &&
                          pixel.green() - pixel.blue() > 10) {
                        ++greenAxisPixels;
                      }
                    }
                  }
                  const QRect centerOcclusionRect(
                      frame.width() / 2 - 5, frame.height() / 2 - 5, 11, 11);
                  int centerAxisTintPixels = 0;
                  for (int y = centerOcclusionRect.top();
                       y <= centerOcclusionRect.bottom(); ++y) {
                    for (int x = centerOcclusionRect.left();
                         x <= centerOcclusionRect.right(); ++x) {
                      const QColor pixel = frame.pixelColor(x, y);
                      const bool redAxis =
                          pixel.red() > 90 && pixel.red() - pixel.green() > 40 &&
                          pixel.red() - pixel.blue() > 40;
                      const bool greenAxis =
                          pixel.green() > 90 && pixel.green() - pixel.red() > 40 &&
                          pixel.green() - pixel.blue() > 35;
                      const bool blueAxis =
                          pixel.blue() > 90 && pixel.blue() - pixel.red() > 40 &&
                          pixel.blue() - pixel.green() > 40;
                      if (redAxis || greenAxis || blueAxis) {
                        ++centerAxisTintPixels;
                      }
                    }
                  }
                  const bool depthOcclusionReady =
                      !qEnvironmentVariableIsSet(
                          "GSW_EXPECT_DEPTH_OCCLUSION") ||
                      centerAxisTintPixels <= 3;
                  const bool pagedMeshReady =
                      sourceFaceCount <= 0 ||
                      !qEnvironmentVariableIsSet(
                          "GSW_MESH_RESIDENT_VERTEX_LIMIT") ||
                      viewport->residentMeshTriangleCount() > 0;
                  const bool meshTextureReady =
                      !qEnvironmentVariableIsSet("GSW_EXPECT_MESH_TEXTURE") ||
                      viewport->meshTextureAvailable();
                  const gsw::SceneCoordinateInfo &coordinates =
                      viewport->sceneCoordinates();
                  const bool coordinateMetadataReady =
                      coordinates.valid &&
                      viewport->referencePlaneMode() ==
                          gsw::NativeViewport::ReferencePlaneMode::ModelBase &&
                      std::abs(viewport->referencePlaneElevation() -
                               coordinates.globalMinimum.z) < 1.0e-8;
                  const auto *coordinateReportAction =
                      window.findChild<QAction *>(
                          QStringLiteral("exportCoordinateReportAction"));
                  const bool coordinateReportReady =
                      coordinateReportAction != nullptr &&
                      coordinateReportAction->isEnabled();
                  const auto *moveModelAction =
                      window.findChild<QAction *>(
                          QStringLiteral("moveModelAction"));
                  const auto *rotateModelAction =
                      window.findChild<QAction *>(
                          QStringLiteral("rotateModelAction"));
                  const auto *scaleModelAction =
                      window.findChild<QAction *>(
                          QStringLiteral("scaleModelAction"));
                  const bool modelSelectionAvailable =
                      viewport->selectableModelAvailable() &&
                      moveModelAction != nullptr &&
                      moveModelAction->isEnabled() &&
                      rotateModelAction != nullptr &&
                      rotateModelAction->isEnabled() &&
                      scaleModelAction != nullptr &&
                      scaleModelAction->isEnabled();
                  viewport->selectModel();
                  const bool modelSelectionReady =
                      modelSelectionAvailable && viewport->modelSelected();
                  const QPointF dragStart(viewport->width() * 0.5,
                                          viewport->height() * 0.5);
                  const QPointF dragEnd = dragStart + QPointF(28.0, -18.0);
                  const QPointF globalDragStart =
                      viewport->mapToGlobal(dragStart.toPoint());
                  const QPointF globalDragEnd =
                      viewport->mapToGlobal(dragEnd.toPoint());
                  QMouseEvent hoverStart(
                      QEvent::MouseMove, dragStart, dragStart,
                      globalDragStart, Qt::NoButton, Qt::NoButton,
                      Qt::NoModifier);
                  QCoreApplication::sendEvent(viewport, &hoverStart);
                  viewport->selectModelForMove();
                  QMouseEvent modelMove(
                      QEvent::MouseMove, dragEnd, dragEnd, globalDragEnd,
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                  QMouseEvent modelConfirm(
                      QEvent::MouseButtonPress, dragEnd, dragEnd,
                      globalDragEnd, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
                  QCoreApplication::sendEvent(viewport, &modelMove);
                  QCoreApplication::sendEvent(viewport, &modelConfirm);
                  const bool modelMoveReady =
                      modelSelectionReady &&
                      viewport->modelTranslation().lengthSquared() > 1.0e-8F;
                  viewport->selectModelForRotate();
                  const QPointF rotateEnd = dragEnd + QPointF(44.0, 36.0);
                  const QPointF globalRotateEnd =
                      viewport->mapToGlobal(rotateEnd.toPoint());
                  QMouseEvent modelRotate(
                      QEvent::MouseMove, rotateEnd, rotateEnd,
                      globalRotateEnd, Qt::NoButton, Qt::NoButton,
                      Qt::NoModifier);
                  QMouseEvent rotationConfirm(
                      QEvent::MouseButtonPress, rotateEnd, rotateEnd,
                      globalRotateEnd, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
                   QCoreApplication::sendEvent(viewport, &modelRotate);
                   QCoreApplication::sendEvent(viewport, &rotationConfirm);
                   const QQuaternion viewRotation = viewport->modelRotation();
                   const bool modelRotateReady = !viewRotation.isIdentity();
                   viewport->selectModelForRotate();
                   viewport->selectModelForRotate();
                   const QPointF trackballEnd =
                       rotateEnd + QPointF(-31.0, 47.0);
                   const QPointF globalTrackballEnd =
                       viewport->mapToGlobal(trackballEnd.toPoint());
                   QMouseEvent modelTrackball(
                       QEvent::MouseMove, trackballEnd, trackballEnd,
                       globalTrackballEnd, Qt::NoButton, Qt::NoButton,
                       Qt::NoModifier);
                   QMouseEvent trackballConfirm(
                       QEvent::MouseButtonPress, trackballEnd, trackballEnd,
                       globalTrackballEnd, Qt::LeftButton, Qt::LeftButton,
                       Qt::NoModifier);
                   QCoreApplication::sendEvent(viewport, &modelTrackball);
                   QCoreApplication::sendEvent(viewport, &trackballConfirm);
                   const bool modelTrackballReady =
                       std::abs(QQuaternion::dotProduct(
                                    viewRotation.normalized(),
                                    viewport->modelRotation().normalized())) <
                        0.99999F;
                    viewport->selectModelForScale();
                    const QPointF scaleEnd =
                        trackballEnd + QPointF(54.0, -32.0);
                    const QPointF globalScaleEnd =
                        viewport->mapToGlobal(scaleEnd.toPoint());
                    QMouseEvent modelScale(
                        QEvent::MouseMove, scaleEnd, scaleEnd,
                        globalScaleEnd, Qt::NoButton, Qt::NoButton,
                        Qt::NoModifier);
                    QMouseEvent scaleConfirm(
                        QEvent::MouseButtonPress, scaleEnd, scaleEnd,
                        globalScaleEnd, Qt::LeftButton, Qt::LeftButton,
                        Qt::NoModifier);
                    QCoreApplication::sendEvent(viewport, &modelScale);
                    QCoreApplication::sendEvent(viewport, &scaleConfirm);
                    const bool modelScaleReady =
                        (viewport->modelScale() - QVector3D(1.0F, 1.0F, 1.0F))
                            .lengthSquared() > 1.0e-8F;
                    smokeTestCompleted = sourceVertexCount > 0 &&
                                       greenAxisPixels >= 30 &&
                                       depthOcclusionReady &&
                                       pagedMeshReady && meshTextureReady &&
                                       coordinateMetadataReady &&
                                       coordinateReportReady &&
                                        modelSelectionReady && modelMoveReady &&
                                        modelRotateReady && modelTrackballReady &&
                                        modelScaleReady;
                  smokeTestFailureCode = smokeTestCompleted ? 0 : 5;
                  if (!frame.isNull()) {
                    frame.save(QDir::temp().filePath(
                        QStringLiteral("gsw-%1-smoke.png")
                            .arg(QFileInfo(smokeScenePath)
                                     .completeBaseName())));
                  }
                  qInfo() << "Reference-axes smoke:" << "vertices"
                          << sourceVertexCount << "green-pixels"
                          << greenAxisPixels << "required" << 30
                          << "center-axis-tint-pixels"
                          << centerAxisTintPixels << "occlusion-ready"
                          << depthOcclusionReady
                          << "resident-mesh-triangles"
                          << viewport->residentMeshTriangleCount()
                          << "mesh-texture-ready"
                          << viewport->meshTextureAvailable()
                          << "coordinate-metadata-ready"
                          << coordinateMetadataReady
                          << "coordinate-report-ready"
                          << coordinateReportReady
                          << "model-selection-ready"
                           << modelSelectionReady << "model-move-ready"
                           << modelMoveReady << "model-rotate-ready"
                            << modelRotateReady << "model-trackball-ready"
                            << modelTrackballReady << "model-scale-ready"
                            << modelScaleReady;
                  application.exit(smokeTestFailureCode);
                });
          });
      QObject::connect(
          viewport, &gsw::NativeViewport::sceneLoadFailed, &application,
          [&application, &smokeTestFailureCode](const QString &,
                                                const QString &) {
            smokeTestFailureCode = 4;
            application.exit(smokeTestFailureCode);
          });
      viewport->setScene(smokeScenePath, 8);
      QTimer::singleShot(180000, &application,
                         [&application, &smokeTestFailureCode]() {
                           smokeTestFailureCode = 6;
                           application.exit(smokeTestFailureCode);
                         });
    } else {
      smokeTestFailureCode = 3;
      QTimer::singleShot(0, &application,
                         [&application, &smokeTestFailureCode]() {
                           application.exit(smokeTestFailureCode);
                         });
    }
  } else if (infiniteGridSmokeTest) {
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
          viewport->setAxisView(gsw::NavigationAxis::PositiveX);
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
          auto *viewport = window.findChild<gsw::NativeViewport *>(
              QStringLiteral("nativeViewport"));
          const QLabel *scaleStatus = window.findChild<QLabel *>(
              QStringLiteral("uiScaleStatus"));
          const QLabel *rendererStatus = nullptr;
          for (const QLabel *label : window.findChildren<QLabel *>()) {
            if (label->property("gswStatusRole").toString() ==
                QStringLiteral("renderer")) {
              rendererStatus = label;
              break;
            }
          }
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

          QImage viewportFrame;
          if (viewport != nullptr) {
            viewportFrame = viewport->grabFramebuffer();
            if (!viewportFrame.isNull()) {
              viewportFrame.save(QDir::temp().filePath(
                  QStringLiteral("gsw-compact-overlay-smoke.png")));
            }
          }

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
              projectDock->minimumWidth() == 0 &&
              inspectorDock->minimumWidth() == 0 &&
              taskDock->minimumHeight() <=
                  taskDock->titleBarWidget()->height() +
                      gsw::AppTheme::scaled(2, uiScalePercent) &&
              viewport != nullptr && !viewportFrame.isNull() &&
              viewport->minimumWidth() == 0 &&
              viewport->minimumHeight() == 0 &&
              taskDock->height() <=
                  gsw::AppTheme::scaled(140, uiScalePercent) &&
              rendererStatus != nullptr &&
              rendererStatus->text().contains(QStringLiteral("FPS")) &&
              rendererStatus->text().contains(QStringLiteral("ms")) &&
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
    QTimer::singleShot(250, &application,
                       [&application, &window, &smokeTestCompleted,
                        requestedSwapInterval]() {
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
          QStringLiteral("pointRenderAction"),
      };
      smokeTestCompleted =
          window.isVisible() &&
          QSurfaceFormat::defaultFormat().swapInterval() ==
              requestedSwapInterval;
      for (const QString &objectName : requiredEntryActions) {
        const QAction *action = window.findChild<QAction *>(objectName);
        smokeTestCompleted = smokeTestCompleted && action != nullptr && action->isEnabled();
      }
      const QLabel *projectName = window.findChild<QLabel *>(
          QStringLiteral("projectNameValue"));
      const QLabel *projectRoot = window.findChild<QLabel *>(
          QStringLiteral("projectRootValue"));
      const QAction *clearDataset = window.findChild<QAction *>(
          QStringLiteral("clearDatasetAction"));
      const QAction *clearReconstruction = window.findChild<QAction *>(
          QStringLiteral("clearReconstructionAction"));
      const QAction *clearScene = window.findChild<QAction *>(
          QStringLiteral("clearSceneAction"));
      const QAction *clearTasks = window.findChild<QAction *>(
          QStringLiteral("clearTasksAction"));
      smokeTestCompleted =
          smokeTestCompleted && projectName != nullptr &&
          projectName->text() == QStringLiteral("未命名工程") &&
          projectRoot != nullptr &&
          clearDataset != nullptr && !clearDataset->isEnabled() &&
          clearReconstruction != nullptr &&
          !clearReconstruction->isEnabled() && clearScene != nullptr &&
          !clearScene->isEnabled() && clearTasks != nullptr &&
          clearTasks->isEnabled() &&
          projectRoot->text().contains(QStringLiteral("首次保存"));
      application.exit(smokeTestCompleted ? 0 : 2);
    });
  }
  const int exitCode = application.exec();
  return smokeTestCompleted ? exitCode : smokeTestFailureCode;
}
