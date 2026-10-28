#include "WorkbenchIcons.h"

#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>

#include <array>
#include <memory>

namespace gsw {
namespace {
constexpr std::array<const char *, static_cast<std::size_t>(WorkbenchIconId::Count)> assets = {
  "file-plus-corner", "folder-open", "save", "save-pen", "images", "import", "file-output",
  "scan", "play", "pause", "step-forward", "square", "box", "mouse-pointer-2",
  "move-3d", "rotate-3d", "scaling", "axis-3d", "globe", "box", "lock", "lock-open",
  "focus", "camera", "orbit", "hand", "search", "view", "chart-scatter", "cuboid", "circle-dot",
  "square-dashed-mouse-pointer", "lasso-select", "paintbrush", "eye", "square-dashed", "replace",
  "trash", "undo-2", "redo-2", "crop", "refresh-cw", "sliders-horizontal", "folder", "file", "images", "layers",
  "list-checks", "scroll-text", "info", "copy", "panels-top-left", "x", "video", "image", "house",
  "chevron-left", "chevron-right", "plus", "minus", "circle-question-mark", "info", "triangle-alert", "check", "globe",
  "maximize-2", "minimize-2"
};

std::shared_ptr<QSvgRenderer> rendererFor(const WorkbenchIconId id, const QColor &ink) {
  const auto index = static_cast<std::size_t>(id);
  if (index >= assets.size()) return {};
  const quint64 key = (static_cast<quint64>(index) << 32) | ink.rgba();
  // All icon requests originate on the UI thread. Cached, colour-resolved SVG
  // renderers keep viewport redraws free of XML parsing or filesystem reads.
  static QHash<quint64, std::shared_ptr<QSvgRenderer>> renderers;
  if (auto found = renderers.constFind(key); found != renderers.cend()) return found.value();
  QFile file(QStringLiteral(":/icons/lucide/%1.svg").arg(QString::fromLatin1(assets[index])));
  if (!file.open(QIODevice::ReadOnly)) return {};
  QByteArray svg = file.readAll();
  svg.replace("currentColor", ink.name(QColor::HexRgb).toLatin1());
  // Lucide uses 2 units at a 24-unit viewbox; at our 20 px default this gives
  // a consistent, readable 1.67 px stroke without altering vendored artwork.
  auto renderer = std::make_shared<QSvgRenderer>(svg);
  if (!renderer->isValid()) return {};
  renderers.insert(key, renderer);
  return renderer;
}

class WorkbenchIconEngine final : public QIconEngine {
public:
  WorkbenchIconEngine(WorkbenchIconId id, const QPalette &palette, bool accent,
                      WorkbenchIconSurface surface)
      : mId(id), mPalette(palette), mAccent(accent), mSurface(surface) {}
  QIconEngine *clone() const override { return new WorkbenchIconEngine(mId, mPalette, mAccent, mSurface); }
  QString key() const override { return QStringLiteral("gsw-lucide"); }
  QString iconName() override { return workbenchIconAssetName(mId); }
  bool isNull() override { return !rendererFor(mId, mPalette.color(QPalette::WindowText)); }
  QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }
  void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
    paintWorkbenchIcon(*painter, mId, rect,
                       workbenchIconInk(mPalette, mode, state, mAccent, mSurface));
  }
  QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
    return scaledPixmap(size, mode, state, 1.0);
  }
  QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
    QPixmap image(QSize(qMax(1, qRound(size.width() * scale)), qMax(1, qRound(size.height() * scale))));
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paint(&painter, QRect(QPoint(), size), mode, state);
    return image;
  }
private:
  WorkbenchIconId mId;
  QPalette mPalette;
  bool mAccent;
  WorkbenchIconSurface mSurface;
};
} // namespace

QString workbenchIconAssetName(const WorkbenchIconId id) {
  const auto index = static_cast<std::size_t>(id);
  return index < assets.size() ? QString::fromLatin1(assets[index]) : QString();
}

QColor workbenchIconInk(const QPalette &palette, const QIcon::Mode mode,
                        const QIcon::State state, const bool accent,
                        const WorkbenchIconSurface surface) {
  if (mode == QIcon::Disabled)
    return palette.color(QPalette::Disabled, QPalette::WindowText);
  if (surface == WorkbenchIconSurface::OnAccent)
    return palette.color(QPalette::HighlightedText);
  // Selected rows/menus remain tinted with ordinary readable ink. Checked
  // controls use the palette's readable action ink, never blanket white.
  if (mode == QIcon::Selected) return palette.color(QPalette::WindowText);
  if (state == QIcon::On || accent) return palette.color(QPalette::Link);
  return palette.color(QPalette::WindowText);
}

QIcon workbenchIcon(const WorkbenchIconId id, const QPalette &palette, const bool accent,
                    const WorkbenchIconSurface surface) {
  return QIcon(new WorkbenchIconEngine(id, palette, accent, surface));
}

void paintWorkbenchIcon(QPainter &painter, const WorkbenchIconId id,
                         const QRectF &rect, const QColor &ink) {
  const auto renderer = rendererFor(id, ink);
  if (!renderer || rect.isEmpty()) return;
  const qreal edge = qMin(rect.width(), rect.height());
  const QRectF square(rect.center() - QPointF(edge * 0.5, edge * 0.5), QSizeF(edge, edge));
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setOpacity(painter.opacity() * ink.alphaF());
  renderer->render(&painter, square);
  painter.restore();
}
} // namespace gsw
