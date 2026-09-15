#include "GaussianGpuBuffer.h"
#include <array>
#include <algorithm>
#include <limits>

namespace gsw {
void GaussianGpuBuffer::initialize() {
  if (mVertexArray) return;
  initializeOpenGLFunctions();
  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &mMaxTexels);
  glGenBuffers(2, mBuffers);
  glGenTextures(2, mTextures);
  glGenVertexArrays(1, &mVertexArray);
}

void GaussianGpuBuffer::release() {
  if (!mVertexArray) return;
  glDeleteVertexArrays(1, &mVertexArray);
  glDeleteTextures(2, mTextures);
  glDeleteBuffers(2, mBuffers);
  mVertexArray = 0;
  mBuffers[0] = mBuffers[1] = mTextures[0] = mTextures[1] = 0;
  mMaxTexels = 0;
  mAttributesPending = mOrderPending = false;
  mClearPending = false;
  mVertices.clear();
  mOrder.clear();
}

void GaussianGpuBuffer::clear() {
  mVertices = {};
  mOrder = {};
  mSorter = GaussianDepthSorter{};
  mAttributesPending = mOrderPending = false;
  mClearPending = true;
}

bool GaussianGpuBuffer::supports(qsizetype count) const {
  return mVertexArray && count > 0 && count <= mMaxTexels / 4 &&
      count <= std::numeric_limits<GLsizei>::max();
}

void GaussianGpuBuffer::setVertices(const QVector<PointCloudVertex> &vertices) {
  mVertices = vertices;
  mClearPending = false;
  mAttributesPending = true;
}

void GaussianGpuBuffer::sort(const QVector3D &forward) {
  // Copy only indices. The sorter's scratch is reused rather than sharing a
  // writable order array across frames and triggering a Qt detach each pass.
  const auto &indices = mSorter.sortIndices(mVertices, forward);
  mOrder.resize(indices.size());
  std::copy(indices.cbegin(), indices.cend(), mOrder.begin());
  mOrderPending = true;
}

void GaussianGpuBuffer::upload() {
  if (!mVertexArray) return;
  if (mClearPending) {
    for (GLuint buffer : mBuffers) {
      glBindBuffer(GL_TEXTURE_BUFFER, buffer);
      glBufferData(GL_TEXTURE_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    }
    mClearPending = false;
  }
  if (mAttributesPending) {
    // RGBA32F is available in desktop GL 3.3. Four texels retain all 14 float
    // attributes losslessly; the source index stays on the CPU for editing.
    QVector<std::array<float, 16>> packed(mVertices.size());
    auto *out = packed.data();
    for (qsizetype i = 0; i < mVertices.size(); ++i) {
      const auto &v = mVertices.at(i);
      out[i] = {v.x, v.y, v.z, v.opacity, v.red, v.green, v.blue, v.scaleX,
                v.scaleY, v.scaleZ, v.rotationW, v.rotationX,
                v.rotationY, v.rotationZ, 0.0F, 0.0F};
    }
    glBindBuffer(GL_TEXTURE_BUFFER, mBuffers[0]);
    glBufferData(GL_TEXTURE_BUFFER, packed.size() * sizeof(packed[0]), packed.constData(), GL_STATIC_DRAW);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_BUFFER, mTextures[0]);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, mBuffers[0]);
    mAttributesPending = false;
    ++mAttributeUploads;
  }
  if (mOrderPending) {
    glBindBuffer(GL_TEXTURE_BUFFER, mBuffers[1]);
    glBufferData(GL_TEXTURE_BUFFER, mOrder.size() * sizeof(quint32), mOrder.constData(), GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_BUFFER, mTextures[1]);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, mBuffers[1]);
    mOrderPending = false;
    ++mOrderUploads;
  }
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
  glActiveTexture(GL_TEXTURE0);
}

void GaussianGpuBuffer::bind() {
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_BUFFER, mTextures[0]);
  glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_BUFFER, mTextures[1]);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(mVertexArray);
}

void GaussianGpuBuffer::unbind() {
  glBindVertexArray(0);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_BUFFER, 0);
  glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_BUFFER, 0);
  glActiveTexture(GL_TEXTURE0);
}
}
