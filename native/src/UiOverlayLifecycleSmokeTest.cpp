#include "UiOverlayLifecycleSmokeTest.h"
#include "NativeViewport.h"
#include "AppTheme.h"
#include "AppLanguage.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QOpenGLContext>
#include <QScopeGuard>
#include <QTimer>
#include <functional>

namespace gsw {
namespace {
bool present(NativeViewport &viewport, const std::function<void()> &action) {
  const auto before = viewport.renderedFrameSerial();
  quint64 presented = before;
  auto connection = QObject::connect(&viewport, &QOpenGLWidget::frameSwapped, &viewport,
      [&] { presented = viewport.renderedFrameSerial(); });
  const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
  action();
  QElapsedTimer limit; limit.start();
  while (presented <= before && limit.elapsed() < 3000) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
  }
  return presented > before;
}
}

bool runUiOverlayLifecycleSmokeTest(NativeViewport &viewport) {
  if (!viewport.isValid() || !viewport.context() ||
      !viewport.window()->windowHandle() || !viewport.window()->isVisible()) return false;
  const QString language = AppLanguage::current();
  const auto theme = AppTheme::currentTheme();
  const QSize windowSize = viewport.window()->size();
  const auto target = viewport.viewTarget();
  const auto orbit = viewport.viewOrbitAngles();
  const auto distance = viewport.viewDistance();
  const auto transform = viewport.modelTranslation();
  const auto rotation = viewport.modelRotation();
  const auto scale = viewport.modelScale();
  const auto oldGrid = viewport.viewportGridSettings();
  const auto cleanup = qScopeGuard([&] {
    viewport.setUiPaintObservationEnabled(false);
    AppLanguage::apply(language, false);
    AppTheme::applyTheme(*qApp, theme, false);
    viewport.window()->resize(windowSize);
  });
  viewport.setUiPaintObservationEnabled(true);
  if (!present(viewport, [&] { viewport.update(); })) return false;
  auto last = [&]() -> UiPaintSample {
    const auto &observation = viewport.uiPaintObservation();
    return observation.count ? observation.samples[observation.count - 1] : UiPaintSample{};
  };
  if (!last().cacheGeneration || !last().ownedCpuBytes) {
    qCritical() << "UI cache lifecycle: real navigation coverage path is not active";
    return false;
  }
  if (!present(viewport, [&] { viewport.update(); }) || !last().cacheHit) return false;
  for (int cycle = 0; cycle < 20; ++cycle) {
    auto generation = last().cacheGeneration;
    const UiTheme next = cycle % 2 ? theme : theme == UiTheme::Light ? UiTheme::Dark : UiTheme::Light;
    if (!present(viewport, [&] { AppTheme::applyTheme(*qApp, next, false); }) ||
        last().cacheGeneration <= generation) return false;
    generation = last().cacheGeneration;
    if (!present(viewport, [&] { viewport.window()->resize(windowSize + QSize(cycle % 2 ? 0 : 80, cycle % 2 ? 0 : 50)); }) ||
        last().cacheGeneration <= generation) return false;
    if (last().ownedCpuBytes > 8 * 1024 * 1024 || last().requestedGpuBytes > 8 * 1024 * 1024)
      return false;
  }
  for (const QString &nextLanguage : AppLanguage::supported()) {
    if (nextLanguage == AppLanguage::current()) continue;
    const auto generation = last().cacheGeneration;
    if (!present(viewport, [&] { AppLanguage::apply(nextLanguage, false); }) ||
        last().cacheGeneration <= generation) return false;
  }
  // Theme, language and owned-window layout refresh are presentation-only.
  const bool unchanged = target == viewport.viewTarget() && orbit == viewport.viewOrbitAngles() &&
      distance == viewport.viewDistance() && transform == viewport.modelTranslation() &&
      rotation == viewport.modelRotation() && scale == viewport.modelScale() &&
      oldGrid.toJson() == viewport.viewportGridSettings().toJson();
  const auto &observed = viewport.uiPaintObservation();
  if (observed.overflow || !unchanged) return false;
  qInfo() << "UI_OVERLAY_LIFECYCLE:20theme/resize cycles, all live languages, bounded native-DPR cache; real cross-monitor NOT_RUN"
          << "samples" << observed.count << "generation" << last().cacheGeneration;
  return true;
}
} // namespace gsw
