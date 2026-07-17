#pragma once

#include <QString>
#include <QStringList>

namespace gsw {

class BackendLocator final {
public:
  [[nodiscard]] static QString
  findRepositoryRoot(const QString &applicationDirectory,
                     const QString &configuredRoot = {});
  [[nodiscard]] static QStringList
  gaussianPythonCandidates(const QString &repositoryRoot,
                           const QStringList &searchVolumeRoots = {});
  [[nodiscard]] static QString
  findGaussianPython(const QString &repositoryRoot,
                     const QStringList &searchVolumeRoots = {});
};

} // namespace gsw
