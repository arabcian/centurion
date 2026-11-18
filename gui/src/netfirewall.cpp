#include "netfirewall.h"

#include "netrules.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

enum { IdRole = Qt::UserRole };
enum { ColOn, ColDir, ColAction, ColProto, ColPorts, ColRemote, ColIface, ColNote, ColHits, ColCount };

static QString str(const QJsonObject &o, const char *k) { return o.value(QLatin1String(k)).toString(); }

namespace {
struct Service { const char *name, *proto, *ports; };
// Ports people open on a desktop. The name is only a shortcut: it fills the fields below.
const Service SERVICES[] = {
    {"SSH", "tcp", "22"}, {"HTTP / HTTPS", "any", "80, 443"}, {"DNS", "any", "53"}, {"NTP", "udp", "123"},
    {"Samba / SMB", "tcp", "139, 445"}, {"NFS", "any", "111, 2049"}, {"CUPS printing", "any", "631"}, {"mDNS (Avahi)", "udp", "5353"},
    {"KDE Connect", "any", "1714-1764"}, {"Syncthing", "any", "22000"}, {"Sunshine streaming", "any", "47984-48010"},
    {"Steam Remote Play", "any", "27031-27036"}, {"WireGuard", "udp", "51820"}, {"BitTorrent", "any", "6881-6889"}, {"VNC", "tcp", "5900"}, {"RDP", "tcp", "3389"},
};

void setCombo(QComboBox *c, const QString &data) {
    const int i = c->findData(data);
    if (i >= 0) c->setCurrentIndex(i);
}
}  // namespace

/// The add / edit window. Returns false when cancelled.
static bool ruleDialog(QWidget *parent, QJsonObject &rule, const QString &title) {
    QDialog d(parent);
    d.setWindowTitle(title);
    d.setMinimumWidth(460);
    auto *v = new QVBoxLayout(&d);
    auto *form = new QFormLayout;
    auto *service = new QComboBox;
    service->addItem(FirewallTab::tr("Custom"));
    for (const Service &s : SERVICES) service->addItem(QString::fromUtf8(s.name));
    auto *dir = new QComboBox;
    dir->addItem(FirewallTab::tr("Incoming"), QStringLiteral("in"));
    dir->addItem(FirewallTab::tr("Outgoing"), QStringLiteral("out"));
    auto *action = new QComboBox;
    action->addItem(FirewallTab::tr("Allow"), QStringLiteral("allow"));
    action->addItem(FirewallTab::tr("Block (silently drop)"), QStringLiteral("block"));
    action->addItem(FirewallTab::tr("Reject (refuse at once)"), QStringLiteral("reject"));
    auto *proto = new QComboBox;
    proto->addItem(QStringLiteral("TCP"), QStringLiteral("tcp"));
    proto->addItem(QStringLiteral("UDP"), QStringLiteral("udp"));
    proto->addItem(QStringLiteral("TCP + UDP"), QStringLiteral("any"));
    auto *ports = new QLineEdit(str(rule, "ports"));
    ports->setPlaceholderText(FirewallTab::tr("22, 80, 8000-8100 (empty: every port)"));
    auto *remote = new QComboBox;
    remote->setEditable(true);
    remote->setInsertPolicy(QComboBox::NoInsert);
    remote->addItem(FirewallTab::tr("Anywhere"), QString());
    remote->addItem(FirewallTab::tr("Local networks only"), QStringLiteral("lan"));
    remote->lineEdit()->setPlaceholderText(QStringLiteral("203.0.113.7, 198.51.100.0/24"));
    remote->setToolTip(FirewallTab::tr("The other side of the connection: an address, a CIDR block or a range; several separated by commas.\nLocal networks include everyone on the network you are connected to, on public Wi-Fi too."));
    const QString r = str(rule, "remote");
    if (r.isEmpty()) remote->setCurrentIndex(0);
    else if (r == QLatin1String("lan")) remote->setCurrentIndex(1);
    else remote->setEditText(r);
    auto *iface = new QLineEdit(str(rule, "iface"));
    iface->setPlaceholderText(FirewallTab::tr("every interface (or wlan0, wg*)"));
    auto *note = new QLineEdit(str(rule, "note"));
    note->setMaxLength(120);
    setCombo(dir, str(rule, "dir"));
    setCombo(action, str(rule, "action"));
    setCombo(proto, str(rule, "proto"));
    QObject::connect(service, &QComboBox::activated, &d, [=](int i) {
        if (i <= 0) return;
        const Service &s = SERVICES[i - 1];
        setCombo(proto, QLatin1String(s.proto));
        ports->setText(QString::fromLatin1(s.ports));
        if (note->text().isEmpty()) note->setText(QString::fromUtf8(s.name));
    });
    form->addRow(FirewallTab::tr("Service"), service);
    form->addRow(FirewallTab::tr("Direction"), dir);
    form->addRow(FirewallTab::tr("Action"), action);
    form->addRow(FirewallTab::tr("Protocol"), proto);
    form->addRow(FirewallTab::tr("Ports"), ports);
    form->addRow(FirewallTab::tr("Remote address"), remote);
    form->addRow(FirewallTab::tr("Interface"), iface);
    form->addRow(FirewallTab::tr("Note"), note);
    v->addLayout(form);
    auto *error = new QLabel;
    error->setStyleSheet(QStringLiteral("color:%1").arg(QLatin1String(theme::DANGER)));
    error->setWordWrap(true);
    error->hide();
    v->addWidget(error);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(box);
    const auto remoteText = [remote] {
        const int i = remote->findText(remote->currentText());
        return i >= 0 ? remote->itemData(i).toString() : remote->currentText().trimmed();
    };
    QObject::connect(box, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    QObject::connect(box, &QDialogButtonBox::accepted, &d, [&] {
        QString e;
        if (!validPortList(ports->text())) e = FirewallTab::tr("Ports: numbers from 1 to 65535 or ranges such as 8000-8100, at most 32 items.");
        else if (!validRemote(remoteText())) e = FirewallTab::tr("Remote address: use addresses, CIDR blocks or ranges, not host names.");
        else if (!validInterface(iface->text().trimmed())) e = FirewallTab::tr("Interface: a name such as wlan0 or wg*. Loopback is never filtered.");
        else if (proto->currentData() == QLatin1String("any") && ports->text().trimmed().isEmpty() && remoteText().isEmpty() && iface->text().trimmed().isEmpty())
            e = FirewallTab::tr("This rule would match everything. Use the default policy above instead.");
        error->setText(e);
        error->setVisible(!e.isEmpty());
        if (e.isEmpty()) d.accept();
    });
    if (d.exec() != QDialog::Accepted) return false;
    rule[QStringLiteral("dir")] = dir->currentData().toString();
    rule[QStringLiteral("action")] = action->currentData().toString();
    rule[QStringLiteral("proto")] = proto->currentData().toString();
    rule[QStringLiteral("ports")] = ports->text().trimmed();
    rule[QStringLiteral("remote")] = remoteText();
    rule[QStringLiteral("iface")] = iface->text().trimmed();
    rule[QStringLiteral("note")] = note->text().trimmed();
    return true;
}

FirewallTab::FirewallTab(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    auto policyBox = [this] {
        auto *c = new QComboBox;
        c->addItem(tr("Allow"), QStringLiteral("allow"));
        c->addItem(tr("Block"), QStringLiteral("block"));
        c->addItem(tr("Reject"), QStringLiteral("reject"));
        c->setToolTip(tr("What happens to a new connection no rule below decides.\n"
                         "Block drops it silently; Reject refuses it at once, so the other side gets an error instead of a timeout.\n"
                         "Connections that are already open keep working."));
        return c;
    };
    auto *top = new QHBoxLayout;
    enabled_ = new QCheckBox(tr("Firewall on"));
    enabled_->setToolTip(tr("Off: the policies and rules stay saved but nothing is filtered."));
    top->addWidget(enabled_);
    top->addSpacing(14);
    top->addWidget(new QLabel(tr("Incoming by default")));
    inbound_ = policyBox();
    top->addWidget(inbound_);
    top->addWidget(new QLabel(tr("Outgoing by default")));
    outbound_ = policyBox();
    top->addWidget(outbound_);
    pingIn_ = new QCheckBox(tr("Answer ping"));
    pingIn_->setToolTip(tr("Off: this machine does not answer ICMP echo requests, unless a rule allows the sender."));
    pingOut_ = new QCheckBox(tr("Send ping"));
    pingOut_->setToolTip(tr("Off: programs on this machine cannot send ICMP echo requests, unless a rule allows the destination."));
    top->addWidget(pingIn_);
    top->addWidget(pingOut_);
    top->addStretch(1);
    v->addLayout(top);

    info_ = new QLabel;
    info_->setTextFormat(Qt::RichText);
    info_->setWordWrap(true);
    v->addWidget(info_);

    rules_ = new QTreeWidget;
    rules_->setColumnCount(ColCount);
    rules_->setHeaderLabels({tr("On"), tr("Direction"), tr("Action"), tr("Protocol"), tr("Ports"), tr("Remote address"), tr("Interface"), tr("Note"), tr("Hits")});
    rules_->setRootIsDecorated(false);
    rules_->setAlternatingRowColors(true);
    rules_->setUniformRowHeights(true);
    rules_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rules_->setContextMenuPolicy(Qt::CustomContextMenu);
    rules_->header()->setStretchLastSection(false);
    rules_->header()->setSectionResizeMode(ColNote, QHeaderView::Stretch);
    rules_->setColumnWidth(ColOn, 40);
    rules_->setColumnWidth(ColDir, 90);
    rules_->setColumnWidth(ColAction, 80);
    rules_->setColumnWidth(ColPorts, 150);
    rules_->setColumnWidth(ColRemote, 170);
    v->addWidget(rules_, 1);

    auto *row = new QHBoxLayout;
    auto button = [row](const QString &text) { auto *b = new QPushButton(text); row->addWidget(b); return b; };
    auto *bAdd = button(tr("Add rule…"));
    auto *bEdit = button(tr("Edit…"));
    auto *bToggle = button(tr("On / off"));
    auto *bUp = button(tr("Move up"));
    auto *bDown = button(tr("Move down"));
    auto *bDel = button(tr("Remove"));
    bDel->setObjectName(QStringLiteral("btnDanger"));
    row->addStretch(1);
    auto *bCheck = button(tr("Check live rules…"));
    bCheck->setToolTip(tr("Asks the kernel what is really loaded: hit counters for every rule, and other firewalls on this machine that can override these rules. Needs the administrator password."));
    v->addLayout(row);

    auto *note = new QLabel(tr("The first matching rule decides, from the top down; the default policy applies when none matches. "
                               "Loopback, replies to existing connections, and what the network itself needs (DHCP, IPv6 neighbour discovery) always pass. "
                               "Traffic this machine forwards for virtual machines and containers is not filtered, except connections to ports it publishes for them, which count as incoming. "
                               "The rules are enforced only while the centurion-netguard service runs."));
    note->setProperty("role", "muted");
    note->setWordWrap(true);
    v->addWidget(note);

    connect(enabled_, &QCheckBox::clicked, this, [this](bool on) {
        const QString in = inbound_->currentData().toString(), out = outbound_->currentData().toString();
        if (on && (in != QLatin1String("allow") || out != QLatin1String("allow"))) {
            QString ask = tr("Turn the firewall on? New incoming connections: %1. New outgoing connections: %2.").arg(policyName(in), policyName(out));
            if (in != QLatin1String("allow") && !exposed_.isEmpty()) ask += QStringLiteral("\n\n") + tr("Services other machines can reach now:\n%1").arg(exposed_.mid(0, 12).join(QLatin1Char('\n')));
            if (QMessageBox::question(this, tr("Firewall"), ask) != QMessageBox::Yes) { restore(); return; }
        }
        Q_EMIT changeRequested({{"enabled", on}});
    });
    connect(pingIn_, &QCheckBox::clicked, this, [this](bool on) { Q_EMIT changeRequested({{"ping_in", on}}); });
    connect(pingOut_, &QCheckBox::clicked, this, [this](bool on) { Q_EMIT changeRequested({{"ping_out", on}}); });
    connect(inbound_, &QComboBox::activated, this, [this] { policyChosen(true); });
    connect(outbound_, &QComboBox::activated, this, [this] { policyChosen(false); });
    connect(bAdd, &QPushButton::clicked, this, &FirewallTab::addRule);
    connect(bEdit, &QPushButton::clicked, this, &FirewallTab::editRule);
    connect(rules_, &QTreeWidget::itemDoubleClicked, this, &FirewallTab::editRule);
    connect(bToggle, &QPushButton::clicked, this, &FirewallTab::toggleRules);
    connect(bDel, &QPushButton::clicked, this, &FirewallTab::removeRules);
    connect(bUp, &QPushButton::clicked, this, [this] { moveRule(-1); });
    connect(bDown, &QPushButton::clicked, this, [this] { moveRule(1); });
    connect(bCheck, &QPushButton::clicked, this, &FirewallTab::inspectRequested);
    connect(rules_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu m(this);
        if (rules_->itemAt(pos)) {
            m.addAction(tr("Edit…"), this, &FirewallTab::editRule);
            m.addAction(tr("On / off"), this, &FirewallTab::toggleRules);
            m.addAction(tr("Move up"), this, [this] { moveRule(-1); });
            m.addAction(tr("Move down"), this, [this] { moveRule(1); });
            m.addSeparator();
            m.addAction(tr("Remove"), this, &FirewallTab::removeRules);
        } else {
            m.addAction(tr("Add rule…"), this, &FirewallTab::addRule);
        }
        m.exec(rules_->viewport()->mapToGlobal(pos));
    });
}

QString FirewallTab::policyName(const QString &policy) const {
    return policy == QLatin1String("block") ? tr("block") : policy == QLatin1String("reject") ? tr("reject") : tr("allow");
}

void FirewallTab::applyStatus(const QJsonObject &status) {
    status_ = status;
    fw_ = status.value(QLatin1String("firewall")).toObject();
    restore();
}

void FirewallTab::restore() {
    const QSignalBlocker b1(enabled_), b2(pingIn_), b3(pingOut_), b4(inbound_), b5(outbound_);
    enabled_->setChecked(fw_.value(QLatin1String("enabled")).toBool());
    pingIn_->setChecked(fw_.value(QLatin1String("ping_in")).toBool(true));
    pingOut_->setChecked(fw_.value(QLatin1String("ping_out")).toBool(true));
    setCombo(inbound_, fw_.value(QLatin1String("inbound")).toString(QStringLiteral("allow")));
    setCombo(outbound_, fw_.value(QLatin1String("outbound")).toString(QStringLiteral("allow")));
    fill();
}

void FirewallTab::fill() {
    const QList<int> keep = selectedIds();
    QHash<int, QJsonObject> shadowed;
    for (const QJsonValue &x : fw_.value(QLatin1String("shadowed")).toArray())
        shadowed.insert(x.toObject().value(QLatin1String("id")).toInt(), x.toObject());
    rules_->clear();
    const QJsonArray rules = fw_.value(QLatin1String("rules")).toArray();
    QHash<int, int> position;
    for (int i = 0; i < rules.size(); ++i) position.insert(rules.at(i).toObject().value(QLatin1String("id")).toInt(), i + 1);
    int on = 0;
    for (const QJsonValue &x : rules) {
        const QJsonObject r = x.toObject();
        const int id = r.value(QLatin1String("id")).toInt();
        const bool enabled = r.value(QLatin1String("enabled")).toBool(true);
        on += enabled;
        const QString action = str(r, "action"), proto = str(r, "proto"), remote = str(r, "remote");
        const QString hitKey = QStringLiteral("r%1").arg(id);
        auto *it = new QTreeWidgetItem({enabled ? QStringLiteral("✓") : QString(),
                                        str(r, "dir") == QLatin1String("in") ? tr("← in") : tr("→ out"), policyName(action),
                                        proto == QLatin1String("any") ? QStringLiteral("TCP + UDP") : proto.toUpper(),
                                        str(r, "ports").isEmpty() ? tr("all") : str(r, "ports"),
                                        remote.isEmpty() ? tr("anywhere") : remote == QLatin1String("lan") ? tr("local networks") : remote,
                                        str(r, "iface").isEmpty() ? tr("all") : str(r, "iface"), str(r, "note"),
                                        hits_.contains(hitKey) ? QLocale().toString(hits_.value(hitKey)) : QStringLiteral("—")});
        it->setData(0, IdRole, id);
        const char *colour = !enabled ? theme::MUTED : action == QLatin1String("allow") ? theme::OK : theme::DANGER;
        it->setForeground(ColAction, QColor(QLatin1String(colour)));
        if (!enabled) for (int c = 0; c < ColCount; ++c) it->setForeground(c, QColor(QLatin1String(theme::MUTED)));
        if (shadowed.contains(id)) {
            const QString tip = tr("Never reached: rule %1 above already decides everything this rule matches.")
                                    .arg(position.value(shadowed.value(id).value(QLatin1String("by")).toInt()));
            for (int c = 0; c < ColCount; ++c) { it->setToolTip(c, tip); it->setForeground(c, QColor(QLatin1String(theme::WARN))); }
        }
        rules_->addTopLevelItem(it);
        if (keep.contains(id)) it->setSelected(true);
    }
    QStringList lines;
    const bool active = enabled_->isChecked();
    if (!active) {
        lines << tr("The firewall is off: every connection the blacklist and the Wine guard do not stop is allowed.");
    } else {
        lines << tr("<b>On.</b> New incoming connections: <b>%1</b> · outgoing: <b>%2</b> · %3 of %4 rule(s) active")
                     .arg(policyName(inbound_->currentData().toString()), policyName(outbound_->currentData().toString())).arg(on).arg(rules.size());
        if (!status_.value(QLatin1String("running")).toBool())
            lines << QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(theme::WARN), tr("The daemon is not running, so nothing is filtered right now."));
    }
    if (!shadowed.isEmpty())
        lines << QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(theme::WARN), tr("%1 rule(s) can never match (shown in yellow; hover for the reason).").arg(shadowed.size()));
    info_->setText(lines.join(QStringLiteral("<br>")));
}

QList<int> FirewallTab::selectedIds() const {
    QList<int> ids;
    for (const QTreeWidgetItem *it : rules_->selectedItems()) ids << it->data(0, IdRole).toInt();
    return ids;
}

void FirewallTab::policyChosen(bool inbound) {
    QComboBox *box = inbound ? inbound_ : outbound_;
    const QString policy = box->currentData().toString();
    const char *key = inbound ? "inbound" : "outbound";
    QJsonObject req{{QLatin1String(key), policy}};
    const bool stricter = policy != QLatin1String("allow") && fw_.value(QLatin1String(key)).toString(QStringLiteral("allow")) == QLatin1String("allow");
    if (stricter && enabled_->isChecked() && inbound && !exposed_.isEmpty()) {
        const QStringList shown = exposed_.mid(0, 12);
        if (QMessageBox::question(this, tr("Firewall"), tr("Other machines can reach these services now and will lose access unless a rule allows them:\n\n%1\n\nConnections already open keep working. Continue?")
                                      .arg(shown.join(QLatin1Char('\n')))) != QMessageBox::Yes) { restore(); return; }
    }
    if (stricter && !inbound) {
        QMessageBox q(QMessageBox::Warning, tr("Firewall"),
                      tr("Every program on this machine loses new outgoing connections, unless a rule allows them. "
                         "Without DNS and web rules the internet stops working.\n\nAdd rules that allow DNS, NTP and web traffic first?"),
                      QMessageBox::Cancel, this);
        auto *withRules = q.addButton(tr("Block and allow DNS, NTP, web"), QMessageBox::AcceptRole);
        auto *bare = q.addButton(tr("Block everything"), QMessageBox::DestructiveRole);
        q.exec();
        if (q.clickedButton() == withRules) {
            const auto rule = [](const char *proto, const char *ports, const char *note) {
                return QJsonObject{{"dir", "out"}, {"action", "allow"}, {"proto", proto}, {"ports", ports}, {"note", note}};
            };
            req[QStringLiteral("add")] = QJsonArray{rule("any", "53", "DNS"), rule("udp", "123", "NTP"), rule("any", "80, 443", "Web")};
        } else if (q.clickedButton() != bare) {
            restore();
            return;
        }
    }
    Q_EMIT changeRequested(req);
}

void FirewallTab::addRule() {
    QJsonObject r{{"dir", "in"}, {"action", "allow"}, {"proto", "tcp"}};
    if (ruleDialog(this, r, tr("Add firewall rule"))) Q_EMIT changeRequested({{"add", QJsonArray{r}}});
}

void FirewallTab::editRule() {
    const QList<int> ids = selectedIds();
    if (ids.size() != 1) return;
    for (const QJsonValue &x : fw_.value(QLatin1String("rules")).toArray()) {
        QJsonObject r = x.toObject();
        if (r.value(QLatin1String("id")).toInt() != ids.first()) continue;
        if (ruleDialog(this, r, tr("Edit firewall rule"))) Q_EMIT changeRequested({{"update", QJsonArray{r}}});
        return;
    }
}

void FirewallTab::toggleRules() {
    const QList<int> ids = selectedIds();
    QJsonArray update;
    for (const QJsonValue &x : fw_.value(QLatin1String("rules")).toArray()) {
        QJsonObject r = x.toObject();
        if (!ids.contains(r.value(QLatin1String("id")).toInt())) continue;
        r[QStringLiteral("enabled")] = !r.value(QLatin1String("enabled")).toBool(true);
        update.append(r);
    }
    if (!update.isEmpty()) Q_EMIT changeRequested({{"update", update}});
}

void FirewallTab::removeRules() {
    const QList<int> ids = selectedIds();
    if (ids.isEmpty() || QMessageBox::question(this, tr("Firewall"), tr("Remove %1 rule(s)?").arg(ids.size())) != QMessageBox::Yes) return;
    QJsonArray a;
    for (int id : ids) a.append(id);
    Q_EMIT changeRequested({{"remove", a}});
}

void FirewallTab::moveRule(int by) {
    const QList<int> ids = selectedIds();
    if (ids.size() == 1) Q_EMIT changeRequested({{"move", QJsonObject{{"id", ids.first()}, {"by", by}}}});
}

void FirewallTab::showInspection(const QJsonObject &report) {
    const QJsonObject hits = report.value(QLatin1String("hits")).toObject();
    hits_.clear();
    for (auto it = hits.constBegin(); it != hits.constEnd(); ++it)
        hits_.insert(it.key(), it.value().toObject().value(QLatin1String("packets")).toInteger());
    fill();
    QStringList out;
    const bool wanted = report.value(QLatin1String("active")).toBool(), present = report.value(QLatin1String("present")).toBool();
    if (!present) out << tr("PROBLEM: Centurion's table is not loaded, so neither the firewall nor the blacklist is being enforced. "
                            "Start the centurion-netguard service; if something ran \"nft flush ruleset\", reload it.");
    else if (wanted) out << tr("Centurion's table is loaded and the firewall is active.");
    else out << tr("Centurion's table is loaded; the firewall has nothing to enforce right now.");
    if (wanted && present)
        out << tr("New connections that reached the default policy: %1 incoming, %2 outgoing.")
                   .arg(hits_.value(QStringLiteral("default_fw_in"))).arg(hits_.value(QStringLiteral("default_fw_out")));
    out << QString() << tr("== Other firewalls on the same hooks ==");
    const QJsonArray others = report.value(QLatin1String("others")).toArray();
    if (others.isEmpty()) out << tr("None. Centurion's rules decide alone.");
    bool drops = false;
    for (const QJsonValue &x : others) {
        const QJsonObject o = x.toObject();
        const bool drop = str(o, "policy") == QLatin1String("drop");
        drops |= drop;
        out << QStringLiteral("%1 %2 / %3: %4, policy %5").arg(str(o, "family"), str(o, "table"), str(o, "chain"), str(o, "hook"), str(o, "policy"));
    }
    if (!others.isEmpty())
        out << tr("A packet must pass every one of these. A drop there wins over an \"allow\" here, so a port opened here can still be closed by them.");
    if (drops) out << tr("At least one of them drops by default: check it if an allowed port stays unreachable.");
    QStringList legacy;
    for (const QJsonValue &x : report.value(QLatin1String("legacy")).toArray()) legacy << x.toString();
    if (!legacy.isEmpty()) out << tr("iptables-legacy tables are also loaded (%1); nft cannot show their rules. Check them with iptables -S.").arg(legacy.join(QStringLiteral(", ")));
    out << QString() << tr("== Loaded rules (address lists omitted) ==") << str(report, "rules");

    auto *d = new QDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(tr("Live firewall rules"));
    d->resize(760, 560);
    auto *v = new QVBoxLayout(d);
    auto *text = new QPlainTextEdit(out.join(QLatin1Char('\n')));
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(text, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(box, &QDialogButtonBox::rejected, d, &QDialog::close);
    v->addWidget(box);
    d->show();
}
