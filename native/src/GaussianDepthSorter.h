#pragma once

#include "PlyPointCloudLoader.h"

namespace gsw {

// Exact back-to-front float-depth ordering, with sourceIndex as the tie-breaker.
// Scratch storage belongs to a scene, not a frame; no quantization or sampling.
class GaussianDepthSorter {
public:
  const QVector<quint32> &sortIndices(const QVector<PointCloudVertex> &vertices, const QVector3D &forward);
  void sort(QVector<PointCloudVertex> &vertices, const QVector3D &forward);

private:
  struct Entry {
    quint32 key;
    quint32 index;
  };
  QVector<Entry> mOrder;
  QVector<Entry> mScratch;
  QVector<quint32> mIndices;
  QVector<PointCloudVertex> mGathered;
};

} // namespace gsw
