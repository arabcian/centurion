#pragma once
// Health → System tools → Configuration files: a fixed list of system configuration files (the list
// sysadmin-helper accepts) edited in the GUI. Sysctl, modprobe and modules-load files also get a table
// ("Settings"); every file has the plain text. Saving shows the difference first and goes through
// sysadmin-helper, which validates, backs the old file up and writes atomically; any backup can be
// restored the same way.
#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;

class ConfigPage : public QWidget {
    Q_OBJECT
public:
    explicit ConfigPage(QWidget *parent = nullptr);

    /// Line difference of two texts, "- " / "+ " / "  " prefixed, unchanged runs shortened (also for tests).
    static QString diff(const QString &before, const QString &after);

    struct File;  // one entry of the fixed file list (configpage.cpp)

private:
    const File &file() const;
    void load();                 // the selected file from disk into both views
    bool confirmLeave();         // unsaved changes: ask before they are dropped
    void tableToText();
    void textToTable();
    void save();
    void restore();
    void addRow();
    void fillLive();

    QComboBox *files_;
    QLabel *path_, *status_;
    QTabWidget *tabs_;
    QTableWidget *table_;
    QWidget *tableBox_;
    QComboBox *addKind_ = nullptr;
    QPlainTextEdit *text_;
    QPushButton *save_, *restore_;
    QString disk_;               // the file as last read
    QStringList base_;           // the text the table rows point into (row UserRole = line index, -1 = new)
    bool syncing_ = false;
};
