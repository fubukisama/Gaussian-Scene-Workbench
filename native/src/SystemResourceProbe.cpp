#include "SystemResourceProbe.h"

#include <QByteArray>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#endif

namespace gsw {
namespace {

qint64 boundedBytes(const quint64 bytes) {
  constexpr auto maximum =
      static_cast<quint64>(std::numeric_limits<qint64>::max());
  return static_cast<qint64>(bytes > maximum ? maximum : bytes);
}

void probeSystemMemory(SystemResourceSnapshot &snapshot) {
#ifdef Q_OS_WIN
  MEMORYSTATUSEX memory{};
  memory.dwLength = static_cast<DWORD>(sizeof(memory));
  if (GlobalMemoryStatusEx(&memory) != FALSE) {
    snapshot.physicalTotalBytes = boundedBytes(memory.ullTotalPhys);
    snapshot.physicalAvailableBytes = boundedBytes(memory.ullAvailPhys);
    // Despite its name, ullAvailPageFile is the current process's remaining
    // commit allowance, not the size or free space of a disk paging file.
    snapshot.commitAvailableBytes = boundedBytes(memory.ullAvailPageFile);
    snapshot.systemMemoryKnown = true;
  }
#else
  Q_UNUSED(snapshot);
#endif
}

#ifdef Q_OS_WIN
class DxgiModule final {
public:
  DxgiModule()
      : mHandle(LoadLibraryExW(L"dxgi.dll", nullptr,
                              LOAD_LIBRARY_SEARCH_SYSTEM32)) {}
  ~DxgiModule() {
    if (mHandle != nullptr) {
      FreeLibrary(mHandle);
    }
  }
  DxgiModule(const DxgiModule &) = delete;
  DxgiModule &operator=(const DxgiModule &) = delete;

  [[nodiscard]] HMODULE handle() const { return mHandle; }

private:
  HMODULE mHandle = nullptr;
};

template <typename T> struct ReleaseComObject final {
  void operator()(T *object) const noexcept {
    if (object != nullptr) {
      object->Release();
    }
  }
};

template <typename T>
using ComObject = std::unique_ptr<T, ReleaseComObject<T>>;

bool probeDxgi(QOpenGLContext *context, QOpenGLFunctions *functions,
               SystemResourceSnapshot &snapshot) {
  const bool exposesDeviceLuid =
      context->hasExtension(QByteArrayLiteral("GL_EXT_memory_object_win32")) ||
      context->hasExtension(QByteArrayLiteral("GL_EXT_semaphore_win32"));
  if (!exposesDeviceLuid) {
    return false;
  }
  using GetUnsignedBytevProc = void(QOPENGLF_APIENTRYP)(GLenum, GLubyte *);
  const auto getUnsignedBytev = reinterpret_cast<GetUnsignedBytevProc>(
      context->getProcAddress("glGetUnsignedBytevEXT"));
  if (getUnsignedBytev == nullptr) {
    return false;
  }

  constexpr GLenum kDeviceLuidExt = 0x9599;
  constexpr GLenum kDeviceNodeMaskExt = 0x959A;
  std::array<GLubyte, 8> luidBytes{};
  getUnsignedBytev(kDeviceLuidExt, luidBytes.data());
  GLint signedNodeMask = 0;
  functions->glGetIntegerv(kDeviceNodeMaskExt, &signedNodeMask);
  const auto nodeMask = static_cast<quint32>(signedNodeMask);
  snapshot.nodeMask = nodeMask;
  // Multiple device nodes have separate budgets. Do not attribute a single
  // node's headroom to an entire linked-device OpenGL context.
  if (!std::has_single_bit(nodeMask)) {
    return false;
  }
  LUID luid{};
  static_assert(sizeof(luid) == luidBytes.size());
  std::memcpy(&luid, luidBytes.data(), sizeof(luid));

  // Resolve only the Windows system DLL. No adapter is enumerated or chosen by
  // vendor/name/maximum VRAM when the OpenGL device identity is unavailable.
  const DxgiModule module;
  if (module.handle() == nullptr) {
    return false;
  }
  using CreateFactoryProc = HRESULT(WINAPI *)(REFIID, void **);
  const auto createFactory = reinterpret_cast<CreateFactoryProc>(
      GetProcAddress(module.handle(), "CreateDXGIFactory1"));
  if (createFactory == nullptr) {
    return false;
  }

  IDXGIFactory4 *rawFactory = nullptr;
  const HRESULT factoryResult = createFactory(
      __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&rawFactory));
  const ComObject<IDXGIFactory4> factory(rawFactory);
  if (FAILED(factoryResult) || factory == nullptr) {
    return false;
  }
  IDXGIAdapter3 *rawAdapter = nullptr;
  const HRESULT adapterResult = factory->EnumAdapterByLuid(
      luid, __uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&rawAdapter));
  const ComObject<IDXGIAdapter3> adapter(rawAdapter);
  if (FAILED(adapterResult) || adapter == nullptr) {
    return false;
  }

  DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
  const auto nodeIndex = static_cast<UINT>(std::countr_zero(nodeMask));
  if (FAILED(adapter->QueryVideoMemoryInfo(
          nodeIndex, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
    return false;
  }
  // Budget and CurrentUsage refer to this application process. This is not a
  // system-wide used/free VRAM counter. A successful zero-headroom observation
  // remains known and must not be replaced with a generous fallback value.
  snapshot.gpuBudgetBytes = boundedBytes(memory.Budget);
  snapshot.gpuUsageBytes = boundedBytes(memory.CurrentUsage);
  snapshot.gpuAvailableBytes = boundedBytes(
      memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage
                                         : 0);
  snapshot.gpuSource = GpuProbeSource::DxgiProcessBudget;
  DXGI_ADAPTER_DESC1 description{};
  if (SUCCEEDED(adapter->GetDesc1(&description))) {
    snapshot.gpuDedicatedBytes = boundedBytes(description.DedicatedVideoMemory);
    snapshot.adapterName = QString::fromWCharArray(description.Description);
  }
  return true;
}
#endif

void probeNvx(QOpenGLContext *context, QOpenGLFunctions *functions,
              SystemResourceSnapshot &snapshot) {
  if (!context->hasExtension(QByteArrayLiteral("GL_NVX_gpu_memory_info"))) {
    return;
  }
  constexpr GLenum kDedicatedVideoMemoryNvx = 0x9047;
  constexpr GLenum kCurrentAvailableVideoMemoryNvx = 0x9049;
  GLint dedicatedKilobytes = -1;
  GLint availableKilobytes = -1;
  functions->glGetIntegerv(kDedicatedVideoMemoryNvx, &dedicatedKilobytes);
  functions->glGetIntegerv(kCurrentAvailableVideoMemoryNvx,
                           &availableKilobytes);
  if (dedicatedKilobytes <= 0 || availableKilobytes < 0) {
    return;
  }
  snapshot.gpuDedicatedBytes =
      static_cast<qint64>(dedicatedKilobytes) * 1024;
  snapshot.gpuAvailableBytes =
      static_cast<qint64>(availableKilobytes) * 1024;
  // NVX reports approximate unused dedicated memory associated with the
  // current context. It does not supply a DXGI process budget or process usage.
  snapshot.gpuBudgetBytes = 0;
  snapshot.gpuUsageBytes = 0;
  snapshot.gpuSource = GpuProbeSource::NvxApproximate;
}

} // namespace

SystemResourceSnapshot SystemResourceProbe::probe(QOpenGLContext *context) {
  SystemResourceSnapshot snapshot;
  probeSystemMemory(snapshot);
  if (context == nullptr || QOpenGLContext::currentContext() != context) {
    return snapshot;
  }
  QOpenGLFunctions *functions = context->functions();
  if (functions == nullptr) {
    return snapshot;
  }
  const GLubyte *renderer = functions->glGetString(GL_RENDERER);
  if (renderer != nullptr) {
    snapshot.adapterName = QString::fromLatin1(
        reinterpret_cast<const char *>(renderer));
  }
#ifdef Q_OS_WIN
  if (probeDxgi(context, functions, snapshot)) {
    return snapshot;
  }
#endif
  probeNvx(context, functions, snapshot);
  return snapshot;
}

} // namespace gsw
