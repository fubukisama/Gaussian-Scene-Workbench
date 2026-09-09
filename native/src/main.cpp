#include "AppTheme.h"
#include "AppLanguage.h"
#include "LanguageSmokeTest.h"
#include "MainWindow.h"
#include "NativeViewport.h"
#include "MultiSceneSmokeTest.h"

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
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVariantAnimation>
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
  const QStringList smokeArguments = application.arguments();
  if (std::any_of(smokeArguments.cbegin(), smokeArguments.cend(), [](const QString &argument) {
        return argument.startsWith(QStringLiteral("--smoke-test"));
      })) {
    // All GUI smoke tests must be independent of the user's edit lock/layout
    // preferences and must never write test state into interactive settings.
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
  QCommandLineOption languageOption(QStringLiteral("language"),
      QStringLiteral("UI language: zh_CN, en_US or ja_JP (does not change saved preference)."),
      QStringLiteral("locale"));
  parser.addOption(languageOption);
  QCommandLineOption languageSmokeOption(QStringLiteral("smoke-test-language"),
      QStringLiteral("Verify the selected UI language and embedded translations."));
  parser.addOption(languageSmokeOption);
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
  QCommandLineOption orthographicNavigationSmokeTestOption(
      QStringLiteral("smoke-test-orthographic-navigation"),
      QStringLiteral("Verify navigation preserves orthographic projection."));
  parser.addOption(orthographicNavigationSmokeTestOption);
  QCommandLineOption referenceAxesSmokeTestOption(
      QStringLiteral("smoke-test-reference-axes"),
      QStringLiteral("Verify reference axes remain visible after scene load."));
  parser.addOption(referenceAxesSmokeTestOption);
  QCommandLineOption multiSceneSmokeTestOption(
      QStringLiteral("smoke-test-multi-scene"),
      QStringLiteral("Verify multi-object import, rendering, selection and persistence."));
  parser.addOption(multiSceneSmokeTestOption);
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

  QString languageOverride = parser.value(languageOption);
  const auto arguments = application.arguments();
  if (languageOverride.isEmpty() && std::any_of(arguments.cbegin(), arguments.cend(),
      [](const QString &argument) { return argument.startsWith(QStringLiteral("--smoke-test")); })) {
    languageOverride = QStringLiteral("zh_CN");
  }
  if (!gsw::AppLanguage::initialize(languageOverride)) {
    qCritical() << "Invalid UI language or missing embedded translation catalog:" << languageOverride;
    return 5;
  }
  gsw::AppTheme::apply(application, scalePercent, false);

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
  const bool orthographicNavigationSmokeTest =
      parser.isSet(orthographicNavigationSmokeTestOption);
  const bool referenceAxesSmokeTest =
      parser.isSet(referenceAxesSmokeTestOption);
  const bool gpuPreviewInteropProbe =
      parser.isSet(gpuPreviewInteropProbeOption);
  const bool smokeTest = parser.isSet(smokeTestOption) ||
                         parser.isSet(languageSmokeOption) ||
                         parser.isSet(multiSceneSmokeTestOption) ||
                         importDialogSmokeTest || displayLayoutSmokeTest ||
                         exitConfirmationSmokeTest || infiniteGridSmokeTest ||
                         orthographicNavigationSmokeTest ||
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
  if (parser.isSet(languageSmokeOption)) {
    QTimer::singleShot(650, &application, [&] {
      smokeTestCompleted = gsw::runLanguageSmokeTest(window);
      application.exit(smokeTestCompleted ? 0 : 2);
    });
  } else if (parser.isSet(multiSceneSmokeTestOption)) {
    QTimer::singleShot(650, &application, [&] {
      smokeTestCompleted = gsw::runMultiSceneSmokeTest(window);
      application.exit(smokeTestCompleted ? 0 : 2);
    });
  } else if (gpuPreviewInteropProbe) {
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
                  // The compact marker sits entirely behind this solid box.
                  // Its former scene-radius Z line extended above the box;
                  // visibility now belongs to the unoccluded zoom smoke.
                  const bool axisVisibilityReady =
                      qEnvironmentVariableIsSet("GSW_EXPECT_DEPTH_OCCLUSION")
                          ? greenAxisPixels == 0
                          : greenAxisPixels >= 30;
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
                  auto *findModelAction = window.findChild<QAction *>(
                      QStringLiteral("findModelAction"));
                  const auto *selectionToolbar =
                      window.findChild<QToolBar *>(
                          QStringLiteral("selectionToolbar"));
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
                  const QPointF precisePickPosition =
                      QPointF(viewport->width() * 0.5,
                              viewport->height() * 0.5) +
                      (sourceFaceCount > 0 ? QPointF() : QPointF(120.0, 0.0));
                  const QPointF globalPrecisePickPosition =
                      viewport->mapToGlobal(precisePickPosition.toPoint());
                  QMouseEvent precisePickPress(
                      QEvent::MouseButtonPress, precisePickPosition,
                      precisePickPosition, globalPrecisePickPosition,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                  QMouseEvent precisePickRelease(
                      QEvent::MouseButtonRelease, precisePickPosition,
                      precisePickPosition, globalPrecisePickPosition,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                  QCoreApplication::sendEvent(viewport, &precisePickPress);
                  QCoreApplication::sendEvent(viewport, &precisePickRelease);
                  const bool precisePickSelected = viewport->modelSelected();
                  // Mesh fixtures cover the viewport centre.  The point-only
                  // fixture contains only the eight AABB corners; one corner
                  // lies on the diagonal view ray, so sample a nearby empty
                  // position that remains well inside the projected box.
                  const bool preciseModelPickReady =
                      sourceFaceCount > 0 ? precisePickSelected
                                          : !precisePickSelected;
                  viewport->clearSelection();
                  const float distanceBeforeFind = viewport->viewDistance();
                  if (findModelAction != nullptr) {
                    findModelAction->trigger();
                  }
                  const bool findModelReady =
                      findModelAction != nullptr &&
                      findModelAction->isEnabled() &&
                      selectionToolbar != nullptr &&
                      selectionToolbar->isVisible() &&
                      selectionToolbar->actions().contains(findModelAction) &&
                      viewport->modelSelected() &&
                      (viewport->viewTarget() - coordinates.localCenter())
                              .lengthSquared() <
                          1.0e-6F &&
                      viewport->viewDistance() < distanceBeforeFind;
                  viewport->repaint();
                  const QImage focusedModelFrame =
                      viewport->grabFramebuffer();
                  if (!focusedModelFrame.isNull()) {
                    focusedModelFrame.save(QDir::temp().filePath(
                        QStringLiteral("gsw-find-model-focus-smoke.png")));
                  }
                  window.repaint();
                  const QImage focusedWindowFrame =
                      window.grab().toImage();
                  if (!focusedWindowFrame.isNull()) {
                    focusedWindowFrame.save(QDir::temp().filePath(
                        QStringLiteral("gsw-find-model-window-smoke.png")));
                  }
                  const bool modelSelectionReady =
                      modelSelectionAvailable && viewport->modelSelected();
                  viewport->setModelGizmoMode(gsw::TransformGizmoMode::Move);
                  const bool globalOrientationReady =
                      !viewport->modelGizmoOrientationLocked() &&
                      !viewport->modelGizmoUsesLocalOrientation();
                  viewport->setModelGizmoMode(gsw::TransformGizmoMode::Scale);
                  const bool lockedOrientationReady =
                      viewport->modelGizmoOrientationLocked() &&
                      viewport->modelGizmoUsesLocalOrientation();
                  viewport->repaint();
                  const QImage lockedOrientationFrame =
                      viewport->grabFramebuffer();
                  if (!lockedOrientationFrame.isNull()) {
                    lockedOrientationFrame.save(QDir::temp().filePath(
                        QStringLiteral(
                            "gsw-transform-orientation-locked-smoke.png")));
                  }
                  QKeyEvent lockedOrientationToggle(
                      QEvent::KeyPress, Qt::Key_Comma, Qt::NoModifier);
                  QCoreApplication::sendEvent(viewport,
                                              &lockedOrientationToggle);
                  viewport->setModelGizmoMode(gsw::TransformGizmoMode::Move);
                  const bool lockedToggleIgnored =
                      !viewport->modelGizmoUsesLocalOrientation();
                  QKeyEvent localOrientationToggle(
                      QEvent::KeyPress, Qt::Key_Comma, Qt::NoModifier);
                  QCoreApplication::sendEvent(viewport,
                                              &localOrientationToggle);
                  const bool localOrientationReady =
                      viewport->modelGizmoUsesLocalOrientation();
                  QKeyEvent restoreGlobalOrientation(
                      QEvent::KeyPress, Qt::Key_Comma, Qt::NoModifier);
                  QCoreApplication::sendEvent(viewport,
                                              &restoreGlobalOrientation);
                  const bool orientationControlReady =
                      globalOrientationReady && lockedOrientationReady &&
                      lockedToggleIgnored && localOrientationReady &&
                      !viewport->modelGizmoUsesLocalOrientation();
                  viewport->setModelGizmoMode(
                      gsw::TransformGizmoMode::Transform);
                  viewport->repaint();
                  const QImage transformGizmoFrame =
                      viewport->grabFramebuffer();
                  if (!transformGizmoFrame.isNull()) {
                    transformGizmoFrame.save(QDir::temp().filePath(
                        QStringLiteral("gsw-transform-gizmo-smoke.png")));
                  }
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
                    bool edgeOnRotationReady = true;
                    for (const bool orthographic : {false, true}) {
                    for (const bool ringDrag : {true, false}) {
                      viewport->setModelTransform({}, {});
                      edgeOnRotationReady &= viewport->focusModel();
                      viewport->setAxisView(gsw::NavigationAxis::PositiveX);
                      if (auto *animation = viewport->findChild<QVariantAnimation *>()) {
                        animation->setCurrentTime(animation->duration());
                      }
                      viewport->setModelGizmoMode(gsw::TransformGizmoMode::Rotate);
                      const QPointF start(viewport->width() * 0.5 + 45.0,
                                          viewport->height() * 0.5);
                      const QPointF end = start + QPointF(32.0, 0.0);
                      const auto mouse = [&](QEvent::Type type, QPointF point,
                                             Qt::MouseButton button, Qt::MouseButtons buttons,
                                             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
                        QMouseEvent event(type, point, point,
                            viewport->mapToGlobal(point.toPoint()), button, buttons, modifiers);
                        QCoreApplication::sendEvent(viewport, &event);
                      };
                      if (viewport->orthographicProjection() != orthographic) {
                        const auto navigation = gsw::navigationGizmoLayout(
                            QMatrix4x4(), QSizeF(viewport->width(), viewport->height()),
                            QFontMetricsF(viewport->font()).height());
                        mouse(QEvent::MouseButtonPress, navigation.projectionCube.center(),
                              Qt::LeftButton, Qt::LeftButton);
                        mouse(QEvent::MouseButtonRelease, navigation.projectionCube.center(),
                              Qt::LeftButton, Qt::NoButton);
                      }
                      mouse(QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
                      if (ringDrag) {
                        mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
                      } else {
                        viewport->selectModelForRotate();
                        QKeyEvent z(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier);
                        QCoreApplication::sendEvent(viewport, &z);
                      }
                      const bool active = viewport->modelTransformActive();
                      mouse(QEvent::MouseMove, end, Qt::NoButton,
                            ringDrag ? Qt::LeftButton : Qt::NoButton);
                      QVector3D axis;
                      float angle = 0.0F;
                      viewport->modelRotation().getAxisAndAngle(&axis, &angle);
                      const bool rotated = active && std::abs(angle) > 1.0F && std::abs(axis.z()) > 0.99F &&
                                           viewport->orthographicProjection() == orthographic;
                      edgeOnRotationReady &= rotated;
                      qInfo() << "Edge-on Z rotation:" << "ortho" << orthographic
                              << "ring" << ringDrag << "active" << active
                              << "pitch" << viewport->viewOrbitAngles().pitchDegrees
                              << "angle" << angle << "axis" << axis << "ready" << rotated;
                      if (ringDrag) {
                        viewport->grabFramebuffer().save(QDir::temp().filePath(
                            orthographic ? QStringLiteral("gsw-edge-on-z-rotation-ortho.png")
                                         : QStringLiteral("gsw-edge-on-z-rotation.png")));
                      }
                      const QQuaternion dragged = viewport->modelRotation();
                      mouse(QEvent::MouseMove, end, Qt::NoButton,
                            ringDrag ? Qt::LeftButton : Qt::NoButton, Qt::ShiftModifier);
                      edgeOnRotationReady &= gsw::rotationsEquivalent(viewport->modelRotation(),
                          QQuaternion::fromAxisAndAngle(axis, angle * 0.1F));
                      mouse(QEvent::MouseMove, end, Qt::NoButton,
                            ringDrag ? Qt::LeftButton : Qt::NoButton, Qt::ControlModifier);
                      edgeOnRotationReady &= gsw::rotationsEquivalent(viewport->modelRotation(),
                          QQuaternion::fromAxisAndAngle(axis, std::round(angle / 5.0F) * 5.0F));
                      if (ringDrag) {
                        mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
                        edgeOnRotationReady &= !viewport->modelTransformActive();
                        edgeOnRotationReady &= gsw::rotationsEquivalent(viewport->modelRotation(), dragged);
                        viewport->undoEdit();
                        edgeOnRotationReady &= viewport->modelRotation().isIdentity();
                        viewport->redoEdit();
                        edgeOnRotationReady &= gsw::rotationsEquivalent(viewport->modelRotation(), dragged);
                      } else {
                        QKeyEvent four(QEvent::KeyPress, Qt::Key_4, Qt::NoModifier, QStringLiteral("4"));
                        QKeyEvent five(QEvent::KeyPress, Qt::Key_5, Qt::NoModifier, QStringLiteral("5"));
                        QCoreApplication::sendEvent(viewport, &four);
                        QCoreApplication::sendEvent(viewport, &five);
                        edgeOnRotationReady &= gsw::rotationsEquivalent(viewport->modelRotation(),
                            QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 45.0F));
                      QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                      QCoreApplication::sendEvent(viewport, &cancel);
                        edgeOnRotationReady &= viewport->modelRotation().isIdentity();
                      }
                    }
                    }
                    qInfo() << "Edge-on rotation controls/undo:" << edgeOnRotationReady;
                    bool gizmoZoomReady = true;
                    for (const bool orthographic : {false, true}) {
                      for (const auto mode : {gsw::TransformGizmoMode::Rotate,
                                              gsw::TransformGizmoMode::Transform}) {
                        viewport->setModelTransform({}, {});
                        gizmoZoomReady &= viewport->focusModel();
                        viewport->setModelGizmoMode(mode);
                        if (viewport->orthographicProjection() != orthographic) {
                          const auto navigation = gsw::navigationGizmoLayout(
                              {}, viewport->size(), QFontMetricsF(viewport->font()).height());
                          const QPointF point = navigation.projectionCube.center();
                          for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                            QMouseEvent event(type, point, point, viewport->mapToGlobal(point.toPoint()),
                                Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                Qt::NoModifier);
                            QCoreApplication::sendEvent(viewport, &event);
                          }
                        }
                        const QPointF center(viewport->width() * 0.5, viewport->height() * 0.5);
                        qreal previousRadius = 0;
                        float previousDistance = 0;
                        int sample = 0;
                        // Keep the sampled outer ring inside the viewport;
                        // overflowing at closer ranges is correct world-size behavior.
                        for (const int steps : {-8, 3, 3, 2}) {
                          QWheelEvent zoom(center, viewport->mapToGlobal(center.toPoint()), {},
                              QPoint(0, steps * 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                          QCoreApplication::sendEvent(viewport, &zoom);
                          // Measure the actual UI hit target, through hover feedback, not a
                          // parallel copy of the layout calculation. This catches a viewport
                          // caller accidentally reintroducing distance-compensated sizing.
                          int first = -1, last = -1;
                          for (int offset = 1; offset < viewport->width() * 0.45; ++offset) {
                            const QPointF point = center + QPointF(offset, 0);
                            QMouseEvent hover(QEvent::MouseMove, point, point,
                                viewport->mapToGlobal(point.toPoint()), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                            QCoreApplication::sendEvent(viewport, &hover);
                            if (viewport->toolTip().startsWith(QStringLiteral("绕视图轴旋转"))) {
                              if (first < 0) first = offset;
                              last = offset;
                            }
                          }
                          const qreal radius = (first + last) * 0.5;
                          const float distance = viewport->viewDistance();
                          bool ready = first > 0 && viewport->orthographicProjection() == orthographic;
                          if (previousRadius > 0) {
                            ready &= std::abs(radius / previousRadius - previousDistance / distance) < 0.03;
                          }
                          gizmoZoomReady &= ready;
                          qInfo() << "World-size gizmo zoom:" << "ortho" << orthographic
                                  << "combined" << (mode == gsw::TransformGizmoMode::Transform)
                                  << "sample" << sample << "distance" << distance
                                  << "hit-radius" << radius << "ready" << ready;
                          viewport->grabFramebuffer().save(QDir::temp().filePath(
                              QStringLiteral("gsw-model-gizmo-%1-%2-%3.png")
                                  .arg(orthographic ? "ortho" : "persp")
                                  .arg(mode == gsw::TransformGizmoMode::Transform ? "combined" : "rotate")
                                  .arg(sample++)));
                          previousRadius = radius;
                          previousDistance = distance;
                        }
                      }
                    }
                    smokeTestCompleted = sourceVertexCount > 0 &&
                                       axisVisibilityReady &&
                                       depthOcclusionReady &&
                                       pagedMeshReady && meshTextureReady &&
                                       coordinateMetadataReady &&
                                       coordinateReportReady &&
                                        preciseModelPickReady &&
                                        findModelReady &&
                                        orientationControlReady &&
                                        modelSelectionReady && modelMoveReady &&
                                        modelRotateReady && modelTrackballReady &&
                                        modelScaleReady && edgeOnRotationReady && gizmoZoomReady;
                  smokeTestFailureCode = smokeTestCompleted ? 0 : 5;
                  if (!frame.isNull()) {
                    frame.save(QDir::temp().filePath(
                        QStringLiteral("gsw-%1-smoke.png")
                            .arg(QFileInfo(smokeScenePath)
                                     .completeBaseName())));
                  }
                  qInfo() << "Reference-axes smoke:" << "vertices"
                          << sourceVertexCount << "green-pixels"
                          << greenAxisPixels << "axis-visibility-ready"
                          << axisVisibilityReady
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
                          << "precise-model-pick-ready"
                          << preciseModelPickReady
                          << "find-model-ready" << findModelReady
                          << "orientation-control-ready"
                          << orientationControlReady
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
  } else if (orthographicNavigationSmokeTest) {
    QTimer::singleShot(
        450, &application,
        [&application, &window, &smokeTestCompleted,
         &smokeTestFailureCode]() {
          auto *viewport =
              qobject_cast<gsw::NativeViewport *>(window.centralWidget());
          if (viewport == nullptr) {
            smokeTestFailureCode = 2;
            application.exit(smokeTestFailureCode);
            return;
          }

          const auto drag = [viewport](const Qt::MouseButton button,
                                       const QPointF &start,
                                       const QPointF &finish,
                                       const Qt::KeyboardModifiers modifiers) {
            const QPointF globalStart =
                viewport->mapToGlobal(start.toPoint());
            const QPointF globalFinish =
                viewport->mapToGlobal(finish.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, start, start,
                              globalStart, button, button, modifiers);
            QMouseEvent move(QEvent::MouseMove, finish, finish, globalFinish,
                             Qt::NoButton, button, modifiers);
            QMouseEvent release(QEvent::MouseButtonRelease, finish, finish,
                                globalFinish, button, Qt::NoButton,
                                modifiers);
            QCoreApplication::sendEvent(viewport, &press);
            QCoreApplication::sendEvent(viewport, &move);
            QCoreApplication::sendEvent(viewport, &release);
          };

          viewport->setAxisView(gsw::NavigationAxis::PositiveX);
          const bool perspectiveAxisPreserved =
              !viewport->orthographicProjection();
          const gsw::NavigationGizmoLayout gizmo =
              gsw::navigationGizmoLayout(
                  QMatrix4x4(), QSizeF(viewport->width(), viewport->height()),
                  QFontMetricsF(viewport->font()).height());
          drag(Qt::LeftButton, gizmo.projectionCube.center(),
               gizmo.projectionCube.center(), Qt::NoModifier);
          const bool centerEntersOrthographic =
              viewport->orthographicProjection();

          viewport->setAxisView(gsw::NavigationAxis::PositiveY);
          const bool orthographicAxisPreserved =
              viewport->orthographicProjection();
          const QPointF canvasStart(viewport->width() * 0.50,
                                    viewport->height() * 0.45);
          drag(Qt::LeftButton, canvasStart,
               canvasStart + QPointF(32.0, -24.0), Qt::NoModifier);
          const bool canvasOrbitKeepsOrthographic =
              viewport->orthographicProjection();

          viewport->setAxisView(gsw::NavigationAxis::PositiveX);
          const QPointF gizmoOrbitStart =
              gizmo.center +
              QPointF(gizmo.radius * 0.62, gizmo.radius * 0.18);
          drag(Qt::LeftButton, gizmoOrbitStart,
               gizmoOrbitStart + QPointF(30.0, -18.0), Qt::NoModifier);
          const bool gizmoOrbitKeepsOrthographic =
              viewport->orthographicProjection();

          drag(Qt::MiddleButton, canvasStart,
               canvasStart + QPointF(-28.0, 20.0), Qt::NoModifier);
          const QPointF globalCanvasStart =
              viewport->mapToGlobal(canvasStart.toPoint());
          QWheelEvent zoom(canvasStart, globalCanvasStart, QPoint(),
                           QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                           Qt::NoScrollPhase, false);
          QCoreApplication::sendEvent(viewport, &zoom);
          const bool panZoomKeepOrthographic =
              viewport->orthographicProjection();

          drag(Qt::LeftButton, gizmo.projectionLabel.center(),
               gizmo.projectionLabel.center(), Qt::NoModifier);
          const bool labelReturnsPerspective =
              !viewport->orthographicProjection();

          drag(Qt::LeftButton, gizmo.projectionCube.center(),
               gizmo.projectionCube.center(), Qt::NoModifier);
          drag(Qt::LeftButton, gizmo.projectionCube.center(),
               gizmo.projectionCube.center(), Qt::ShiftModifier);
          const gsw::OrbitAngles resetAngles = viewport->viewOrbitAngles();
          const bool shiftRestoresDefaultPerspective =
              !viewport->orthographicProjection() &&
              std::abs(resetAngles.yawDegrees - 42.0F) < 0.01F &&
              std::abs(resetAngles.pitchDegrees - 24.0F) < 0.01F;

          smokeTestCompleted =
              perspectiveAxisPreserved && centerEntersOrthographic &&
              orthographicAxisPreserved && canvasOrbitKeepsOrthographic &&
              gizmoOrbitKeepsOrthographic && panZoomKeepOrthographic &&
              labelReturnsPerspective && shiftRestoresDefaultPerspective;
          if (!perspectiveAxisPreserved) {
            smokeTestFailureCode = 3;
          } else if (!centerEntersOrthographic) {
            smokeTestFailureCode = 4;
          } else if (!orthographicAxisPreserved) {
            smokeTestFailureCode = 5;
          } else if (!canvasOrbitKeepsOrthographic) {
            smokeTestFailureCode = 6;
          } else if (!gizmoOrbitKeepsOrthographic) {
            smokeTestFailureCode = 7;
          } else if (!panZoomKeepOrthographic) {
            smokeTestFailureCode = 8;
          } else if (!labelReturnsPerspective) {
            smokeTestFailureCode = 9;
          } else if (!shiftRestoresDefaultPerspective) {
            smokeTestFailureCode = 10;
          } else {
            smokeTestFailureCode = 0;
          }
          qInfo() << "Unity-style orientation smoke:"
                  << "perspective-axis" << perspectiveAxisPreserved
                  << "center-orthographic" << centerEntersOrthographic
                  << "orthographic-axis" << orthographicAxisPreserved
                  << "canvas-orbit" << canvasOrbitKeepsOrthographic
                  << "gizmo-orbit" << gizmoOrbitKeepsOrthographic
                  << "pan-zoom" << panZoomKeepOrthographic
                  << "label-perspective" << labelReturnsPerspective
                  << "shift-reset" << shiftRestoresDefaultPerspective;
          const QImage orientationFrame = viewport->grabFramebuffer();
          if (!orientationFrame.isNull()) {
            orientationFrame.save(QDir::temp().filePath(
                QStringLiteral("gsw-unity-orientation-smoke.png")));
          }
          // Zoom-in must keep enlarging the real shaft, including beyond the
          // old saturation region. Distant real-world axes may become subpixel.
          const auto markerPixels = [](const QImage &frame) {
            int greenPixels = 0;
            const QRect region = QRect(frame.width() / 2 - 150, 0,
                                        300, frame.height())
                                     .intersected(frame.rect());
            for (int y = region.top(); y <= region.bottom(); ++y) {
              for (int x = region.left(); x <= region.right(); ++x) {
                const QColor color = frame.pixelColor(x, y);
                if (color.green() > 100 && color.green() - color.red() > 35 &&
                    color.green() - color.blue() > 25) {
                  ++greenPixels;
                }
              }
            }
            return greenPixels;
          };
          const int nearMarkerPixels = markerPixels(orientationFrame);
          const QPointF zoomPosition(viewport->width() * 0.5,
                                      viewport->height() * 0.5);
          const auto zoomReferenceAxis = [&](const int steps) {
            QWheelEvent event(zoomPosition,
                viewport->mapToGlobal(zoomPosition.toPoint()), QPoint(),
                QPoint(0, steps * 120), Qt::NoButton, Qt::NoModifier,
                Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(viewport, &event);
          };
          zoomReferenceAxis(4);
          const QImage closeFrame = viewport->grabFramebuffer();
          closeFrame.save(QDir::temp().filePath(
              QStringLiteral("gsw-reference-axis-close.png")));
          const int closeMarkerPixels = markerPixels(closeFrame);
          zoomReferenceAxis(4);
          const QImage closerFrame = viewport->grabFramebuffer();
          closerFrame.save(QDir::temp().filePath(
              QStringLiteral("gsw-reference-axis-closer.png")));
          const int closerMarkerPixels = markerPixels(closerFrame);
          zoomReferenceAxis(-8);
          QWheelEvent midZoom(zoomPosition,
                               viewport->mapToGlobal(zoomPosition.toPoint()),
                               QPoint(), QPoint(0, -6 * 120), Qt::NoButton,
                               Qt::NoModifier, Qt::NoScrollPhase, false);
          QCoreApplication::sendEvent(viewport, &midZoom);
          const QImage midFrame = viewport->grabFramebuffer();
          midFrame.save(QDir::temp().filePath(
              QStringLiteral("gsw-reference-axis-mid.png")));
          const int midMarkerPixels = markerPixels(midFrame);
          QWheelEvent farZoom(zoomPosition,
                               viewport->mapToGlobal(zoomPosition.toPoint()),
                               QPoint(), QPoint(0, -18 * 120), Qt::NoButton,
                               Qt::NoModifier, Qt::NoScrollPhase, false);
          QCoreApplication::sendEvent(viewport, &farZoom);
          const QImage farFrame = viewport->grabFramebuffer();
          farFrame.save(QDir::temp().filePath(
              QStringLiteral("gsw-reference-axis-far.png")));
          const int farMarkerPixels = markerPixels(farFrame);
          drag(Qt::LeftButton, gizmo.projectionLabel.center(),
               gizmo.projectionLabel.center(), Qt::NoModifier);
          const QImage orthoFrame = viewport->grabFramebuffer();
          orthoFrame.save(QDir::temp().filePath(
              QStringLiteral("gsw-reference-axis-ortho.png")));
          const int orthoMarkerPixels = markerPixels(orthoFrame);
          const bool physicalAxisScaling = nearMarkerPixels >= 60 &&
                                      closeMarkerPixels > nearMarkerPixels * 1.5 &&
                                      closerMarkerPixels > closeMarkerPixels * 1.5 &&
                                      nearMarkerPixels > midMarkerPixels * 1.5 &&
                                      farMarkerPixels < nearMarkerPixels / 10 &&
                                      orthoMarkerPixels < nearMarkerPixels / 10;
          qInfo() << "Reference-axis physical scaling:" << physicalAxisScaling
                  << "close" << closeMarkerPixels << "closer" << closerMarkerPixels
                  << "near" << nearMarkerPixels << "mid" << midMarkerPixels
                  << "far" << farMarkerPixels
                  << "orthographic" << orthoMarkerPixels;
          if (!physicalAxisScaling) {
            smokeTestCompleted = false;
            smokeTestFailureCode = 11;
          }
          application.exit(smokeTestFailureCode);
        });
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
          const gsw::NavigationGizmoLayout gizmo =
              gsw::navigationGizmoLayout(
                  QMatrix4x4(), QSizeF(viewport->width(), viewport->height()),
                  QFontMetricsF(viewport->font()).height());
          const QPointF projectionPosition = gizmo.projectionCube.center();
          const QPointF globalProjectionPosition =
              viewport->mapToGlobal(projectionPosition.toPoint());
          QMouseEvent projectionPress(
              QEvent::MouseButtonPress, projectionPosition,
              projectionPosition, globalProjectionPosition, Qt::LeftButton,
              Qt::LeftButton, Qt::NoModifier);
          QMouseEvent projectionRelease(
              QEvent::MouseButtonRelease, projectionPosition,
              projectionPosition, globalProjectionPosition, Qt::LeftButton,
              Qt::NoButton, Qt::NoModifier);
          QCoreApplication::sendEvent(viewport, &projectionPress);
          QCoreApplication::sendEvent(viewport, &projectionRelease);
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
          QAction *findModel = window.findChild<QAction *>(
              QStringLiteral("findModelAction"));
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
              findModel != nullptr && !findModel->isEnabled() &&
              selectionToolbar->actions().contains(findModel) &&
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
