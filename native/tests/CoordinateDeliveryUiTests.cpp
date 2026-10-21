#include "AppLanguage.h"
#include "ModelExportDialog.h"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QTemporaryDir>
#include <QtTest>
using namespace gsw;
class CoordinateDeliveryUiTests final : public QObject {
  Q_OBJECT
private slots:
  void metricDefaultAndUnitRemainExplicitAcrossLanguages() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString source=directory.filePath("owned.ply");
    const QByteArray bytes="ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nproperty float opacity\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\nproperty float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nend_header\n1 2 3 .5 -2 -2 -2 1 0 0 0 .1 .2 .3\n";
    QFile model(source); QVERIFY(model.open(QIODevice::WriteOnly)); QCOMPARE(model.write(bytes),bytes.size()); model.close();
    const QJsonArray rotation{QJsonArray{1,0,0},QJsonArray{0,1,0},QJsonArray{0,0,1}};
    const QJsonObject transform{{"s",1.},{"R",rotation},{"t",QJsonArray{0,0,0}},{"applied",true}};
    auto sourceTransform=transform;sourceTransform["s"]=.01;
    QJsonObject metadata{{"schema","gsw.training_camera_contract/1"},{"input_data_identity",QString(64,'e')},
      {"ply_sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},
      {"scale",QJsonObject{{"status","source_declared"},{"source_unit","cm"},{"canonical_unit","m"},
          {"meters_per_unit",.01},{"evidence",QJsonArray{QJsonObject{{"kind","declaration"}}}}}},
      {"transforms",QJsonObject{{"source_to_canonical",sourceTransform},{"canonical_to_training",transform}}}};
    QFile sidecar(cameraScaleMetadataPath(source)); QVERIFY(sidecar.open(QIODevice::WriteOnly));
    QVERIFY(sidecar.write(QJsonDocument(metadata).toJson())>0); sidecar.close();
    ModelExportOptions options; options.sourcePath=source;
    ModelExportDialog dialog(options,false,true,{});
    QCOMPARE(dialog.options().cameraCoordinateRecovery,QString("metric"));
    auto *unit=dialog.findChild<QComboBox *>("modelExportOutputUnit"); QVERIFY(unit);
    unit->setCurrentIndex(unit->findData("mm"));
    for(const QString &language:QStringList{"zh_CN","en_US","ja_JP"}) {
      QVERIFY(AppLanguage::apply(language,false));
      QCOMPARE(dialog.options().cameraOutputUnit,QString("mm"));
      QCOMPARE(dialog.options().cameraCoordinateRecovery,QString("metric"));
    }
    auto *coordinates=dialog.findChild<QComboBox *>("modelExportCoordinates");QVERIFY(coordinates);
    coordinates->setCurrentIndex(coordinates->findData("source"));unit->setCurrentIndex(0);
    const auto *information=dialog.findChild<QLabel *>("modelExportCoordinateInfo");QVERIFY(information);
    QVERIFY(information->text().split('\n').at(1).endsWith("cm"));
    QFile corrupt(cameraScaleMetadataPath(source)); QVERIFY(corrupt.open(QIODevice::WriteOnly));
    metadata["ply_sha256"]=QString(64,'a'); QVERIFY(corrupt.write(QJsonDocument(metadata).toJson())>0); corrupt.close();
    ModelExportDialog rejected(options,false,true,{});
    QVERIFY(rejected.options().cameraCoordinateRecovery.isEmpty());
    QVERIFY(!cameraScaleRecoveryCapability(source).metric);
  }
};
int main(int argc,char **argv) {
  qputenv("QT_QPA_PLATFORM","offscreen"); QApplication application(argc,argv);
  QCoreApplication::setOrganizationName("GSW-Owned-Acceptance");
  QCoreApplication::setApplicationName("CoordinateDeliveryUiTests");
  AppLanguage::initialize("zh_CN"); CoordinateDeliveryUiTests test;
  return QTest::qExec(&test,argc,argv);
}
#include "CoordinateDeliveryUiTests.moc"
