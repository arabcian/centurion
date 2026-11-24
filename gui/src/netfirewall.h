#pragma once
// Health → Network → Firewall: default inbound / outbound policy and the ordered port rules.
// The tab only shows and edits; netguard-helper validates and stores, the daemon enforces
// (crates/centurion-helpers/src/firewall.rs). Nothing here runs unless the user acts.
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QTreeWidget;

class FirewallTab : public QWidget {
    Q_OBJECT
public:
    explicit FirewallTab(QWidget *parent = nullptr);
    /// A status answer of netguard-helper.
    void applyStatus(const QJsonObject &status);
    /// A request was refused: back to what is really configured.
    void restore();
    void previewConnection(const QJsonObject &record = {});
    /// Services other machines can reach now ("SSH: tcp 22"), named when inbound is about to be blocked.
    void setExposed(const QStringList &services) { exposed_ = services; }
    /// The answer of {"op":"fw_inspect"}: fills the hit counters and opens the report.
    void showInspection(const QJsonObject &report);

Q_SIGNALS:
    /// Fields of one {"op":"firewall"} request.
    void changeRequested(const QJsonObject &request);
    void inspectRequested();

private:
    void fill();
    void submitChange(QJsonObject request, const QJsonObject &expected);
    void addRule();
    void editRule();
    void toggleRules();
    void removeRules();
    void moveRule(int by);
    void policyChosen(bool inbound);
    QList<int> selectedIds() const;
    QString policyName(const QString &policy) const;

    QCheckBox *enabled_, *pingIn_, *pingOut_;
    QComboBox *inbound_, *outbound_;
    QLabel *info_;
    QTreeWidget *rules_;
    QJsonObject status_, fw_, pinnedFw_;
    QHash<QString, qint64> hits_;   // "r<id>" → packets, from the last inspection
    QStringList exposed_;
};
