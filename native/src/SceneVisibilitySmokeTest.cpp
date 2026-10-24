#include "SceneVisibilitySmokeTest.h"
#include "AppLanguage.h"
#include "AppTheme.h"
#include "MainWindow.h"
#include "NativeViewport.h"
#include "WorkspaceDocument.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <array>
#include <algorithm>
#include <functional>

namespace gsw {
namespace {
bool waitUntil(const std::function<bool()> &predicate, int timeout = 7000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeout) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return predicate();
}

struct Patch {
  int count = 0;
  QPointF center;
};

std::array<Patch, 4> patches(const QImage &image) {
  std::array<Patch, 4> result;
  // Only the scene body: exclude badges, axes, toolbar and lower caption.
  for (int y = 110; y < image.height() - 120; ++y) {
    for (int x = 20; x < image.width() - 20; ++x) {
      const QColor color = image.pixelColor(x, y);
      const int r = color.red(), g = color.green(), b = color.blue();
      const std::array<bool, 4> matches{
          r > 75 && r > g * 3 && r > b * 3,
          g > 75 && g > r * 3 && g > b * 3,
          b > 75 && b > r * 3 && b > g * 3,
          r > 75 && b > 75 && r > g * 3 && b > g * 3};
      for (int kind = 0; kind < 4; ++kind) {
        if (matches[kind]) {
          ++result[kind].count;
          result[kind].center += QPointF(x, y);
        }
      }
    }
  }
  for (auto &patch : result) if (patch.count) patch.center /= patch.count;
  return result;
}

QByteArray sha256(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return hash.result();
}

QTreeWidgetItem *sceneItem(QTreeWidget *tree, const QString &id) {
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, Qt::UserRole + 2).toString() == id) return *it;
  return nullptr;
}

bool fixture(const QString &path, int kind) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const bool gaussian = kind >= 2;
  const bool mesh = kind == 1;
  QByteArray ply = "ply\nformat ascii 1.0\nelement vertex ";
  ply += mesh ? "4\n" : "2401\n";
  ply += "property float x\nproperty float y\nproperty float z\n";
  if (gaussian) {
    ply += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\n";
    if (kind == 2) ply += "property float scale_2\n";
    ply += "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
  } else {
    ply += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  }
  if (mesh) ply += "element face 2\nproperty list uchar int vertex_indices\n";
  ply += "end_header\n";
  const float center = -4.5F + kind * 3.0F;
  const auto vertex = [&](float x, float y) {
    QByteArray data = QString("%1 %2 0 ").arg(center + x, 0, 'f', 4).arg(y, 0, 'f', 4).toUtf8();
    if (kind == 0) data += "240 12 12\n";
    else if (kind == 1) data += "12 240 12\n";
    else if (kind == 2) data += "-1.5 -1.5 1.5 3 -3.3 -3.3 -3.3 1 0 0 0\n";
    else data += "1.5 -1.5 1.5 3 -3.3 -3.3 1 0 0 0\n";
    return data;
  };
  if (mesh) {
    for (const auto &point : std::array<QPointF, 4>{QPointF(-0.75, -0.75), QPointF(0.75, -0.75), QPointF(-0.75, 0.75), QPointF(0.75, 0.75)})
      ply += vertex(static_cast<float>(point.x()), static_cast<float>(point.y()));
    ply += "3 0 1 2\n3 2 1 3\n";
  } else {
    for (int y = -24; y <= 24; ++y)
      for (int x = -24; x <= 24; ++x) ply += vertex(x / 32.0F, y / 32.0F);
  }
  return file.write(ply) == ply.size();
}
}

bool runSceneVisibilitySmokeTest(MainWindow &window) {
  auto *viewport = window.findChild<NativeViewport *>();
  auto *document = window.findChild<WorkspaceDocument *>();
  auto *tree = window.findChild<QTreeWidget *>("projectTree");
  QTemporaryDir temporary;
  const auto check = [](bool success, const char *message) {
    if (!success) qCritical() << "SCENE_VISIBILITY FAIL:" << message;
    return success;
  };
  if (!check(viewport && document && tree && temporary.isValid(), "window services")) return false;
  window.resize(1440, 900);
  QString error;
  const QString working = QDir(temporary.path()).filePath("working");
  if (!QDir().mkpath(working) || !document->createUntitled(working, "Visibility smoke", &error)) return false;
  auto *lockTools = window.findChild<QAction *>("lockEditToolsAction");
  if (!check(lockTools, "edit lock action")) return false;
  lockTools->setChecked(false);
  QStringList ids, paths;
  QList<QByteArray> hashes;
  for (int kind = 0; kind < 4; ++kind) {
    const QString path = QDir(temporary.path()).filePath(QString("visibility-%1.ply").arg(kind));
    if (!check(fixture(path, kind) && document->addScenePath(path, &error), "import fixture")) return false;
    ids.append(document->activeSceneId());
    paths.append(path);
    hashes.append(sha256(path));
    if (!check(waitUntil([&] {
          return kind == 1 ? viewport->meshRenderingAvailable()
              : kind >= 2 ? viewport->gaussianRenderingAvailable() : viewport->selectableModelAvailable();
        }), "point, mesh, 3DGS and 2DGS resources loaded")) return false;
    if (kind >= 2 && !check(viewport->sourceHasSurfelAttributes() == (kind == 3), "2DGS is a real two-scale surfel fixture")) return false;
  }
  if (!check(viewport->sceneObjectCount() == 4 && viewport->visibleSceneObjectCount() == 4, "all imported objects visible by default")) return false;
  viewport->setAxisView(NavigationAxis::PositiveZ);
  waitUntil([&] { return false; }, 400);
  viewport->selectAllModels();
  if (!check(viewport->focusModel(), "fit imported models")) return false;
  viewport->clearSelection();
  QImage frame;
  std::array<Patch, 4> baseline;
  if (!check(waitUntil([&] {
        frame = viewport->grabFramebuffer();
        baseline = patches(frame);
        return std::all_of(baseline.begin(), baseline.end(), [](const Patch &patch) { return patch.count > 120; });
      }), "point, mesh, 3DGS and 2DGS simultaneously rendered")) {
    qInfo() << "SCENE_VISIBILITY pixels" << baseline[0].count << baseline[1].count << baseline[2].count << baseline[3].count << frame.size();
    return false;
  }
  const QString evidence = QDir(qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR", QDir::tempPath())).filePath(
      "scene-visibility-" + AppLanguage::current() + (AppTheme::currentTheme() == UiTheme::Light ? "-light" : "-dark"));
  QDir().mkpath(evidence);
  frame.save(QDir(evidence).filePath("all-visible.png"));
  const auto initialObjects = document->sceneObjects();
  const auto target = viewport->viewTarget();
  const float distance = viewport->viewDistance();
  const auto angles = viewport->viewOrbitAngles();
  const QString active = document->activeSceneId();
  const auto selectedTreeIds = [&] {
    QSet<QString> selected;
    for (const auto *item : tree->selectedItems()) {
      const QString id = item->data(0, Qt::UserRole + 2).toString();
      if (!id.isEmpty()) selected.insert(id);
    }
    return selected;
  };
  for (int kind = 0; kind < 4; ++kind) {
    auto *item = sceneItem(tree, ids[kind]);
    if (!check(item && item->checkState(1) == Qt::Checked, "visible eye cell state")) return false;
    tree->scrollToItem(item);
    const QPoint point(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2,
                       tree->visualItemRect(item).center().y());
    const auto selectedBeforeEye = selectedTreeIds();
    const auto eyeClick = [&] {
      QMouseEvent down(QEvent::MouseButtonPress, point, tree->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QMouseEvent up(QEvent::MouseButtonRelease, point, tree->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(tree->viewport(), &down);
      QApplication::sendEvent(tree->viewport(), &up);
    };
    eyeClick();
    if (!check(waitUntil([&] {
          frame = viewport->grabFramebuffer();
          return !viewport->sceneObjectVisible(ids[kind]) && patches(frame)[kind].count < baseline[kind].count / 8;
        }), "eye click hides the actual rendered model")) return false;
    if (!check(document->activeSceneId() == active && viewport->viewTarget() == target &&
               viewport->viewDistance() == distance && viewport->viewOrbitAngles() == angles &&
               viewport->selectedSceneIds().isEmpty() && selectedTreeIds() == selectedBeforeEye,
               "eye click leaves active model, camera and exact tree selection unchanged")) return false;
    lockTools->setChecked(true);
    const auto beforeMiss = viewport->viewTarget();
    if (!check(!viewport->centerObservationAt(baseline[kind].center / viewport->devicePixelRatioF()) &&
               viewport->viewTarget() == beforeMiss, "hidden geometry cannot be picked for observation")) return false;
    lockTools->setChecked(false);
    frame.save(QDir(evidence).filePath(QString("hidden-%1.png").arg(kind)));
    item = sceneItem(tree, ids[kind]);
    if (!check(item && item->checkState(1) == Qt::Unchecked, "hidden eye cell state")) return false;
    // Refreshing the tree may replace item pointers; use the actual new cell.
    const QPoint restoredPoint(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2,
                              tree->visualItemRect(item).center().y());
    QMouseEvent restoreDown(QEvent::MouseButtonPress, restoredPoint, tree->viewport()->mapToGlobal(restoredPoint), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent restoreUp(QEvent::MouseButtonRelease, restoredPoint, tree->viewport()->mapToGlobal(restoredPoint), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &restoreDown);
    QApplication::sendEvent(tree->viewport(), &restoreUp);
    if (!check(waitUntil([&] { return viewport->sceneObjectVisible(ids[kind]) && patches(viewport->grabFramebuffer())[kind].count > baseline[kind].count / 2; }), "eye click restores model without re-import")) return false;
    if (!check(selectedTreeIds() == selectedBeforeEye, "restoring an eye retains exact selected tree rows")) return false;
  }
  auto *hide = window.findChild<QAction *>("hideSelectedModelsAction");
  auto *show = window.findChild<QAction *>("showSelectedModelsAction");
  auto *isolate = window.findChild<QAction *>("isolateSelectedModelsAction");
  auto *showAll = window.findChild<QAction *>("showAllModelsAction");
  if (!check(hide && show && isolate && showAll, "batch visibility actions")) return false;
  const auto selectRows = [&](const QStringList &selected) {
    tree->clearSelection();
    for (const QString &id : selected) if (auto *item = sceneItem(tree, id)) item->setSelected(true);
  };
  selectRows({ids[0], ids[2]});
  hide->trigger();
  const QSet<QString> firstPair{ids[0], ids[2]};
  if (!check(viewport->visibleSceneObjectCount() == 2 && !viewport->sceneObjectVisible(ids[0]) && !viewport->sceneObjectVisible(ids[2]) &&
             selectedTreeIds() == firstPair, "batch hide preserves exact hidden selected rows")) return false;
  show->trigger();
  if (!check(viewport->visibleSceneObjectCount() == 4 && selectedTreeIds() == firstPair,
             "batch show includes and preserves exact selected hidden rows")) return false;
  if (!check(document->setSceneObjectVisibility({ids[0]}, false) && selectedTreeIds() == firstPair,
             "mixed visible/hidden keyboard selection")) return false;
  const auto pressSpace = [&] {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ");
    QApplication::sendEvent(tree, &event);
  };
  pressSpace();
  if (!check(viewport->visibleSceneObjectCount() == 2 && !viewport->sceneObjectVisible(ids[0]) &&
             !viewport->sceneObjectVisible(ids[2]) && selectedTreeIds() == firstPair,
             "Space hides a mixed selection and preserves the exact hidden rows")) return false;
  pressSpace();
  if (!check(viewport->visibleSceneObjectCount() == 4 && selectedTreeIds() == firstPair,
             "Space reveals an all-hidden selection without selecting other rows")) return false;
  selectRows({ids[1], ids[3]});
  isolate->trigger();
  if (!check(viewport->visibleSceneObjectCount() == 2 && viewport->sceneObjectVisible(ids[1]) && viewport->sceneObjectVisible(ids[3]) &&
             selectedTreeIds() == QSet<QString>{ids[1], ids[3]}, "isolate preserves exact selected model rows")) return false;
  const auto visibilityState = document->sceneCollectionJson();
  for (const QString &language : AppLanguage::supported()) {
    if (!check(AppLanguage::apply(language, false), "live language switch")) return false;
    if (!check(document->sceneCollectionJson() == visibilityState && viewport->visibleSceneObjectCount() == 2 &&
               show->text() == QCoreApplication::translate("Workbench", "显示所选模型") &&
               hide->text() == QCoreApplication::translate("Workbench", "隐藏所选模型") &&
               isolate->text() == QCoreApplication::translate("Workbench", "仅显示所选模型") &&
               showAll->text() == QCoreApplication::translate("Workbench", "显示全部模型"), "language switch preserves visibility and updates exact action labels")) return false;
    window.grab().save(QDir(evidence).filePath("window-" + language + ".png"));
  }
  const UiTheme originalTheme = AppTheme::currentTheme();
  for (const UiTheme theme : {UiTheme::Light, UiTheme::Dark}) {
    AppTheme::applyTheme(*qApp, theme, false);
    if (!check(document->sceneCollectionJson() == visibilityState && viewport->visibleSceneObjectCount() == 2,
               "live theme switch preserves hidden states and sources")) return false;
    for (int kind = 0; kind < 4; ++kind) {
      auto *item = sceneItem(tree, ids[kind]);
      if (!check(item && item->checkState(1) == (kind == 1 || kind == 3 ? Qt::Checked : Qt::Unchecked),
                 "eye cells retain their state in both themes")) return false;
    }
  }
  AppTheme::applyTheme(*qApp, originalTheme, false);
  if (!check(document->setSceneObjectVisibility(ids, false) && viewport->visibleSceneObjectCount() == 0,
             "all models can be hidden without removing any object")) return false;
  viewport->selectAllModels();
  viewport->selectModelForMove();
  if (!check(viewport->selectedModelCount() == 0 && !viewport->focusModel() &&
             !viewport->modelTransformActive() && !viewport->hasEditableScene(),
             "an all-hidden collection cannot be selected, framed or edited")) return false;
  if (!check(waitUntil([&] {
        frame = viewport->grabFramebuffer();
        const auto hidden = patches(frame);
        for (int kind = 0; kind < 4; ++kind)
          if (hidden[kind].count >= baseline[kind].count / 8) return false;
        return true;
      }), "all-hidden framebuffer contains no imported geometry")) return false;
  frame.save(QDir(evidence).filePath("all-hidden.png"));
  if (!check(document->isolateSceneObjects({ids[1], ids[3]}), "restore isolated visibility before saving")) return false;
  const QString saved = QDir(temporary.path()).filePath("visibility.gsw.json");
  if (!check(document->saveManifest(saved, &error) && document->load(saved, &error), "save and reopen visibility")) return false;
  if (!check(document->sceneObjects().size() == 4 && viewport->visibleSceneObjectCount() == 2 &&
             !viewport->sceneObjectVisible(ids[0]) && viewport->sceneObjectVisible(ids[1]) &&
             !viewport->sceneObjectVisible(ids[2]) && viewport->sceneObjectVisible(ids[3]), "hidden state persists across reopen")) return false;
  showAll->trigger();
  if (!check(viewport->visibleSceneObjectCount() == 4, "show all restores persisted hidden models")) return false;
  const auto restoredObjects = document->sceneObjects();
  for (int kind = 0; kind < 4; ++kind) {
    const auto &before = initialObjects[kind];
    const auto &after = restoredObjects[kind];
    if (!check(before.id == after.id && before.path == after.path && before.translation == after.translation &&
               before.rotation == after.rotation && before.scale == after.scale && before.vertexCount == after.vertexCount &&
               hashes[kind] == sha256(paths[kind]), "visibility never rewrites sources or model transforms")) return false;
  }
  document->createUntitled(temporary.path(), "Empty", &error);
  qInfo() << "SCENE_VISIBILITY PASS: real point/mesh/3DGS/2DGS render suppression, eye picking isolation, batch hide/show/isolate, live trilingual labels, save/reopen, unchanged sources and transforms";
  return true;
}
}
