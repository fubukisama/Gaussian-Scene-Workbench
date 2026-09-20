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
  void setVertices(const QVector<PointCloudVertex> &vertices,
                   const GaussianShData &sh, const QBitArray &selected);
  void sort(const QVector3D &forward);
  void upload();
  void bind();
  void unbind();
  void bindTextures();
  void unbindTextures();
  int shDegree() const { return mShReady ? mSh.degree : -1; }
  int shCoefficientCount() const { return mSh.coefficientCount(); }
  quint64 shUploads() const { return mShUploads; }
  quint64 attributeUploads() const { return mAttributeUploads; }
  quint64 orderUploads() const { return mOrderUploads; }

private:
  GLuint mBuffers[3]{};
  GLuint mTextures[3]{};
  GLuint mVertexArray = 0;
  GLint mMaxTexels = 0;
  QVector<PointCloudVertex> mVertices;
  GaussianShData mSh;
  QBitArray mSelected;
  bool mShPending = false;
  bool mShReady = false;
  quint64 mShUploads = 0;
  GaussianDepthSorter mSorter;
  QVector<quint32> mOrder;
  bool mAttributesPending = false;
  bool mOrderPending = false;
  bool mClearPending = false;
  quint64 mAttributeUploads = 0;
  quint64 mOrderUploads = 0;
};
}
