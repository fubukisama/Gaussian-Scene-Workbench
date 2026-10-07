#pragma once

namespace gsw {
class MainWindow;
bool runResourceBudgetSmokeTest(MainWindow &window);
bool runResourceBudgetMeshSmokeTest(MainWindow &window);
bool runResourceBudgetSharedMeshSmokeTest(MainWindow &window);
bool runResourceBudgetTexturePageSmokeTest(MainWindow &window);
bool runResourceBudgetMinimumPageSmokeTest(MainWindow &window);
bool runResourceBudgetCacheFirstSmokeTest(MainWindow &window);
bool runResourceBudgetHudSmokeTest(MainWindow &window);
}
