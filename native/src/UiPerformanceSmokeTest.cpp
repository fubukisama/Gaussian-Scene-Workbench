#include "UiPerformanceSmokeTest.h"
#include "NativeViewport.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QTimer>
#include <QToolBar>
#include <QVariant>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <random>
#include <vector>

namespace gsw {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int mediumCount = 512202;
constexpr int recordCapacity = 512;

QString sha256(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return QString::fromLatin1(hash.result().toHex());
}

bool writeNew(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
      file.write(bytes) == bytes.size() && file.flush();
}

void dispatchFor(int milliseconds) {
  QEventLoop loop;
  QTimer timer;
  timer.setTimerType(Qt::PreciseTimer);
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  timer.start(milliseconds);
  loop.exec();
}

bool waitUntil(const std::function<bool()> &predicate, int timeout) {
  const auto until = Clock::now() + std::chrono::milliseconds(timeout);
  while (!predicate() && Clock::now() < until) dispatchFor(10);
  return predicate();
}

QByteArray header(int kind) {
  const bool mesh = kind == 1, gaussian = kind >= 2;
  QByteArray result = "ply\nformat ascii 1.0\nelement vertex ";
  result += mesh ? "4\n" : "441\n";
  result += "property float x\nproperty float y\nproperty float z\n";
  if (gaussian) {
    result += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\n";
    if (kind == 2) result += "property float scale_2\n";
    result += "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
  } else result += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  if (mesh) result += "element face 2\nproperty list uchar int vertex_indices\n";
  return result + "end_header\n";
}

QByteArray lightFixture(int kind) {
  QByteArray bytes = header(kind);
  const auto vertex = [kind](float x, float y) {
    QByteArray line = QString("%1 %2 0 ").arg(x).arg(y).toUtf8();
    if (kind < 2) line += "225 100 30\n";
    else if (kind == 2) line += "1 -1 -1 3 -3.5 -3.5 -3.5 1 0 0 0\n";
    else line += "1 -1 -1 3 -3.5 -3.5 1 0 0 0\n";
    return line;
  };
  if (kind == 1) {
    bytes += vertex(-1, -1) + vertex(1, -1) + vertex(-1, 1) + vertex(1, 1);
    bytes += "3 0 1 2\n3 2 1 3\n";
  } else {
    for (int y = -10; y <= 10; ++y)
      for (int x = -10; x <= 10; ++x) bytes += vertex(x / 10.0F, y / 10.0F);
  }
  return bytes;
}

QByteArray mediumHeader() {
  QByteArray result("ply\nformat binary_little_endian 1.0\nelement vertex 512202\n");
  for (const char *field : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                           "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
    result += QByteArray("property float ") + field + '\n';
  return result + "end_header\n";
}

// These bytes are generated once on this runner. Both executables subsequently
// read this same file; equal PRNG seeds across compilers are not our proof.
bool createMedium(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
  if (file.write(mediumHeader()) != mediumHeader().size()) return false;
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  std::mt19937 random(42);
  std::uniform_real_distribution<float> position(-1.0F, 1.0F);
  for (int i = 0; i < mediumCount; ++i)
    stream << position(random) << position(random) << position(random)
           << 0.7F << -0.3F << 0.1F << 0.0F << -5.0F << -6.0F << -7.0F
           << 0.7F << 0.1F << 0.2F << 0.4F;
  return stream.status() == QDataStream::Ok && file.flush();
}

struct Fixture {
  QString path;
  QJsonObject identity;
};

bool getFixture(const QDir &directory, const QString &id, int kind, Fixture &fixture) {
  const bool medium = kind == 4;
  const QString path = directory.filePath(id + ".ply");
  const QString receipt = path + ".json";
  const QJsonObject parameters{{"generator", "scheduling-seed42-v1"}, {"kind", id},
      {"seed", 42}, {"vertices", medium ? mediumCount : kind == 1 ? 4 : 441},
      {"faces", kind == 1 ? 2 : 0}, {"shDegree", kind >= 2 ? 0 : -1},
      {"scales", kind == 3 ? 2 : kind >= 2 ? 3 : 0}};
  if (!QFileInfo::exists(path)) {
    if (QFileInfo::exists(receipt)) return false;
    if (medium ? !createMedium(path) : !writeNew(path, lightFixture(kind))) return false;
    const QString digest = sha256(path);
    if (digest.isEmpty() || !writeNew(receipt, QJsonDocument(QJsonObject{
        {"schema", "gsw.ui_performance_fixture/1"}, {"parameters", parameters},
        {"bytes", QFileInfo(path).size()}, {"sha256", digest}}).toJson())) return false;
  }
  QFile metadata(receipt), source(path);
  if (!metadata.open(QIODevice::ReadOnly) || !source.open(QIODevice::ReadOnly)) return false;
  QJsonParseError parse;
  const QJsonObject stored = QJsonDocument::fromJson(metadata.readAll(), &parse).object();
  if (parse.error != QJsonParseError::NoError || stored.value("schema") != "gsw.ui_performance_fixture/1" ||
      stored.value("parameters").toObject() != parameters ||
      stored.value("bytes").toInteger() != source.size() || stored.value("sha256").toString() != sha256(path)) return false;
  if (medium) {
    const QByteArray expected = mediumHeader();
    if (source.size() != expected.size() + qint64(mediumCount) * 14 * 4 || source.read(expected.size()) != expected) return false;
  } else if (source.readAll() != lightFixture(kind)) return false;
  fixture = {path, stored};
  fixture.identity.insert("path", path);
  return true;
}

struct Camera {
  QVector3D target;
  float distance;
  OrbitAngles orbit;
  bool orthographic;
  bool operator==(const Camera &) const = default;
};
Camera camera(const NativeViewport &v) { return {v.viewTarget(), v.viewDistance(), v.viewOrbitAngles(), v.orthographicProjection()}; }
QJsonArray vector(const QVector3D &v) { return {v.x(), v.y(), v.z()}; }
QJsonObject json(const Camera &c) {
  return {{"target", vector(c.target)}, {"distance", c.distance},
      {"orbit", QJsonArray{c.orbit.yawDegrees, c.orbit.pitchDegrees, c.orbit.rollDegrees}},
      {"orthographic", c.orthographic}};
}
QJsonObject model(const NativeViewport &v) {
  const auto q = v.modelRotation();
  return {{"translation", vector(v.modelTranslation())}, {"scale", vector(v.modelScale())},
      {"rotation", QJsonArray{q.scalar(), q.x(), q.y(), q.z()}}, {"selected", v.modelSelected()},
      {"sourceShDegree", v.sourceShDegree()}, {"effectiveShDegree", v.effectiveShDegree()},
      {"renderedPointCount", v.renderedPointCount()}, {"residentMeshTriangles", v.residentMeshTriangleCount()},
      {"meshRenderingAvailable", v.meshRenderingAvailable()}, {"meshPagingSettled", v.meshPagingSettled()},
      {"pointPagingSettled", v.pointPagingSettled()},
      {"gaussian", v.sourceHasGaussianAttributes()}, {"surfel", v.sourceHasSurfelAttributes()},
      {"indexed", v.indexedGaussianRendering()}, {"attributeUploads", qint64(v.gaussianAttributeUploads())},
      {"shUploads", qint64(v.gaussianShUploads())}, {"orderUploads", qint64(v.gaussianOrderUploads())},
      {"renderMode", int(v.renderMode())}};
}

struct Presentation { quint64 serial; qint64 ns; };
struct EventRecord {
  qint64 dispatchNs, handlerDoneNs, presentNs;
  quint64 beforeSerial, presentSerial;
  Camera before, afterDispatch, afterSwap;
};

bool zeroTimerActive(const NativeViewport &viewport) {
  for (auto *timer : viewport.findChildren<QTimer *>())
    if (timer->isActive() && timer->interval() == 0) return true;
  return false;
}

bool fullyExposed(const NativeViewport &viewport) {
  auto *window = viewport.window();
  if (!window || !window->windowHandle() || !window->windowHandle()->isExposed() ||
      window->isMinimized() || !viewport.isVisible() || viewport.size() != QSize(640, 360)) return false;
  const QRect global(viewport.mapToGlobal(QPoint(0, 0)), viewport.size());
  auto *screen = window->windowHandle()->screen();
  if (!screen || !screen->availableGeometry().contains(global)) return false;
  for (const QWidget *parent = viewport.parentWidget(); parent; parent = parent->parentWidget()) {
    const QRect relative(viewport.mapTo(parent, QPoint(0, 0)), viewport.size());
    if (!parent->rect().contains(relative)) return false;
  }
  return true;
}
}

int runUiPerformanceSmokeTest(NativeViewport &viewport) {
  const auto epoch = Clock::now();
  const auto now = [&]() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - epoch).count(); };
  const QString outputPath = qEnvironmentVariable("GSW_UI_PERF_OUTPUT_DIR");
  const QString fixturePath = qEnvironmentVariable("GSW_UI_PERF_FIXTURE_DIR");
  const QString scenario = qEnvironmentVariable("GSW_UI_PERF_SCENARIO");
  const QString arm = qEnvironmentVariable("GSW_UI_PERF_ARM");
  const QString source = qEnvironmentVariable("GSW_UI_PERF_SOURCE_COMMIT");
  const QString runner = qEnvironmentVariable("GSW_UI_PERF_RUNNER_ID");
  const int round = qEnvironmentVariableIntValue("GSW_UI_PERF_ROUND");
  QDir output(outputPath), fixtures(fixturePath);
  // Never enter the recording through an ordinary user launch or inspect a
  // user workspace. The remote workflow gives this process an isolated store.
  if (qEnvironmentVariable("GITHUB_ACTIONS") != "true" ||
      qApp->property("gswSmokeSettingsDirectory").toString().isEmpty() ||
      !QDir::isAbsolutePath(outputPath) || !output.exists() ||
      !output.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty() ||
      !QDir::isAbsolutePath(fixturePath) || !fixtures.exists() ||
      QFileInfo(outputPath).isSymLink() || QFileInfo(fixturePath).isSymLink()) {
    qCritical() << "UI_PERFORMANCE FAIL: fresh owned absolute remote output/settings required";
    return 2;
  }
  QStringList failures, unready;
  QJsonArray cases;
  QJsonObject environment;
  const qsizetype atEntry = viewport.uiPaintObservation().count;
  std::vector<Presentation> presentations;
  presentations.reserve(recordCapacity);
  bool presentationOverflow = false;
  quint64 lastPresented = viewport.renderedFrameSerial(), metrics = 0, zeroCallbacks = 0;
  QList<QMetaObject::Connection> connections;
  connections.append(QObject::connect(&viewport, &QOpenGLWidget::frameSwapped, &viewport, [&] {
    const quint64 serial = viewport.renderedFrameSerial();
    if (serial > lastPresented) {
      lastPresented = serial;
      if (presentations.size() < recordCapacity) presentations.push_back({serial, now()});
      else presentationOverflow = true;
    }
  }));
  connections.append(QObject::connect(&viewport, &NativeViewport::frameMetricsChanged, &viewport,
      [&](double, double) { ++metrics; }));
  for (auto *timer : viewport.findChildren<QTimer *>())
    if (timer->interval() == 0) connections.append(QObject::connect(timer, &QTimer::timeout, &viewport, [&] { ++zeroCallbacks; }));
  const auto disconnect = qScopeGuard([&] { for (const auto &c : connections) QObject::disconnect(c); });
  const auto fail = [&](const QString &reason) { failures.append(reason); };
  const auto inconclusive = [&](const QString &reason) { unready.append(reason); };
  const auto finish = [&]() {
    const auto &observed = viewport.uiPaintObservation();
    QJsonArray samples;
    for (qsizetype i = 0; i < std::clamp(observed.count, qsizetype(0), UiPaintObservation::capacity); ++i) {
      const auto &s = observed.samples[size_t(i)];
      samples.append(QJsonObject{{"frameSerial", qint64(s.frameSerial)}, {"hostNanoseconds", s.hostNanoseconds},
          {"logicalSize", QJsonArray{s.logicalWidth, s.logicalHeight}}, {"actualDpr", s.devicePixelRatio},
          {"ownedCpuBytes", qint64(s.ownedCpuBytes)}, {"requestedGpuBytes", qint64(s.requestedGpuBytes)},
          {"cacheGeneration", qint64(s.cacheGeneration)}, {"cacheHit", s.cacheHit}, {"diagnosticOverlap", s.diagnosticOverlap}});
      if (s.diagnosticOverlap && !failures.contains("diagnostic_overlap")) fail("diagnostic_overlap");
    }
    if ((!observed.enabled || (observed.count == 0 && unready.isEmpty()) || observed.overflow || presentationOverflow) &&
        !failures.contains("passive_observation_incomplete")) fail("passive_observation_incomplete");
    const QString status = !failures.isEmpty() ? "FAIL" : !unready.isEmpty() ? "INCONCLUSIVE" : "RECORDED";
    const QJsonObject result{{"schema", "gsw.native_ui_performance_process/1"}, {"status", status},
        {"arm", arm}, {"round", round}, {"scenario", scenario}, {"sourceCommit", source}, {"runnerId", runner},
        {"executableSha256", sha256(QCoreApplication::applicationFilePath())}, {"environment", environment},
        {"preWindowWarmRange", QJsonArray{0, atEntry}}, {"cases", cases},
        {"observation", QJsonObject{{"enabled", observed.enabled}, {"capacity", UiPaintObservation::capacity},
            {"count", observed.count}, {"overflow", observed.overflow}, {"samples", samples}}},
        {"failures", QJsonArray::fromStringList(failures)}, {"unready", QJsonArray::fromStringList(unready)},
        {"measurementLimitations", QJsonArray{"CPU host submission, not GPU time", "Qt injected event to fresh swap, not input-to-photon",
            "Requested bytes, not driver residency", "Light theme only; dark, live cross-monitor DPR, NVIDIA driver and physical input NOT_RUN"}},
        {"wallNanoseconds", now()}};
    if (!writeNew(output.filePath("result.json"), QJsonDocument(result).toJson())) {
      qCritical() << "UI_PERFORMANCE FAIL: cannot retain fresh result"; return 2;
    }
    qInfo() << "UI_PERFORMANCE" << status << "samples" << observed.count << "cases" << cases.size()
            << "failures" << failures << "unready" << unready;
    return !failures.isEmpty() ? 2 : !unready.isEmpty() ? 3 : 0;
  };
  if ((arm != "A" && arm != "B") || round < 1 || round > 3 || runner.isEmpty() ||
      !QRegularExpression("^[0-9a-f]{40}$").match(source).hasMatch() ||
      (scenario != "empty" && scenario != "light" && scenario != "medium") ||
      !viewport.scenePath().isEmpty() || viewport.trainingGpuPreviewActive() ||
      !viewport.uiPaintObservation().enabled) {
    fail("invalid_isolated_process_contract"); return finish();
  }
  if (qEnvironmentVariableIsSet("GSW_RENDER_DIAGNOSTICS_DIR") ||
      qEnvironmentVariableIsSet("GSW_RENDER_DIAGNOSTICS_SMOKE")) {
    fail("diagnostics_enabled"); return finish();
  }
  const QString marker = fixtures.filePath("ownership.json");
  const QByteArray markerBytes("{\"schema\":\"gsw.ui_performance_owned_fixtures/1\"}\n");
  if (!QFileInfo::exists(marker)) {
    if (!fixtures.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty() || !writeNew(marker, markerBytes)) {
      fail("unowned_fixture_directory"); return finish();
    }
  } else {
    QFile file(marker);
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != markerBytes) {
      fail("invalid_fixture_ownership"); return finish();
    }
  }
  auto *window = viewport.window();
  // Only this isolated smoke window is resized. Inspector chrome is excluded
  // to keep the actual viewport equal, including on a small DPR2 CI display.
  for (auto *dock : window->findChildren<QDockWidget *>()) dock->hide();
  for (auto *bar : window->findChildren<QToolBar *>()) bar->hide();
  window->setMinimumSize(0, 0);
  viewport.setFixedSize(640, 360);
  window->adjustSize();
  if (auto *screen = window->windowHandle() ? window->windowHandle()->screen() : nullptr)
    window->move(screen->availableGeometry().center() - window->rect().center());
  if (!waitUntil([&] { return fullyExposed(viewport) && viewport.context() && viewport.context()->isValid(); }, 3000)) {
    inconclusive("fixed_viewport_not_fully_exposed"); return finish();
  }
  dispatchFor(150);
  auto *context = viewport.context();
  viewport.makeCurrent();
  auto *gl = context->functions();
  const auto glString = [&](GLenum name) { const auto *value = gl->glGetString(name); return value ? QString::fromLatin1(reinterpret_cast<const char *>(value)) : QString(); };
  GLint actualViewport[4]{};
  gl->glGetIntegerv(GL_VIEWPORT, actualViewport);
  const auto fmt = context->format();
  const QFontInfo font(viewport.font());
  auto *screen = window->windowHandle()->screen();
  environment = {{"logicalSize", QJsonArray{viewport.width(), viewport.height()}},
      {"physicalSize", QJsonArray{qRound(viewport.width() * viewport.devicePixelRatioF()), qRound(viewport.height() * viewport.devicePixelRatioF())}},
      {"glViewport", QJsonArray{actualViewport[0], actualViewport[1], actualViewport[2], actualViewport[3]}},
      {"actualDpr", viewport.devicePixelRatioF()}, {"qtScaleFactor", qEnvironmentVariable("QT_SCALE_FACTOR")},
      {"qtVersion", qVersion()}, {"qpaPlatform", QGuiApplication::platformName()},
      {"glVendor", glString(GL_VENDOR)}, {"glRenderer", glString(GL_RENDERER)}, {"glVersion", glString(GL_VERSION)},
      {"swapInterval", fmt.swapInterval()}, {"requestedSwapInterval", viewport.format().swapInterval()},
      {"samples", fmt.samples()}, {"theme", qApp->property("gswUiTheme").toString()},
      {"fontFamily", font.family()}, {"fontPointSize", font.pointSizeF()}, {"screenRefreshRate", screen->refreshRate()},
      {"screenLogicalDpi", screen->logicalDotsPerInch()}, {"sessionName", qEnvironmentVariable("SESSIONNAME")},
      {"fullyExposed", fullyExposed(viewport)}, {"captureDuringMeasurements", false}, {"glQueriesDuringMeasurements", false}};
  viewport.doneCurrent();
  if (environment.value("glViewport").toArray().at(2).toInt() != environment.value("physicalSize").toArray().at(0).toInt() ||
      environment.value("glViewport").toArray().at(3).toInt() != environment.value("physicalSize").toArray().at(1).toInt()) {
    inconclusive("actual_GL_viewport_size_mismatch"); return finish();
  }
  viewport.setEditToolsLocked(true);
  viewport.setInteractionMode(NativeViewport::InteractionMode::Inspect);
  viewport.setShowCameras(false);
  viewport.setShowObservationTrackball(false);
  viewport.clearSelection();
  const QStringList ids = scenario == "empty" ? QStringList{"empty"} :
      scenario == "light" ? QStringList{"points", "mesh", "3dgs", "2dgs"} : QStringList{"medium"};
  const auto mouse = [&](QEvent::Type type, const QPoint &point, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(point), QPointF(viewport.mapToGlobal(point)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &event);
  };
  const auto awaitFresh = [&](quint64 before, int timeout) {
    if (lastPresented > before) return true;
    QEventLoop loop;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer); timer.setSingleShot(true);
    const auto connection = QObject::connect(&viewport, &QOpenGLWidget::frameSwapped, &loop, [&] {
      if (lastPresented > before) loop.quit();
    });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeout); loop.exec(); QObject::disconnect(connection);
    return lastPresented > before;
  };
  for (const QString &id : ids) {
    const int kind = id == "points" ? 0 : id == "mesh" ? 1 : id == "3dgs" ? 2 : id == "2dgs" ? 3 : 4;
    Fixture fixture;
    qint64 sourceCount = 0, faceCount = 0;
    if (id != "empty") {
      if (!getFixture(fixtures, id, kind, fixture)) { fail("fixture_bytes_or_parameters_mismatch:" + id); break; }
      bool loaded = false;
      QString loadError;
      sourceCount = -1; faceCount = -1;
      const auto loadedConnection = QObject::connect(&viewport, &NativeViewport::sceneLoaded, &viewport,
          [&](qint64 count, qsizetype, qint64 faces, qsizetype) { sourceCount = count; faceCount = faces; loaded = true; });
      const auto errorConnection = QObject::connect(&viewport, &NativeViewport::sceneLoadFailed, &viewport,
          [&](const QString &, const QString &message) { loadError = message; });
      viewport.setScene(fixture.path, kind == 4 ? mediumCount : kind == 1 ? 4 : 441);
      viewport.setRenderMode(kind == 1 ? NativeViewport::RenderMode::Mesh : kind >= 2 ? NativeViewport::RenderMode::Gaussians : NativeViewport::RenderMode::Points);
      const bool ready = waitUntil([&] {
        return !loadError.isEmpty() || (loaded && (kind == 1 ? viewport.meshRenderingAvailable() && viewport.meshPagingSettled() :
            kind >= 2 ? viewport.gaussianRenderingAvailable() : viewport.selectableModelAvailable() && viewport.pointPagingSettled()));
      }, 30000);
      QObject::disconnect(loadedConnection); QObject::disconnect(errorConnection);
      if (!loadError.isEmpty()) { fail("fixture_load_failed:" + id + ":" + loadError); break; }
      if (!ready) { inconclusive("fixture_load_deadline:" + id); break; }
      if (sourceCount != (kind == 4 ? mediumCount : kind == 1 ? 4 : 441) || faceCount != (kind == 1 ? 2 : 0) ||
          (kind >= 2 && (viewport.sourceHasSurfelAttributes() != (kind == 3) || !viewport.indexedGaussianRendering()))) {
        fail("actual_renderer_or_source_count_mismatch:" + id); break;
      }
    }
    viewport.resetCamera();
    viewport.clearSelection();
    const QPoint center = viewport.rect().center();
    QPoint point = center;
    mouse(QEvent::MouseButtonPress, point, Qt::MiddleButton, Qt::MiddleButton);
    dispatchFor(30);
    const qsizetype warmBegin = viewport.uiPaintObservation().count;
    const auto warmStart = Clock::now();
    const int warmEvents = scenario == "medium" ? 2 : 4;
    bool complete = true;
    for (int index = 0; index < warmEvents; ++index) {
      const quint64 before = viewport.renderedFrameSerial();
      point += QPoint(index % 2 ? -2 : 2, index % 2 ? 1 : -1);
      const Camera previous = camera(viewport);
      mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::MiddleButton);
      if (camera(viewport) == previous) { fail("warm_navigation_did_not_change_state:" + id); complete = false; break; }
      const int bound = scenario == "medium" ? 5000 : 3000;
      const int remaining = bound - int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - warmStart).count());
      if (remaining <= 0 || !awaitFresh(before, remaining)) { inconclusive("bounded_warmup_deadline:" + id); complete = false; break; }
    }
    const qsizetype warmEnd = viewport.uiPaintObservation().count;
    if (!complete) { mouse(QEvent::MouseButtonRelease, point, Qt::MiddleButton, Qt::NoButton); break; }
    const QJsonObject modelBefore = model(viewport);
    const Camera cameraBefore = camera(viewport);
    const qsizetype observationBegin = viewport.uiPaintObservation().count;
    const size_t presentationBegin = presentations.size();
    std::vector<EventRecord> events;
    const int wanted = scenario == "empty" ? 60 : scenario == "light" ? 52 : 8;
    events.reserve(size_t(wanted));
    const auto measuredStart = Clock::now();
    for (int index = 0; index < wanted; ++index) {
      EventRecord event{};
      event.before = camera(viewport); event.beforeSerial = viewport.renderedFrameSerial();
      point += QPoint(index % 2 ? -2 : 2, index % 2 ? 1 : -1);
      event.dispatchNs = now();
      mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::MiddleButton);
      event.handlerDoneNs = now(); event.afterDispatch = camera(viewport);
      if (event.afterDispatch == event.before) { fail("navigation_did_not_change_state:" + id); complete = false; break; }
      const int bound = scenario == "medium" ? 90000 : 30000;
      const int remaining = bound - int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - measuredStart).count());
      if (remaining <= 0 || !awaitFresh(event.beforeSerial, remaining)) { inconclusive("fresh_swap_deadline:" + id); complete = false; break; }
      const auto found = std::find_if(presentations.begin() + qptrdiff(presentationBegin), presentations.end(), [&](const Presentation &p) {
        return p.serial > event.beforeSerial && p.ns >= event.dispatchNs;
      });
      if (found == presentations.end()) { fail("fresh_swap_not_correlated:" + id); complete = false; break; }
      event.presentSerial = found->serial; event.presentNs = found->ns; event.afterSwap = camera(viewport);
      events.push_back(event);
      if (!(event.afterDispatch == event.afterSwap) || !fullyExposed(viewport)) { fail("camera_or_exposure_changed_before_swap:" + id); complete = false; break; }
    }
    const qsizetype observationEnd = viewport.uiPaintObservation().count;
    const size_t presentationEnd = presentations.size();
    mouse(QEvent::MouseButtonRelease, point, Qt::MiddleButton, Qt::NoButton);
    const QJsonObject modelAfter = model(viewport);
    if (modelBefore != modelAfter) fail("pan_changed_model_fidelity_uploads_or_transform:" + id);
    if (kind >= 2 && id != "empty" && (viewport.renderedPointCount() != (kind == 4 ? mediumCount : 441) ||
        viewport.sourceShDegree() != 0 || viewport.effectiveShDegree() != 0)) fail("gaussian_count_or_SH_reduced:" + id);
    QJsonArray idle;
    if (complete) {
      dispatchFor(600);
      const Camera idleCamera = camera(viewport);
      for (int index = 0; index < 2; ++index) {
        const auto begin = now();
        const quint64 renderBefore = viewport.renderedFrameSerial(), metricsBefore = metrics, zeroBefore = zeroCallbacks;
        const size_t swapsBefore = presentations.size();
        const bool zeroBeforeActive = zeroTimerActive(viewport);
        dispatchFor(1000);
        const QJsonObject windowRecord{{"beginNs", begin}, {"endNs", now()},
            {"newRenderedFrames", qint64(viewport.renderedFrameSerial() - renderBefore)},
            {"freshSwaps", qint64(presentations.size() - swapsBefore)}, {"metricsEmissions", qint64(metrics - metricsBefore)},
            {"zeroTimerCallbacks", qint64(zeroCallbacks - zeroBefore)},
            {"zeroTimerActiveBefore", zeroBeforeActive}, {"zeroTimerActiveAfter", zeroTimerActive(viewport)},
            {"cameraUnchanged", camera(viewport) == idleCamera}};
        idle.append(windowRecord);
        if (viewport.renderedFrameSerial() != renderBefore || presentations.size() != swapsBefore || metrics != metricsBefore ||
            zeroCallbacks != zeroBefore || zeroBeforeActive || zeroTimerActive(viewport) || !(camera(viewport) == idleCamera))
          fail("idle_work_or_camera_change:" + id);
      }
    }
    QJsonArray eventJson, presentationJson;
    for (const auto &e : events) eventJson.append(QJsonObject{{"dispatchNs", e.dispatchNs}, {"handlerDoneNs", e.handlerDoneNs},
        {"presentNs", e.presentNs}, {"beforeRenderSerial", qint64(e.beforeSerial)}, {"presentSerial", qint64(e.presentSerial)},
        {"cameraBefore", json(e.before)}, {"cameraAfterDispatch", json(e.afterDispatch)}, {"cameraAfterSwap", json(e.afterSwap)}});
    for (size_t index = presentationBegin; index < presentationEnd; ++index)
      presentationJson.append(QJsonObject{{"serial", qint64(presentations[index].serial)}, {"ns", presentations[index].ns}});
    cases.append(QJsonObject{{"id", id}, {"fixture", fixture.identity}, {"events", eventJson},
        {"sourceVertexCount", sourceCount}, {"sourceFaceCount", faceCount},
        {"presentations", presentationJson}, {"observationRange", QJsonArray{observationBegin, observationEnd}},
        {"warmupRange", QJsonArray{warmBegin, warmEnd}}, {"warmupEvents", warmEvents},
        {"cameraBefore", json(cameraBefore)}, {"modelBefore", modelBefore}, {"modelAfter", modelAfter},
        {"idleSettleMilliseconds", 600}, {"idle", idle}, {"complete", complete}});
    if (!failures.isEmpty() || !unready.isEmpty()) break;
  }
  return finish();
}
} // namespace gsw
