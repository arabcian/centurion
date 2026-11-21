#pragma once
#include <QWidget>
#include <QJsonObject>
class QPlainTextEdit;
class QTabWidget;
class QPushButton;
class QLabel;

// Five on-demand, read-only diagnostic views. Boot-scoped AER samples are
// saved in the user's own data directory, never by the privileged helper.
class HealthEvidencePage : public QWidget {
public:
    explicit HealthEvidencePage(QWidget *parent = nullptr);
private:
    void collect();
    void display(const QJsonObject &reply);
    QTabWidget *tabs_ = nullptr;
    QPushButton *scan_ = nullptr;
    QLabel *status_ = nullptr;
    QPlainTextEdit *views_[5]{};
    bool busy_ = false;
};
