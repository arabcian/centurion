#pragma once
class QApplication;
class QWidget;
namespace simulation {
// A separate, presentation-only workspace. Never constructs the real shell.
QWidget *makeWorkspace(QApplication &app);
int run(QApplication &app);
}
