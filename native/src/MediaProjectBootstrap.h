#pragma once

#include <QString>
#include <QStringList>

namespace gsw {

[[nodiscard]] QString suggestedMediaProjectName(
    const QStringList &sourcePaths);
[[nodiscard]] QString suggestedMediaSceneName(
    const QStringList &sourcePaths);

} // namespace gsw
