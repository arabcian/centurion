#pragma once
// First-run wizard: hardware found, boot services (commands to copy — nothing is enabled from here),
// first configuration backup, first scene, then shortcut / MangoHud / D-Bus and the Handbook.
//
// Shown once, the first time the window opens (not when the app only starts in the tray), while
// gui.ini has no first_run/done. Every page can be skipped; Cancel skips the rest and still counts
// as done. Home → *Setup wizard…* opens it again. An existing install (saved scenes) is not asked.
#include <QWizard>

class MainWindow;

class FirstRunWizard : public QWizard {
    Q_OBJECT
public:
    explicit FirstRunWizard(MainWindow *win);
    /// Opens the wizard when first_run/done is not set yet (and marks it done for an existing install).
    static void maybeShow(MainWindow *win);
    /// Opens it now (Home → Setup wizard…).
    static void showFor(MainWindow *win);
};
