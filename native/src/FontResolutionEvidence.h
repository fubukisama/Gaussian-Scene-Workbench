#pragma once

#include <QFont>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPaintDevice>

namespace gsw {

// Read-only facts, not a font substitution or a legibility verdict. In the
// pinned Windows Qt6.8.3, multi-family exactMatch queries crashed in six real
// fault contexts. Keep the requested fallback list and actual rendering; do
// not call that optional predicate or invent a successful boolean in its place.
// Actual family, size, weight and separately measured glyph runs remain useful.
inline QJsonObject fontResolutionEvidence(const QFont &requested,
                                          const QPaintDevice *device = nullptr) {
  const QFont bound = device ? QFont(requested, device) : requested;
  const QFontInfo actual(bound);
  QJsonObject result{
      {"requestedFamily", requested.family()},
      {"requestedFamilies", QJsonArray::fromStringList(requested.families())},
      {"requestedPointSize", requested.pointSizeF()},
      {"requestedPixelSize", requested.pixelSize()},
      {"resolvedFamily", actual.family()}, {"pointSize", actual.pointSizeF()},
      {"pixelSize", actual.pixelSize()}, {"weight", int(actual.weight())},
      {"exactMatch", QJsonValue()},
      {"exactMatchQuery", "not_queried_qt683_multifamily_crash"}};
  if (device)
    result.insert("deviceLogicalDpi", QJsonArray{device->logicalDpiX(), device->logicalDpiY()});
  return result;
}

} // namespace gsw
