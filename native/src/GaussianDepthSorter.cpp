#include "GaussianDepthSorter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace gsw {
namespace {
quint32 descendingDepthKey(float depth) {
  static_assert(sizeof(float) == sizeof(quint32) && std::numeric_limits<float>::is_iec559);
  // Signed zero compares equal in the old comparator. Invalid depths sort last
  // deterministically instead of violating std::sort's strict weak ordering.
  if (std::isnan(depth)) return std::numeric_limits<quint32>::max();
  if (depth == 0.0F) depth = 0.0F;
  const auto bits = std::bit_cast<quint32>(depth);
  const auto ascending = bits ^ ((bits & 0x80000000U) ? 0xffffffffU : 0x80000000U);
  return ~ascending;
}
}

const QVector<quint32> &GaussianDepthSorter::sortIndices(
    const QVector<PointCloudVertex> &vertices, const QVector3D &forward) {
  const qsizetype count = vertices.size();
  Q_ASSERT(static_cast<quint64>(count) <= std::numeric_limits<quint32>::max());
  mOrder.resize(count);
  mScratch.resize(count);
  mIndices.resize(count);
  std::array<std::array<quint32, 256>, 4> histogram{};
  const PointCloudVertex *source = vertices.constData();
  Entry *order = mOrder.data();
  bool sourceOrdered = true;
  for (qsizetype i = 0; i < count; ++i) {
    const auto &vertex = source[i];
    const quint32 key = descendingDepthKey(vertex.x * forward.x() +
        vertex.y * forward.y() + vertex.z * forward.z());
    order[i] = {key, static_cast<quint32>(i)};
    for (unsigned pass = 0; pass < 4; ++pass) ++histogram[pass][(key >> (pass * 8)) & 0xffU];
    if (i > 0 && source[i - 1].sourceIndex > vertex.sourceIndex) sourceOrdered = false;
  }
  // Loader/filter output is already in source order, so normal viewport frames
  // take the linear path. Also support reordered inputs and tied depths exactly.
  if (!sourceOrdered) {
    std::stable_sort(order, order + count, [source](const Entry &a, const Entry &b) {
      return source[a.index].sourceIndex < source[b.index].sourceIndex;
    });
  }
  Entry *scratch = mScratch.data();
  for (unsigned pass = 0; pass < 4; ++pass) {
    auto &offsets = histogram[pass];
    quint32 offset = 0;
    for (auto &bucket : offsets) {
      const quint32 size = bucket;
      bucket = offset;
      offset += size;
    }
    for (qsizetype i = 0; i < count; ++i) {
      const auto entry = order[i];
      scratch[offsets[(entry.key >> (pass * 8)) & 0xffU]++] = entry;
    }
    std::swap(order, scratch);
  }
  auto *indices = mIndices.data();
  for (qsizetype i = 0; i < count; ++i) indices[i] = order[i].index;
  return mIndices;
}

void GaussianDepthSorter::sort(QVector<PointCloudVertex> &vertices, const QVector3D &forward) {
  const auto &indices = sortIndices(vertices, forward);
  const auto *source = vertices.constData();
  mGathered.resize(vertices.size());
  auto *gathered = mGathered.data();
  for (qsizetype i = 0; i < vertices.size(); ++i) gathered[i] = source[indices[i]];
  vertices.swap(mGathered);
}

} // namespace gsw
