#pragma once
#include "GaussianDepthSorter.h"
#include <QOpenGLExtraFunctions>

namespace gsw {

// Static Gaussian attributes stay resident; camera motion uploads only uint32
// instance indices. The training CUDA-interop VAO remains a separate path.
class GaussianGpuBuffer : protected QOpenGLExtraFunctions {
public:
  void initialize();
  void release(); // caller must have the owning GL context current
  void clear(); // safe from UI callbacks; GL storage is cleared at next upload
  bool supports(qsizetype count) const;
  void setVertices(const QVector<PointCloudVertex> &vertices);
  void sort(const QVector3D &forward);
  void upload();
  void bind();
  void unbind();
  quint64 attributeUploads() const { return mAttributeUploads; }
  quint64 orderUploads() const { return mOrderUploads; }

private:
  GLuint mBuffers[2]{};
  GLuint mTextures[2]{};
  GLuint mVertexArray = 0;
  GLint mMaxTexels = 0;
  QVector<PointCloudVertex> mVertices;
  GaussianDepthSorter mSorter;
  QVector<quint32> mOrder;
  bool mAttributesPending = false;
  bool mOrderPending = false;
  bool mClearPending = false;
  quint64 mAttributeUploads = 0;
  quint64 mOrderUploads = 0;
};
}
