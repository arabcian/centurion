#pragma once
// Health → SMART (summary): one row per disk from the root helper's smart_json (temperature, life used,
// spare, media errors with their change over 30 days, unsafe shutdowns, TB written, critical warning,
// self-test). Runs when first shown and on "Run again"; each run also records today's sample
// (smartwatch.h). The raw smartctl output is on the SMART details tab.
#include <QJsonArray>
#include <QStringList>
#include <QWidget>

class QLabel;
class QTreeWidget;

class SmartPage : public QWidget {
    Q_OBJECT
public:
    explicit SmartPage(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void start();
    void render(const QJsonArray &disks, const QStringList &alerts);

    QLabel *status_, *alert_;
    QTreeWidget *table_;
    bool ran_ = false, busy_ = false;
};
