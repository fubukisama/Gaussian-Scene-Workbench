#include "SystemResourceProbe.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTest>

class SystemResourceProbeTests final : public QObject {
  Q_OBJECT
private slots:
  void systemMemoryRemainsAvailableWithoutAGraphicsContext();
  void softwareRenderingDoesNotInventDedicatedGpuMemory();
};

void SystemResourceProbeTests::systemMemoryRemainsAvailableWithoutAGraphicsContext() {
  const auto snapshot = gsw::SystemResourceProbe::probe();
  QVERIFY(snapshot.systemMemoryKnown);
  QVERIFY(snapshot.physicalTotalBytes > 0);
  QCOMPARE(snapshot.gpuSource, gsw::GpuProbeSource::Unavailable);
  QCOMPARE(snapshot.gpuDedicatedBytes, qint64(0));
  QCOMPARE(snapshot.gpuAvailableBytes, qint64(0));
}

void SystemResourceProbeTests::softwareRenderingDoesNotInventDedicatedGpuMemory() {
  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;
  context.setFormat(format);
  QVERIFY(context.create());
  QOffscreenSurface surface;
  surface.setFormat(context.format());
  surface.create();
  QVERIFY(context.makeCurrent(&surface));
  const auto *rawRenderer = context.functions()->glGetString(GL_RENDERER);
  QVERIFY(rawRenderer != nullptr);
  const QByteArray renderer(reinterpret_cast<const char *>(rawRenderer));
  qInfo() << "Resource probe real renderer:" << renderer;
  const bool software = renderer.toLower().startsWith("llvmpipe") ||
                        renderer.toLower().startsWith("softpipe");
  if (qEnvironmentVariableIsSet("GSW_EXPECT_SOFTWARE_RENDERER")) {
    QVERIFY2(software, "CI must exercise the real software renderer, not skip its probe");
  }
  if (!software) {
    QSKIP("Software-only probe coverage requires llvmpipe/softpipe; hardware budget tests remain enabled");
  }
  for (int sample = 0; sample < 3; ++sample) {
    const auto snapshot = gsw::SystemResourceProbe::probe(&context);
    QCOMPARE(snapshot.adapterName.toLatin1(), renderer);
    QVERIFY(snapshot.systemMemoryKnown);
    QVERIFY(snapshot.physicalTotalBytes > 0);
    QCOMPARE(snapshot.gpuSource, gsw::GpuProbeSource::Unavailable);
    QCOMPARE(snapshot.gpuDedicatedBytes, qint64(0));
    QCOMPARE(snapshot.gpuAvailableBytes, qint64(0));
    QCOMPARE(snapshot.gpuBudgetBytes, qint64(0));
    QCOMPARE(snapshot.gpuUsageBytes, qint64(0));
  }
  context.doneCurrent();
}

QTEST_MAIN(SystemResourceProbeTests)
#include "SystemResourceProbeTests.moc"
