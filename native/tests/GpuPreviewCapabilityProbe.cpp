#include "TrainingGpuPreviewBuffer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace {
[[noreturn]] void finishProbe(const int code, const QString &message) {
  QTextStream stream(code == 0 ? stdout : stderr);
  stream << message << '\n';
  stream.flush();
  std::_Exit(code);
}

QByteArray readDescriptorPayload(const QString &filePath, QString *error) {
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    *error = QStringLiteral("Unable to read producer descriptor: %1")
                 .arg(file.errorString());
    return {};
  }
  constexpr QByteArrayView prefix("[gsw-training-gpu-preview] ");
  const QList<QByteArray> lines = file.readAll().split('\n');
  for (QByteArray line : lines) {
    line = line.trimmed();
    if (line.startsWith(prefix)) {
      return line.sliced(prefix.size());
    }
  }
  *error = QStringLiteral("Producer descriptor event was not found.");
  return {};
}

bool expectedFirstVertex(const std::array<float, 14> &vertex) {
  constexpr std::array<float, 14> expected = {
      -2.0F, 2.0F, 3.0F, 0.5F, 0.5F, 0.5F, 0.8F,
      0.01F, 0.01F, 0.01F, 1.0F, 0.0F, 0.0F, 0.0F};
  for (size_t index = 0; index < expected.size(); ++index) {
    if (!std::isfinite(vertex.at(index)) ||
        std::abs(vertex.at(index) - expected.at(index)) > 0.0001F) {
      return false;
    }
  }
  return true;
}
} // namespace

int main(int argc, char **argv) {
  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QSurfaceFormat::setDefaultFormat(format);
  QGuiApplication application(argc, argv);

  QOpenGLContext context;
  context.setFormat(format);
  if (!context.create()) {
    finishProbe(2, QStringLiteral("GPU_PREVIEW_INTEROP available=false "
                                  "detail=OpenGL context creation failed"));
  }
  QOffscreenSurface surface;
  surface.setFormat(context.format());
  surface.create();
  if (!surface.isValid() || !context.makeCurrent(&surface)) {
    finishProbe(3, QStringLiteral("GPU_PREVIEW_INTEROP available=false "
                                  "detail=OpenGL context activation failed"));
  }
  const gsw::TrainingGpuPreviewCapability capability =
      gsw::TrainingGpuPreviewBuffer::probe(&context);
  if (!capability.available) {
    context.doneCurrent();
    finishProbe(
        4,
        QStringLiteral("GPU_PREVIEW_INTEROP available=false renderer=%1 detail=%2")
            .arg(capability.renderer, capability.detail));
  }

  const QStringList arguments = application.arguments();
  const int descriptorOption =
      arguments.indexOf(QStringLiteral("--descriptor-file"));
  if (descriptorOption < 0) {
    context.doneCurrent();
    finishProbe(
        0,
        QStringLiteral("GPU_PREVIEW_INTEROP available=true renderer=%1 detail=%2")
            .arg(capability.renderer, capability.detail));
  }
  if (descriptorOption + 1 >= arguments.size()) {
    context.doneCurrent();
    finishProbe(5, QStringLiteral("GPU_PREVIEW_INTEROP descriptor path missing"));
  }

  QString error;
  const QByteArray payload =
      readDescriptorPayload(arguments.at(descriptorOption + 1), &error);
  gsw::TrainingGpuPreviewDescriptor descriptor;
  if (payload.isEmpty() || !gsw::parseTrainingGpuPreviewDescriptor(
                               payload, &descriptor, &error)) {
    context.doneCurrent();
    finishProbe(6, QStringLiteral("GPU_PREVIEW_INTEROP descriptor=false detail=%1")
                       .arg(error));
  }

  gsw::TrainingGpuPreviewBuffer buffer;
  if (!buffer.attach(descriptor, &error)) {
    context.doneCurrent();
    finishProbe(7, QStringLiteral("GPU_PREVIEW_INTEROP import=false detail=%1")
                       .arg(error));
  }

  QElapsedTimer deadline;
  deadline.start();
  while (deadline.elapsed() < 5000 && buffer.iteration() < 12) {
    error.clear();
    const bool frameChanged = buffer.poll(&error);
    Q_UNUSED(frameChanged)
    if (!error.isEmpty()) {
      buffer.release();
      context.doneCurrent();
      finishProbe(8, QStringLiteral("GPU_PREVIEW_INTEROP poll=false detail=%1")
                         .arg(error));
    }
    QThread::msleep(5);
  }
  if (!buffer.hasFrame() || buffer.iteration() != 12 ||
      buffer.pointCount() != 64) {
    const QString detail =
        QStringLiteral("frame timeout count=%1 iteration=%2 generation=%3")
            .arg(buffer.pointCount())
            .arg(buffer.iteration())
            .arg(buffer.generation());
    buffer.release();
    context.doneCurrent();
    finishProbe(9, QStringLiteral("GPU_PREVIEW_INTEROP frame=false detail=%1")
                       .arg(detail));
  }
  if ((buffer.sceneCenter() - QVector3D(0.0F, 2.0F, 3.0F)).length() >
          0.0001F ||
      std::abs(buffer.sceneRadius() - 2.0F) > 0.0001F) {
    const QString detail =
        QStringLiteral("bounds center=(%1,%2,%3) radius=%4")
            .arg(buffer.sceneCenter().x())
            .arg(buffer.sceneCenter().y())
            .arg(buffer.sceneCenter().z())
            .arg(buffer.sceneRadius());
    buffer.release();
    context.doneCurrent();
    finishProbe(12, QStringLiteral("GPU_PREVIEW_INTEROP bounds=false detail=%1")
                        .arg(detail));
  }

  QOpenGLExtraFunctions *gl = context.extraFunctions();
  using GetBufferSubDataProc =
      void(QOPENGLF_APIENTRYP)(GLenum, GLintptr, GLsizeiptr, void *);
  const auto getBufferSubData = reinterpret_cast<GetBufferSubDataProc>(
      context.getProcAddress("glGetBufferSubData"));
  if (getBufferSubData == nullptr) {
    buffer.release();
    context.doneCurrent();
    finishProbe(10,
                QStringLiteral("GPU_PREVIEW_INTEROP data=false detail="
                               "glGetBufferSubData unavailable"));
  }
  gl->glBindBuffer(GL_ARRAY_BUFFER, buffer.activeBuffer());
  while (gl->glGetError() != GL_NO_ERROR) {
  }
  std::array<float, 14> firstVertex{};
  getBufferSubData(GL_ARRAY_BUFFER, 0, gsw::kTrainingGpuPreviewStrideBytes,
                   firstVertex.data());
  const GLenum glError = gl->glGetError();
  gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
  if (glError != GL_NO_ERROR || !expectedFirstVertex(firstVertex)) {
    buffer.release();
    context.doneCurrent();
    finishProbe(
        11,
        QStringLiteral("GPU_PREVIEW_INTEROP data=false detail="
                       "readback GL 0x%1 or ABI mismatch")
            .arg(static_cast<uint>(glError), 0, 16));
  }

  const quint64 generation = buffer.generation();
  buffer.release();
  context.doneCurrent();
  finishProbe(
      0,
      QStringLiteral("GPU_PREVIEW_INTEROP end_to_end=true renderer=%1 "
                     "generation=%2 count=64 iteration=12 bounds=true")
          .arg(capability.renderer)
          .arg(generation));
}
