#include "RenderFrameDiagnostics.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPointer>
#include <QSaveFile>
#include <QScreen>
#include <QUuid>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <vector>

namespace gsw {
namespace {
using GL = QOpenGLFunctions_3_3_Core;
constexpr int kMaxStages = 32;
constexpr qint64 kGpuBudget = 256LL * 1024 * 1024;

QJsonArray rectJson(const QRect &r) { return {r.x(), r.y(), r.width(), r.height()}; }
QJsonArray errors(GL &g) {
  QJsonArray result;
  for (int i = 0; i < 32; ++i) {
    const GLenum e = g.glGetError();
    if (e == GL_NO_ERROR) break;
    result.append(int(e));
  }
  return result;
}
QJsonObject state(GL &g) {
  QJsonObject s;
  const auto integer = [&](const char *key, GLenum p) {
    GLint v = 0; g.glGetIntegerv(p, &v); s.insert(QLatin1String(key), v);
  };
  integer("drawFramebuffer", GL_DRAW_FRAMEBUFFER_BINDING);
  integer("readFramebuffer", GL_READ_FRAMEBUFFER_BINDING);
  integer("readBuffer", GL_READ_BUFFER);
  integer("samples", GL_SAMPLES);
  integer("program", GL_CURRENT_PROGRAM);
  integer("vertexArray", GL_VERTEX_ARRAY_BINDING);
  integer("depthFunc", GL_DEPTH_FUNC);
  integer("blendSrcRgb", GL_BLEND_SRC_RGB);
  integer("blendDstRgb", GL_BLEND_DST_RGB);
  integer("blendSrcAlpha", GL_BLEND_SRC_ALPHA);
  integer("blendDstAlpha", GL_BLEND_DST_ALPHA);
  integer("blendEquationRgb", GL_BLEND_EQUATION_RGB);
  integer("blendEquationAlpha", GL_BLEND_EQUATION_ALPHA);
  integer("stencilFunc", GL_STENCIL_FUNC);
  integer("stencilRef", GL_STENCIL_REF);
  integer("stencilValueMask", GL_STENCIL_VALUE_MASK);
  integer("stencilWriteMask", GL_STENCIL_WRITEMASK);
  integer("stencilFail", GL_STENCIL_FAIL);
  integer("stencilDepthFail", GL_STENCIL_PASS_DEPTH_FAIL);
  integer("stencilDepthPass", GL_STENCIL_PASS_DEPTH_PASS);
  integer("stencilBackFunc", GL_STENCIL_BACK_FUNC);
  integer("stencilBackRef", GL_STENCIL_BACK_REF);
  integer("stencilBackValueMask", GL_STENCIL_BACK_VALUE_MASK);
  integer("stencilBackWriteMask", GL_STENCIL_BACK_WRITEMASK);
  integer("stencilBackFail", GL_STENCIL_BACK_FAIL);
  integer("stencilBackDepthFail", GL_STENCIL_BACK_PASS_DEPTH_FAIL);
  integer("stencilBackDepthPass", GL_STENCIL_BACK_PASS_DEPTH_PASS);
  for (const auto &[name, capability] : {
         std::pair{"depthTest", GLenum(GL_DEPTH_TEST)},
         std::pair{"blend", GLenum(GL_BLEND)},
         std::pair{"scissorTest", GLenum(GL_SCISSOR_TEST)},
         std::pair{"stencilTest", GLenum(GL_STENCIL_TEST)},
         std::pair{"framebufferSrgb", GLenum(GL_FRAMEBUFFER_SRGB)}})
    s.insert(QLatin1String(name), bool(g.glIsEnabled(capability)));
  GLboolean depthMask = GL_FALSE, colors[4]{};
  g.glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
  g.glGetBooleanv(GL_COLOR_WRITEMASK, colors);
  s.insert("depthWriteMask", bool(depthMask));
  s.insert("colorWriteMask", QJsonArray{bool(colors[0]), bool(colors[1]), bool(colors[2]), bool(colors[3])});
  GLint viewport[4]{}, scissor[4]{};
  g.glGetIntegerv(GL_VIEWPORT, viewport); g.glGetIntegerv(GL_SCISSOR_BOX, scissor);
  s.insert("viewport", QJsonArray{viewport[0], viewport[1], viewport[2], viewport[3]});
  s.insert("scissorBox", QJsonArray{scissor[0], scissor[1], scissor[2], scissor[3]});
  GLdouble clear = 0., range[2]{};
  g.glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear); g.glGetDoublev(GL_DEPTH_RANGE, range);
  s.insert("clearDepth", clear); s.insert("depthRange", QJsonArray{range[0], range[1]});
  return s;
}
QJsonObject drawState(GL &g) {
  QJsonObject result = state(g);
  QJsonArray attributes;
  for (GLuint index : {0U, 1U}) {
    QJsonObject attribute{{"index", int(index)}};
    for (const auto &[key, query] : {
           std::pair{"enabled", GLenum(GL_VERTEX_ATTRIB_ARRAY_ENABLED)},
           std::pair{"size", GLenum(GL_VERTEX_ATTRIB_ARRAY_SIZE)},
           std::pair{"type", GLenum(GL_VERTEX_ATTRIB_ARRAY_TYPE)},
           std::pair{"normalized", GLenum(GL_VERTEX_ATTRIB_ARRAY_NORMALIZED)},
           std::pair{"stride", GLenum(GL_VERTEX_ATTRIB_ARRAY_STRIDE)},
           std::pair{"bufferBinding", GLenum(GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING)},
           std::pair{"divisor", GLenum(GL_VERTEX_ATTRIB_ARRAY_DIVISOR)}}) {
      GLint value = 0; g.glGetVertexAttribiv(index, query, &value);
      attribute.insert(QLatin1String(key), value);
    }
    void *pointer = nullptr; g.glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    attribute.insert("pointerOffset", QString::number(quintptr(pointer)));
    attributes.append(attribute);
  }
  result.insert("attributes", attributes);
  const GLuint program = GLuint(result.value("program").toInt());
  if (program) {
    QJsonObject uniforms;
    for (const char *name : {"viewProjection", "view", "projection"}) {
      const GLint location = g.glGetUniformLocation(program, name);
      if (location < 0) continue;
      GLfloat values[16]{}; g.glGetUniformfv(program, location, values);
      QJsonArray matrix; for (GLfloat value : values) matrix.append(double(value));
      uniforms.insert(QLatin1String(name), matrix);
    }
    for (const char *name : {"selected", "indexedAttributes", "surfelRendering"}) {
      const GLint location = g.glGetUniformLocation(program, name);
      if (location >= 0) { GLint value = 0; g.glGetUniformiv(program, location, &value); uniforms.insert(QLatin1String(name), value); }
    }
    const GLint location = g.glGetUniformLocation(program, "pointSize");
    if (location >= 0) { GLfloat value = 0; g.glGetUniformfv(program, location, &value); uniforms.insert("pointSize", double(value)); }
    result.insert("uniforms", uniforms);
  }
  GLint active = 0; g.glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  QJsonArray buffers;
  for (int unit : {2, 3, 4}) {
    g.glActiveTexture(GL_TEXTURE0 + unit);
    GLint texture = 0; g.glGetIntegerv(GL_TEXTURE_BINDING_BUFFER, &texture);
    QJsonObject buffer{{"unit", unit}, {"texture", texture}};
    if (texture) {
      GLint storage = 0, width = 0;
      g.glGetTexLevelParameteriv(GL_TEXTURE_BUFFER, 0, GL_TEXTURE_BUFFER_DATA_STORE_BINDING, &storage);
      g.glGetTexLevelParameteriv(GL_TEXTURE_BUFFER, 0, GL_TEXTURE_WIDTH, &width);
      buffer.insert("storageBuffer", storage); buffer.insert("texels", width);
    }
    buffers.append(buffer);
  }
  g.glActiveTexture(GLenum(active)); result.insert("textureBuffers", buffers);
  return result;
}

// Qt FBO allocation/toImage and pixel readback can touch these bindings. Keep
// the guard alive until all temporary FBOs have been destroyed.
struct StateGuard {
  GL &g;
  GLint draw = 0, read = 0, readBuffer = 0, renderbuffer = 0, active = 0, texture0 = 0;
  GLint packBuffer = 0, unpackBuffer = 0, viewport[4]{}, scissorBox[4]{};
  GLint pack[6]{};
  GLboolean scissor = GL_FALSE, srgb = GL_FALSE;
  static constexpr GLenum packNames[6] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
      GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST};
  explicit StateGuard(GL &f) : g(f) {
    g.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    g.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    g.glGetIntegerv(GL_READ_BUFFER, &readBuffer);
    g.glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    g.glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    g.glActiveTexture(GL_TEXTURE0); g.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
    g.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    g.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    g.glGetIntegerv(GL_VIEWPORT, viewport); g.glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    scissor = g.glIsEnabled(GL_SCISSOR_TEST); srgb = g.glIsEnabled(GL_FRAMEBUFFER_SRGB);
    for (int i = 0; i < 6; ++i) g.glGetIntegerv(packNames[i], &pack[i]);
    g.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); g.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (int i = 0; i < 6; ++i) g.glPixelStorei(packNames[i], i == 0 ? 1 : 0);
    g.glDisable(GL_SCISSOR_TEST); g.glDisable(GL_FRAMEBUFFER_SRGB);
  }
  ~StateGuard() {
    g.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw));
    g.glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read)); g.glReadBuffer(GLenum(readBuffer));
    g.glBindRenderbuffer(GL_RENDERBUFFER, GLuint(renderbuffer));
    g.glActiveTexture(GL_TEXTURE0); g.glBindTexture(GL_TEXTURE_2D, GLuint(texture0));
    g.glActiveTexture(GLenum(active));
    for (int i = 0; i < 6; ++i) g.glPixelStorei(packNames[i], pack[i]);
    g.glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(packBuffer));
    g.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GLuint(unpackBuffer));
    g.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    g.glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    scissor ? g.glEnable(GL_SCISSOR_TEST) : g.glDisable(GL_SCISSOR_TEST);
    srgb ? g.glEnable(GL_FRAMEBUFFER_SRGB) : g.glDisable(GL_FRAMEBUFFER_SRGB);
  }
};
struct ContextGuard {
  QOpenGLContext *previous = QOpenGLContext::currentContext();
  QSurface *surface = previous ? previous->surface() : nullptr;
  QOpenGLWidget *widget;
  bool switched;
  explicit ContextGuard(QOpenGLWidget *w) : widget(w), switched(previous != w->context()) {
    if (switched) w->makeCurrent();
  }
  ~ContextGuard() {
    if (!switched) return;
    widget->doneCurrent();
    if (previous && surface) previous->makeCurrent(surface);
  }
};
QJsonObject desktop(QOpenGLWidget *w, const QString &path) {
  QJsonObject result{{"source", "QScreen::grabWindow(0)"}, {"saved", false}};
  if (!w || !w->isVisible() || w->window()->isMinimized() ||
      !w->window()->isActiveWindow() || !w->window()->windowHandle() ||
      !w->window()->windowHandle()->isExposed()) {
    result.insert("reason", "viewport-not-active-and-exposed"); return result;
  }
  const QRect global(w->mapToGlobal(QPoint(0, 0)), w->size());
  QScreen *screen = nullptr;
  for (QScreen *candidate : QGuiApplication::screens())
    if (candidate->geometry().contains(global)) { screen = candidate; break; }
  result.insert("globalRect", rectJson(global));
  if (!screen || global.isEmpty()) {
    result.insert("reason", "viewport-not-fully-on-one-screen"); return result;
  }
  result.insert("screen", screen->name());
  result.insert("screenGeometry", rectJson(screen->geometry()));
  result.insert("screenDpr", screen->devicePixelRatio());
  // With WId zero Qt expects coordinates relative to this screen.
  const QPoint p = global.topLeft() - screen->geometry().topLeft();
  const QPixmap pixels = screen->grabWindow(0, p.x(), p.y(), global.width(), global.height());
  result.insert("pixelSize", QJsonArray{pixels.width(), pixels.height()});
  result.insert("pixelDpr", pixels.devicePixelRatio());
  const bool ok = !pixels.isNull() && pixels.save(path, "PNG");
  result.insert("saved", ok); result.insert("path", path);
  if (!ok) result.insert("reason", "desktop-capture-or-save-failed");
  return result;
}
struct Snapshot {
  QJsonObject info;
  std::unique_ptr<QOpenGLFramebufferObject> fbo;
};

Snapshot copyFramebuffer(GL &g, GLuint source, const QSize &size,
                         bool depthStencil, const QString &name, const QJsonObject &meta) {
  Snapshot s;
  s.info = {{"name", name}, {"meta", meta}, {"state", state(g)},
            {"sourceFramebuffer", int(source)}, {"pixelSize", QJsonArray{size.width(), size.height()}},
            {"colorValid", false}, {"depthValid", false}, {"stencilValid", false},
            {"preexistingGlErrors", errors(g)}};
  StateGuard guard(g);
  QOpenGLFramebufferObjectFormat format;
  format.setInternalTextureFormat(GL_RGBA8); format.setSamples(0);
  format.setAttachment(depthStencil ? QOpenGLFramebufferObject::CombinedDepthStencil
                                   : QOpenGLFramebufferObject::NoAttachment);
  s.fbo = std::make_unique<QOpenGLFramebufferObject>(size, format);
  s.info.insert("allocationErrors", errors(g));
  if (!s.fbo->isValid()) { s.info.insert("reason", "snapshot-allocation-failed"); return s; }
  g.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo->handle());
  s.info.insert("destinationStatus", int(g.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)));
  g.glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
  s.info.insert("sourceStatus", int(g.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)));
  if (source) {
    GLint type = 0, object = 0;
    g.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    g.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
    s.info.insert("sourceColorAttachmentType", type); s.info.insert("sourceColorAttachmentObject", object);
    if (type == GL_RENDERBUFFER && object) {
      GLint width = 0, height = 0, samples = 0, internalFormat = 0;
      g.glBindRenderbuffer(GL_RENDERBUFFER, GLuint(object));
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &internalFormat);
      s.info.insert("sourceColorPixelSize", QJsonArray{width, height});
      s.info.insert("sourceColorSamples", samples); s.info.insert("sourceColorInternalFormat", internalFormat);
    }
  }
  s.info.insert("sourceQueryErrors", errors(g));
  // Preserve the read-buffer selection belonging to the source FBO as well as
  // the caller's READ binding (they need not refer to the same FBO).
  GLint sourceReadBuffer = 0; g.glGetIntegerv(GL_READ_BUFFER, &sourceReadBuffer);
  g.glReadBuffer(source ? GL_COLOR_ATTACHMENT0 : GL_BACK);
  g.glBlitFramebuffer(0, 0, size.width(), size.height(), 0, 0, size.width(), size.height(),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
  const QJsonArray colorErrors = errors(g);
  s.info.insert("colorCopyErrors", colorErrors); s.info.insert("colorValid", colorErrors.isEmpty());
  if (depthStencil) {
    GLint type = 0, object = 0, sourceFormat = 0, stencilType = 0, destinationFormat = 0;
    g.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    g.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
    if (type == GL_RENDERBUFFER && object) {
      g.glBindRenderbuffer(GL_RENDERBUFFER, GLuint(object));
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &sourceFormat);
      GLint width = 0, height = 0, samples = 0;
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
      s.info.insert("sourceDepthPixelSize", QJsonArray{width, height}); s.info.insert("sourceDepthSamples", samples);
    }
    s.info.insert("depthAttachmentType", type); s.info.insert("depthAttachmentObject", object);
    s.info.insert("sourceDepthInternalFormat", sourceFormat);
    g.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilType);
    g.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
    if (object) {
      g.glBindRenderbuffer(GL_RENDERBUFFER, GLuint(object));
      g.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &destinationFormat);
    }
    s.info.insert("stencilAttachmentType", stencilType);
    s.info.insert("destinationDepthInternalFormat", destinationFormat);
    s.info.insert("attachmentQueryErrors", errors(g));
    for (const auto &[key, mask] : {
           std::pair{"depth", GLbitfield(GL_DEPTH_BUFFER_BIT)},
           std::pair{"stencil", GLbitfield(GL_STENCIL_BUFFER_BIT)}}) {
      g.glBlitFramebuffer(0, 0, size.width(), size.height(), 0, 0, size.width(), size.height(), mask, GL_NEAREST);
      const QJsonArray e = errors(g);
      s.info.insert(QString::fromLatin1(key) + "CopyErrors", e);
      const bool attachmentPresent = mask == GL_DEPTH_BUFFER_BIT ? type != GL_NONE : stencilType != GL_NONE;
      s.info.insert(QString::fromLatin1(key) + "Valid", e.isEmpty() && attachmentPresent);
    }
  }
  g.glReadBuffer(GLenum(sourceReadBuffer));
  return s;
}

void saveSnapshot(GL &g, Snapshot &s, const QString &base) {
  if (!s.fbo || !s.fbo->isValid()) return;
  g.glBindFramebuffer(GL_FRAMEBUFFER, s.fbo->handle());
  g.glReadBuffer(GL_COLOR_ATTACHMENT0);
  if (s.info.value("colorValid").toBool()) {
    const QString path = base + "-color.png";
    const QImage image = s.fbo->toImage();
    const QJsonArray e = errors(g);
    const bool ok = e.isEmpty() && !image.isNull() && image.save(path);
    s.info.insert("colorReadErrors", e); s.info.insert("colorValid", ok);
    if (ok) s.info.insert("colorPath", path);
  }
  const int width = s.fbo->width(), height = s.fbo->height();
  g.glBindFramebuffer(GL_FRAMEBUFFER, s.fbo->handle());
  g.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  for (int i = 0; i < 6; ++i) g.glPixelStorei(StateGuard::packNames[i], i == 0 ? 1 : 0);
  if (s.info.value("depthValid").toBool()) {
    std::vector<float> depth(size_t(width) * size_t(height));
    g.glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
    const QJsonArray e = errors(g);
    QImage image(width, height, QImage::Format_Grayscale16);
    bool finite = true;
    double minimum = 1., maximum = 0.;
    for (int y = 0; y < height; ++y) {
      auto *row = reinterpret_cast<quint16 *>(image.scanLine(height - y - 1));
      for (int x = 0; x < width; ++x) {
        const float value = depth[size_t(y) * size_t(width) + size_t(x)];
        finite = finite && std::isfinite(value);
        if (std::isfinite(value)) { minimum = std::min(minimum, double(value)); maximum = std::max(maximum, double(value)); }
        row[x] = quint16(std::clamp(std::isfinite(value) ? double(value) : 0., 0., 1.) * 65535.);
      }
    }
    const QString png = base + "-depth.png", raw = base + "-depth.f32";
    QFile file(raw);
    const qint64 bytes = qint64(depth.size() * sizeof(float));
    const bool rawOk = e.isEmpty() && finite && file.open(QIODevice::WriteOnly) &&
        file.write(reinterpret_cast<const char *>(depth.data()), bytes) == bytes;
    const bool ok = rawOk && image.save(png);
    s.info.insert("depthReadErrors", e); s.info.insert("depthValid", ok);
    s.info.insert("depthFinite", finite); s.info.insert("depthMinimum", minimum); s.info.insert("depthMaximum", maximum);
    s.info.insert("depthRawEncoding", "native-float32-bottom-left");
    if (ok) { s.info.insert("depthPath", png); s.info.insert("depthRawPath", raw); }
  }
  if (s.info.value("stencilValid").toBool()) {
    g.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<uchar> stencil(size_t(width) * size_t(height));
    g.glReadPixels(0, 0, width, height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil.data());
    const QJsonArray e = errors(g);
    const QImage image(stencil.data(), width, height, width, QImage::Format_Grayscale8);
    const QString path = base + "-stencil.png";
    const bool ok = e.isEmpty() && image.mirrored().save(path);
    s.info.insert("stencilReadErrors", e); s.info.insert("stencilValid", ok);
    if (ok) s.info.insert("stencilPath", path);
  }
}
} // namespace

struct RenderFrameDiagnostics::Impl {
  QString root, folder, manifestPath;
  QJsonObject manifest;
  QJsonArray notes;
  QJsonArray skippedStages;
  std::vector<Snapshot> stages;
  QPointer<QOpenGLWidget> viewport;
  bool armed = false, recording = false, awaiting = false;
  quint64 serial = 0;
  quint64 skippedNoteCount = 0;
  qint64 allocated = 0;
  QSize size;
  void write() {
    if (manifestPath.isEmpty()) return;
    QSaveFile file(manifestPath);
    if (file.open(QIODevice::WriteOnly)) {
      file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)); file.commit();
    }
  }
};

RenderFrameDiagnostics::RenderFrameDiagnostics(QString root) : m(std::make_unique<Impl>()) { m->root = std::move(root); }
RenderFrameDiagnostics::~RenderFrameDiagnostics() = default;

bool RenderFrameDiagnostics::request(QOpenGLWidget *w, quint64 rendered, quint64 presentedSerial, const QJsonObject &scene) {
  if (m->armed || m->recording || m->awaiting || !w || !w->context()) return false;
  const QFileInfo output(m->root);
  if (!output.isAbsolute() || m->root.startsWith("\\\\") || m->root.startsWith("//")) return false;
  m->folder = QDir(m->root).filePath("capture-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!QDir().mkpath(m->folder + "/stages")) return false;
  m->manifestPath = m->folder + "/manifest.json";
  m->manifest = {{"schemaVersion", 1}, {"status", "armed"}, {"requestRenderedSerial", qint64(rendered)},
                 {"requestPresentedSerial", qint64(presentedSerial)}, {"requestSerialMatch", rendered == presentedSerial},
                 {"requestScene", scene}, {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                 {"maxStages", kMaxStages}, {"gpuBudgetBytes", kGpuBudget}, {"noRerender", true}};
  m->manifest.insert("applicationPath", QCoreApplication::applicationFilePath());
  m->manifest.insert("applicationVersion", QCoreApplication::applicationVersion());
  m->manifest.insert("qtVersion", QString::fromLatin1(qVersion()));
  m->manifest.insert("qtOpenGlEnvironment", qEnvironmentVariable("QT_OPENGL"));
  m->manifest.insert("qtRhiEnvironment", qEnvironmentVariable("QSG_RHI_BACKEND"));
  // This desktop read MUST precede even makeCurrent/defaultFramebufferObject.
  const QJsonObject before = desktop(w, m->folder + "/presented-before.png");
  m->manifest.insert("presentedBefore", before);
  if (!before.value("saved").toBool()) {
    m->manifest.insert("status", "rejected"); m->manifest.insert("reason", before.value("reason")); m->write(); return false;
  }
  m->viewport = w;
  m->size = QSize(qRound(w->width() * w->devicePixelRatioF()), qRound(w->height() * w->devicePixelRatioF()));
  {
    ContextGuard context(w);
    GL g;
    if (QOpenGLContext::currentContext() != w->context() || !g.initializeOpenGLFunctions()) {
      m->manifest.insert("status", "rejected"); m->manifest.insert("reason", "opengl-context-unavailable"); m->write(); return false;
    }
    const QJsonObject originalState = state(g);
    const auto glString = [&](GLenum name) {
      return QString::fromLatin1(reinterpret_cast<const char *>(g.glGetString(name)));
    };
    m->manifest.insert("glVendor", glString(GL_VENDOR));
    m->manifest.insert("glRenderer", glString(GL_RENDERER));
    m->manifest.insert("glVersion", glString(GL_VERSION));
    StateGuard guard(g);
    Snapshot existing = copyFramebuffer(g, w->defaultFramebufferObject(), m->size, false, "existing-before", {});
    existing.info.insert("state", originalState);
    saveSnapshot(g, existing, m->folder + "/existing-before");
    if (existing.info.value("colorValid").toBool()) {
      const QString target = m->folder + "/existing-before.png";
      if (QFile::rename(existing.info.value("colorPath").toString(), target)) existing.info.insert("colorPath", target);
    }
    m->manifest.insert("existingBefore", existing.info);
  }
  m->stages.clear(); m->notes = {}; m->skippedStages = {}; m->skippedNoteCount = 0;
  m->allocated = 0; m->armed = true; m->write(); return true;
}

bool RenderFrameDiagnostics::beginFrame(QOpenGLWidget *w, quint64 serial, const QJsonObject &scene) {
  if (!m->armed || m->viewport != w || QOpenGLContext::currentContext() != w->context()) return false;
  m->armed = false; m->recording = true; m->serial = serial;
  m->size = QSize(qRound(w->width() * w->devicePixelRatioF()), qRound(w->height() * w->devicePixelRatioF()));
  m->manifest.insert("stageFrameSerial", qint64(serial)); m->manifest.insert("frameScene", scene);
  m->manifest.insert("widgetFramebuffer", int(w->defaultFramebufferObject()));
  m->manifest.insert("widgetDpr", w->devicePixelRatioF()); m->manifest.insert("status", "capturing");
  m->manifest.insert("logicalWidgetSize", QJsonArray{w->width(), w->height()});
  return true;
}
bool RenderFrameDiagnostics::pendingFrame() const { return m->armed; }
bool RenderFrameDiagnostics::recording() const { return m->recording; }
void RenderFrameDiagnostics::note(const QString &name, const QJsonObject &meta) {
  if (!m->recording) return;
  if (m->notes.size() >= 256) { ++m->skippedNoteCount; return; }
  GL g; if (!g.initializeOpenGLFunctions()) return;
  const QJsonArray previousErrors = errors(g);
  const QJsonObject draw = drawState(g);
  m->notes.append(QJsonObject{{"name", name}, {"meta", meta}, {"state", draw},
      {"preexistingGlErrors", previousErrors}, {"queryGlErrors", errors(g)}});
}
void RenderFrameDiagnostics::stage(const QString &name, const QJsonObject &meta) {
  if (!m->recording) return;
  const qint64 bytes = qint64(m->size.width()) * m->size.height() * 8;
  if (m->stages.size() >= kMaxStages || bytes <= 0 || m->allocated + bytes > kGpuBudget) {
    m->manifest.insert("stageLimitReached", true);
    if (m->skippedStages.size() < 256)
      m->skippedStages.append(QJsonObject{{"name", name}, {"reason", "snapshot-count-or-byte-budget"}, {"requestedBytes", bytes}});
    return;
  }
  GL g; if (!g.initializeOpenGLFunctions()) return;
  GLint source = 0; g.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &source);
  m->stages.push_back(copyFramebuffer(g, GLuint(source), m->size, true, name, meta));
  m->allocated += bytes;
}
void RenderFrameDiagnostics::endFrame(quint64 serial) {
  if (!m->recording) return;
  m->recording = false; m->awaiting = true;
  m->manifest.insert("endFrameSerial", qint64(serial));
  m->manifest.insert("status", "awaiting-presentation");
}
void RenderFrameDiagnostics::presented(QOpenGLWidget *w, quint64 serial) {
  if (!m->awaiting || m->viewport != w) return;
  m->awaiting = false;
  m->manifest.insert("stagePresentedSerial", qint64(serial)); m->manifest.insert("stageSerialMatch", serial == m->serial);
  m->manifest.insert("presentedAfter", desktop(w, m->folder + "/presented-after.png"));
  ContextGuard context(w);
  GL g;
  if (QOpenGLContext::currentContext() == w->context() && g.initializeOpenGLFunctions()) {
    StateGuard guard(g);
    QJsonArray result;
    for (size_t i = 0; i < m->stages.size(); ++i) {
      auto &s = m->stages[i];
      QString safeName = s.info.value("name").toString();
      for (QChar &c : safeName) if (!c.isLetterOrNumber() && c != '-' && c != '_') c = '-';
      const QString base = m->folder + "/stages/" + QString::number(i).rightJustified(2, '0') + "-" + safeName.left(80);
      saveSnapshot(g, s, base); result.append(s.info);
    }
    m->manifest.insert("stages", result);
    m->stages.clear(); // release owned FBOs before restoring caller GL/context
  } else {
    m->manifest.insert("flushError", "opengl-context-unavailable");
  }
  m->manifest.insert("notes", m->notes);
  m->manifest.insert("skippedNoteCount", qint64(m->skippedNoteCount));
  m->manifest.insert("skippedStages", m->skippedStages);
  m->manifest.insert("gpuSnapshotBytes", m->allocated);
  m->manifest.insert("status", "complete");
  m->manifest.insert("completedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)); m->write();
}
QString RenderFrameDiagnostics::lastManifestPath() const { return m->manifestPath; }
void RenderFrameDiagnostics::release() {
  m->stages.clear(); m->armed = m->recording = m->awaiting = false;
}
} // namespace gsw
