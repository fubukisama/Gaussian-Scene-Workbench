#include "ViewportStatusControlsSmokeTest.h"
#include "AppLanguage.h"
#include "MainWindow.h"
#include "NativeViewport.h"
#include "SceneCoordinates.h"
#include "ViewportCamera.h"
#include "ViewportGridDialog.h"
#include "ViewportGridSettings.h"
#include "WorkspaceDocument.h"
#include "../tests/UiFormVisualProbe.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace gsw {
namespace {
using Settings = ViewportGridSettings;

bool check(bool good, const char *message) {
  if (!good) qCritical() << "VIEWPORT_STATUS_CONTROLS FAIL:" << message;
  return good;
}

QString translated(const char *source) {
  return QCoreApplication::translate("Workbench", source);
}

void dispatchFor(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

bool waitUntil(const std::function<bool()> &predicate, int timeout = 1800) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeout) dispatchFor(10);
  return predicate();
}

QByteArray contents(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QByteArray sha256(const QString &path) {
  return QCryptographicHash::hash(contents(path), QCryptographicHash::Sha256);
}

bool writeFixture(const QString &path, const QByteArray &unit) {
  // Same public PLY unit convention as CoordinateSceneSmokeTest, with no
  // declaration at all for arbitrary/unknown coordinates. This is an owned
  // generic point patch, not research input or a scene-specific scale prior.
  QByteArray ply("ply\nformat ascii 1.0\n");
  if (!unit.isEmpty()) ply += "comment unit " + unit + '\n';
  ply += "element vertex 81\nproperty double x\nproperty double y\nproperty double z\n"
         "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
  const double coordinateScale = unit == QByteArrayLiteral("mm") ? 1000.0 : 1.0;
  for (int y = -4; y <= 4; ++y)
    for (int x = -4; x <= 4; ++x)
      ply += QStringLiteral("%1 %2 %3 210 110 35\n")
          .arg(x * coordinateScale / 4.0, 0, 'g', 17)
          .arg(y * coordinateScale / 4.0, 0, 'g', 17)
          .arg((x + y) * coordinateScale / 32.0, 0, 'g', 17).toUtf8();
  QFile output(path);
  return output.open(QIODevice::WriteOnly) && output.write(ply) == ply.size();
}

bool sameObjects(const QList<SceneObject> &before, const QList<SceneObject> &after) {
  if (before.size() != after.size()) return false;
  for (qsizetype i = 0; i < before.size(); ++i) {
    const auto &a = before[i];
    const auto &b = after[i];
    if (a.id != b.id || QFileInfo(a.path).canonicalFilePath() != QFileInfo(b.path).canonicalFilePath() ||
        a.translation != b.translation || a.rotation != b.rotation || a.scale != b.scale ||
        a.visible != b.visible || a.vertexCount != b.vertexCount) return false;
  }
  return true;
}

struct Snapshot final {
  QJsonObject collection;
  QJsonValue gridField;
  std::optional<QJsonObject> typedGrid;
  QVector3D target;
  float distance = 0.0F;
  OrbitAngles orbit;
  bool orthographic = false;
  QVector3D modelTranslation;
  QQuaternion modelRotation;
  QVector3D modelScale;

  Snapshot(const WorkspaceDocument &document, const NativeViewport &viewport)
      : collection(document.sceneCollectionJson()),
        gridField(QJsonDocument::fromJson(document.recoveryManifestJson()).object()
                      .value(QStringLiteral("viewportGrid"))),
        typedGrid(viewport.viewportGridSettings().toJson()),
        target(viewport.viewTarget()), distance(viewport.viewDistance()),
        orbit(viewport.viewOrbitAngles()), orthographic(viewport.orthographicProjection()),
        modelTranslation(viewport.modelTranslation()), modelRotation(viewport.modelRotation()),
        modelScale(viewport.modelScale()) {}

  bool unchanged(const WorkspaceDocument &document, const NativeViewport &viewport) const {
    return typedGrid.has_value() && sourceAndCameraUnchanged(document, viewport) &&
        gridField == QJsonDocument::fromJson(document.recoveryManifestJson()).object()
                         .value(QStringLiteral("viewportGrid")) &&
        typedGrid == viewport.viewportGridSettings().toJson();
  }

  bool sourceAndCameraUnchanged(const WorkspaceDocument &document, const NativeViewport &viewport) const {
    return collection == document.sceneCollectionJson() &&
        target == viewport.viewTarget() && distance == viewport.viewDistance() &&
        orbit == viewport.viewOrbitAngles() && orthographic == viewport.orthographicProjection() &&
        modelTranslation == viewport.modelTranslation() && modelRotation == viewport.modelRotation() &&
        modelScale == viewport.modelScale();
  }
};

struct GridControls final {
  QComboBox *mode = nullptr;
  QLineEdit *spacing = nullptr;
  QComboBox *subdivisions = nullptr;
  QDoubleSpinBox *density = nullptr;
  QComboBox *unit = nullptr;
  QCheckBox *grid = nullptr;
  QCheckBox *axes = nullptr;
  QLabel *provenance = nullptr;
  QLabel *validation = nullptr;
  QPushButton *apply = nullptr;
  QPushButton *cancel = nullptr;

  explicit GridControls(QDialog &dialog) {
    mode = dialog.findChild<QComboBox *>(QStringLiteral("gridSpacingMode"));
    spacing = dialog.findChild<QLineEdit *>(QStringLiteral("gridMajorSpacing"));
    subdivisions = dialog.findChild<QComboBox *>(QStringLiteral("gridSubdivisions"));
    density = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("gridDensity"));
    unit = dialog.findChild<QComboBox *>(QStringLiteral("gridDisplayUnit"));
    grid = dialog.findChild<QCheckBox *>(QStringLiteral("gridVisibleCheck"));
    axes = dialog.findChild<QCheckBox *>(QStringLiteral("gridAxesVisibleCheck"));
    provenance = dialog.findChild<QLabel *>(QStringLiteral("gridUnitProvenance"));
    validation = dialog.findChild<QLabel *>(QStringLiteral("gridValidation"));
    apply = dialog.findChild<QPushButton *>(QStringLiteral("gridApplyButton"));
    cancel = dialog.findChild<QPushButton *>(QStringLiteral("gridCancelButton"));
  }
  bool valid() const {
    return mode && spacing && subdivisions && density && unit && grid && axes &&
        provenance && validation && apply && cancel;
  }
  void chooseMode(Settings::SpacingMode value) const {
    mode->setCurrentIndex(mode->findData(static_cast<int>(value)));
  }
  void chooseUnit(Settings::DisplayUnit value) const {
    unit->setCurrentIndex(unit->findData(static_cast<int>(value)));
  }
};

// Supports the real action's exec() path and a modeless implementation without
// private access. The timer closes only the test-owned dialog under this window.
bool dialogExercise(MainWindow &window, const char *objectName,
    const std::function<void()> &trigger, const std::function<bool(QDialog &)> &exercise) {
  QEventLoop loop;
  QElapsedTimer elapsed;
  elapsed.start();
  bool done = false, seen = false, passed = false, driving = false;
  QTimer poll;
  poll.setInterval(10);
  QObject::connect(&poll, &QTimer::timeout, &window, [&] {
    if (done || driving) return;
    QDialog *dialog = nullptr;
    for (auto *candidate : window.findChildren<QDialog *>(QString::fromLatin1(objectName)))
      if (candidate->isVisible()) { dialog = candidate; break; }
    if (dialog) {
      driving = true;
      seen = true;
      QPointer<QDialog> guarded(dialog);
      passed = exercise(*dialog);
      if (guarded && guarded->isVisible()) guarded->reject();
      done = true;
      driving = false;
      loop.quit();
    } else if (elapsed.elapsed() > 1800) {
      // An unexpected modal owned by this test window must not strand CI.
      auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (modal && window.isAncestorOf(modal)) modal->reject();
      done = true;
      loop.quit();
    }
  });
  poll.start();
  trigger();
  if (!done) loop.exec();
  poll.stop();
  return check(seen && passed, objectName);
}

bool near(double actual, double expected) {
  return std::isfinite(actual) && std::isfinite(expected) &&
      std::abs(actual - expected) <= 1e-10 * std::max(1.0, std::abs(expected));
}

bool shownValue(const QLineEdit &edit, double expected) {
  bool ok = false;
  const double actual = QLocale::c().toDouble(edit.text(), &ok);
  return ok && near(actual, expected);
}

bool saveImage(QWidget &widget, const QString &directory, const QString &name) {
  return check(QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(name)),
               "retain actual window/dialog evidence");
}

bool closeDetails(QDialog &dialog) {
  auto *close = dialog.findChild<QPushButton *>(QStringLiteral("closeViewportDetailsButton"));
  if (!check(close && close->isVisible() && close->isEnabled(),
             "details expose the actual enabled native close control")) return false;
  close->click();
  return check(!dialog.isVisible(), "details close button dismisses the test-owned dialog");
}

QString evidenceDirectory(const QString &output, const QString &locale, const QString &theme) {
  return QDir(output).filePath(QStringLiteral("viewport-status-%1-%2").arg(locale, theme));
}

struct GridInk final {
  QRect roi;
  qsizetype majorOnly = 0;
  qsizetype subdivided = 0;

  double majorCoverage() const {
    return majorOnly / static_cast<double>(roi.width() * roi.height());
  }
  double subdividedCoverage() const {
    return subdivided / static_cast<double>(roi.width() * roi.height());
  }
};

qsizetype neutralAddedInk(const QImage &off, const QImage &on, const QRect &roi) {
  qsizetype count = 0;
  for (int y = roi.top(); y <= roi.bottom(); ++y) {
    for (int x = roi.left(); x <= roi.right(); ++x) {
      const QColor a = off.pixelColor(x, y);
      const QColor b = on.pixelColor(x, y);
      // Orange fixture points are deliberately excluded. Both default themes
      // use neutral grid lines; compare with grid-off pixels instead of naming
      // a private shader RGB or assuming a fixed background luminance.
      const int offChroma = std::max({a.red(), a.green(), a.blue()}) -
                            std::min({a.red(), a.green(), a.blue()});
      const int onChroma = std::max({b.red(), b.green(), b.blue()}) -
                           std::min({b.red(), b.green(), b.blue()});
      const int delta = std::max({std::abs(a.red() - b.red()),
          std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
      if (offChroma <= 28 && onChroma <= 28 && delta >= 6) ++count;
    }
  }
  return count;
}

std::optional<GridInk> actualGridInk(NativeViewport &viewport, WorkspaceDocument &document,
    const QString &source, double rawMajorSpacing, const QString &directory, const QString &name) {
  // Navigation is an intentional setup operation before the preservation
  // baseline. Axis view uses the same public path as the navigation gizmo.
  viewport.clearSelection();
  viewport.setAxisView(NavigationAxis::PositiveZ);
  dispatchFor(cameraSnapDurationMilliseconds + 40);
  if (!check(viewport.orthographicProjection(), "shader oracle uses the actual top orthographic view"))
    return std::nullopt;
  const Snapshot before(document, viewport);
  const Settings original = viewport.viewportGridSettings();
  const QByteArray sourceBytes = contents(source);
  QString error;
  const auto restore = qScopeGuard([&] { viewport.setViewportGridSettings(original, &error); });
  Settings requested = original;
  requested.spacingMode = Settings::SpacingMode::Fixed;
  requested.majorSpacing = rawMajorSpacing;
  requested.subdivisions = 1;
  requested.density = 1.0;
  requested.axesVisible = false;
  requested.gridVisible = false;
  const auto capture = [&](const Settings &settings, const QString &suffix) -> std::optional<QImage> {
    const quint64 serial = viewport.renderedFrameSerial();
    if (!check(viewport.setViewportGridSettings(settings, &error) &&
        waitUntil([&] { return viewport.renderedFrameSerial() > serial; }, 1000),
        "public grid controls schedule an actual changed OpenGL frame")) return std::nullopt;
    const QImage image = viewport.grabFramebuffer();
    if (!check(!image.isNull() && QDir().mkpath(directory) &&
        image.save(QDir(directory).filePath(name + suffix + QStringLiteral(".png"))),
        "retain actual grid-off/major/subdivided framebuffers")) return std::nullopt;
    return image;
  };
  const auto off = capture(requested, QStringLiteral("-grid-off"));
  if (!off) return std::nullopt;
  requested.gridVisible = true;
  const auto major = capture(requested, QStringLiteral("-fixed-n1"));
  if (!major) return std::nullopt;
  requested.subdivisions = 5;
  const auto subdivided = capture(requested, QStringLiteral("-fixed-n5"));
  if (!subdivided) return std::nullopt;
  if (!check(off->size() == major->size() && off->size() == subdivided->size() &&
      off->width() >= 200 && off->height() >= 200, "comparable real grid framebuffers have a body ROI"))
    return std::nullopt;
  GridInk ink;
  // Exclude the HUD, left tools, right navigation and bottom status. Geometry
  // colors are excluded by neutralAddedInk, not erased or replaced by a mock.
  ink.roi = QRect(qRound(off->width() * 0.18), qRound(off->height() * 0.25),
                  qRound(off->width() * 0.60), qRound(off->height() * 0.50));
  ink.majorOnly = neutralAddedInk(*off, *major, ink.roi);
  ink.subdivided = neutralAddedInk(*off, *subdivided, ink.roi);
  qInfo() << "VIEWPORT_STATUS_CONTROLS actual grid ink" << name << "ROI" << ink.roi
          << "raw major" << rawMajorSpacing << "N1/N5 pixels" << ink.majorOnly << ink.subdivided
          << "coverage" << ink.majorCoverage() << ink.subdividedCoverage();
  if (!check(ink.majorOnly > std::max(50, ink.roi.width() / 4) &&
      ink.subdivided > ink.majorOnly * 1.2,
      "actual fixed grid is visible and changing subdivisions creates additional neutral grid coverage"))
    return std::nullopt;
  if (!check(viewport.setViewportGridSettings(original, &error) &&
      viewport.viewportGridSettings().toJson() == original.toJson() &&
      before.sourceAndCameraUnchanged(document, viewport) && !sourceBytes.isEmpty() && contents(source) == sourceBytes,
      "actual shader exercise restores grid settings and preserves same-source poses/camera/bytes")) return std::nullopt;
  return ink;
}
} // namespace

bool runViewportStatusControlsSmokeTest(MainWindow &window) {
  auto *viewport = window.findChild<NativeViewport *>();
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *gridAction = window.findChild<QAction *>(QStringLiteral("viewportGridSettingsAction"));
  auto *detailsAction = window.findChild<QAction *>(QStringLiteral("viewportDetailsAction"));
  auto *status = window.findChild<QLabel *>(QStringLiteral("viewportStatusLabel"));
  auto *detailsButton = window.findChild<QToolButton *>(QStringLiteral("viewportDetailsButton"));
  const QString output = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR", QDir::tempPath());
  if (!check(viewport && document && gridAction && detailsAction && status && detailsButton &&
      QDir().mkpath(output), "public viewport/grid/details actions and native controls exist")) return false;
  QTemporaryDir temporary(QDir(output).filePath(QStringLiteral("viewport-status-owned-XXXXXX")));
  if (!check(temporary.isValid(), "owned bounded fixtures beside the CI evidence")) return false;
  const QString originalLanguage = AppLanguage::current();
  const QString theme = qApp->property("gswUiTheme").toString();
  QString error;
  const auto cleanup = qScopeGuard([&] {
    AppLanguage::apply(originalLanguage, false);
    document->createUntitled(temporary.path(), {}, &error);
  });
  QElapsedTimer total;
  total.start();

  // Closing a modeless WA_DeleteOnClose dialog only queues its destruction.
  // Reopen synchronously, before processing that DeferredDelete event, to
  // catch reuse of a closing dialog independently of platform/event timing.
  detailsAction->trigger();
  QPointer<QDialog> closing;
  for (auto *dialog : window.findChildren<QDialog *>(QStringLiteral("viewportDetailsDialog")))
    if (dialog->isVisible()) { closing = dialog; break; }
  if (!check(closing && closeDetails(*closing), "close details before deferred destruction")) return false;
  detailsAction->trigger();
  QPointer<QDialog> reopened;
  for (auto *dialog : window.findChildren<QDialog *>(QStringLiteral("viewportDetailsDialog")))
    if (dialog->isVisible()) { reopened = dialog; break; }
  if (!check(reopened && reopened != closing,
      "immediate details reopen must not reuse the pending-delete dialog")) return false;
  dispatchFor(20);
  if (!check(reopened && reopened->isVisible() &&
      reopened->findChild<QPlainTextEdit *>(QStringLiteral("viewportDetailsText")) &&
      closeDetails(*reopened), "reopened details survive deferred destruction and remain usable")) return false;

  const auto loadFixture = [&](const QString &name, const QByteArray &unit) {
    const QString path = QDir(temporary.path()).filePath(name + QStringLiteral(".ply"));
    const QString root = QDir(temporary.path()).filePath(name + QStringLiteral("-workspace"));
    if (!writeFixture(path, unit) || !QDir().mkpath(root) ||
        !document->createUntitled(root, {}, &error) ||
        !document->addScenePath(path, &error)) return QString{};
    if (!waitUntil([&] { return viewport->scenePath() == path && viewport->selectableModelAvailable() &&
                              viewport->sceneReferenceCoordinates().valid; })) return QString{};
    // Exercise non-identity source poses; presentation settings must not edit
    // these values. No raw-camera equality is assumed between different units.
    const float unitsPerMetre = unit == QByteArrayLiteral("mm") ? 1000.0F : 1.0F;
    if (!document->setSceneTransform(QVector3D(0.2F, -0.1F, 0.05F) * unitsPerMetre,
        QQuaternion::fromAxisAndAngle(0, 0, 1, 13), {1.125F, 1.125F, 1.125F}, &error)) return QString{};
    return path;
  };

  const QString unknownPath = loadFixture(QStringLiteral("unknown"), {});
  if (!check(!unknownPath.isEmpty() && !sceneMetresPerCoordinate(viewport->sceneReferenceCoordinates()),
      "undeclared coordinates remain unknown rather than becoming metres")) return false;
  const QByteArray unknownHash = sha256(unknownPath);
  const Snapshot unknownBefore(*document, *viewport);
  if (!dialogExercise(window, "viewportGridDialog", [&] { gridAction->trigger(); }, [&](QDialog &dialog) {
    GridControls controls(dialog);
    if (!check(controls.valid(), "real grid form controls exist")) return false;
    if (qEnvironmentVariableIsSet("GSW_UI_SURFACE_EVIDENCE")) {
      auto *actual = qobject_cast<ViewportGridDialog *>(&dialog);
      if (!check(actual && tests::captureUiFormMatrix(true, dialog, QStringLiteral("unknown-unit-grid-controls"),
          QSize(900, 600), QSize(640, 600), [&] {
        const auto committed = actual->settings().toJson();
        return QJsonObject{{"committed", committed.value_or(QJsonObject{})},
            {"committedValid", committed.has_value()},
            {"mode", QJsonValue::fromVariant(controls.mode->currentData())},
            {"spacing", controls.spacing->text()},
            {"subdivisions", QJsonValue::fromVariant(controls.subdivisions->currentData())},
            {"density", controls.density->value()},
            {"unit", QJsonValue::fromVariant(controls.unit->currentData())},
            {"gridVisible", controls.grid->isChecked()}, {"axesVisible", controls.axes->isChecked()},
            {"scenes", document->sceneCollectionJson()}};
      }) && unknownBefore.unchanged(*document, *viewport),
          "grid native matrix preserves uncommitted controls, source collection and camera")) return false;
    }
    auto *model = qobject_cast<QStandardItemModel *>(controls.unit->model());
    bool disabledMetric = model != nullptr;
    for (const auto unit : {Settings::DisplayUnit::Millimetres, Settings::DisplayUnit::Centimetres,
                           Settings::DisplayUnit::Metres}) {
      const int index = controls.unit->findData(static_cast<int>(unit));
      disabledMetric &= index >= 0 && model && model->item(index) && !model->item(index)->isEnabled();
    }
    if (!check(disabledMetric && controls.unit->currentData().toInt() == static_cast<int>(Settings::DisplayUnit::Scene) &&
        controls.unit->currentText().contains(QStringLiteral("u")) &&
        controls.provenance->text().contains(sceneLengthUnitDescription(viewport->sceneReferenceCoordinates())),
        "unknown-unit form shows raw u and disables every physical-unit conversion")) return false;
    controls.chooseMode(Settings::SpacingMode::Fixed);
    controls.spacing->setText(QStringLiteral("-1"));
    if (!check(!controls.apply->isEnabled() && !controls.validation->text().isEmpty() &&
        unknownBefore.unchanged(*document, *viewport), "invalid negative input disables accept without writing state")) return false;
    // Explicitly calling accept must not bypass the disabled button's check.
    dialog.accept();
    if (!check(dialog.isVisible() && unknownBefore.unchanged(*document, *viewport),
               "programmatic accept also refuses invalid spacing")) return false;
    controls.spacing->setText(QStringLiteral("1.75e-2"));
    controls.subdivisions->setCurrentIndex(controls.subdivisions->findData(5));
    controls.grid->setChecked(false);
    controls.axes->setChecked(false);
    if (!check(controls.apply->isEnabled(), "valid scientific notation can be edited without committing")) return false;
    controls.cancel->click();
    return true;
  }) || !check(unknownBefore.unchanged(*document, *viewport) && sha256(unknownPath) == unknownHash,
      "cancel preserves collection, grid field, source pose, camera and original bytes")) return false;
  if (!check(viewport->viewportDetailsText().contains(QStringLiteral(" u")),
             "unknown-unit viewport distances remain u")) return false;
  if (!dialogExercise(window, "viewportDetailsDialog", [&] { detailsButton->click(); }, [&](QDialog &dialog) {
    auto *plain = dialog.findChild<QPlainTextEdit *>(QStringLiteral("viewportDetailsText"));
    if (!check(detailsButton->isVisible() && plain && plain->isReadOnly() &&
        plain->toPlainText().contains(QStringLiteral(" u")), "native details button opens read-only raw-unit details")) return false;
    return closeDetails(dialog);
  }) || !check(unknownBefore.unchanged(*document, *viewport), "details button is presentation-only")) return false;

  Settings dormantPreference = viewport->viewportGridSettings();
  dormantPreference.displayUnit = Settings::DisplayUnit::Metres;
  const auto dormantJson = dormantPreference.toJson();
  if (!check(dormantJson && document->setViewportGridSettings(*dormantJson, &error) &&
      waitUntil([&] { return viewport->viewportGridSettings().toJson() == dormantJson; }),
      "restore a valid saved metric display preference before physical metadata is available")) return false;
  const Snapshot dormantBefore(*document, *viewport);
  if (!dialogExercise(window, "viewportGridDialog", [&] { gridAction->trigger(); }, [&](QDialog &dialog) {
    GridControls controls(dialog);
    if (!check(controls.valid() && controls.unit->currentText().contains(QStringLiteral("u")) &&
        controls.apply->isEnabled(), "unknown metadata displays u without fabricating a metre conversion")) return false;
    controls.apply->click();
    return true;
  }) || !check(dormantBefore.unchanged(*document, *viewport),
      "accepting an unchanged unknown-unit form preserves its dormant saved unit preference")) return false;

  const QString metricPath = loadFixture(QStringLiteral("metres"), QByteArrayLiteral("m"));
  const auto ratio = sceneMetresPerCoordinate(viewport->sceneReferenceCoordinates());
  if (!check(!metricPath.isEmpty() && ratio && near(*ratio, 1.0), "real declared-metre source metadata is admitted")) return false;
  const QByteArray metricHash = sha256(metricPath);
  const Settings beforeLargePreference = viewport->viewportGridSettings();
  const Snapshot beforeLarge(*document, *viewport);
  Settings tooLarge = beforeLargePreference;
  tooLarge.spacingMode = Settings::SpacingMode::Fixed;
  tooLarge.majorSpacing = std::numeric_limits<double>::max();
  const auto largeJson = tooLarge.toJson();
  if (!check(largeJson && !viewport->validateViewportGridSettingsForDisplay(tooLarge, &error) &&
      !error.isEmpty() && beforeLarge.unchanged(*document, *viewport),
      "explicit Apply preflight rejects a GPU-unrepresentable interval without modifying state")) return false;
  if (!check(document->setViewportGridSettings(*largeJson, &error) &&
      waitUntil([&] { return viewport->viewportGridSettings().toJson() == largeJson; }) &&
      viewport->viewportDetailsText().contains(translated("当前间距超出视口显示范围，请调整网格间距。")) &&
      beforeLarge.sourceAndCameraUnchanged(*document, *viewport),
      "project replay preserves valid raw preferences and exposes renderer limits instead of leaking old preferences")) return false;
  const auto normalJson = beforeLargePreference.toJson();
  if (!check(normalJson && document->setViewportGridSettings(*normalJson, &error) &&
      waitUntil([&] { return viewport->viewportGridSettings().toJson() == normalJson; }) &&
      beforeLarge.sourceAndCameraUnchanged(*document, *viewport),
      "restoring representable display settings leaves source poses and camera unchanged")) return false;
  const QString shaderEvidence = QDir(output).filePath(QStringLiteral("viewport-status-shader-") + theme);
  const auto metreInk = actualGridInk(*viewport, *document, metricPath, 1.25 / *ratio,
                                    shaderEvidence, QStringLiteral("metres"));
  if (!metreInk) return false;
  const Snapshot metricBefore(*document, *viewport);
  const QStringList locales{QStringLiteral("zh_CN"), QStringLiteral("en_US"), QStringLiteral("ja_JP")};
  if (!dialogExercise(window, "viewportGridDialog", [&] { gridAction->trigger(); }, [&](QDialog &dialog) {
    GridControls controls(dialog);
    if (!check(controls.valid(), "known-unit form controls exist")) return false;
    controls.chooseMode(Settings::SpacingMode::Automatic);
    controls.density->setValue(1.0);
    controls.chooseMode(Settings::SpacingMode::Fixed);
    controls.spacing->setText(QStringLiteral("1.25"));
    controls.subdivisions->setCurrentIndex(controls.subdivisions->findData(5));
    const auto unitOnlyCamera = Snapshot(*document, *viewport);
    controls.chooseUnit(Settings::DisplayUnit::Millimetres);
    if (!check(shownValue(*controls.spacing, 1.25 * *ratio / 0.001) &&
        unitOnlyCamera.unchanged(*document, *viewport), "display-only metre-to-mm conversion preserves raw project and camera")) return false;
    const QString numeric = controls.spacing->text();
    const auto beforeMode = controls.mode->currentData();
    const auto beforeUnit = controls.unit->currentData();
    const auto beforeSubdivisions = controls.subdivisions->currentData();
    for (const QString &locale : locales) {
      if (!check(AppLanguage::apply(locale, false), "switch language while grid edits remain uncommitted")) return false;
      dispatchFor(10);
      if (!check(dialog.windowTitle() == translated("网格设置") &&
          controls.apply->text() == translated("确定") && controls.cancel->text() == translated("取消") &&
          controls.grid->text() == translated("显示网格") && controls.axes->text() == translated("显示坐标轴") &&
          controls.mode->itemText(controls.mode->findData(static_cast<int>(Settings::SpacingMode::Fixed))) == translated("固定间距") &&
          controls.unit->currentText() == translated("毫米（mm）"), "grid captions immediately use the selected catalog")) return false;
      for (QWidget *field : QList<QWidget *>{controls.mode, controls.spacing, controls.subdivisions,
                                           controls.density, controls.unit}) {
        QLabel *caption = nullptr;
        for (auto *candidate : dialog.findChildren<QLabel *>())
          if (candidate->buddy() == field) { caption = candidate; break; }
        if (!check(caption && caption->isVisible() && !caption->text().isEmpty() &&
            caption->width() >= QFontMetrics(caption->font()).horizontalAdvance(caption->text()) &&
            caption->height() >= QFontMetrics(caption->font()).height(),
            "every translated form caption has real visible geometry, not a collapsed zero-width column")) return false;
      }
      if (!check(controls.spacing->text() == numeric && controls.mode->currentData() == beforeMode &&
          controls.unit->currentData() == beforeUnit && controls.subdivisions->currentData() == beforeSubdivisions &&
          near(controls.density->value(), 1.0) && controls.grid->isChecked() && controls.axes->isChecked() &&
          metricBefore.unchanged(*document, *viewport), "language refresh retains uncommitted values and all scene/camera state")) return false;
      if (!saveImage(dialog, evidenceDirectory(output, locale, theme), QStringLiteral("grid-dialog.png"))) return false;
    }
    controls.cancel->click();
    return true;
  }) || !check(metricBefore.unchanged(*document, *viewport), "cancel after language and unit changes commits nothing")) return false;

  if (!dialogExercise(window, "viewportGridDialog", [&] { gridAction->trigger(); }, [&](QDialog &dialog) {
    GridControls controls(dialog);
    if (!check(controls.valid(), "accepted form has real native controls")) return false;
    controls.chooseMode(Settings::SpacingMode::Automatic);
    controls.density->setValue(1.0);
    controls.chooseMode(Settings::SpacingMode::Fixed);
    controls.chooseUnit(Settings::DisplayUnit::Scene);
    controls.spacing->setText(QStringLiteral("1.25"));
    controls.subdivisions->setCurrentIndex(controls.subdivisions->findData(5));
    controls.chooseUnit(Settings::DisplayUnit::Centimetres);
    if (!check(shownValue(*controls.spacing, 1.25 * *ratio / 0.01), "known units convert to centimetres")) return false;
    controls.chooseUnit(Settings::DisplayUnit::Metres);
    if (!check(shownValue(*controls.spacing, 1.25 * *ratio) && controls.apply->isEnabled(),
               "known units convert to metres before explicit acceptance")) return false;
    controls.apply->click();
    return true;
  })) return false;
  const auto accepted = viewport->viewportGridSettings();
  const auto acceptedJson = accepted.toJson();
  if (!check(acceptedJson && accepted.spacingMode == Settings::SpacingMode::Fixed &&
      accepted.majorSpacing == 1.25 && accepted.subdivisions == 5 && accepted.density == 1.0 &&
      accepted.displayUnit == Settings::DisplayUnit::Metres && accepted.gridVisible && accepted.axesVisible &&
      document->sceneCollectionJson() == metricBefore.collection &&
      viewport->viewTarget() == metricBefore.target && viewport->viewDistance() == metricBefore.distance &&
      viewport->viewOrbitAngles() == metricBefore.orbit &&
      viewport->orthographicProjection() == metricBefore.orthographic &&
      document->viewportGridSettings() == *acceptedJson,
      "explicit accept stores raw fixed spacing/subdivisions/density without changing objects or camera")) return false;
  const auto recovery = QJsonDocument::fromJson(document->recoveryManifestJson()).object();
  if (!check(recovery.value(QStringLiteral("viewportGrid")) == *acceptedJson,
             "recovery manifest retains the exact accepted grid settings")) return false;
  const auto originalObjects = document->sceneObjects();
  const QString manifest = QDir(temporary.path()).filePath(QStringLiteral("viewport-grid.gsw.json"));
  if (!check(document->saveManifest(manifest, &error) && document->load(manifest, &error),
             "accepted grid settings save and reopen through the real workspace manifest")) return false;
  if (!check(waitUntil([&] { return viewport->selectableModelAvailable() &&
      viewport->viewportGridSettings().toJson() == acceptedJson; }) &&
      document->viewportGridSettings() == *acceptedJson && sameObjects(originalObjects, document->sceneObjects()) &&
      sha256(metricPath) == metricHash && sha256(unknownPath) == unknownHash,
      "reopen preserves grid settings and source-unit poses without rewriting PLY files")) return false;

  for (const QString &locale : locales) {
    if (!check(AppLanguage::apply(locale, false), "switch details and compact status language")) return false;
    if (!check(waitUntil([&] { return viewport->viewportPerformanceText() == translated("空闲"); }, 1100),
               "real displayed frames settle to idle before inspecting details")) return false;
    const Snapshot before(*document, *viewport);
    if (!check(status->isVisible() && detailsButton->isVisible() &&
        status->text().contains(translated("空闲")) &&
        viewport->viewportStatusText().contains(translated("空闲")),
        "compact native status exposes idle rather than stale live FPS")) return false;
    if (!dialogExercise(window, "viewportDetailsDialog", [&] { detailsAction->trigger(); }, [&](QDialog &dialog) {
      auto *plain = dialog.findChild<QPlainTextEdit *>(QStringLiteral("viewportDetailsText"));
      if (!check(plain && plain->isReadOnly(), "menu action opens native read-only details")) return false;
      const QString value = plain->toPlainText();
      if (!check(value == viewport->viewportDetailsText() &&
          value.contains(translated("网格和基准面仅用于显示参考，不代表重建精度。")) &&
          value.contains(translated("帧率只统计实际绘制并完成显示的视口帧；空闲时间不计入，也不是训练速度。")) &&
          value.contains(sceneLengthUnitDescription(viewport->sceneReferenceCoordinates())),
          "details use current language and actual source provenance, with no accuracy or training-speed claim")) return false;
      if (locale == locales.first()) {
        for (const QString &liveLocale : locales) {
          if (!check(AppLanguage::apply(liveLocale, false) &&
              plain->toPlainText() == viewport->viewportDetailsText() && plain->isReadOnly() &&
              before.unchanged(*document, *viewport),
              "an already-open details dialog refreshes language immediately without modifying state")) return false;
        }
        if (!check(AppLanguage::apply(locale, false) &&
            plain->toPlainText() == viewport->viewportDetailsText(),
            "restore the screenshot locale without rebuilding the details dialog")) return false;
      }
      const QString majorPrefix = translated("主网格间距：%1").arg(QString{});
      QString majorRow;
      for (const QString &row : value.split(QLatin1Char('\n')))
        if (row.startsWith(majorPrefix)) majorRow = row.mid(majorPrefix.size());
      const QRegularExpression numberAndMetres(QStringLiteral("^([0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)\\s+m$"));
      const auto match = numberAndMetres.match(majorRow.trimmed());
      if (!check(match.hasMatch() && near(match.captured(1).toDouble(), accepted.majorSpacing * *ratio),
                 "fixed grid details report the requested physical interval using actual reference-unit ratio")) return false;
      if (!saveImage(dialog, evidenceDirectory(output, locale, theme), QStringLiteral("details-dialog.png"))) return false;
      return closeDetails(dialog);
    }) || !check(before.unchanged(*document, *viewport), "details menu does not mutate project or camera")) return false;
    if (!saveImage(window, evidenceDirectory(output, locale, theme), QStringLiteral("full-window.png"))) return false;
  }

  // Bounded second known-unit source prevents an accidental hardcoded 1 m/u.
  // Compare its own reference ratio and source poses, never its raw view camera
  // with the previous metre source's numerically different display frame.
  const QString mmPath = loadFixture(QStringLiteral("millimetres"), QByteArrayLiteral("mm"));
  const auto mmRatio = sceneMetresPerCoordinate(viewport->sceneReferenceCoordinates());
  if (!check(!mmPath.isEmpty() && mmRatio && near(*mmRatio, 0.001),
             "declared millimetre metadata retains its distinct physical ratio")) return false;
  const QByteArray mmHash = sha256(mmPath);
  if (!check(AppLanguage::apply(originalLanguage, false),
             "equivalent-unit shader comparison uses the same UI language")) return false;
  const auto millimetreInk = actualGridInk(*viewport, *document, mmPath, 1.25 / *mmRatio,
                                         shaderEvidence, QStringLiteral("millimetres"));
  if (!millimetreInk) return false;
  qInfo() << "VIEWPORT_STATUS_CONTROLS equivalent physical grid coverage m/mm"
          << metreInk->majorCoverage() << millimetreInk->majorCoverage()
          << metreInk->subdividedCoverage() << millimetreInk->subdividedCoverage();
  if (!check(std::abs(metreInk->majorCoverage() - millimetreInk->majorCoverage()) <=
          metreInk->majorCoverage() * 0.10 &&
      std::abs(metreInk->subdividedCoverage() - millimetreInk->subdividedCoverage()) <=
          metreInk->subdividedCoverage() * 0.10,
      "physically equivalent m/mm sources and grid intervals have matching real grid coverage within ten percent")) return false;
  const Snapshot mmBefore(*document, *viewport);
  if (!dialogExercise(window, "viewportGridDialog", [&] { gridAction->trigger(); }, [&](QDialog &dialog) {
    GridControls controls(dialog);
    if (!check(controls.valid(), "millimetre form has native controls")) return false;
    controls.chooseMode(Settings::SpacingMode::Fixed);
    controls.chooseUnit(Settings::DisplayUnit::Scene);
    controls.spacing->setText(QStringLiteral("1.25"));
    controls.chooseUnit(Settings::DisplayUnit::Metres);
    if (!check(shownValue(*controls.spacing, 1.25 * *mmRatio) && mmBefore.unchanged(*document, *viewport),
               "millimetre-to-metre display conversion preserves the same-source raw state")) return false;
    controls.chooseUnit(Settings::DisplayUnit::Scene);
    if (!check(shownValue(*controls.spacing, 1.25), "unit roundtrip retains raw millimetre spacing")) return false;
    controls.cancel->click();
    return true;
  }) || !check(mmBefore.unchanged(*document, *viewport) && sha256(mmPath) == mmHash &&
      sha256(metricPath) == metricHash && sha256(unknownPath) == unknownHash,
      "all unit controls and cancellation preserve every original fixture byte and same-source state")) return false;
  qInfo() << "VIEWPORT_STATUS_CONTROLS PASS:" << theme
          << "unknown/m/mm; real actions/button; invalid/cancel; immediate 3 locales; save/reopen; read-only details; elapsed ms"
          << total.elapsed();
  return true;
}

} // namespace gsw
