#include "AppLanguage.h"
#include "ModelExportDialog.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
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
  void recordedPointsMeshAndGaussiansCombineFrameRecoveryAndSceneEdit() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    const QJsonArray identity{QJsonArray{1,0,0},QJsonArray{0,1,0},QJsonArray{0,0,1}};
    const QJsonObject transform{{"s",1.},{"R",identity},{"t",QJsonArray{0,0,0}},{"applied",true}};
    for(const QString &kind:QStringList{"point_cloud","mesh","gaussian"}) {
      const QString source=directory.filePath(kind+".ply");
      QByteArray bytes="ply\nformat ascii 1.0\nelement vertex 3\nproperty double x\nproperty double y\nproperty double z\n";
      if(kind=="gaussian")for(const auto *property:{"opacity","scale_0","scale_1","scale_2","rot_0","rot_1","rot_2","rot_3","f_dc_0","f_dc_1","f_dc_2"})bytes+=QByteArray("property float ")+property+'\n';
      if(kind=="mesh")bytes+="element face 1\nproperty list uchar int vertex_indices\n";
      bytes+="end_header\n";
      for(int i=0;i<3;++i)bytes+=QByteArray::number(i)+" 2 3"+(kind=="gaussian"?QByteArray(" .5 -2 -2 -2 1 0 0 0 .1 .2 .3"):QByteArray{})+'\n';
      if(kind=="mesh")bytes+="3 0 1 2\n";
      QFile file(source);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),bytes.size());file.close();
      const QJsonObject metadata{{"schema","gsw.training_camera_contract/1"},{"input_data_identity",QString(64,'e')},
          {"ply_sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},
          {"scale",QJsonObject{{"status","source_declared"},{"source_unit","m"},{"canonical_unit","m"},{"meters_per_unit",1.},
            {"evidence",QJsonArray{QJsonObject{{"kind","declaration"}}}}}},
          {"transforms",QJsonObject{{"source_to_canonical",transform},{"canonical_to_training",transform}}}};
      QFile sidecar(cameraScaleMetadataPath(source));QVERIFY(sidecar.open(QIODevice::WriteOnly));QVERIFY(sidecar.write(QJsonDocument(metadata).toJson())>0);sidecar.close();
      ModelExportOptions sourceOptions;sourceOptions.sourcePath=source;
      ModelExportDialog dialog(sourceOptions,kind=="mesh",kind=="gaussian",{});
      auto *coordinates=dialog.findChild<QComboBox *>("modelExportCoordinates");QVERIFY(coordinates);
      auto *edit=dialog.findChild<QCheckBox *>("modelExportBakeSceneTransform");QVERIFY(edit);
      QVERIFY(coordinates->findData("canonical")>=0);QVERIFY(coordinates->findData("training")>=0);
      coordinates->setCurrentIndex(coordinates->findData("source"));edit->setChecked(true);
      auto *unit=dialog.findChild<QComboBox *>("modelExportOutputUnit");QVERIFY(unit);unit->setCurrentIndex(unit->findData("cm"));
      QSet<QString> labels;
      for(const QString &language:QStringList{"zh_CN","en_US","ja_JP"}) {
        QVERIFY(AppLanguage::apply(language,false));
        const auto actual=dialog.options();QCOMPARE(actual.cameraCoordinateRecovery,QString("source"));QVERIFY(actual.applyTransform);
        QCOMPARE(actual.recordedRepresentation,kind);QCOMPARE(actual.cameraOutputUnit,QString("cm"));QVERIFY(!edit->text().isEmpty());
        labels.insert(edit->text());
      }
      QCOMPARE(labels.size(),3);
      coordinates->setCurrentIndex(coordinates->findData("canonical"));unit->setCurrentIndex(0);
      const auto *information=dialog.findChild<QLabel *>("modelExportCoordinateInfo");QVERIFY(information);
      const auto outputLine=information->text().split('\n')[1];
      QVERIFY(outputLine.contains("canonical"));QVERIFY(outputLine.endsWith("m"));
      edit->setChecked(false);coordinates->setCurrentIndex(0);
      QCOMPARE(dialog.options().cameraCoordinateRecovery,QString("preserve"));QVERIFY(!dialog.options().applyTransform);
      auto *formats=dialog.findChild<QComboBox *>("modelExportFormat");QVERIFY(formats);
      formats->setCurrentIndex(formats->findData(int(ModelExportFormat::Xyz)));
      QVERIFY(dialog.options().cameraCoordinateRecovery.isEmpty());QVERIFY(!edit->isEnabled());
      QVERIFY(!dialog.options().applyTransform);
    }
  }
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
