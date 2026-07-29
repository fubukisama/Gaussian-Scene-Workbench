#include "TrainingGpuPreviewBuffer.h"

#include <QByteArray>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsw {

namespace {

constexpr GLenum kHandleTypeOpaqueWin32Ext = 0x9587;
constexpr GLenum kHandleTypeOpaqueWin32KmtExt = 0x9588;
constexpr GLenum kDedicatedMemoryObjectExt = 0x9581;
constexpr GLenum kDeviceLuidExt = 0x9599;
constexpr GLenum kDeviceNodeMaskExt = 0x959A;

QString rendererName(QOpenGLContext *context) {
  if (context == nullptr || context->functions() == nullptr) {
    return {};
  }
  const GLubyte *renderer = context->functions()->glGetString(GL_RENDERER);
  return renderer == nullptr ? QString() : QString::fromLatin1(
                                               reinterpret_cast<const char *>(
                                                   renderer));
}

bool requiredExtensionsAvailable(QOpenGLContext *context) {
  return context != nullptr &&
         context->hasExtension(QByteArrayLiteral("GL_EXT_memory_object")) &&
         context->hasExtension(
             QByteArrayLiteral("GL_EXT_memory_object_win32"));
}

bool requiredFunctionsAvailable(QOpenGLContext *context) {
  return context != nullptr &&
         context->getProcAddress("glCreateMemoryObjectsEXT") != nullptr &&
         context->getProcAddress("glDeleteMemoryObjectsEXT") != nullptr &&
         context->getProcAddress("glMemoryObjectParameterivEXT") != nullptr &&
         context->getProcAddress("glImportMemoryWin32HandleEXT") != nullptr &&
         context->getProcAddress("glBufferStorageMemEXT") != nullptr;
}

} // namespace

TrainingGpuPreviewCapability
TrainingGpuPreviewBuffer::probe(QOpenGLContext *context) {
  TrainingGpuPreviewCapability capability;
  capability.renderer = rendererName(context);
#ifndef Q_OS_WIN
  capability.detail =
      QStringLiteral("GPU shared preview currently requires Windows.");
  return capability;
#else
  if (context == nullptr || QOpenGLContext::currentContext() != context) {
    capability.detail =
        QStringLiteral("No current OpenGL context is available.");
    return capability;
  }
  if (!requiredExtensionsAvailable(context)) {
    capability.detail = QStringLiteral(
        "OpenGL driver does not expose GL_EXT_memory_object and "
        "GL_EXT_memory_object_win32.");
    return capability;
  }
  if (!requiredFunctionsAvailable(context)) {
    capability.detail =
        QStringLiteral("OpenGL external-memory entry points are unavailable.");
    return capability;
  }
  capability.available = true;
  capability.detail = QStringLiteral("CUDA VMM -> Win32 handle -> OpenGL");
  return capability;
#endif
}

void TrainingGpuPreviewBuffer::setError(QString *target,
                                        const QString &message) const {
  if (target != nullptr) {
    *target = message;
  }
}

bool TrainingGpuPreviewBuffer::loadExtensionFunctions(
    QOpenGLContext *context, QString *errorMessage) {
  const TrainingGpuPreviewCapability capability = probe(context);
  if (!capability.available) {
    setError(errorMessage, capability.detail);
    return false;
  }
  mCreateMemoryObjects = reinterpret_cast<CreateMemoryObjectsProc>(
      context->getProcAddress("glCreateMemoryObjectsEXT"));
  mDeleteMemoryObjects = reinterpret_cast<DeleteMemoryObjectsProc>(
      context->getProcAddress("glDeleteMemoryObjectsEXT"));
  mImportMemoryWin32Handle =
      reinterpret_cast<ImportMemoryWin32HandleProc>(
          context->getProcAddress("glImportMemoryWin32HandleEXT"));
  mMemoryObjectParameteriv =
      reinterpret_cast<MemoryObjectParameterivProc>(
          context->getProcAddress("glMemoryObjectParameterivEXT"));
  mBufferStorageMem = reinterpret_cast<BufferStorageMemProc>(
      context->getProcAddress("glBufferStorageMemEXT"));
  mGetUnsignedBytev = reinterpret_cast<GetUnsignedBytevProc>(
      context->getProcAddress("glGetUnsignedBytevEXT"));
  return mCreateMemoryObjects != nullptr && mDeleteMemoryObjects != nullptr &&
         mImportMemoryWin32Handle != nullptr &&
         mMemoryObjectParameteriv != nullptr && mBufferStorageMem != nullptr;
}

bool TrainingGpuPreviewBuffer::openControlObjects(QString *errorMessage) {
#ifndef Q_OS_WIN
  setError(errorMessage, QStringLiteral("Windows shared objects unavailable."));
  return false;
#else
  const HANDLE mapping = OpenFileMappingW(
      FILE_MAP_READ, FALSE,
      reinterpret_cast<LPCWSTR>(mDescriptor.controlMapping.utf16()));
  if (mapping == nullptr) {
    setError(errorMessage,
             QStringLiteral("OpenFileMappingW failed (%1).")
                 .arg(GetLastError()));
    return false;
  }
  mControlMappingHandle = reinterpret_cast<quintptr>(mapping);
  const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0,
                                   kTrainingGpuPreviewControlBytes);
  if (view == nullptr) {
    setError(errorMessage,
             QStringLiteral("MapViewOfFile failed (%1).").arg(GetLastError()));
    return false;
  }
  mControlView = static_cast<const char *>(view);
  const HANDLE frame = OpenEventW(
      SYNCHRONIZE, FALSE,
      reinterpret_cast<LPCWSTR>(mDescriptor.frameEvent.utf16()));
  if (frame == nullptr) {
    setError(errorMessage,
             QStringLiteral("OpenEventW(frame) failed (%1).")
                 .arg(GetLastError()));
    return false;
  }
  mFrameEventHandle = reinterpret_cast<quintptr>(frame);
  for (int index = 0; index < kTrainingGpuPreviewSlotCount; ++index) {
    const HANDLE releaseEvent = OpenEventW(
        EVENT_MODIFY_STATE, FALSE,
        reinterpret_cast<LPCWSTR>(mDescriptor.releaseEvents.at(index).utf16()));
    if (releaseEvent == nullptr) {
      setError(errorMessage,
               QStringLiteral("OpenEventW(release %1) failed (%2).")
                   .arg(index)
                   .arg(GetLastError()));
      return false;
    }
    mReleaseEventHandles.at(index) =
        reinterpret_cast<quintptr>(releaseEvent);
  }
  return true;
#endif
}

bool TrainingGpuPreviewBuffer::importMemory(QString *errorMessage) {
#ifndef Q_OS_WIN
  setError(errorMessage, QStringLiteral("Win32 memory import unavailable."));
  return false;
#else
  HANDLE importedHandle = reinterpret_cast<HANDLE>(
      static_cast<quintptr>(mDescriptor.memoryHandle));
  bool closeImportedHandle = false;
  GLenum handleType = kHandleTypeOpaqueWin32KmtExt;
  if (mDescriptor.memoryHandleType ==
      TrainingGpuPreviewHandleType::OpaqueWin32) {
    HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE,
                                  static_cast<DWORD>(mDescriptor.producerPid));
    if (producer == nullptr) {
      setError(errorMessage,
               QStringLiteral("OpenProcess for GPU preview failed (%1).")
                   .arg(GetLastError()));
      return false;
    }
    HANDLE duplicate = nullptr;
    const BOOL duplicated =
        DuplicateHandle(producer, importedHandle, GetCurrentProcess(),
                        &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(producer);
    if (!duplicated || duplicate == nullptr) {
      setError(errorMessage,
               QStringLiteral("DuplicateHandle for CUDA VMM failed (%1).")
                   .arg(GetLastError()));
      return false;
    }
    importedHandle = duplicate;
    closeImportedHandle = true;
    handleType = kHandleTypeOpaqueWin32Ext;
  }

  while (glGetError() != GL_NO_ERROR) {
  }
  mCreateMemoryObjects(1, &mMemoryObject);
  const GLint dedicated = GL_TRUE;
  mMemoryObjectParameteriv(mMemoryObject, kDedicatedMemoryObjectExt,
                           &dedicated);
  mImportMemoryWin32Handle(mMemoryObject, mDescriptor.allocationBytes,
                           handleType, importedHandle);
  // EXT_external_objects_win32 keeps ownership of NT handles with the app.
  if (closeImportedHandle) {
    CloseHandle(importedHandle);
  }
  GLenum error = glGetError();
  if (error != GL_NO_ERROR || mMemoryObject == 0) {
    setError(errorMessage,
             QStringLiteral("OpenGL CUDA-memory import failed (GL 0x%1).")
                 .arg(static_cast<uint>(error), 0, 16));
    return false;
  }

  glGenBuffers(kTrainingGpuPreviewSlotCount, mBuffers.data());
  glGenVertexArrays(kTrainingGpuPreviewSlotCount, mVertexArrays.data());
  for (int slot = 0; slot < kTrainingGpuPreviewSlotCount; ++slot) {
    glBindBuffer(GL_ARRAY_BUFFER, mBuffers.at(slot));
    mBufferStorageMem(GL_ARRAY_BUFFER,
                      static_cast<GLsizeiptr>(mDescriptor.slotBytes),
                      mMemoryObject,
                      static_cast<GLuint64>(slot) * mDescriptor.slotBytes);
    error = glGetError();
    if (error != GL_NO_ERROR) {
      setError(errorMessage,
               QStringLiteral("OpenGL shared buffer binding failed (GL 0x%1).")
                   .arg(static_cast<uint>(error), 0, 16));
      return false;
    }

    glBindVertexArray(mVertexArrays.at(slot));
    glBindBuffer(GL_ARRAY_BUFFER, mBuffers.at(slot));
    const auto setAttribute = [this](const GLuint index, const GLint count,
                                     const quintptr offset) {
      glEnableVertexAttribArray(index);
      glVertexAttribPointer(
          index, count, GL_FLOAT, GL_FALSE, kTrainingGpuPreviewStrideBytes,
          reinterpret_cast<const void *>(offset));
      glVertexAttribDivisor(index, 1);
    };
    setAttribute(0, 3, 0);
    setAttribute(1, 3, 3 * sizeof(float));
    setAttribute(2, 1, 6 * sizeof(float));
    setAttribute(3, 3, 7 * sizeof(float));
    setAttribute(4, 4, 10 * sizeof(float));
  }
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return true;
#endif
}

bool TrainingGpuPreviewBuffer::attach(
    const TrainingGpuPreviewDescriptor &descriptor, QString *errorMessage) {
  release();
  if (descriptor.state != TrainingGpuPreviewState::Ready) {
    setError(errorMessage,
             QStringLiteral("GPU preview descriptor is not ready."));
    return false;
  }
  QOpenGLContext *context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    setError(errorMessage,
             QStringLiteral("No current OpenGL context for GPU preview."));
    return false;
  }
  initializeOpenGLFunctions();
  mDescriptor = descriptor;
  if (!loadExtensionFunctions(context, errorMessage)) {
    release();
    return false;
  }
  if (!mDescriptor.deviceLuid.isEmpty()) {
    if (mGetUnsignedBytev == nullptr) {
      setError(errorMessage,
               QStringLiteral("OpenGL device identity query is unavailable."));
      release();
      return false;
    }
    std::array<GLubyte, 8> luid{};
    mGetUnsignedBytev(kDeviceLuidExt, luid.data());
    GLint nodeMask = 0;
    glGetIntegerv(kDeviceNodeMaskExt, &nodeMask);
    const QString openGlLuid =
        QString::fromLatin1(
            QByteArray(reinterpret_cast<const char *>(luid.data()),
                       static_cast<qsizetype>(luid.size()))
                .toHex());
    if (openGlLuid.compare(mDescriptor.deviceLuid,
                           Qt::CaseInsensitive) != 0 ||
        static_cast<quint32>(nodeMask) != mDescriptor.deviceNodeMask) {
      setError(errorMessage,
               QStringLiteral("CUDA and OpenGL are using different GPUs."));
      release();
      return false;
    }
  }
  if (!openControlObjects(errorMessage)) {
    release();
    return false;
  }
  if (!importMemory(errorMessage)) {
    release();
    return false;
  }
  mAttached = true;
  mInitialPoll = true;
  return true;
}

bool TrainingGpuPreviewBuffer::readControl(
    TrainingGpuPreviewControlSnapshot *snapshot,
    QString *errorMessage) const {
#ifndef Q_OS_WIN
  Q_UNUSED(snapshot)
  setError(errorMessage, QStringLiteral("Windows control mapping unavailable."));
  return false;
#else
  if (mControlView == nullptr) {
    setError(errorMessage,
             QStringLiteral("GPU preview control mapping is not open."));
    return false;
  }
  QString lastError;
  for (int attempt = 0; attempt < 4; ++attempt) {
    MemoryBarrier();
    const QByteArray copy(mControlView, kTrainingGpuPreviewHeaderBytes);
    MemoryBarrier();
    if (parseTrainingGpuPreviewControl(copy, snapshot, &lastError)) {
      return true;
    }
    SwitchToThread();
  }
  setError(errorMessage, lastError);
  return false;
#endif
}

void TrainingGpuPreviewBuffer::signalRelease(const int slot) const {
#ifdef Q_OS_WIN
  if (slot >= 0 && slot < kTrainingGpuPreviewSlotCount &&
      mReleaseEventHandles.at(slot) != 0) {
    SetEvent(reinterpret_cast<HANDLE>(mReleaseEventHandles.at(slot)));
  }
#else
  Q_UNUSED(slot)
#endif
}

void TrainingGpuPreviewBuffer::releaseCompletedSlots() {
  for (qsizetype index = mPendingReleases.size() - 1; index >= 0; --index) {
    PendingRelease &pending = mPendingReleases[index];
    const GLenum result = glClientWaitSync(pending.fence, 0, 0);
    if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
      continue;
    }
    glDeleteSync(pending.fence);
    signalRelease(pending.slot);
    mPendingReleases.removeAt(index);
  }
}

bool TrainingGpuPreviewBuffer::poll(QString *errorMessage) {
  if (!mAttached) {
    return false;
  }
  releaseCompletedSlots();
#ifdef Q_OS_WIN
  bool shouldRead = mInitialPoll;
  mInitialPoll = false;
  const DWORD eventResult = WaitForSingleObject(
      reinterpret_cast<HANDLE>(mFrameEventHandle), 0);
  if (eventResult == WAIT_OBJECT_0) {
    shouldRead = true;
  } else if (eventResult != WAIT_TIMEOUT) {
    setError(errorMessage,
             QStringLiteral("Waiting for GPU preview frame failed (%1).")
                 .arg(GetLastError()));
    return false;
  }
  if (!shouldRead) {
    return false;
  }

  TrainingGpuPreviewControlSnapshot snapshot;
  if (!readControl(&snapshot, errorMessage)) {
    return false;
  }
  if (snapshot.capacity != mDescriptor.capacity ||
      snapshot.slotBytes != mDescriptor.slotBytes ||
      snapshot.allocationBytes != mDescriptor.allocationBytes) {
    setError(errorMessage,
             QStringLiteral("GPU preview descriptor/control geometry mismatch."));
    release();
    return false;
  }
  if (snapshot.state == TrainingGpuPreviewControlState::Closing ||
      snapshot.state == TrainingGpuPreviewControlState::Failed) {
    release();
    return true;
  }

  int newestSlot = -1;
  quint64 newestGeneration = 0;
  for (int slot = 0; slot < kTrainingGpuPreviewSlotCount; ++slot) {
    const quint64 generation = snapshot.slotSnapshots.at(slot).generation;
    if (generation > mSeenGenerations.at(slot) &&
        generation > newestGeneration) {
      newestGeneration = generation;
      newestSlot = slot;
    }
  }
  if (newestSlot < 0) {
    return false;
  }

  for (int slot = 0; slot < kTrainingGpuPreviewSlotCount; ++slot) {
    const quint64 generation = snapshot.slotSnapshots.at(slot).generation;
    if (generation <= mSeenGenerations.at(slot)) {
      continue;
    }
    mSeenGenerations.at(slot) = generation;
    if (slot != newestSlot && slot != mActiveSlot) {
      signalRelease(slot);
    }
  }

  if (mActiveSlot >= 0 && mActiveSlot != newestSlot) {
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (fence != nullptr) {
      glFlush();
      mPendingReleases.append(PendingRelease{mActiveSlot, fence});
    } else {
      glFinish();
      signalRelease(mActiveSlot);
    }
  }
  const TrainingGpuPreviewSlotSnapshot &frame =
      snapshot.slotSnapshots.at(newestSlot);
  mActiveSlot = newestSlot;
  mPointCount = frame.pointCount;
  mIteration = frame.iteration;
  mGeneration = frame.generation;
  return true;
#else
  Q_UNUSED(errorMessage)
  return false;
#endif
}

void TrainingGpuPreviewBuffer::release() {
#ifdef Q_OS_WIN
  const bool hasCurrentContext = QOpenGLContext::currentContext() != nullptr;
  if (hasCurrentContext &&
      (mMemoryObject != 0 || mBuffers.at(0) != 0 ||
       mVertexArrays.at(0) != 0)) {
    glFinish();
    for (const PendingRelease &pending : std::as_const(mPendingReleases)) {
      if (pending.fence != nullptr) {
        glDeleteSync(pending.fence);
      }
    }
    if (mVertexArrays.at(0) != 0 || mVertexArrays.at(1) != 0) {
      glDeleteVertexArrays(kTrainingGpuPreviewSlotCount,
                           mVertexArrays.data());
    }
    if (mBuffers.at(0) != 0 || mBuffers.at(1) != 0) {
      glDeleteBuffers(kTrainingGpuPreviewSlotCount, mBuffers.data());
    }
    if (mMemoryObject != 0 && mDeleteMemoryObjects != nullptr) {
      mDeleteMemoryObjects(1, &mMemoryObject);
    }
  }
  for (int slot = 0; slot < kTrainingGpuPreviewSlotCount; ++slot) {
    signalRelease(slot);
  }
  mPendingReleases.clear();
  if (mControlView != nullptr) {
    UnmapViewOfFile(mControlView);
    mControlView = nullptr;
  }
  if (mControlMappingHandle != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(mControlMappingHandle));
    mControlMappingHandle = 0;
  }
  if (mFrameEventHandle != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(mFrameEventHandle));
    mFrameEventHandle = 0;
  }
  for (quintptr &handle : mReleaseEventHandles) {
    if (handle != 0) {
      CloseHandle(reinterpret_cast<HANDLE>(handle));
      handle = 0;
    }
  }
#endif
  mMemoryObject = 0;
  mBuffers.fill(0);
  mVertexArrays.fill(0);
  mSeenGenerations.fill(0);
  mDescriptor = {};
  mActiveSlot = -1;
  mPointCount = 0;
  mIteration = 0;
  mGeneration = 0;
  mAttached = false;
  mInitialPoll = true;
}

} // namespace gsw
