#include "IconUiSmokeTest.h"

#include "AppLanguage.h"
#include "AppTheme.h"
#include "MainWindow.h"
#include "NativeViewport.h"
#include "TransformGizmo.h"
#include "WorkbenchIcons.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace gsw {
namespace {
void settle(const int milliseconds = 60) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
}

void click(QWidget &widget, const QPointF &position) {
  const QPointF global = widget.mapToGlobal(position.toPoint());
  QMouseEvent move(QEvent::MouseMove, position, global, Qt::NoButton,
                    Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&widget, &move);
  QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton,
                     Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&widget, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, position, global,
                       Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&widget, &release);
  QToolTip::hideText();
  settle();
}

QAction *scaleCommand(MainWindow &window, const int percentage) {
  auto *display = window.findChild<QMenu *>(QStringLiteral("displaySettingsMenu"));
  if (!display) return nullptr;
  for (auto *action : display->findChildren<QAction *>()) {
    if (action->isCheckable() && action->data().typeId() == QMetaType::Int &&
        action->data().toInt() == percentage) return action;
  }
  return nullptr;
}

bool writeFixture(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  QByteArray data = "ply\nformat ascii 1.0\nelement vertex 8\nproperty float x\n"
      "property float y\nproperty float z\nproperty uchar red\nproperty uchar green\n"
      "property uchar blue\nelement face 6\nproperty list uchar int vertex_indices\nend_header\n";
  for (int vertex = 0; vertex < 8; ++vertex) {
    data += QStringLiteral("%1 %2 %3 210 80 180\n")
        .arg(vertex & 1 ? 0.5 : -0.5).arg(vertex & 2 ? 0.5 : -0.5)
        .arg(vertex & 4 ? 0.5 : -0.5).toUtf8();
  }
  data += "4 0 1 3 2\n4 4 6 7 5\n4 0 4 5 1\n4 2 3 7 6\n4 0 2 6 4\n4 1 5 7 3\n";
  return file.write(data) == data.size();
}

QByteArray fileHash(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

qreal luminance(const QColor &color) {
  const auto linear = [](qreal value) {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
         0.0722 * linear(color.blueF());
}

bool hasVisibleInk(const QImage &image, const QColor &background) {
  qreal coverage = 0;
  qreal red = 0, green = 0, blue = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = image.pixelColor(x, y);
      if (color.alpha() < 64) continue;
      const qreal weight = color.alphaF();
      coverage += weight;
      red += color.redF() * weight; green += color.greenF() * weight;
      blue += color.blueF() * weight;
    }
  }
  // At 90%, a 1.5 px horizontal stroke can lie halfway between two raster
  // rows. Count alpha coverage rather than demanding an opaque center row.
  if (coverage < 6.0) return false;
  const QColor ink = QColor::fromRgbF(red / coverage, green / coverage,
                                      blue / coverage);
  const qreal first = luminance(ink), second = luminance(background);
  return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05) >= 3.0;
}

QRect physicalRect(const QRectF &logical, const NativeViewport &viewport,
                    const QImage &frame) {
  const qreal xScale = frame.width() / static_cast<qreal>(viewport.width());
  const qreal yScale = frame.height() / static_cast<qreal>(viewport.height());
  return QRect(QPoint(qFloor(logical.left() * xScale), qFloor(logical.top() * yScale)),
                QPoint(qCeil(logical.right() * xScale), qCeil(logical.bottom() * yScale)))
      .intersected(frame.rect());
}

bool selectedToolIsVisible(const QImage &button) {
  int bluePixels = 0, whitePixels = 0;
  for (int y = 0; y < button.height(); ++y) {
    for (int x = 0; x < button.width(); ++x) {
      const auto color = button.pixelColor(x, y);
      if (color.blue() > 130 && color.blue() > color.red() * 1.4 &&
          color.blue() > color.green() * 1.25) ++bluePixels;
      if (color.red() > 220 && color.green() > 220 && color.blue() > 220) ++whitePixels;
    }
  }
  return bluePixels > button.width() * button.height() / 4 && whitePixels >= 6;
}
} // namespace

bool runIconUiSmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](const bool condition, const char *message) {
    if (!condition) { qCritical() << "ICON_UI FAIL:" << message; passed = false; }
  };
  // This command is only valid in the application's isolated smoke-test launch.
  // Never borrow an interactive project or its preferences for visual QA.
  const QString isolatedSettings = qApp->property("gswSmokeSettingsDirectory").toString();
  auto *viewport = window.findChild<NativeViewport *>();
  check(QFileInfo(isolatedSettings).isDir(), "settings are isolated before the real window is constructed");
  check(viewport && viewport->scenePath().isEmpty() && viewport->sceneObjectCount() <= 1,
        "test owns an initially empty viewport");
  if (!QFileInfo(isolatedSettings).isDir() || !viewport || !viewport->scenePath().isEmpty() ||
      viewport->sceneObjectCount() > 1) return false;
  QString output = qEnvironmentVariable("GSW_SMOKE_OUTPUT_DIR");
  if (output.isEmpty()) output = QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("test-output/icon-ui"));
  check(QDir().mkpath(output), "snapshot directory is writable");
  if (!passed) return false;
  QTemporaryDir temporary(QDir(output).filePath(QStringLiteral("fixtures-XXXXXX")));
  const QString path = QDir(temporary.path()).filePath(QStringLiteral("icon-ui-cube.ply"));
  if (!temporary.isValid() || !writeFixture(path)) return false;
  const QByteArray originalHash = fileHash(path);
  const auto originalTheme = AppTheme::currentThemeMode();
  const int originalScale = qApp->property("gswUiScalePercent").toInt();
  const bool automaticScale = qApp->property("gswAutomaticUiScale").toBool();
  const bool originalLock = viewport->editToolsLocked();
  viewport->setScene(path, 0);
  QElapsedTimer load; load.start();
  while (!viewport->meshRenderingAvailable() && load.elapsed() < 15000) settle(20);
  check(viewport->meshRenderingAvailable(), "disposable cube reaches the real mesh renderer");
  if (!passed) return false;
  viewport->setRenderMode(NativeViewport::RenderMode::Mesh);
  viewport->setEditToolsLocked(false);
  viewport->setShowObservationTrackball(false);
  viewport->setModelTransform(QVector3D(0.15F, 0.10F, 0.20F),
      QQuaternion::fromEulerAngles(10.0F, 20.0F, 5.0F), QVector3D(1.1F, 0.95F, 1.2F));
  viewport->selectModel();
  viewport->resetCamera();
  const auto translation = viewport->modelTranslation();
  const auto rotation = viewport->modelRotation();
  const auto modelScale = viewport->modelScale();
  const auto unchanged = [&] {
    return viewport->modelTranslation() == translation && viewport->modelRotation() == rotation &&
           viewport->modelScale() == modelScale && viewport->scenePath() == path &&
           !viewport->modelTransformActive() && fileHash(path) == originalHash;
  };
  auto *light = window.findChild<QAction *>(QStringLiteral("lightThemeAction"));
  auto *dark = window.findChild<QAction *>(QStringLiteral("darkThemeAction"));
  check(light && dark, "real appearance commands are available");
  if (!light || !dark) return false;
  const QString locale = AppLanguage::current();
  const std::array<QIcon::Mode, 4> iconModes = {
      QIcon::Normal, QIcon::Disabled, QIcon::Normal, QIcon::Selected};
  const std::array<QIcon::State, 4> iconStates = {
      QIcon::Off, QIcon::Off, QIcon::On, QIcon::Off};
  const int iconCount = static_cast<int>(WorkbenchIconId::Count);
  for (auto *theme : {light, dark}) {
    theme->trigger();
    for (const int percentage : {90, 100, 150}) {
      auto *scale = scaleCommand(window, percentage);
      check(scale != nullptr, "real manual scale command is available");
      if (!scale) continue;
      scale->trigger(); settle();
      check(qApp->property("gswUiScalePercent").toInt() == percentage,
            "the actual application font and toolbar scale changed");
      check(window.isVisible() && !window.isMinimized() && viewport->isVisible(),
            "real window and viewport remain visible during framebuffer acceptance");
      if (!window.isVisible() || window.isMinimized() || !viewport->isVisible()) return false;
      const QString prefix = locale + QLatin1Char('-') + theme->data().toString() +
                             QLatin1Char('-') + QString::number(percentage);
      qInfo().noquote() << "ICON_UI scenario:" << prefix;
      const int edge = AppTheme::scaled(20, percentage);
      const int cell = edge + AppTheme::scaled(14, percentage);
      const int rows = (iconCount + 11) / 12;
      QImage atlas(12 * cell, rows * cell * 4, QImage::Format_ARGB32);
      atlas.fill(window.palette().color(QPalette::Window));
      QPainter atlasPainter(&atlas);
      for (int state = 0; state < 4; ++state) {
        for (int index = 0; index < iconCount; ++index) {
          const auto id = static_cast<WorkbenchIconId>(index);
          const auto icon = workbenchIcon(id, window.palette());
          const auto glyph = icon.pixmap(QSize(edge, edge), iconModes[state], iconStates[state]).toImage();
          check(!icon.isNull() && !glyph.isNull() &&
                hasVisibleInk(glyph, window.palette().color(QPalette::Window)),
                "every shared icon renders visible Normal, Disabled, On and Selected ink");
          const QPixmap highDpi = icon.pixmap(QSize(20, 20), 2.0,
                                               iconModes[state], iconStates[state]);
          check(highDpi.size() == QSize(40, 40) &&
                std::abs(highDpi.devicePixelRatio() - 2.0) < 0.01 &&
                hasVisibleInk(highDpi.toImage(), window.palette().color(QPalette::Window)),
                "DPR 2 requests yield physical 40 px / logical 20 px icons with visible ink");
          atlasPainter.drawImage(QPoint((index % 12) * cell + (cell - edge) / 2,
              (state * rows + index / 12) * cell + (cell - edge) / 2), glyph);
        }
      }
      atlasPainter.end();
      check(atlas.save(QDir(output).filePath(prefix + QStringLiteral("-icon-atlas.png"))),
            "all icon states are captured for review");

      int toolbarIcons = 0;
      for (auto *toolbar : window.findChildren<QToolBar *>()) {
        if (!toolbar->isVisibleTo(&window)) continue;
        check(toolbar->iconSize() == QSize(edge, edge), "real toolbar icon size follows UI scale");
        for (auto *button : toolbar->findChildren<QToolButton *>()) {
          if (!button->isVisibleTo(&window) || !button->defaultAction()) continue;
          auto *action = button->defaultAction();
          if (!action->property("gswWorkbenchIconId").isValid()) continue;
          ++toolbarIcons;
          check(!button->icon().isNull(), "visible toolbar action has its shared vector glyph");
          check(button->height() >= AppTheme::scaled(28, percentage),
                "actual toolbar keeps usable click targets at every scale");
        }
        check(toolbar->grab().save(QDir(output).filePath(prefix + QLatin1Char('-') +
            toolbar->objectName() + QStringLiteral(".png"))), "actual toolbar snapshot is saved");
      }
      check(toolbarIcons >= 12, "several workbench toolbars are visibly exercised");
      for (const auto &entry : {std::pair{"pointRenderAction", NativeViewport::RenderMode::Points},
                                std::pair{"meshRenderAction", NativeViewport::RenderMode::Mesh}}) {
        auto *action = window.findChild<QAction *>(QLatin1String(entry.first));
        QToolButton *button = nullptr;
        for (auto *candidate : window.findChildren<QToolButton *>())
          if (candidate->defaultAction() == action && candidate->isVisibleTo(&window)) { button = candidate; break; }
        check(action && action->isEnabled() && button, "actual render-mode toolbar button is clickable");
        if (action && action->isEnabled() && button) {
          click(*button, button->rect().center());
          check(action->isChecked() && viewport->renderMode() == entry.second,
                "clicking the real icon button changes the public render mode");
        }
      }
      auto rail = transformToolStripLayout(viewport->size(), QFontMetricsF(viewport->font()).height());
      const std::array<TransformGizmoMode, 4> tools = {TransformGizmoMode::Move,
          TransformGizmoMode::Rotate, TransformGizmoMode::Scale, TransformGizmoMode::Transform};
      for (int index = 0; index < 4; ++index) {
        click(*viewport, rail.buttons[index].center());
        check(viewport->modelGizmoMode() == tools[index] && viewport->modelSelected(),
              "clicking each drawn edit tool selects its public gizmo mode");
        check(unchanged(), "changing a toolbar tool preserves source data and existing model transforms");
        const QImage frame = viewport->grabFramebuffer();
        check(selectedToolIsVisible(frame.copy(physicalRect(rail.buttons[index], *viewport, frame))),
              "selected tool has a visible blue surface and light glyph in the real framebuffer");
      }
      const bool lockedLocal = viewport->modelGizmoUsesLocalOrientation();
      click(*viewport, rail.orientationButton.center());
      check(viewport->modelGizmoOrientationLocked() && viewport->modelGizmoUsesLocalOrientation() == lockedLocal,
            "locked coordinate icon cannot change combined-tool orientation");
      click(*viewport, rail.buttons[0].center());
      const bool local = viewport->modelGizmoUsesLocalOrientation();
      click(*viewport, rail.orientationButton.center());
      check(viewport->modelGizmoUsesLocalOrientation() != local,
            "coordinate icon toggles Global and Local for the move tool");
      click(*viewport, rail.orientationButton.center());
      check(viewport->modelGizmoUsesLocalOrientation() == local && unchanged(),
            "coordinate toggle returns without altering model or source");
      const QImage frame = viewport->grabFramebuffer();
      check(!frame.isNull(), "actual OpenGL framebuffer is available");
      check(frame.save(QDir(output).filePath(prefix + QStringLiteral("-viewport.png"))),
            "actual viewport snapshot is saved");
      check(frame.copy(physicalRect(rail.background, *viewport, frame)).save(
          QDir(output).filePath(prefix + QStringLiteral("-edit-rail.png"))),
          "actual edit rail snapshot is saved");
      check(window.grab().save(QDir(output).filePath(prefix + QStringLiteral("-window.png"))),
            "full real application snapshot is saved");
    }
  }
  if (auto *scale = scaleCommand(window, 150)) scale->trigger();
  const int originalMinimumHeight = viewport->minimumHeight();
  const int originalMaximumHeight = viewport->maximumHeight();
  viewport->setFixedHeight(180);
  settle();
  const auto shortRail = transformToolStripLayout(viewport->size(),
      QFontMetricsF(viewport->font()).height());
  const QRectF shortCanvas(QPointF(), viewport->size());
  check(shortCanvas.contains(shortRail.background) &&
        shortCanvas.contains(shortRail.orientationButton),
        "short viewport keeps its complete edit rail and coordinate control visible at 150 percent");
  const std::array<TransformGizmoMode, 4> shortTools = {TransformGizmoMode::Move,
      TransformGizmoMode::Rotate, TransformGizmoMode::Scale, TransformGizmoMode::Transform};
  for (int index = 0; index < 4; ++index) {
    check(shortCanvas.contains(shortRail.buttons[index]),
          "short viewport contains every drawn edit-tool click target");
    click(*viewport, shortRail.buttons[index].center());
    check(viewport->modelGizmoMode() == shortTools[index] && unchanged(),
          "every short-viewport tool remains clickable without altering the model");
  }
  click(*viewport, shortRail.buttons[0].center());
  const bool shortLocal = viewport->modelGizmoUsesLocalOrientation();
  click(*viewport, shortRail.orientationButton.center());
  check(viewport->modelGizmoUsesLocalOrientation() != shortLocal && unchanged(),
        "short-viewport coordinate control remains clickable");
  click(*viewport, shortRail.orientationButton.center());
  const QImage shortFrame = viewport->grabFramebuffer();
  check(shortFrame.save(QDir(output).filePath(locale + QStringLiteral("-dark-150-short-viewport.png"))) &&
        shortFrame.copy(physicalRect(shortRail.background, *viewport, shortFrame)).save(
            QDir(output).filePath(locale + QStringLiteral("-dark-150-short-rail.png"))),
        "short-viewport framebuffer and edit-rail evidence are saved");
  viewport->setMinimumHeight(originalMinimumHeight);
  viewport->setMaximumHeight(originalMaximumHeight);
  settle();
  viewport->setEditToolsLocked(originalLock);
  if (automaticScale) {
    if (auto *automatic = window.findChild<QAction *>(QStringLiteral("autoUiScaleAction"))) automatic->trigger();
  } else if (auto *scale = scaleCommand(window, originalScale)) scale->trigger();
  if (auto *theme = window.findChild<QAction *>(originalTheme == UiThemeMode::Automatic
          ? QStringLiteral("automaticThemeAction") : originalTheme == UiThemeMode::Light
          ? QStringLiteral("lightThemeAction") : QStringLiteral("darkThemeAction"))) theme->trigger();
  check(fileHash(path) == originalHash, "test fixture source is unchanged after the complete matrix");
  qInfo().noquote() << "ICON_UI smoke:" << locale << (passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw
