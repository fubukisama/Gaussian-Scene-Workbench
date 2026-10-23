#include "QualityMipModelContract.h"

#include <QCryptographicHash>
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

namespace {
bool writeBytes(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QString digest(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QJsonObject fusedContract(const QByteArray &model) {
  return {{"schema", "gsw.quality_model/1"}, {"representation", "fused"}, {"filter", "full_mip"},
      {"filter_3d_applied", true}, {"filter_2d", QJsonObject{{"variance", .3}, {"opacity_compensation", true}}},
      {"raw_source_sha256", QString(64, 'a')}, {"fused_sha256", digest(model)}, {"plan_hash", QString(64, 'b')}};
}
}

class QualityMipViewportTests final : public QObject {
  Q_OBJECT
private slots:
  void acceptsOnlyHashBoundFusedContract() {
    const QString base = qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT",
        "D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));
    QTemporaryDir temporary(QDir(base).filePath("owned-contract-XXXXXX"));
    QVERIFY(temporary.isValid());
    const QByteArray model("ply\nformat ascii 1.0\nend_header\n");
    const QString path = temporary.filePath("model.ply");
    QVERIFY(writeBytes(path, model));
    const auto plain = gsw::QualityMipModelContract::load(path, true, false, false);
    QVERIFY(plain.errorCode.isEmpty());
    QVERIFY(!plain.enabled);
    const QString sidecar = temporary.filePath("model.quality.json");
    QVERIFY(writeBytes(sidecar, QJsonDocument(fusedContract(model)).toJson()));
    const auto fused = gsw::QualityMipModelContract::load(path, true, false, false);
    QVERIFY2(fused.errorCode.isEmpty(), qPrintable(fused.errorCode));
    QVERIFY(fused.enabled);
    QCOMPARE(fused.pixelVariance, .3F);
    QVERIFY(fused.opacityCompensation);
    QVERIFY(writeBytes(path, model + "changed\n"));
    const auto changed = gsw::QualityMipModelContract::load(path, true, false, false);
    QCOMPARE(changed.errorCode, QStringLiteral("quality_mip_model_hash_mismatch"));
    QVERIFY(!changed.enabled);
  }

  void rejectsRawDoubleApplicationMissingSidecarAndSurfel() {
    const QString base = qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT",
        "D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));
    QTemporaryDir temporary(QDir(base).filePath("owned-reject-XXXXXX"));
    const QByteArray model("ply\nformat ascii 1.0\nend_header\n");
    const QString path = temporary.filePath("model.ply");
    QVERIFY(writeBytes(path, model));
    QCOMPARE(gsw::QualityMipModelContract::load(path, true, false, false, true).errorCode,
        QStringLiteral("quality_mip_sidecar_required"));
    QCOMPARE(gsw::QualityMipModelContract::load(path, true, false, true).errorCode,
        QStringLiteral("quality_mip_raw_requires_fusion"));
    auto contract = fusedContract(model);
    contract["representation"] = "raw";
    contract["raw_source_sha256"] = digest(model);
    contract["filter_3d_applied"] = false;
    QVERIFY(writeBytes(temporary.filePath("model.quality.json"), QJsonDocument(contract).toJson()));
    QCOMPARE(gsw::QualityMipModelContract::load(path, true, false, false).errorCode,
        QStringLiteral("quality_mip_raw_requires_fusion"));
    QVERIFY(writeBytes(temporary.filePath("model.quality.json"), QJsonDocument(fusedContract(model)).toJson()));
    QCOMPARE(gsw::QualityMipModelContract::load(path, true, false, true).errorCode,
        QStringLiteral("quality_mip_double_application_risk"));
    QCOMPARE(gsw::QualityMipModelContract::load(path, true, true, false).errorCode,
        QStringLiteral("quality_mip_representation_unsupported"));
  }

  void rejectsNonTrainingOrUnknownWidthProvenance() {
    const QString base = qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT",
        "D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));
    QTemporaryDir temporary(QDir(base).filePath("owned-widths-XXXXXX"));
    QVERIFY(temporary.isValid());
    const QByteArray model("ply\nformat ascii 1.0\nend_header\n");
    const QByteArray widths("known widths");
    const QString path=temporary.filePath("model.ply");
    QVERIFY(writeBytes(path,model));QVERIFY(writeBytes(temporary.filePath("widths.npy"),widths));
    auto contract=fusedContract(model);
    for (const auto &extra : {QJsonObject{{"frame","world"}},QJsonObject{{"unknown",true}}}) {
      QJsonObject provenance{{"path","widths.npy"},{"sha256",digest(widths)}};
      for (auto iterator=extra.begin();iterator!=extra.end();++iterator) provenance[iterator.key()]=iterator.value();
      contract["filter_widths"]=provenance;
      QVERIFY(writeBytes(temporary.filePath("model.quality.json"),QJsonDocument(contract).toJson()));
      QCOMPARE(gsw::QualityMipModelContract::load(path,true,false,false).errorCode,
          QStringLiteral("quality_mip_filter_widths_invalid"));
    }
  }

  void freshPairedExportRebindsFusedBytesPreservesProvenanceAndNeverOverwrites() {
    const QString base = qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT",
        "D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));
    QTemporaryDir temporary(QDir(base).filePath("owned-export-XXXXXX"));
    const QByteArray source("ply\nformat ascii 1.0\nend_header\nsource\n");
    const QString sourcePath=temporary.filePath("source.ply");
    QVERIFY(writeBytes(sourcePath,source));
    const QByteArray widths("owned source-width provenance");
    QVERIFY(writeBytes(temporary.filePath("widths.npy"),widths));
    auto metadata=fusedContract(source);
    metadata["filter_widths"]=QJsonObject{{"path","widths.npy"},{"sha256",digest(widths)},{"frame","training"}};
    QVERIFY(writeBytes(temporary.filePath("source.quality.json"),QJsonDocument(metadata).toJson()));
    const auto contract=gsw::QualityMipModelContract::load(sourcePath,true,false,false);
    QVERIFY2(contract.enabled,qPrintable(contract.errorCode));
    const QByteArray cropped("ply\nformat ascii 1.0\ncomment GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1\nend_header\ncropped\n");
    const QString output=temporary.filePath("cropped.ply");
    QString error;
    gsw::QualityMipExportTransaction transaction(contract,output);
    QVERIFY2(transaction.open(error),qPrintable(error));
    QCOMPARE(transaction.device().write(cropped),cropped.size());
    QVERIFY2(transaction.commit(error),qPrintable(error));
    const auto reopened=gsw::QualityMipModelContract::load(output,true,false,false,true);
    QVERIFY2(reopened.enabled,qPrintable(reopened.errorCode));
    QCOMPARE(reopened.metadata["fused_sha256"].toString(),digest(cropped));
    QCOMPARE(reopened.metadata["raw_source_sha256"],metadata["raw_source_sha256"]);
    QCOMPARE(reopened.metadata["plan_hash"],metadata["plan_hash"]);
    QVERIFY(!reopened.metadata.contains("filter_widths"));
    QCOMPARE(reopened.metadata["source_filter_widths_sha256"].toString(),digest(widths));
    gsw::QualityMipExportTransaction duplicate(contract,output);
    QVERIFY(!duplicate.open(error));
    QCOMPARE(error,QStringLiteral("quality_mip_export_requires_fresh_destination"));
    const QString sidecarOnly=temporary.filePath("existing.quality.json");
    QVERIFY(writeBytes(sidecarOnly,"foreign bytes"));
    gsw::QualityMipExportTransaction foreign(contract,temporary.filePath("existing.ply"));
    QVERIFY(!foreign.open(error));
    QFile existing(sidecarOnly);QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(),QByteArray("foreign bytes"));
    QFile original(sourcePath);QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(),source);
  }

  void failedPairNeverPublishesOrOverwritesForeignOutputs() {
    const QString base = qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT",
        "D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));
    QTemporaryDir temporary(QDir(base).filePath("owned-failure-XXXXXX"));
    QVERIFY(temporary.isValid());
    const QByteArray source("ply\nformat ascii 1.0\nend_header\nsource\n");
    const QString sourcePath=temporary.filePath("source.ply");
    QVERIFY(writeBytes(sourcePath,source));
    QVERIFY(writeBytes(temporary.filePath("source.quality.json"),QJsonDocument(fusedContract(source)).toJson()));
    const auto contract=gsw::QualityMipModelContract::load(sourcePath,true,false,false);
    QVERIFY(contract.enabled);
    QString error;
    {
      gsw::QualityMipExportTransaction missingMarker(contract,temporary.filePath("invalid.ply"));
      QVERIFY(missingMarker.open(error));
      QCOMPARE(missingMarker.device().write(source),source.size());
      QVERIFY(!missingMarker.commit(error));
      QCOMPARE(error,QStringLiteral("quality_mip_export_marker_required"));
    }
    QVERIFY(!QFileInfo::exists(temporary.filePath("invalid.ply")));
    QVERIFY(!QFileInfo::exists(temporary.filePath("invalid.quality.json")));
    const QByteArray marked("ply\nformat ascii 1.0\ncomment GSW_QUALITY_REQUIRES_SIDECAR full_mip_fused_v1\nend_header\ncopy\n");
    {
      gsw::QualityMipExportTransaction appearedAfterOpen(contract,temporary.filePath("raced.ply"));
      QVERIFY(appearedAfterOpen.open(error));
      QCOMPARE(appearedAfterOpen.device().write(marked),marked.size());
      QVERIFY(writeBytes(temporary.filePath("raced.quality.json"),"foreign"));
      QVERIFY(!appearedAfterOpen.commit(error));
      QCOMPARE(error,QStringLiteral("quality_mip_export_requires_fresh_destination"));
    }
    QVERIFY(!QFileInfo::exists(temporary.filePath("raced.ply")));
    QFile foreign(temporary.filePath("raced.quality.json"));
    QVERIFY(foreign.open(QIODevice::ReadOnly));
    QCOMPARE(foreign.readAll(),QByteArray("foreign"));
    QVERIFY(QDir(temporary.path()).entryList({".gsw-*"},QDir::Files|QDir::Hidden).isEmpty());
  }

  void actualOffscreenGlFilterAndSameCameraImage() {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) QSKIP("native OpenGL not_run: offscreen context unavailable");
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface))
      QSKIP("native OpenGL not_run: offscreen surface unavailable");
    auto *gl = context.extraFunctions();
    gl->initializeOpenGLFunctions();
    qInfo() << "Actual native GL renderer:" << reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER));
    QOpenGLShaderProgram program;
    QVERIFY2(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
        "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}"), qPrintable(program.log()));
    QVERIFY2(program.addShaderFromSourceCode(QOpenGLShader::Fragment,
        QByteArray("#version 330 core\n") + gsw::qualityMipProjectionGlsl() + R"GLSL(
uniform bool fullMip;
uniform bool imagePass;
uniform vec3 rawCovariance;
uniform float rawOpacity;
uniform vec2 center;
out vec4 color;
void main(){
  float opacity=rawOpacity;
  vec3 covariance=gswApplyGaussian2dFilter(rawCovariance,opacity,fullMip);
  if(!imagePass){color=vec4(covariance,opacity);return;}
  vec2 delta=gl_FragCoord.xy-center;
  float determinant=covariance.x*covariance.z-covariance.y*covariance.y;
  float power=-0.5*(covariance.z*delta.x*delta.x-2.0*covariance.y*delta.x*delta.y+covariance.x*delta.y*delta.y)/determinant;
  float alpha=opacity*exp(power);
  color=vec4(vec3(0.8,0.4,0.2)*alpha,alpha);
}
)GLSL"), qPrintable(program.log()));
    QVERIFY2(program.link(), qPrintable(program.log()));
    QOpenGLFramebufferObjectFormat fboFormat;
    fboFormat.setInternalTextureFormat(GL_RGBA32F);
    QOpenGLFramebufferObject sample(1, 1, fboFormat);
    QVERIFY(sample.isValid());
    QVERIFY(sample.bind());
    GLuint vao = 0;
    gl->glGenVertexArrays(1, &vao);
    gl->glBindVertexArray(vao);
    gl->glViewport(0, 0, 1, 1);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_DEPTH_TEST);
    QVERIFY(program.bind());
    program.setUniformValue("rawCovariance", QVector3D(.4F, .1F, .7F));
    program.setUniformValue("rawOpacity", .6F);
    program.setUniformValue("imagePass", false);
    for (const bool mip : {false, true}) {
      program.setUniformValue("fullMip", mip);
      gl->glDrawArrays(GL_TRIANGLES, 0, 3);
      float rgba[4]{};
      gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, rgba);
      QVERIFY(std::abs(rgba[0] - (mip ? .7 : .49)) < 2e-6);
      QVERIFY(std::abs(rgba[1] - .1) < 2e-6);
      QVERIFY(std::abs(rgba[2] - (mip ? 1. : .79)) < 2e-6);
      QVERIFY(std::abs(rgba[3] - (mip ? .6 * std::sqrt(.27 / .69) : .6)) < 2e-6);
    }
    const QString base = qEnvironmentVariable("GSW_QUALITY_MIP_TEST_ROOT",
        "D:/Codex/validation/gsw-quality-upgrade-20261023/native-mip-evidence");
    QVERIFY(QDir().mkpath(base));
    QTemporaryDir evidence(QDir(base).filePath("gpu-XXXXXX"));
    QVERIFY(evidence.isValid());
    evidence.setAutoRemove(false);
    QOpenGLFramebufferObject image(128, 96, fboFormat);
    QVERIFY(image.isValid());
    QVERIFY(image.bind());
    gl->glViewport(0, 0, 128, 96);
    // One fixed 3D camera: fx80/fy60, center(64,48), point(.1,.2,2),
    // diagonal sigma(.02,.03,.04). Independent J*Sigma*J^T literals:
    // covariance(.6464,.0096,.8244), projected center(68,54).
    program.setUniformValue("rawCovariance", QVector3D(.6464F, .0096F, .8244F));
    program.setUniformValue("center", QVector2D(68.F, 54.F));
    program.setUniformValue("imagePass", true);
    for (const bool mip : {false, true}) {
      program.setUniformValue("fullMip", mip);
      gl->glDrawArrays(GL_TRIANGLES, 0, 3);
      float rgba[4]{};
      gl->glReadPixels(68, 54, 1, 1, GL_RGBA, GL_FLOAT, rgba);
      const double variance = mip ? .3 : .09;
      const double xx=.6464+variance, yy=.8244+variance, xy=.0096;
      const double det=xx*yy-xy*xy;
      const double compensation=mip ? std::sqrt((.6464*.8244-.0096*.0096)/det) : 1.;
      const double alpha=.6*compensation*std::exp(-.5*(yy*.25-2*xy*.25+xx*.25)/det);
      QVERIFY(std::abs(rgba[3]-alpha)<2e-6);
      const QString path=evidence.filePath(mip ? "same-camera-full-mip.png" : "same-camera-legacy.png");
      QVERIFY(image.toImage().save(path));
      qInfo().noquote() << "Actual native GPU PNG:" << path << "center alpha:" << rgba[3];
    }
    gl->glBindVertexArray(0);
    gl->glDeleteVertexArrays(1, &vao);
    QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR));
    program.release();
    context.doneCurrent();
  }
};

QTEST_MAIN(QualityMipViewportTests)
#include "QualityMipViewportTests.moc"
