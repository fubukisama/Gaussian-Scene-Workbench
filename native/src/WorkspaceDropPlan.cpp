#include "WorkspaceDropPlan.h"
#include "DatasetImportPlan.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace gsw {

WorkspaceDropPlan WorkspaceDropPlan::fromUrls(const QList<QUrl> &urls) {
  WorkspaceDropPlan plan;
  QSet<QString> seen;
  for (const auto &url : urls) {
    if (!url.isLocalFile()) {
      plan.error = Error::NonLocal;
      plan.kind = Kind::Invalid;
      return plan;
    }
    const QFileInfo file(url.toLocalFile());
    if (!file.exists() || (!file.isFile() && !file.isDir()) || !file.isReadable()) {
      plan.error = Error::Unreadable;
      plan.problemPath = file.absoluteFilePath();
      plan.kind = Kind::Invalid;
      return plan;
    }
    const QString path = QDir::cleanPath(file.canonicalFilePath());
#ifdef Q_OS_WIN
    const QString key = path.toCaseFolded();
#else
    const QString key = path;
#endif
    if (seen.contains(key)) continue;
    seen.insert(key);
    const QString suffix = file.suffix().toLower();
    Kind kind = Kind::Invalid;
    if (file.isDir() || DatasetImportPlan::supportsFile(path)) kind = Kind::Media;
    else if (suffix == QStringLiteral("ply") || suffix == QStringLiteral("spz")) kind = Kind::Models;
    else if (file.fileName().endsWith(QStringLiteral(".gsw.json"), Qt::CaseInsensitive) ||
             suffix == QStringLiteral("gswjson")) kind = Kind::Project;
    if (kind == Kind::Invalid) {
      plan.error = Error::Unsupported;
      plan.problemPath = path;
      plan.kind = Kind::Invalid;
      return plan;
    }
    if ((plan.kind != Kind::Invalid && plan.kind != kind) ||
        (kind == Kind::Project && !plan.paths.isEmpty())) {
      plan.error = Error::Mixed;
      plan.kind = Kind::Invalid;
      return plan;
    }
    plan.kind = kind;
    plan.paths.append(path);
  }
  return plan;
}

} // namespace gsw
