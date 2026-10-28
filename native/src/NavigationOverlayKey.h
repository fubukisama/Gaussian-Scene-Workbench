#pragma once

#include "BlenderNavigationPresentation.h"
#include <QByteArray>
#include <QDataStream>
#include <QIODevice>

namespace gsw {

// Exact display-input identity, not a rounded camera hash. Keep every semantic
// draw input, including instant language changes, in this one-entry cache key.
inline QByteArray navigationOverlayKey(const NavigationGizmoLayout &layout,
    const BlenderNavigationPresentationState &state, const QString &language) {
  QByteArray key;
  QDataStream out(&key, QIODevice::WriteOnly);
  out.setVersion(QDataStream::Qt_6_8);
  out << language << layout.center << layout.radius << layout.lineWidth
      << layout.rotateBounds << layout.zoomButton << layout.panButton
      << layout.cameraButton << layout.projectionButton << layout.projectionLabel;
  for (const auto &handle : layout.handles)
    out << int(handle.axis) << handle.center << handle.radius << handle.depth
        << handle.axisIndex << handle.positive << handle.hidden << handle.aligned;
  out << int(state.hover.part) << int(state.hover.axis)
      << int(state.press.part) << int(state.press.axis)
      << state.interactionActive << state.light << state.orthographic
      << state.cameraActive << state.cameraAvailable << state.backdrop;
  return key;
}

inline QRectF navigationOverlayBounds(const NavigationGizmoLayout &layout) {
  return layout.rotateBounds.united(layout.zoomButton).united(layout.panButton)
      .united(layout.cameraButton).united(layout.projectionButton)
      .united(layout.projectionLabel).adjusted(-4, -4, 4, 4);
}

} // namespace gsw
