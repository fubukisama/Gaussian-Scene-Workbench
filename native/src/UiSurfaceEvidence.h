#pragma once

#include <QJsonArray>
#include <QString>

class QWidget;

namespace gsw {

// Optional remote Windows smoke adapter. The caller attests that this process
// opens only test-owned synthetic data. Requires GITHUB_ACTIONS=true, an explicit
// --smoke-test* argument and main's isolated smoke-settings directory. root must
// be absolute, absent, and have an existing non-link parent; sourceCommit is a
// full SHA-1. Never shows, raises, activates or closes a window. The application
// owns the adapter. A false result means no adapter was installed.
//
// Captures already exposed, active, unobscured owned windows after 80 ms without
// relevant layout/display events. Limits: 160 images, 512 observations, 256 font
// records per image, 16M physical pixels per image, and ten minutes. Missing or
// short-lived surfaces are reported, never represented as visually accepted.
// Images and ui-surfaces.json are evidence only; existing smoke assertions retain
// responsibility for requests, cancellation, input/output and lifecycle safety.
bool installUiSurfaceEvidence(const QString &root, const QString &sourceCommit);

// Read-only QWidget-default font facts, including the root and its visible
// descendants in the same window. A fixed mixed-script probe observes fallback
// glyph runs on each actual widget paint device. No title, widget text, paths or
// logs are read. This does not measure custom QPainter font roles or legibility.
QJsonArray uiSurfaceFontEvidence(QWidget *window);

} // namespace gsw
