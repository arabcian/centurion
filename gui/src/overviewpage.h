#pragma once
// Health → Overview: one card per health area, colored by its level, and the diagnostic report button.
// The levels come from tune-profile-helper ("overview" op, read-only, Rust, no root); this page only lays
// them out. It refreshes when it is shown, once at start-up, when a kernel fault arrives (debounced), and
// on Refresh — no timer.
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QGridLayout;
class QLabel;
class QTimer;

class OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit OverviewPage(QWidget *parent = nullptr);
    void refresh();          // one helper call, unless one is already running
    void scheduleRefresh();  // a kernel fault arrived: refresh once the burst is over
    int worst() const { return worst_; }  // -1 no data, 0 ok, 1 watch, 2 attention
    /// The cards on screen, as the helper last returned them (empty before the first reading).
    QJsonArray cards() const;

Q_SIGNALS:
    /// A card's "Show" button: the Health sub-tab to open ("monitor", "boot", "battery", "cooling", "backup", "dgpu", "sleep").
    void openPage(const QString &target);
    /// Worst level of the cards: the Home badge and the tray entry follow it.
    void worstChanged(int level);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void onReply(const QJsonObject &r);
    void rebuildCards(const QJsonArray &cards);
    void report();
    static QString cardTitle(const QString &id);
    static QString cardBody(const QJsonObject &card);
    static QString levelWord(int level);
    static QString targetFor(const QString &id);

    QGridLayout *grid_;
    QLabel *summary_, *status_;
    QTimer *debounce_;
    QByteArray shown_;  // JSON of the cards on screen (an identical reply does not rebuild them)
    int worst_ = -1;
    bool busy_ = false, reporting_ = false;
};
