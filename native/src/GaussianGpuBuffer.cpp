#include "GaussianGpuBuffer.h"
#include <array>
#include <algorithm>
#include <limits>

namespace gsw {
void GaussianGpuBuffer::initialize() {
  if (mVertexArray) return;
  initializeOpenGLFunctions();
  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &mMaxTexels);
  glGenBuffers(3, mBuffers);
  glGenTextures(3, mTextures);
  glGenVertexArrays(1, &mVertexArray);
}

void GaussianGpuBuffer::release() {
  if (!mVertexArray) return;
  glDeleteVertexArrays(1, &mVertexArray);
  glDeleteTextures(3, mTextures);
  glDeleteBuffers(3, mBuffers);
  mVertexArray = 0;
  std::fill(std::begin(mBuffers), std::end(mBuffers), 0);
  std::fill(std::begin(mTextures), std::end(mTextures), 0);
  mMaxTexels = 0;
  mAttributesPending = mOrderPending = false;
  mClearPending = false;
  mVertices.clear();
  mOrder.clear();
  mSh = {};
  mSelected = {};
  mShReady = mShPending = false;
}

void GaussianGpuBuffer::clear() {
  mVertices = {};
  mOrder = {};
  mSh = {};
  mSelected = {};
  mShReady = mShPending = false;
  mSorter = GaussianDepthSorter{};
  mAttributesPending = mOrderPending = false;
  mClearPending = true;
}

bool GaussianGpuBuffer::supports(qsizetype count) const {
  return mVertexArray && count > 0 && count <= mMaxTexels / 4 &&
      count <= std::numeric_limits<GLsizei>::max();
}

void GaussianGpuBuffer::setVertices(const QVector<PointCloudVertex> &vertices,
    const GaussianShData &sh, const QBitArray &selected) {
  if (mSh.degree != sh.degree || mSh.coefficients.constData() != sh.coefficients.constData()) {
    mSh = sh;
    mShPending = true;
    mShReady = false;
  }
  mSelected = selected;
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
    const auto &sources = mSh.sourceIndices;
    qsizetype cursor = 0;
    const bool sourceOrder = std::is_sorted(mVertices.cbegin(), mVertices.cend(),
        [](const auto &a, const auto &b) { return a.sourceIndex < b.sourceIndex; });
    for (qsizetype i = 0; i < mVertices.size(); ++i) {
      const auto &v = mVertices.at(i);
      // Resident order is monotonic; compatibility rendering sorts vertices.
      if (cursor >= sources.size() || sources.at(cursor) != v.sourceIndex) {
        if (sourceOrder && cursor < sources.size() && sources.at(cursor) < v.sourceIndex)
          while (cursor < sources.size() && sources.at(cursor) < v.sourceIndex) ++cursor;
        else
          cursor = std::lower_bound(sources.cbegin(), sources.cend(), v.sourceIndex) - sources.cbegin();
      }
      const float shIndex = cursor < sources.size() && sources.at(cursor) == v.sourceIndex
          ? static_cast<float>(cursor) : -1.0F;
      const bool selected = v.sourceIndex < mSelected.size() && mSelected.testBit(v.sourceIndex);
      out[i] = {v.x, v.y, v.z, v.opacity, v.red, v.green, v.blue, v.scaleX,
                v.scaleY, v.scaleZ, v.rotationW, v.rotationX,
                v.rotationY, v.rotationZ, shIndex, selected ? 1.0F : 0.0F};
      ++cursor;
    }
    glBindBuffer(GL_TEXTURE_BUFFER, mBuffers[0]);
    glBufferData(GL_TEXTURE_BUFFER, packed.size() * sizeof(packed[0]), packed.constData(), GL_STATIC_DRAW);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_BUFFER, mTextures[0]);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, mBuffers[0]);
    mAttributesPending = false;
    ++mAttributeUploads;
  }
  if (mShPending) {
    const qsizetype texels = mSh.coefficients.size() / 3;
    mShReady = mSh.degree >= 0 && texels > 0 && texels <= mMaxTexels;
    glBindBuffer(GL_TEXTURE_BUFFER, mBuffers[2]);
    // Allocation errors must not turn into out-of-range shader reads.
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    glBufferData(GL_TEXTURE_BUFFER, mShReady ? mSh.coefficients.size() * sizeof(float) : 0,
                 mShReady ? mSh.coefficients.constData() : nullptr, GL_STATIC_DRAW);
    mShReady = glGetError() == GL_NO_ERROR && mShReady;
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_BUFFER, mTextures[2]);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGB32F, mBuffers[2]);
    mShPending = false;
    ++mShUploads;
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
  bindTextures();
  glBindVertexArray(mVertexArray);
}

void GaussianGpuBuffer::bindTextures() {
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_BUFFER, mTextures[0]);
  glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_BUFFER, mTextures[1]);
  glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_BUFFER, mTextures[2]);
  glActiveTexture(GL_TEXTURE0);
}

void GaussianGpuBuffer::unbind() {
  glBindVertexArray(0);
  unbindTextures();
}

void GaussianGpuBuffer::unbindTextures() {
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_BUFFER, 0);
  glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_BUFFER, 0);
  glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_BUFFER, 0);
  glActiveTexture(GL_TEXTURE0);
}
}
