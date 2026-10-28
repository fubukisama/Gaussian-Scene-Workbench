#include "GizmoViewportSmokeTest.h"

#include "AppLanguage.h"
#include "AppTheme.h"
#include "NativeViewport.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QVariant>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

namespace gsw {
namespace {
constexpr int imageLimit = 160;
constexpr int candidateLimitPerAxis = 8192;
constexpr qint64 pixelLimit = 16LL * 1024 * 1024;
struct Aborted { bool unsupported = false; };

void dispatchFor(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!condition() && elapsed.elapsed() < timeoutMs) dispatchFor(10);
  return condition();
}

QString sha256(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

QJsonArray vector(const QVector3D &value) { return {value.x(), value.y(), value.z()}; }
QJsonArray point(const QPointF &value) { return {value.x(), value.y()}; }
QJsonArray quaternion(const QQuaternion &value) { return {value.scalar(), value.x(), value.y(), value.z()}; }

struct State {
  QVector3D translation, scale, cameraTarget;
  QQuaternion rotation;
  OrbitAngles cameraAngles;
  float distance = 0, fov = 0;
  bool orthographic = false;
};

State stateOf(const NativeViewport &viewport) {
  return {viewport.modelTranslation(), viewport.modelScale(), viewport.viewTarget(),
      viewport.modelRotation(), viewport.viewOrbitAngles(), viewport.viewDistance(),
      viewport.perspectiveFovDegrees(), viewport.orthographicProjection()};
}

bool sameRotation(const QQuaternion &a, const QQuaternion &b) {
  const QQuaternion first = a.normalized(), second = b.normalized();
  return std::min((first - second).length(), (first + second).length()) < 1e-5F;
}

bool sameTransform(const State &a, const State &b) {
  return (a.translation - b.translation).length() < 1e-5F &&
      (a.scale - b.scale).length() < 1e-5F && sameRotation(a.rotation, b.rotation);
}

bool sameCamera(const State &a, const State &b) {
  return a.cameraTarget == b.cameraTarget && a.cameraAngles == b.cameraAngles &&
      a.distance == b.distance && a.fov == b.fov && a.orthographic == b.orthographic;
}

enum class Operation { Move, Rotate, Scale };
struct Spec { const char *name; TransformGizmoMode mode; Operation operation; };
constexpr std::array<Spec, 6> specs{{
    {"move", TransformGizmoMode::Move, Operation::Move},
    {"rotate", TransformGizmoMode::Rotate, Operation::Rotate},
    {"scale", TransformGizmoMode::Scale, Operation::Scale},
    {"combined-move", TransformGizmoMode::Transform, Operation::Move},
    {"combined-rotate", TransformGizmoMode::Transform, Operation::Rotate},
    {"combined-scale", TransformGizmoMode::Transform, Operation::Scale}}};

bool writeCube(const QString &path) {
  // Synthetic gray geometry only. No original photo, training model or user
  // document is loaded. Native loader triangulates these six planar quads.
  QByteArray bytes("ply\nformat ascii 1.0\ncomment unit m\n"
      "comment crs: gsw-gizmo-viewport-owned-frame\nelement vertex 8\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\n"
      "element face 6\nproperty list uchar int vertex_indices\nend_header\n");
  for (int index = 0; index < 8; ++index)
    bytes += QStringLiteral("%1 %2 %3 128 128 128\n")
        .arg(index & 1 ? 1 : -1).arg(index & 2 ? 1 : -1).arg(index & 4 ? 1 : -1).toUtf8();
  bytes += "4 0 1 3 2\n4 4 6 7 5\n4 0 4 5 1\n4 2 3 7 6\n4 0 2 6 4\n4 1 5 7 3\n";
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
      file.write(bytes) == bytes.size() && file.flush();
}

int visibleAxis(QRgb pixel) {
  const int r = qRed(pixel), g = qGreen(pixel), b = qBlue(pixel);
  // Independent broad color categories, not a copy of the gizmo's geometry,
  // painter, palette or hit-test. The neutral mesh cannot satisfy these tests.
  if (r >= 80 && r > 1.4 * g && r > 1.4 * b) return 0;
  if (g >= 55 && g > 1.35 * r && g > 1.15 * b) return 1;
  if (b >= 70 && b > 1.35 * r && b > 1.15 * g) return 2;
  return -1;
}

class GizmoSmoke final {
public:
  GizmoSmoke(NativeViewport &viewport, QString root, QString source)
      : v(viewport), mRoot(std::move(root)), mSource(std::move(source)) {}

  bool run() {
    try { exercise(); }
    catch (const Aborted &reason) { mUnsupported = reason.unsupported; }
    if (v.modelTransformActive()) key(Qt::Key_Escape);
    if (!mCurrent.isEmpty()) {
      if (!mCurrent.contains("status")) mCurrent.insert("status", mUnsupported ? "unsupported" : "failed");
      mCases.append(mCurrent);
      mCurrent = {};
    }
    const QString finalHash = mFixture.isEmpty() ? QString() : sha256(mFixture);
    if (!mFixtureHash.isEmpty() && finalHash != mFixtureHash)
      mFailures.append(QStringLiteral("owned-fixture-changed"));
    const bool passed = mCompleted && !mUnsupported && mFailures.isEmpty() &&
        mCases.size() == 30 && finalHash == mFixtureHash && !mFixtureHash.isEmpty();
    const QJsonObject report{{"schema", "gsw.gizmo_viewport_smoke/1"},
        {"sourceCommit", mSource}, {"status", passed ? "passed" : mUnsupported ? "unsupported" : "failed"},
        {"locale", AppLanguage::current()},
        {"theme", AppTheme::currentTheme() == UiTheme::Light ? "light" : "dark"},
        {"fixture", "fixtures/gizmo-cube.ply"}, {"fixtureSha256Before", mFixtureHash},
        {"fixtureSha256After", finalHash}, {"fixtureUnchanged", finalHash == mFixtureHash && !finalHash.isEmpty()},
        {"imageCount", mImageCount}, {"imageLimit", imageLimit},
        {"candidateLimitPerAxis", candidateLimitPerAxis}, {"failures", mFailures},
        {"publicInputOnly", true}, {"noPrivateGeometryOrPickCalls", true},
        {"noVisibleHandleDoesNotEstablishGeometryBug", true},
        {"actualFramebufferNotOffscreenQImageRenderer", true}, {"cases", mCases},
        {"finalPublicState", publicState()}};
    QSaveFile output(QDir(mRoot).filePath(QStringLiteral("gizmo-viewport.json")));
    const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    const bool saved = output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
    qInfo() << "GIZMO_VIEWPORT" << (passed && saved ? "PASS" : mUnsupported ? "UNSUPPORTED" : "FAIL")
            << "cases" << mCases.size() << "images" << mImageCount << "failures" << mFailures;
    return passed && saved;
  }

private:
  void require(bool condition, const char *code, bool unsupported = false) {
    if (condition) return;
    mFailures.append(QString::fromLatin1(code));
    mCurrent.insert("failure", QString::fromLatin1(code));
    mCurrent.insert("failurePublicState", publicState());
    mCurrent.insert("status", unsupported ? "unsupported" : "failed");
    throw Aborted{unsupported};
  }

  QJsonObject publicState() const {
    const auto state = stateOf(v);
    QJsonArray selected;
    for (const auto &id : v.selectedSceneIds()) selected.append(id);
    const auto coordinates = v.sceneCoordinates();
    return {{"translationLocal", vector(state.translation)},
        {"translationSource", vector(v.modelExportOptions().transform.translation)},
        {"rotationWxyz", quaternion(state.rotation)}, {"scale", vector(state.scale)},
        {"modelTransformActive", v.modelTransformActive()}, {"modelSelected", v.modelSelected()},
        {"gizmoMode", int(v.modelGizmoMode())}, {"selectedSceneIds", selected},
        {"cameraTarget", vector(state.cameraTarget)}, {"cameraDistance", state.distance},
        {"cameraAngles", QJsonArray{state.cameraAngles.yawDegrees, state.cameraAngles.pitchDegrees,
                                    state.cameraAngles.rollDegrees}},
        {"perspectiveFov", state.fov}, {"orthographic", state.orthographic},
        {"displayScale", coordinates.displayScale}, {"sourceToSceneUnitScale", v.sourceToSceneUnitScale()},
        {"sceneLeafName", QFileInfo(v.scenePath()).fileName()}, {"editToolsLocked", v.editToolsLocked()},
        {"renderedFrameSerial", qint64(v.renderedFrameSerial())}};
  }

  void observe(const char *phase) {
    QJsonArray phases = mCurrent.value("observations").toArray();
    phases.append(QJsonObject{{"phase", QString::fromLatin1(phase)}, {"publicState", publicState()}});
    mCurrent.insert("observations", phases);
  }

  void start(const QString &name) {
    require(mCurrent.isEmpty(), "overlapping-test-cases");
    mCurrent = {{"name", name}, {"fixtureSha256", mFixtureHash}};
  }

  void complete() {
    mCurrent.insert("status", "passed");
    mCases.append(mCurrent);
    mCurrent = {};
  }

  void mouse(QEvent::Type type, const QPointF &position, Qt::MouseButton button,
             Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, position, v.mapToGlobal(position.toPoint()),
                      button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&v, &event);
  }

  void key(int code, const QString &text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
    QApplication::sendEvent(&v, &press);
    QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier, text);
    QApplication::sendEvent(&v, &release);
  }

  void click(const QPointF &position) {
    mouse(QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton);
  }

  bool exposed() const {
    return v.isVisible() && !v.window()->isMinimized() && v.window()->windowHandle() &&
        v.window()->windowHandle()->isExposed() && v.isValid() && v.context() && v.context()->isValid();
  }

  QImage capture(const char *phase) {
    require(exposed(), "real-viewport-not-exposed", true);
    dispatchFor(25);
    require(exposed(), "real-viewport-lost-before-capture", true);
    const QImage image = v.grabFramebuffer();
    require(exposed() && !image.isNull(), "real-framebuffer-unavailable", true);
    const qreal dpr = v.devicePixelRatioF();
    require(std::isfinite(dpr) && dpr > 0 && image.width() > 0 && image.height() > 0 &&
        qint64(image.width()) * image.height() <= pixelLimit &&
        std::abs(image.width() - v.width() * dpr) <= 1 &&
        std::abs(image.height() - v.height() * dpr) <= 1,
        "framebuffer-physical-size-invalid", true);
    require(mImageCount < imageLimit, "raw-image-limit-exceeded");
    const QString leaf = QStringLiteral("%1-%2-%3.png").arg(++mImageCount, 3, 10, QLatin1Char('0'))
        .arg(mCurrent.value("name").toString(), QString::fromLatin1(phase));
    const QString path = QDir(mRoot).filePath(leaf);
    require(!QFileInfo::exists(path), "raw-image-already-exists");
    QSaveFile output(path);
    require(output.open(QIODevice::WriteOnly) && image.save(&output, "PNG") && output.commit(), "raw-image-write-failed");
    const QString hash = sha256(path);
    require(!hash.isEmpty(), "raw-image-digest-failed");
    QJsonArray frames = mCurrent.value("frames").toArray();
    frames.append(QJsonObject{{"phase", QString::fromLatin1(phase)}, {"image", leaf}, {"sha256", hash},
        {"physicalSize", QJsonArray{image.width(), image.height()}},
        {"logicalSize", QJsonArray{v.width(), v.height()}}, {"widgetDpr", dpr},
        {"imageDprMetadata", image.devicePixelRatio()},
        {"publicState", publicState()}, {"fixtureSha256", sha256(mFixture)}});
    mCurrent.insert("frames", frames);
    return image;
  }

  void prepare(bool edge, bool orthographic, TransformGizmoMode mode) {
    if (v.modelTransformActive()) key(Qt::Key_Escape);
    v.setEditToolsLocked(false);
    v.setModelTransform({}, {}, {1, 1, 1});
    v.setInteractionMode(NativeViewport::InteractionMode::Inspect);
    v.resetCamera();
    if (edge) {
      v.setAxisView(NavigationAxis::PositiveX);
      dispatchFor(cameraSnapDurationMilliseconds + 80);
    }
    if (v.orthographicProjection() != orthographic) {
      // This is the public navigation control, not the private transform layout.
      const auto navigation = navigationGizmoLayout(QMatrix4x4(), v.size(), QFontMetricsF(v.font()).height());
      click(navigation.projectionButton.center());
    }
    require(v.orthographicProjection() == orthographic, "projection-control-did-not-select-requested-view");
    v.selectModel();
    v.setModelGizmoMode(mode);
    require(v.modelSelected() && !v.modelTransformActive() && v.modelGizmoMode() == mode,
            "public-model-selection-or-mode-failed");
    mouse(QEvent::MouseMove, {v.width() * .15, v.height() * .78}, Qt::NoButton, Qt::NoButton);
    dispatchFor(30);
  }

  struct Handle { QPointF position; int axis = -1; };
  std::optional<Handle> visibleHandle(const QImage &image, Operation operation) {
    const double sx = double(image.width()) / v.width(), sy = double(image.height()) / v.height();
    const QPointF center(v.width() * .5, v.height() * .5);
    // Center-only ROI excludes top/right navigation, left rail and captions.
    const QRect roi(qRound(image.width() * .18), qRound(image.height() * .18),
                    qRound(image.width() * .64), qRound(image.height() * .64));
    // Public chrome layouts are exclusion masks only. They never generate a
    // model-handle coordinate or invoke the model gizmo's layout/hit-test.
    const qreal fontHeight = QFontMetricsF(v.font()).height();
    const QRectF rail = transformToolStripLayout(v.size(), fontHeight).background.adjusted(-3, -3, 3, 3);
    const auto navigation = navigationGizmoLayout(QMatrix4x4(), v.size(), fontHeight);
    const QRectF navigationArea = navigation.rotateBounds.united(navigation.zoomButton)
        .united(navigation.panButton).united(navigation.cameraButton)
        .united(navigation.projectionButton).united(navigation.projectionLabel).adjusted(-3, -3, 3, 3);
    const QRectF hint = v.selectionHintBounds();
    std::array<QList<QPointF>, 3> candidates;
    std::array<QSet<quint64>, 3> cells;
    std::array<qint64, 3> pixelCounts{};
    bool truncated = false;
    for (int y = roi.top(); y <= roi.bottom(); ++y) {
      for (int x = roi.left(); x <= roi.right(); ++x) {
        const int axis = visibleAxis(image.pixel(x, y));
        if (axis < 0) continue;
        const QPointF location((x + .5) / sx, (y + .5) / sy);
        if (rail.contains(location) || navigationArea.contains(location) || hint.contains(location)) continue;
        ++pixelCounts[std::size_t(axis)];
        if (QLineF(center, location).length() < 18) continue;
        const quint64 cell = (quint64(x / 3) << 32) | quint32(y / 3);
        auto &seen = cells[std::size_t(axis)];
        if (seen.contains(cell)) continue;
        seen.insert(cell);
        auto &list = candidates[std::size_t(axis)];
        if (list.size() >= candidateLimitPerAxis) { truncated = true; continue; }
        list.append(location);
      }
    }
    mCurrent.insert("axisColoredPixelCounts", QJsonArray{pixelCounts[0], pixelCounts[1], pixelCounts[2]});
    mCurrent.insert("candidateSamplingTruncated", truncated);
    mCurrent.insert("searchRoiPhysical", QJsonArray{roi.x(), roi.y(), roi.width(), roi.height()});
    mCurrent.insert("chromeExclusionMasksLogical", QJsonArray{
        QJsonArray{rail.x(), rail.y(), rail.width(), rail.height()},
        QJsonArray{navigationArea.x(), navigationArea.y(), navigationArea.width(), navigationArea.height()},
        QJsonArray{hint.x(), hint.y(), hint.width(), hint.height()}});
    static const std::array<QString, 3> labels{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
    for (int axis = 0; axis < 3; ++axis) {
      auto &list = candidates[std::size_t(axis)];
      std::stable_sort(list.begin(), list.end(), [&](const QPointF &a, const QPointF &b) {
        return QLineF(center, a).length() > QLineF(center, b).length();
      });
      const char *source = operation == Operation::Move ? "沿 %1 轴移动" :
          operation == Operation::Rotate ? "绕 %1 轴旋转" : "沿局部 %1 轴缩放";
      const QString expected = QCoreApplication::translate("Workbench", source).arg(labels[std::size_t(axis)]);
      for (const QPointF &candidate : list) {
        mouse(QEvent::MouseMove, candidate, Qt::NoButton, Qt::NoButton);
        if (v.toolTip().startsWith(expected)) return Handle{candidate, axis};
      }
    }
    return std::nullopt;
  }

  void drag(const Handle &handle, const QPointF &delta) {
    mouse(QEvent::MouseButtonPress, handle.position, Qt::LeftButton, Qt::LeftButton);
    require(v.modelTransformActive(), "visible-public-axis-did-not-begin-transform");
    for (int step = 1; step <= 4; ++step)
      mouse(QEvent::MouseMove, handle.position + delta * (step / 4.), Qt::NoButton, Qt::LeftButton);
  }

  void changedOnlyExpected(const State &before, Operation operation, int axis) {
    const State after = stateOf(v);
    require(sameCamera(before, after), "model-drag-changed-camera");
    if (operation == Operation::Move) {
      const auto delta = after.translation - before.translation;
      require(delta[axis] > 1e-5F, "outward-axis-drag-did-not-move-positive-model-axis");
      for (int other = 0; other < 3; ++other)
        if (other != axis) require(std::abs(delta[other]) < 1e-5F, "axis-move-changed-other-axis");
      require(sameRotation(before.rotation, after.rotation) && (before.scale - after.scale).length() < 1e-5F,
              "axis-move-changed-rotation-or-scale");
    } else if (operation == Operation::Scale) {
      require(after.scale[axis] > before.scale[axis] + 1e-4F, "outward-axis-drag-did-not-increase-scale");
      for (int other = 0; other < 3; ++other)
        if (other != axis) require(std::abs(after.scale[other] - before.scale[other]) < 1e-5F,
                                   "axis-scale-changed-other-axis");
      require((before.translation - after.translation).length() < 1e-5F && sameRotation(before.rotation, after.rotation),
              "axis-scale-changed-translation-or-rotation");
    } else {
      const QVector3D rotatedAxis(after.rotation.x(), after.rotation.y(), after.rotation.z());
      require(std::abs(rotatedAxis[axis]) > 1e-5F && !sameRotation(before.rotation, after.rotation),
              "visible-ring-drag-did-not-rotate");
      for (int other = 0; other < 3; ++other)
        if (other != axis) require(std::abs(rotatedAxis[other]) < 1e-5F, "axis-rotation-changed-other-axis");
      require((before.translation - after.translation).length() < 1e-5F && (before.scale - after.scale).length() < 1e-5F,
              "axis-rotation-changed-translation-or-scale");
    }
  }

  void mouseCase(const Spec &spec, bool edge, bool orthographic) {
    start(QStringLiteral("%1-%2-%3").arg(edge ? "axis-edge" : "oblique",
          orthographic ? "ortho" : "perspective", spec.name));
    prepare(edge, orthographic, spec.mode);
    const State before = stateOf(v);
    const QImage frame = capture("before");
    const auto target = visibleHandle(frame, spec.operation);
    require(target.has_value(), "no-visible-colored-axis-with-public-hover-feedback", true);
    mCurrent.insert("handleAxis", target->axis);
    mCurrent.insert("handleLogicalPosition", point(target->position));
    const QPointF radial = target->position - QPointF(v.width() * .5, v.height() * .5);
    const qreal radius = std::hypot(radial.x(), radial.y());
    require(radius > 18, "visible-handle-too-close-to-center", true);
    const QPointF direction = radial / radius;
    const QPointF delta = spec.operation == Operation::Rotate
        ? QPointF(-direction.y(), direction.x()) * 24 : direction * 24;
    mCurrent.insert("dragLogicalDelta", point(delta));
    drag(*target, delta);
    changedOnlyExpected(before, spec.operation, target->axis);
    capture("active-drag");
    mouse(QEvent::MouseButtonRelease, target->position + delta, Qt::LeftButton, Qt::NoButton);
    require(!v.modelTransformActive(), "mouse-release-did-not-commit-transform");
    changedOnlyExpected(before, spec.operation, target->axis);
    const State committed = stateOf(v);
    capture("committed");
    v.undoEdit();
    require(sameTransform(stateOf(v), before) && sameCamera(stateOf(v), before), "mouse-transform-undo-failed");
    capture("undone");
    v.redoEdit();
    require(sameTransform(stateOf(v), committed) && sameCamera(stateOf(v), before), "mouse-transform-redo-failed");
    capture("redone");
    v.undoEdit();
    drag(*target, delta);
    require(!sameTransform(stateOf(v), before), "cancel-case-had-no-live-transform-to-cancel");
    key(Qt::Key_Escape);
    mouse(QEvent::MouseButtonRelease, target->position + delta, Qt::LeftButton, Qt::NoButton);
    require(!v.modelTransformActive() && sameTransform(stateOf(v), before) && sameCamera(stateOf(v), before),
            "escape-did-not-restore-pre-drag-transform-and-camera");
    capture("cancelled");
    complete();
  }

  void numericInput(Operation operation) {
    key(operation == Operation::Move ? Qt::Key_G : operation == Operation::Rotate ? Qt::Key_R : Qt::Key_S);
    require(v.modelTransformActive(), "numeric-shortcut-did-not-begin-transform");
    key(Qt::Key_Z);
    if (operation == Operation::Move) key(Qt::Key_1, QStringLiteral("1"));
    else if (operation == Operation::Rotate) {
      key(Qt::Key_9, QStringLiteral("9")); key(Qt::Key_0, QStringLiteral("0"));
    } else key(Qt::Key_2, QStringLiteral("2"));
  }

  void numericCase(Operation operation, bool orthographic) {
    const char *operationName = operation == Operation::Move ? "G-Z-1" : operation == Operation::Rotate ? "R-Z-90" : "S-Z-2";
    start(QStringLiteral("numeric-%1-%2").arg(orthographic ? "ortho" : "perspective", operationName));
    prepare(false, orthographic, TransformGizmoMode::Transform);
    const State before = stateOf(v);
    capture("before");
    numericInput(operation);
    observe("active-before-cancel");
    key(Qt::Key_Escape);
    require(!v.modelTransformActive() && sameTransform(stateOf(v), before) && sameCamera(stateOf(v), before),
            "numeric-cancel-did-not-restore-transform");
    numericInput(operation);
    key(Qt::Key_Return);
    require(!v.modelTransformActive() && sameCamera(stateOf(v), before), "numeric-commit-changed-camera-or-stayed-active");
    const State committed = stateOf(v);
    if (operation == Operation::Move) {
      const double sourceToScene = v.sourceToSceneUnitScale(), displayScale = v.sceneCoordinates().displayScale;
      require(std::isfinite(sourceToScene) && sourceToScene > 0 && std::isfinite(displayScale) && displayScale > 0,
              "numeric-translation-unit-chain-invalid");
      const QVector3D local = committed.translation - before.translation;
      const double sceneZ = double(local.z()) * sourceToScene / displayScale;
      mCurrent.insert("observedSceneUnitTranslationZ", sceneZ);
      require(local.z() > 0 && std::abs(sceneZ - 1.) < 1e-4 &&
          std::abs(local.x()) < 1e-5F && std::abs(local.y()) < 1e-5F &&
          sameRotation(committed.rotation, before.rotation) && (committed.scale - before.scale).length() < 1e-5F,
          "G-Z-1-did-not-move-one-shared-scene-unit-only-on-Z");
    } else if (operation == Operation::Rotate) {
      const float halfRoot = float(std::sqrt(.5));
      require(sameRotation(committed.rotation, QQuaternion(halfRoot, 0, 0, halfRoot)) &&
          (committed.translation - before.translation).length() < 1e-5F && (committed.scale - before.scale).length() < 1e-5F,
          "R-Z-90-did-not-rotate-only-Z-by-quarter-turn");
    } else {
      require((committed.scale - QVector3D(1, 1, 2)).length() < 1e-5F &&
          (committed.translation - before.translation).length() < 1e-5F && sameRotation(committed.rotation, before.rotation),
          "S-Z-2-did-not-double-only-Z-scale");
    }
    capture("committed");
    v.undoEdit();
    require(sameTransform(stateOf(v), before), "numeric-transform-undo-failed");
    observe("undone");
    v.redoEdit();
    require(sameTransform(stateOf(v), committed), "numeric-transform-redo-failed");
    observe("redone");
    v.undoEdit();
    require(sameTransform(stateOf(v), before) && sameCamera(stateOf(v), before), "numeric-final-restore-failed");
    complete();
  }

  void exercise() {
    require(waitUntil([&] { return exposed(); }, 10000), "real-Windows-OpenGL-viewport-unavailable", true);
    require(v.width() >= 400 && v.height() >= 280, "viewport-too-small-for-isolated-handle-ROI", true);
    require(QDir().mkpath(QDir(mRoot).filePath(QStringLiteral("fixtures"))), "fixture-directory-write-failed");
    mFixture = QDir(mRoot).filePath(QStringLiteral("fixtures/gizmo-cube.ply"));
    require(writeCube(mFixture), "owned-cube-write-failed");
    mFixtureHash = sha256(mFixture);
    require(!mFixtureHash.isEmpty(), "owned-cube-digest-failed");
    const SceneObject object{QStringLiteral("gizmo-viewport-owned-cube"), mFixture, {}, {}, {1, 1, 1}, 8};
    v.setSceneObjects({object}, object.id);
    require(waitUntil([&] { return v.meshRenderingAvailable() && v.selectableModelAvailable() && v.meshPagingSettled(); }, 15000),
            "owned-cube-not-ready");
    v.setRenderMode(NativeViewport::RenderMode::Mesh);
    v.setShowCameras(false);
    v.setShowObservationTrackball(false);
    auto grid = v.viewportGridSettings();
    grid.gridVisible = false; grid.axesVisible = false;
    QString gridError;
    require(v.setViewportGridSettings(grid, &gridError), "public-grid-isolation-rejected");
    for (bool edge : {false, true})
      for (bool orthographic : {false, true})
        for (const auto &spec : specs) mouseCase(spec, edge, orthographic);
    for (bool orthographic : {false, true})
      for (Operation operation : {Operation::Move, Operation::Rotate, Operation::Scale}) numericCase(operation, orthographic);
    require(sha256(mFixture) == mFixtureHash, "owned-fixture-changed-during-public-input");
    mCompleted = true;
  }

  NativeViewport &v;
  QString mRoot, mSource, mFixture, mFixtureHash;
  QJsonArray mCases, mFailures;
  QJsonObject mCurrent;
  int mImageCount = 0;
  bool mCompleted = false;
  bool mUnsupported = false;
};
} // namespace

bool runGizmoViewportSmokeTest(NativeViewport &viewport) {
  const QString root = qEnvironmentVariable("GSW_GIZMO_VIEWPORT_EVIDENCE");
  const QString source = qEnvironmentVariable("GSW_UI_SOURCE_COMMIT");
  const QFileInfo location(root);
  const bool fresh = !location.exists() || (location.isDir() &&
      QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty());
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QGuiApplication::platformName() != QStringLiteral("windows") ||
      !qApp->arguments().contains(QStringLiteral("--smoke-test-gizmo-viewport")) ||
      qApp->property("gswSmokeSettingsDirectory").toString().isEmpty() ||
      !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{40}$")).match(source).hasMatch() ||
      !QDir::isAbsolutePath(root) || !fresh || location.isSymLink() || location.isJunction() ||
      !viewport.scenePath().isEmpty() || viewport.sceneObjectCount() != 0 ||
      viewport.hasProcessingPreview() || viewport.trainingGpuPreviewActive() ||
      !QDir().mkpath(root)) {
    qCritical("GIZMO_VIEWPORT FAIL: explicit remote isolated empty-scene contract required");
    return false;
  }
  return GizmoSmoke(viewport, root, source.toLower()).run();
}
} // namespace gsw
