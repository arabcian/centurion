#pragma once
// PipeWire Doctor → Settings: a short list of well-known PipeWire / WirePlumber options, each with a hint,
// written to Centurion's own drop-in files (50-centurion.conf) in the user's config folder. No root; deleting
// the files, or "Remove all changes", returns every option to its default.
#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;

class PipewireConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit PipewireConfigDialog(QWidget *parent = nullptr);

private:
    void load();
    void save();
    void removeAll();
    void restart(const QString &done);
    void applyLock();  // read-only while Centurion is locked (unlock.h)

    QTableWidget *table_;
    QLabel *hint_, *status_, *lockNote_;
    QPushButton *apply_, *reset_, *unlockBtn_;
};
