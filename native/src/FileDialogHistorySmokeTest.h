#pragma once

class QMainWindow;

namespace gsw {
bool runFileDialogHistorySmokeTest(QMainWindow &workbench);
bool runFileDialogHistoryPersistenceSmokeTest(QMainWindow &workbench, bool restartedChild);
bool runFileDialogHistoryRankingSmokeTest(QMainWindow &workbench);
bool runFileDialogHistoryFilterSmokeTest(QMainWindow &workbench);
bool runFileDialogHistoryUiSmokeTest(QMainWindow &workbench);
}
