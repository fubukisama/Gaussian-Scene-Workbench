#include "AppLanguage.h"
#include "NativeViewport.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
bool write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class NativeViewportLocaleTests final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QVERIFY(gsw::AppLanguage::initialize(QStringLiteral("zh_CN")));
  }

  void languageIsPresentationNotSceneMutation() {
    gsw::NativeViewport viewport;
    QVERIFY(!viewport.isVisible());
    QVERIFY(viewport.context() == nullptr);
    const QVector3D translation(1, 2, 3), scale(2, 3, 4);
    const QQuaternion rotation = QQuaternion::fromEulerAngles(12, 23, 34);
    viewport.setModelTransform(translation, rotation, scale);
    const auto actualRotation = viewport.modelRotation();
    const auto selected = viewport.selectedSceneIds();
    const auto target = viewport.viewTarget();
    const auto distance = viewport.viewDistance();
    QSignalSpy coordinates(&viewport, &gsw::NativeViewport::sceneCoordinatesChanged);
    QSignalSpy cameras(&viewport, &gsw::NativeViewport::cameraTrajectoryChanged);
    QSignalSpy loads(&viewport, &gsw::NativeViewport::sceneLoadStarted);
    QSignalSpy selections(&viewport, &gsw::NativeViewport::sceneSelectionChanged);
    for (const auto &language : gsw::AppLanguage::supported()) {
      QVERIFY(gsw::AppLanguage::apply(language, false));
      QCOMPARE(viewport.modelTranslation(), translation);
      QCOMPARE(viewport.modelRotation(), actualRotation);
      QCOMPARE(viewport.modelScale(), scale);
      QCOMPARE(viewport.selectedSceneIds(), selected);
      QCOMPARE(viewport.viewTarget(), target);
      QCOMPARE(viewport.viewDistance(), distance);
      qInfo() << "LOCALE_PRESENTATION_SIGNALS:" << language
              << "coordinates" << coordinates.size() << "cameras" << cameras.size();
      QCOMPARE(coordinates.size(), 0);
      QCOMPARE(cameras.size(), 0);
      QCOMPARE(loads.size(), 0);
      QCOMPARE(selections.size(), 0);
      QVERIFY(!viewport.isVisible());
      QVERIFY(viewport.context() == nullptr);
    }
  }

  void projectionTooltipRetranslatesUnderStationaryPointer() {
    const auto previousLanguage = gsw::AppLanguage::current();
    const auto restoreLanguage = qScopeGuard([&] {
      gsw::AppLanguage::apply(previousLanguage, false);
    });
    QVERIFY(gsw::AppLanguage::apply(QStringLiteral("zh_CN"), false));
    gsw::NativeViewport viewport;
    viewport.resize(960, 720);
    viewport.setEditToolsLocked(true);
    viewport.setModelTransform({1, 2, 3}, QQuaternion::fromEulerAngles(12, 23, 34), {2, 3, 4});
    const auto translation = viewport.modelTranslation();
    const auto rotation = viewport.modelRotation();
    const auto scale = viewport.modelScale();
    const auto selected = viewport.selectedSceneIds();
    const auto target = viewport.viewTarget();
    const auto distance = viewport.viewDistance();
    const auto angles = viewport.viewOrbitAngles();
    QSignalSpy coordinates(&viewport, &gsw::NativeViewport::sceneCoordinatesChanged);
    QSignalSpy cameras(&viewport, &gsw::NativeViewport::cameraTrajectoryChanged);
    QSignalSpy loads(&viewport, &gsw::NativeViewport::sceneLoadStarted);
    QSignalSpy selections(&viewport, &gsw::NativeViewport::sceneSelectionChanged);

    const auto layout = gsw::navigationGizmoLayout(
        QMatrix4x4(), viewport.size(), QFontMetricsF(viewport.font()).height());
    const auto point = layout.projectionButton.center();
    const auto mouse = [&](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons) {
      QMouseEvent event(type, point, point, viewport.mapToGlobal(point.toPoint()),
                        button, buttons, Qt::NoModifier);
      QCoreApplication::sendEvent(&viewport, &event);
    };
    const auto click = [&] {
      mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
      mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    };
    // One real hover establishes the pointer location. Language changes and
    // clicks must refresh the tooltip without moving the pointer afterward.
    mouse(QEvent::MouseMove, Qt::NoButton, Qt::NoButton);
    QVERIFY(!viewport.orthographicProjection());
    struct ExpectedLocale {
      const char *locale;
      const char *toOrthographic;
      const char *toPerspective;
    };
    const ExpectedLocale locales[] = {
        {"zh_CN", "切换到正交投影", "切换到透视投影"},
        {"en_US", "Switch to Orthographic Projection", "Switch to Perspective Projection"},
        {"ja_JP", "平行投影に切り替え", "透視投影に切り替え"}};
    for (const bool orthographic : {false, true}) {
      if (viewport.orthographicProjection() != orthographic) click();
      for (const auto &locale : locales) {
        QVERIFY(gsw::AppLanguage::apply(QString::fromLatin1(locale.locale), false));
        const auto expected = QString::fromUtf8(orthographic ? locale.toPerspective : locale.toOrthographic);
        const auto toggled = QString::fromUtf8(orthographic ? locale.toOrthographic : locale.toPerspective);
        QCOMPARE(viewport.toolTip(), expected);
        QCOMPARE(viewport.orthographicProjection(), orthographic);
        click();
        QCOMPARE(viewport.orthographicProjection(), !orthographic);
        QCOMPARE(viewport.toolTip(), toggled);
        click();
        QCOMPARE(viewport.orthographicProjection(), orthographic);
        QCOMPARE(viewport.toolTip(), expected);
        QCOMPARE(viewport.modelTranslation(), translation);
        QCOMPARE(viewport.modelRotation(), rotation);
        QCOMPARE(viewport.modelScale(), scale);
        QCOMPARE(viewport.selectedSceneIds(), selected);
        QCOMPARE(viewport.viewTarget(), target);
        QCOMPARE(viewport.viewDistance(), distance);
        QVERIFY(viewport.viewOrbitAngles() == angles);
        QCOMPARE(coordinates.size(), 0);
        QCOMPARE(cameras.size(), 0);
        QCOMPARE(loads.size(), 0);
        QCOMPARE(selections.size(), 0);
        QVERIFY(!viewport.hasUnsavedSceneEdits());
        QVERIFY(!viewport.modelTransformActive());
        QVERIFY(!viewport.isVisible());
        QVERIFY(viewport.context() == nullptr);
      }
    }
  }

  void cameraErrorRetranslatesWithoutDataNotifications() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString model = root.filePath(QStringLiteral("owned-camera-model.ply"));
    QVERIFY(write(model, "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n"));
    QVERIFY(write(root.filePath(QStringLiteral("owned-camera-model.camera-scale.json")), "{broken"));
    gsw::NativeViewport viewport;
    // The public camera loader is independent of GL. This widget is never
    // shown and never creates a GL context or consumes a GPU render surface.
    viewport.setScene(model, 1);
    QTRY_VERIFY_WITH_TIMEOUT(!viewport.cameraTrajectoryError().isEmpty(), 5000);
    QCOMPARE(viewport.cameraCount(), qsizetype(0));
    const QVector3D translation(7, 11, 13), scale(3, 2, 5);
    const QQuaternion rotation = QQuaternion::fromEulerAngles(9, 19, 29);
    viewport.setModelTransform(translation, rotation, scale);
    const auto actualRotation = viewport.modelRotation();
    QSignalSpy coordinates(&viewport, &gsw::NativeViewport::sceneCoordinatesChanged);
    QSignalSpy cameras(&viewport, &gsw::NativeViewport::cameraTrajectoryChanged);
    QSignalSpy loads(&viewport, &gsw::NativeViewport::sceneLoadStarted);
    QSet<QString> errors;
    for (const auto &language : gsw::AppLanguage::supported()) {
      QVERIFY(gsw::AppLanguage::apply(language, false));
      const QString expected = QCoreApplication::translate("Workbench", "模型相机记录无效：%1。未借用附近相机记录。")
          .arg(QStringLiteral("camera_metadata_invalid_json"));
      QCOMPARE(viewport.cameraTrajectoryError(), expected);
      errors.insert(viewport.cameraTrajectoryError());
      QCOMPARE(viewport.modelTranslation(), translation);
      QCOMPARE(viewport.modelRotation(), actualRotation);
      QCOMPARE(viewport.modelScale(), scale);
      QCOMPARE(viewport.scenePath(), model);
      qInfo() << "LOCALE_CAMERA_ERROR_SIGNALS:" << language
              << "coordinates" << coordinates.size() << "cameras" << cameras.size();
      QCOMPARE(coordinates.size(), 0);
      QCOMPARE(cameras.size(), 0);
      QCOMPARE(loads.size(), 0);
      QVERIFY(!viewport.isVisible());
      QVERIFY(viewport.context() == nullptr);
    }
    QCOMPARE(errors.size(), 3);
    // The fix must not silence real metadata/load events. Explicit refresh
    // still delivers those events through the production asynchronous path.
    viewport.refreshActiveSceneMetadata();
    QTRY_VERIFY_WITH_TIMEOUT(coordinates.size() > 0 && cameras.size() > 0, 5000);
    QVERIFY(!viewport.cameraTrajectoryError().isEmpty());
    QVERIFY(!viewport.isVisible());
    QVERIFY(viewport.context() == nullptr);
  }
};

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("GSW-Owned-Viewport-Locale-Test"));
  QCoreApplication::setApplicationName(QStringLiteral("NonDisplayedViewportLocale"));
  QTemporaryDir settings;
  if (!settings.isValid()) return 2;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
  NativeViewportLocaleTests test;
  return QTest::qExec(&test, argc, argv);
}

#include "NativeViewportLocaleTests.moc"
