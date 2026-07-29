#pragma once

#include "TrainingGpuPreviewProtocol.h"

#include <QOpenGLExtraFunctions>
#include <QString>
#include <QVector>
#include <QVector3D>

#include <array>

class QOpenGLContext;

namespace gsw {

struct TrainingGpuPreviewCapability final {
  bool available = false;
  QString renderer;
  QString detail;
};

class TrainingGpuPreviewBuffer final : protected QOpenGLExtraFunctions {
public:
  TrainingGpuPreviewBuffer() = default;
  ~TrainingGpuPreviewBuffer() = default;

  TrainingGpuPreviewBuffer(const TrainingGpuPreviewBuffer &) = delete;
  TrainingGpuPreviewBuffer &operator=(const TrainingGpuPreviewBuffer &) =
      delete;

  [[nodiscard]] static TrainingGpuPreviewCapability
  probe(QOpenGLContext *context);

  [[nodiscard]] bool attach(const TrainingGpuPreviewDescriptor &descriptor,
                            QString *errorMessage = nullptr);
  [[nodiscard]] bool poll(QString *errorMessage = nullptr);
  void release();

  [[nodiscard]] bool attached() const { return mAttached; }
  [[nodiscard]] bool hasFrame() const {
    return mAttached && mActiveSlot >= 0 && mPointCount > 0;
  }
  [[nodiscard]] GLuint activeVertexArray() const {
    return mActiveSlot >= 0 ? mVertexArrays.at(mActiveSlot) : 0;
  }
  [[nodiscard]] GLuint activeBuffer() const {
    return mActiveSlot >= 0 ? mBuffers.at(mActiveSlot) : 0;
  }
  [[nodiscard]] GLsizei pointCount() const {
    return static_cast<GLsizei>(mPointCount);
  }
  [[nodiscard]] quint64 iteration() const { return mIteration; }
  [[nodiscard]] quint64 generation() const { return mGeneration; }
  [[nodiscard]] QVector3D sceneCenter() const { return mSceneCenter; }
  [[nodiscard]] float sceneRadius() const { return mSceneRadius; }
  [[nodiscard]] QString sessionId() const { return mDescriptor.sessionId; }
  [[nodiscard]] QString device() const { return mDescriptor.device; }

private:
  using CreateMemoryObjectsProc =
      void(QOPENGLF_APIENTRYP)(GLsizei, GLuint *);
  using DeleteMemoryObjectsProc =
      void(QOPENGLF_APIENTRYP)(GLsizei, const GLuint *);
  using ImportMemoryWin32HandleProc =
      void(QOPENGLF_APIENTRYP)(GLuint, GLuint64, GLenum, void *);
  using MemoryObjectParameterivProc =
      void(QOPENGLF_APIENTRYP)(GLuint, GLenum, const GLint *);
  using BufferStorageMemProc =
      void(QOPENGLF_APIENTRYP)(GLenum, GLsizeiptr, GLuint, GLuint64);
  using GetUnsignedBytevProc =
      void(QOPENGLF_APIENTRYP)(GLenum, GLubyte *);

  struct PendingRelease final {
    int slot = -1;
    GLsync fence = nullptr;
  };

  [[nodiscard]] bool loadExtensionFunctions(QOpenGLContext *context,
                                            QString *errorMessage);
  [[nodiscard]] bool openControlObjects(QString *errorMessage);
  [[nodiscard]] bool importMemory(QString *errorMessage);
  [[nodiscard]] bool readControl(
      TrainingGpuPreviewControlSnapshot *snapshot,
      QString *errorMessage) const;
  void releaseCompletedSlots();
  void signalRelease(int slot) const;
  void setError(QString *target, const QString &message) const;

  TrainingGpuPreviewDescriptor mDescriptor;
  CreateMemoryObjectsProc mCreateMemoryObjects = nullptr;
  DeleteMemoryObjectsProc mDeleteMemoryObjects = nullptr;
  ImportMemoryWin32HandleProc mImportMemoryWin32Handle = nullptr;
  MemoryObjectParameterivProc mMemoryObjectParameteriv = nullptr;
  BufferStorageMemProc mBufferStorageMem = nullptr;
  GetUnsignedBytevProc mGetUnsignedBytev = nullptr;
  GLuint mMemoryObject = 0;
  std::array<GLuint, kTrainingGpuPreviewSlotCount> mBuffers{};
  std::array<GLuint, kTrainingGpuPreviewSlotCount> mVertexArrays{};
  std::array<quint64, kTrainingGpuPreviewSlotCount> mSeenGenerations{};
  QVector<PendingRelease> mPendingReleases;
  quintptr mControlMappingHandle = 0;
  const char *mControlView = nullptr;
  quintptr mFrameEventHandle = 0;
  std::array<quintptr, kTrainingGpuPreviewSlotCount> mReleaseEventHandles{};
  int mActiveSlot = -1;
  quint64 mPointCount = 0;
  quint64 mIteration = 0;
  quint64 mGeneration = 0;
  QVector3D mSceneCenter;
  float mSceneRadius = 0.0F;
  bool mAttached = false;
  bool mInitialPoll = true;
};

} // namespace gsw
