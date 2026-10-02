#include "GaussianGpuBuffer.h"
#include <QFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QTest>
#include <cmath>
#include <numbers>

// Independent reference: associated Legendre recurrence, not the upstream
// polynomial implementation. Real orthonormal SH, Condon-Shortley phase.
static double basis(int l, int m, QVector3D direction) {
  direction.normalize();
  const double z = direction.z(), phi = std::atan2(direction.y(), direction.x());
  const int a = std::abs(m);
  double p = 1.0;
  for (int i = 1; i <= a; ++i) p *= -(2 * i - 1) * std::sqrt(std::max(0.0, 1 - z * z));
  if (l > a) {
    double previous = p;
    p = z * (2 * a + 1) * p;
    for (int n = a + 2; n <= l; ++n) {
      const double next = ((2 * n - 1) * z * p - (n + a - 1) * previous) / (n - a);
      previous = p; p = next;
    }
  }
  const double normal = std::sqrt((2 * l + 1) / (4 * std::numbers::pi) *
      std::tgamma(l - a + 1) / std::tgamma(l + a + 1));
  return normal * p * (m == 0 ? 1.0 : std::sqrt(2.0) *
      (m < 0 ? std::sin(a * phi) : std::cos(a * phi)));
}

class GaussianShTests : public QObject {
  Q_OBJECT
private slots:
  void evaluatesAllBandsAndKeepsSourceMapping();
  void intersectsSurfelsWithPerspectiveAndOrthographicProjection();
  void surfelOcclusionUsesSurfaceDepthRatherThanCenterDepth();
};

void GaussianShTests::evaluatesAllBandsAndKeepsSourceMapping() {
  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;
  context.setFormat(format);
  QVERIFY(context.create());
  QOffscreenSurface surface;
  surface.setFormat(context.format()); surface.create();
  QVERIFY(context.makeCurrent(&surface));
  auto *gl = context.extraFunctions();
  gl->initializeOpenGLFunctions();
  QFile file(QStringLiteral(":/shaders/evaluate_sh.glsl"));
  QVERIFY(file.open(QIODevice::ReadOnly));
  QOpenGLShaderProgram program;
  QVERIFY(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
      "#version 330 core\nvoid main(){ vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); gl_Position=vec4(p*2.0-1.0,0,1); }"));
  QVERIFY2(program.addShaderFromSourceCode(QOpenGLShader::Fragment,
      QByteArray("#version 330 core\n") + file.readAll() +
      "\nuniform samplerBuffer gaussianAttributes; uniform int record; uniform vec3 direction; out vec4 color;\n"
      "void main(){ vec4 d=texelFetch(gaussianAttributes,record*4+3);"
      "color=vec4(d.w>0.5 ? texelFetch(gaussianAttributes,record*4+1).rgb : evaluateSh(int(d.z),direction),1); }"),
      qPrintable(program.log()));
  QVERIFY2(program.link(), qPrintable(program.log()));
  QOpenGLFramebufferObjectFormat fbFormat;
  fbFormat.setInternalTextureFormat(GL_RGBA32F);
  QOpenGLFramebufferObject framebuffer(1, 1, fbFormat);
  QVERIFY(framebuffer.isValid());
  QVERIFY(framebuffer.bind());
  gl->glViewport(0, 0, 1, 1);
  gl->glDisable(GL_BLEND);
  gl->glDisable(GL_DEPTH_TEST);
  gsw::GaussianGpuBuffer buffer;
  buffer.initialize();
  QVERIFY(buffer.supports(25));
  gsw::GaussianShData sh;
  sh.degree = 4;
  sh.coefficients.fill(0.0F, 25 * 25 * 3);
  QVector<gsw::PointCloudVertex> vertices(25);
  for (int p = 0; p < 25; ++p) {
    vertices[p].sourceIndex = p * 3 + 2;
    sh.sourceIndices.append(vertices[p].sourceIndex);
    for (int c = 0; c < 3; ++c)
      sh.coefficients[(p * 25 + p) * 3 + c] = c == 0 ? 0.19F : c == 1 ? -0.17F : 0.11F;
  }
  QBitArray selected(80);
  buffer.setVertices(vertices, sh, selected);
  buffer.upload();
  QCOMPARE(buffer.shDegree(), 4);
  const auto uploads = buffer.shUploads();
  const QVector<QVector3D> directions{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
      {0.3F,0.5F,0.7F},{-0.7F,0.2F,-0.1F},{0.2F,-0.8F,0.3F}};
  for (int pass = 0; pass < 3; ++pass) {
    if (pass == 1) std::reverse(vertices.begin(), vertices.end());
    if (pass == 2) {
      vertices.remove(4, 6); // partial deletion; remaining SH must not shift
      selected.setBit(vertices[3].sourceIndex);
      vertices[3].red = 1.0F; vertices[3].green = 0.72F; vertices[3].blue = 0.16F;
    }
    buffer.setVertices(vertices, sh, selected);
    buffer.upload(); buffer.bind();
    QCOMPARE(buffer.shUploads(), uploads);
    QVERIFY(program.bind());
    program.setUniformValue("gaussianAttributes", 2);
    program.setUniformValue("gaussianSh", 4);
    program.setUniformValue("shCoefficientCount", 25);
    for (int degree = 0; degree <= 4; ++degree) {
      program.setUniformValue("shDegree", degree);
      for (qsizetype r = 0; r < vertices.size(); ++r) {
        program.setUniformValue("record", int(r));
        const int coefficient = (vertices[r].sourceIndex - 2) / 3;
        const int l = int(std::sqrt(coefficient)), m = coefficient - l * l - l;
        for (const auto &direction : directions) {
          program.setUniformValue("direction", direction);
          gl->glDrawArrays(GL_TRIANGLES, 0, 3);
          float pixel[4]{};
          gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel);
          for (int c = 0; c < 3; ++c) {
            double expected = std::max(0.0, 0.5 + (l <= degree ? basis(l, m, direction) *
                sh.coefficients.at((coefficient * 25 + coefficient) * 3 + c) : 0.0));
            if (selected.testBit(vertices[r].sourceIndex))
              expected = c == 0 ? vertices[r].red : c == 1 ? vertices[r].green : vertices[r].blue;
            QVERIFY2(std::abs(pixel[c] - expected) < 2e-5,
                qPrintable(QString("pass %1 SH %2 coefficient %3 channel %4: %5 != %6")
                    .arg(pass).arg(degree).arg(coefficient).arg(c).arg(pixel[c]).arg(expected)));
          }
        }
      }
    }
    program.release(); buffer.unbind();
  }
  buffer.clear(); buffer.upload();
  QCOMPARE(buffer.shDegree(), -1);
  QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR));
  buffer.release();
}
void GaussianShTests::intersectsSurfelsWithPerspectiveAndOrthographicProjection() {
  QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context; context.setFormat(format); QVERIFY(context.create());
  QOffscreenSurface surface; surface.setFormat(context.format()); surface.create();
  QVERIFY(context.makeCurrent(&surface));
  auto *gl = context.extraFunctions(); gl->initializeOpenGLFunctions();
  QFile file(QStringLiteral(":/shaders/evaluate_surfel.glsl"));
  QVERIFY(file.open(QIODevice::ReadOnly));
  QOpenGLShaderProgram program;
  QVERIFY(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
      "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}"));
  QVERIFY2(program.addShaderFromSourceCode(QOpenGLShader::Fragment,
      QByteArray("#version 330 core\n") + file.readAll() +
      "\nuniform vec3 Tu,Tv,Tw;uniform vec2 pixel;uniform bool bounds;out vec4 color;"
      "void main(){vec2 uv=vec2(0),center=vec2(0),extent=vec2(0);float depth=0;"
      "if(bounds){bool ok=surfelBounds(Tu,Tv,Tw,center,extent);color=ok?vec4(center,extent):vec4(-9999);}"
      "else{bool ok=surfelIntersection(pixel,Tu,Tv,Tw,uv,depth);"
      "color=ok?vec4(uv,depth,surfelPower(uv,pixel,pixel)):vec4(-9999);}}"), qPrintable(program.log()));
  QVERIFY2(program.link(), qPrintable(program.log()));
  QOpenGLFramebufferObjectFormat fbFormat; fbFormat.setInternalTextureFormat(GL_RGBA32F);
  QOpenGLFramebufferObject framebuffer(1, 1, fbFormat);
  QVERIFY(framebuffer.isValid()); QVERIFY(framebuffer.bind());
  GLuint vao = 0; gl->glGenVertexArrays(1, &vao); gl->glBindVertexArray(vao);
  gl->glViewport(0, 0, 1, 1); gl->glDisable(GL_BLEND); gl->glDisable(GL_DEPTH_TEST);
  QVERIFY(program.bind());
  // Forward project known tangent-plane points, then invert on the actual GPU.
  // Unlike a covariance approximation this must remain exact off center.
  for (const auto Tw : {QVector3D(0, 0, 4), QVector3D(0.8F, 0.2F, 4), QVector3D(-0.7F, 0.3F, 4)}) {
    const QVector3D Tu(23, 0, 200), Tv(0, 11, 120);
    program.setUniformValue("Tu", Tu); program.setUniformValue("Tv", Tv); program.setUniformValue("Tw", Tw);
    for (const QVector2D uv : {QVector2D(0,0), QVector2D(1.7F,-0.8F), QVector2D(-2.1F,1.4F)}) {
      const QVector3D h(uv.x(), uv.y(), 1);
      const float depth = QVector3D::dotProduct(Tw, h);
      const QVector2D pixel(QVector3D::dotProduct(Tu, h) / depth, QVector3D::dotProduct(Tv, h) / depth);
      program.setUniformValue("pixel", pixel); program.setUniformValue("bounds", false);
      gl->glDrawArrays(GL_TRIANGLES, 0, 3); float result[4]{};
      gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, result);
      QVERIFY(std::abs(result[0] - uv.x()) < 1e-4F);
      QVERIFY(std::abs(result[1] - uv.y()) < 1e-4F);
      QVERIFY(std::abs(result[2] - depth) < 1e-4F);
    }
    program.setUniformValue("bounds", true); gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    float rectangle[4]{}; gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, rectangle);
    for (int i = 0; i < 128; ++i) {
      const float angle = float(i * 2 * std::numbers::pi / 128);
      const QVector3D h(3 * std::cos(angle), 3 * std::sin(angle), 1);
      const float depth = QVector3D::dotProduct(Tw, h);
      const float x = QVector3D::dotProduct(Tu, h) / depth;
      const float y = QVector3D::dotProduct(Tv, h) / depth;
      QVERIFY(std::abs(x - rectangle[0]) <= rectangle[2] + 1e-3F);
      QVERIFY(std::abs(y - rectangle[1]) <= rectangle[3] + 1e-3F);
    }
  }
  program.setUniformValue("Tw", QVector3D(4, 0, 1)); // support crosses projection pole
  program.setUniformValue("bounds", true); gl->glDrawArrays(GL_TRIANGLES, 0, 3);
  float pole[4]{}; gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pole);
  QCOMPARE(pole[0], -9999.0F);
  QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR));
  gl->glDeleteVertexArrays(1, &vao);
}
void GaussianShTests::surfelOcclusionUsesSurfaceDepthRatherThanCenterDepth() {
  QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context; context.setFormat(format); QVERIFY(context.create());
  QOffscreenSurface surface; surface.setFormat(context.format()); surface.create();
  QVERIFY(context.makeCurrent(&surface));
  auto *gl = context.extraFunctions(); gl->initializeOpenGLFunctions();
  QFile file(QStringLiteral(":/shaders/evaluate_surfel.glsl")); QVERIFY(file.open(QIODevice::ReadOnly));
  QOpenGLShaderProgram program;
  QVERIFY(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
      "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}"));
  QVERIFY2(program.addShaderFromSourceCode(QOpenGLShader::Fragment,
      QByteArray("#version 330 core\n") + file.readAll() +
      "\nuniform bool opaque;uniform float opaqueDepth;uniform vec3 Tu,Tv,Tw,cameraDepth;"
      "uniform vec2 pixel;uniform mat4 projection;out vec4 color;void main(){"
      "if(opaque){color=vec4(0,1,0,1);gl_FragDepth=opaqueDepth;}else{"
      "vec2 uv;float depth;if(!surfelIntersection(pixel,Tu,Tv,Tw,uv,depth))discard;"
      "gl_FragDepth=surfelFragmentDepth(uv,cameraDepth,projection);color=vec4(1,0,0,1);}}"), qPrintable(program.log()));
  QVERIFY2(program.link(), qPrintable(program.log()));
  QOpenGLFramebufferObjectFormat fbFormat; fbFormat.setAttachment(QOpenGLFramebufferObject::Depth);
  QOpenGLFramebufferObject framebuffer(1, 1, fbFormat); QVERIFY(framebuffer.isValid()); QVERIFY(framebuffer.bind());
  GLuint vao = 0; gl->glGenVertexArrays(1, &vao); gl->glBindVertexArray(vao);
  gl->glViewport(0, 0, 1, 1); gl->glDisable(GL_BLEND); gl->glEnable(GL_DEPTH_TEST); gl->glDepthFunc(GL_LESS);
  QVERIFY(program.bind());
  for (bool orthographic : {false, true}) {
    QMatrix4x4 projection;
    if (orthographic) projection.ortho(-4, 4, -4, 4, 0.1F, 100.0F);
    else projection.perspective(46.0F, 1.0F, 0.1F, 100.0F);
    const QVector4D clipU = projection * QVector4D(1, 0, -0.8F, 0);
    const QVector4D clipV = projection * QVector4D(0, 1, 0, 0);
    const QVector4D clipC = projection * QVector4D(0, 0, -4, 1);
    const QVector3D Tw(clipU.w(), clipV.w(), clipC.w());
    const QVector3D Tu = (QVector3D(clipU.x(), clipV.x(), clipC.x()) + Tw) * 50;
    const QVector3D Tv = (QVector3D(clipU.y(), clipV.y(), clipC.y()) + Tw) * 50;
    program.setUniformValue("projection", projection); program.setUniformValue("Tu", Tu);
    program.setUniformValue("Tv", Tv); program.setUniformValue("Tw", Tw);
    program.setUniformValue("cameraDepth", QVector3D(0.8F, 0, 4));
    program.setUniformValue("opaqueDepth", 0.5F * (clipC.z() / clipC.w() + 1));
    for (float u : {-1.0F, 1.0F}) {
      const QVector3D h(u, 0, 1);
      const QVector2D pixel(QVector3D::dotProduct(Tu, h) / QVector3D::dotProduct(Tw, h),
                           QVector3D::dotProduct(Tv, h) / QVector3D::dotProduct(Tw, h));
      program.setUniformValue("pixel", pixel);
      gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      program.setUniformValue("opaque", true); gl->glDrawArrays(GL_TRIANGLES, 0, 3);
      program.setUniformValue("opaque", false); gl->glDrawArrays(GL_TRIANGLES, 0, 3);
      float result[4]{}; gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, result);
      QCOMPARE(result[0], u < 0 ? 1.0F : 0.0F);
      QCOMPARE(result[1], u < 0 ? 0.0F : 1.0F);
    }
  }
  QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR)); gl->glDeleteVertexArrays(1, &vao);
}
QTEST_MAIN(GaussianShTests)
#include "GaussianShTests.moc"
