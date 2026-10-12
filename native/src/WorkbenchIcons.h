#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QRectF>
#include <QString>

class QPainter;

namespace gsw {

enum class WorkbenchIconId {
  NewProject, OpenProject, Save, SaveAs, ImportMedia, ImportModel, ExportModel,
  Reconstruct, Train, Pause, Resume, Stop, GenerateMesh, Inspect,
  Move, Rotate, Scale, Transform, Global, Local, Locked, Unlocked,
  FitView, Camera, Orbit, Pan, Zoom, Projection, Points, Mesh, Gaussians,
  BoxSelect, Lasso, Brush, VisibleOnly, ClearSelection, InvertSelection,
  Delete, Undo, Redo, Crop, Refresh, Settings, Folder, File, Dataset, Scene,
  Tasks, Logs, Details, Copy, FloatPanel, Close, Video, Image, Home,
  Back, Forward, Add, Remove, Help, Info, Warning, Check, Globe,
  FullScreen, Restore, Count
};

// Shared SVG artwork is rendered at the requested logical size and device
// pixel ratio. Callers refresh the palette on appearance changes; drawing
// callers pass their actual foreground ink for hover/selected states.
[[nodiscard]] QIcon workbenchIcon(WorkbenchIconId id, const QPalette &palette,
                                  bool accent = false);
void paintWorkbenchIcon(QPainter &painter, WorkbenchIconId id,
                         const QRectF &rect, const QColor &ink);
[[nodiscard]] QString workbenchIconAssetName(WorkbenchIconId id);

} // namespace gsw
