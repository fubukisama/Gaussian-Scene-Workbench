#pragma once

#include <QString>

namespace gsw {
// Publish an iteration-boundary pause for the active project's actual running
// 3DGS/2DGS job. No selected scene, worker stdin, or process ID is required.
bool requestActiveTrainingPause(const QString &projectRoot,
                                QString *errorMessage = nullptr);
} // namespace gsw
