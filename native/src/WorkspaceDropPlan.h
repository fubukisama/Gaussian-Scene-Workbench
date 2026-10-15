#pragma once

#include <QList>
#include <QStringList>
#include <QUrl>

namespace gsw {

// Classify only selected paths. Recursive media enumeration stays in the
// existing confirmation/worker workflow, never in the drag event hot path.
struct WorkspaceDropPlan final {
  enum class Kind { Invalid, Models, Media, Project };
  enum class Error { None, NonLocal, Unreadable, Unsupported, Mixed, InvalidProject, CopyRequired };
  Kind kind = Kind::Invalid;
  Error error = Error::None;
  QStringList paths;
  QString problemPath;

  [[nodiscard]] static WorkspaceDropPlan fromUrls(const QList<QUrl> &urls);
};

} // namespace gsw
