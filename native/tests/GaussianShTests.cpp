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
QTEST_MAIN(GaussianShTests)
#include "GaussianShTests.moc"
