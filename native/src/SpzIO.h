#pragma once
#include "ModelExport.h"

namespace gsw {
// Codec work is CPU/RAM based. Call off the UI thread. Both operations publish
// atomically, honour cancellation at stage boundaries, and never overwrite sources.
ModelExportResult exportSpz(const ModelExportOptions &options);
ModelExportResult importSpzToPly(const QString &source, const QString &destination,
    const std::function<bool(int)> &cancelled = {});
} // namespace gsw
