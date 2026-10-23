#include "PlyPointCloudLoader.h"
#include "QualityMipModelContract.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

class QualityMipPreviewTests final : public QObject {
  Q_OBJECT
private slots:
  void realPublisherFilesKeepPerSnapshotFilterAndActualGlValues() {
    const QString fixturePath=qEnvironmentVariable("GSW_QUALITY_MIP_PREVIEW_FIXTURE");
    if(fixturePath.isEmpty()) QSKIP("real Publisher preview not_run: set GSW_QUALITY_MIP_PREVIEW_FIXTURE");
    QFile file(fixturePath);QVERIFY(file.open(QIODevice::ReadOnly));
    const auto fixture=QJsonDocument::fromJson(file.readAll()).object();
    const auto mip=gsw::PlyPointCloudLoader::load(fixture["mip"].toString());
    const auto plain=gsw::PlyPointCloudLoader::load(fixture["plain"].toString());
    const auto surfel=gsw::PlyPointCloudLoader::load(fixture["surfel"].toString());
    for(const auto *data:{&mip,&plain,&surfel}) {
      QVERIFY2(data->isValid(),qPrintable(data->error));
      QCOMPARE(data->sourceVertexCount,qint64(2));QVERIFY(data->hasGaussianAttributes);
    }
    QVERIFY(mip.qualityMip.enabled);QVERIFY(!mip.hasSurfelAttributes);
    QCOMPARE(mip.qualityMip.metadata["raw_source_kind"].toString(),QStringLiteral("preview_snapshot_rows_float32"));
    QCOMPARE(mip.qualityMip.metadata["raw_source_sha256"],fixture["raw_sample_sha256"]);
    QVERIFY(!plain.qualityMip.enabled);QVERIFY(!plain.hasSurfelAttributes);
    QVERIFY(!surfel.qualityMip.enabled);QVERIFY(surfel.hasSurfelAttributes);
    // This tests the actual renderer's decision, not IPC attachment/transport.
    QVERIFY(mip.qualityMip.appliesToRasterFrame(false,false));
    QVERIFY(!mip.qualityMip.appliesToRasterFrame(true,false));
    QVERIFY(!mip.qualityMip.appliesToRasterFrame(false,true));

    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context;context.setFormat(format);
    if(!context.create()) QSKIP("native preview OpenGL not_run: context unavailable");
    QOffscreenSurface surface;surface.setFormat(context.format());surface.create();
    if(!surface.isValid()||!context.makeCurrent(&surface)) QSKIP("native preview OpenGL not_run: surface unavailable");
    auto *gl=context.extraFunctions();gl->initializeOpenGLFunctions();
    qInfo()<<"Actual native preview GL renderer:"<<reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER));
    QOpenGLShaderProgram program;
    QVERIFY2(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
        "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}"),qPrintable(program.log()));
    QVERIFY2(program.addShaderFromSourceCode(QOpenGLShader::Fragment,
        QByteArray("#version 330 core\n")+gsw::qualityMipProjectionGlsl()+R"GLSL(
uniform bool fullMip;
uniform bool imagePass;
uniform vec3 rawCovariance;
uniform float rawOpacity;
out vec4 color;
void main(){
  float opacity=rawOpacity;
  vec3 covariance=gswApplyGaussian2dFilter(rawCovariance,opacity,fullMip);
  if(!imagePass){color=vec4(covariance,opacity);return;}
  vec2 delta=gl_FragCoord.xy-vec2(68.0,54.0);
  float determinant=covariance.x*covariance.z-covariance.y*covariance.y;
  float power=-0.5*(covariance.z*delta.x*delta.x-2.0*covariance.y*delta.x*delta.y+covariance.x*delta.y*delta.y)/determinant;
  float alpha=opacity*exp(power);color=vec4(vec3(0.8,0.4,0.2)*alpha,alpha);
}
)GLSL"),qPrintable(program.log()));
    QVERIFY2(program.link(),qPrintable(program.log()));QVERIFY(program.bind());
    QOpenGLFramebufferObjectFormat fboFormat;fboFormat.setInternalTextureFormat(GL_RGBA32F);
    QOpenGLFramebufferObject sample(1,1,fboFormat);QVERIFY(sample.isValid());
    QOpenGLFramebufferObject image(128,96,fboFormat);QVERIFY(image.isValid());
    GLuint vao=0;gl->glGenVertexArrays(1,&vao);gl->glBindVertexArray(vao);
    gl->glDisable(GL_BLEND);gl->glDisable(GL_DEPTH_TEST);
    const QString base=qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT","D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));QTemporaryDir evidence(QDir(base).filePath("publisher-gl-XXXXXX"));
    QVERIFY(evidence.isValid());evidence.setAutoRemove(false);
    // Real file snapshots are loaded in succession. The contract is replaced
    // with each frame, exactly as SceneState does after its generation guard.
    for(const auto *data:{&mip,&plain,&surfel,&mip}) {
      const bool enabled=data->qualityMip.appliesToRasterFrame(false,data->hasSurfelAttributes);
      QCOMPARE(enabled,data==&mip);
      if(data->hasSurfelAttributes) continue; // 2DGS uses its existing surfel shader.
      const auto &vertex=data->vertices.at(0);
      // Independent fixed camera: fx80/fy60, point(.1,.2,2), identity rotation.
      // Raw variances(.0004,.0009,.0016), 3D width .01 -> (.0005,.001,.0017).
      const float xx=1600*vertex.scaleX*vertex.scaleX+4*vertex.scaleZ*vertex.scaleZ;
      const float xy=6*vertex.scaleZ*vertex.scaleZ;
      const float yy=900*vertex.scaleY*vertex.scaleY+9*vertex.scaleZ*vertex.scaleZ;
      QVERIFY(std::abs(vertex.opacity-(enabled?.49391592521042543:.6))<2e-6);
      program.setUniformValue("fullMip",enabled);program.setUniformValue("rawCovariance",QVector3D(xx,xy,yy));
      program.setUniformValue("rawOpacity",vertex.opacity);program.setUniformValue("imagePass",false);
      QVERIFY(sample.bind());gl->glViewport(0,0,1,1);gl->glDrawArrays(GL_TRIANGLES,0,3);
      float rgba[4]{};gl->glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,rgba);
      const double expected[4]={enabled?1.1068:.7364,enabled?.0102:.0096,enabled?1.2153:.9144,enabled?.36595491309940537:.6};
      for(int channel=0;channel<4;++channel) QVERIFY(std::abs(rgba[channel]-expected[channel])<3e-6);
      QVERIFY(image.bind());gl->glViewport(0,0,128,96);program.setUniformValue("imagePass",true);
      gl->glDrawArrays(GL_TRIANGLES,0,3);
      const QString path=evidence.filePath(enabled?"publisher-full-mip.png":"publisher-legacy.png");
      QVERIFY(image.toImage().save(path));qInfo().noquote()<<"Actual Publisher native GL PNG:"<<path;
    }
    gl->glBindVertexArray(0);gl->glDeleteVertexArrays(1,&vao);
    QCOMPARE(gl->glGetError(),GLenum(GL_NO_ERROR));program.release();context.doneCurrent();
  }
};
QTEST_MAIN(QualityMipPreviewTests)
#include "QualityMipPreviewTests.moc"
