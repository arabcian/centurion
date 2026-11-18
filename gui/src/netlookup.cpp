#include "netlookup.h"

#include "netrules.h"
#include "theme.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDnsLookup>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

enum Tool { Whois, Dns, Ports, Ping, Trace };
static constexpr int MAX_PORTS = 64, PORT_TIMEOUT_MS = 3000, WHOIS_TIMEOUT_MS = 40000;

static QString str(const QJsonObject &o, const char *k) { return o.value(QLatin1String(k)).toString(); }

// The field names netguard-helper reports, so that they can be translated.
static const char *const SUMMARY_LABELS[] = {
    QT_TRANSLATE_NOOP("LookupTab", "Range"), QT_TRANSLATE_NOOP("LookupTab", "CIDR"), QT_TRANSLATE_NOOP("LookupTab", "Network"),
    QT_TRANSLATE_NOOP("LookupTab", "Organisation"), QT_TRANSLATE_NOOP("LookupTab", "Country"), QT_TRANSLATE_NOOP("LookupTab", "AS"),
    QT_TRANSLATE_NOOP("LookupTab", "AS name"), QT_TRANSLATE_NOOP("LookupTab", "Abuse contact"), QT_TRANSLATE_NOOP("LookupTab", "Registered"),
    QT_TRANSLATE_NOOP("LookupTab", "Updated"), QT_TRANSLATE_NOOP("LookupTab", "Status"), QT_TRANSLATE_NOOP("LookupTab", "Domain"),
    QT_TRANSLATE_NOOP("LookupTab", "Registrar"), QT_TRANSLATE_NOOP("LookupTab", "Expires"), QT_TRANSLATE_NOOP("LookupTab", "Name servers"),
    QT_TRANSLATE_NOOP("LookupTab", "Origin AS"), QT_TRANSLATE_NOOP("LookupTab", "BGP prefix"), QT_TRANSLATE_NOOP("LookupTab", "RIR"),
    QT_TRANSLATE_NOOP("LookupTab", "Allocated"),
};

LookupTab::LookupTab(HelperCall helper, QWidget *parent) : QWidget(parent), helper_(std::move(helper)) {
    Q_UNUSED(SUMMARY_LABELS);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    auto *row = new QHBoxLayout;
    tool_ = new QComboBox;
    tool_->addItem(tr("Whois"));
    tool_->addItem(tr("DNS records"));
    tool_->addItem(tr("Port check"));
    tool_->addItem(tr("Ping"));
    tool_->addItem(tr("Traceroute"));
    row->addWidget(tool_);
    target_ = new QLineEdit;
    target_->setClearButtonEnabled(true);
    row->addWidget(target_, 2);
    extraLabel_ = new QLabel;
    row->addWidget(extraLabel_);
    extra_ = new QLineEdit;
    row->addWidget(extra_, 1);
    routing_ = new QCheckBox(tr("Routing data"));
    routing_->setChecked(true);
    routing_->setToolTip(tr("Also asks Team Cymru's public service (whois.cymru.com) which network announces the address: origin AS, BGP prefix, AS name.\n"
                            "The address or AS number is sent to that service as well as to the registries."));
    row->addWidget(routing_);
    run_ = new QPushButton(tr("Run"));
    run_->setDefault(true);
    row->addWidget(run_);
    stop_ = new QPushButton(tr("Stop"));
    stop_->setEnabled(false);
    row->addWidget(stop_);
    auto *copy = new QPushButton(tr("Copy"));
    row->addWidget(copy);
    v->addLayout(row);
    state_ = new QLabel;
    state_->setProperty("role", "muted");
    state_->setWordWrap(true);
    v->addWidget(state_);
    summary_ = new QLabel;
    summary_->setTextFormat(Qt::RichText);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_->setWordWrap(true);
    v->addWidget(summary_);
    output_ = new QPlainTextEdit;
    output_->setReadOnly(true);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(output_, 1);

    connect(tool_, &QComboBox::currentIndexChanged, this, &LookupTab::toolChanged);
    connect(run_, &QPushButton::clicked, this, &LookupTab::run);
    connect(stop_, &QPushButton::clicked, this, [this] { stop(); finish(tr("Stopped.")); });
    connect(target_, &QLineEdit::returnPressed, this, &LookupTab::run);
    connect(extra_, &QLineEdit::returnPressed, this, &LookupTab::run);
    connect(copy, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(output_->toPlainText()); });
    toolChanged();
}

void LookupTab::toolChanged() {
    const int t = tool_->currentIndex();
    extra_->clear();
    const bool hasExtra = t == Dns || t == Ports;
    extra_->setVisible(hasExtra);
    extraLabel_->setVisible(hasExtra);
    routing_->setVisible(t == Whois);
    if (t == Dns) {
        extraLabel_->setText(tr("Server"));
        extra_->setPlaceholderText(tr("system resolver (or 1.1.1.1)"));
    } else if (t == Ports) {
        extraLabel_->setText(tr("Ports"));
        extra_->setPlaceholderText(QStringLiteral("22, 80, 443, 8000-8010"));
    }
    target_->setPlaceholderText(t == Whois ? tr("IP address, CIDR block, AS number (AS15169) or domain")
                                : t == Dns ? tr("domain name, or an IP address for its reverse name") : tr("host name or IP address"));
}

void LookupTab::whois(const QString &target) {
    if (!run_->isEnabled()) { stop(); finish(QString()); }   // this question replaces the one still running
    tool_->setCurrentIndex(Whois);
    target_->setText(target);
    run();
}

void LookupTab::stop() {
    ++generation_;
    if (!job_) return;
    for (QProcess *p : job_->findChildren<QProcess *>()) { p->kill(); p->waitForFinished(300); }
    delete job_;
    job_ = nullptr;
}

void LookupTab::finish(const QString &status) {
    // Later, not now: this usually runs inside a signal of something the job owns.
    if (job_) job_->deleteLater();
    job_ = nullptr;
    state_->setText(status);
    run_->setEnabled(true);
    stop_->setEnabled(false);
}

void LookupTab::run() {
    if (!run_->isEnabled()) return;
    const QString target = lookupHost(target_->text());
    if (target.isEmpty()) { target_->setFocus(); return; }
    stop();
    output_->clear();
    summary_->clear();
    run_->setEnabled(false);
    stop_->setEnabled(true);
    state_->setText(tr("Working…"));
    job_ = new QObject(this);
    switch (tool_->currentIndex()) {
    case Whois: runWhois(target); break;
    case Dns: runDns(target); break;
    case Ports: runPorts(target); break;
    case Ping:
        if (!validHost(target)) { finish(tr("That is not a host name or address.")); return; }
        runCommand(QStringLiteral("ping"), {QStringLiteral("-c"), QStringLiteral("4"), QStringLiteral("-W"), QStringLiteral("2"), target});
        break;
    default: {
        if (!validHost(target)) { finish(tr("That is not a host name or address.")); return; }
        const bool full = !QStandardPaths::findExecutable(QStringLiteral("traceroute")).isEmpty();
        runCommand(full ? QStringLiteral("traceroute") : QStringLiteral("tracepath"), {target});
    }
    }
}

void LookupTab::runWhois(const QString &target) {
    const quint64 gen = generation_;
    QJsonObject req{{"op", "whois"}, {"query", target}, {"asn", routing_->isChecked()}};
    helper_(req, [this, gen](const QJsonObject &r) {
        if (gen != generation_) return;
        if (!r.value(QLatin1String("ok")).toBool()) {
            finish(QStringLiteral("whois: ") + (r.isEmpty() ? tr("no answer from the helper") : str(r, "error")));
            return;
        }
        QStringList rows;
        for (const QJsonValue &x : r.value(QLatin1String("summary")).toArray()) {
            const QJsonArray pair = x.toArray();
            const QByteArray label = pair.at(0).toString().toUtf8();
            rows << QStringLiteral("<tr><td>%1:&nbsp;&nbsp;</td><td><b>%2</b></td></tr>").arg(tr(label.constData()).toHtmlEscaped(), pair.at(1).toString().toHtmlEscaped());
        }
        summary_->setText(QStringLiteral("<table>%1</table>").arg(rows.join(QString())));
        QStringList text;
        for (const QJsonValue &x : r.value(QLatin1String("hops")).toArray()) {
            const QJsonObject hop = x.toObject();
            text << QStringLiteral("=== %1 ===").arg(str(hop, "server")) << str(hop, "text").trimmed() << QString();
        }
        output_->setPlainText(text.join(QLatin1Char('\n')));
        QString status = rows.isEmpty() ? tr("The registry sent no structured fields; its full answer is below.")
                         : str(r, "server").isEmpty() ? QString() : tr("Source: %1").arg(str(r, "server"));
        if (!str(r, "warning").isEmpty()) status += QStringLiteral(" · ") + tr("incomplete: %1").arg(str(r, "warning"));
        finish(status);
    }, WHOIS_TIMEOUT_MS);
}

void LookupTab::runDns(const QString &target) {
    QHostAddress server;
    const QString serverText = extra_->text().trimmed();
    if (!serverText.isEmpty() && !server.setAddress(serverText)) { finish(tr("The DNS server must be an IP address.")); return; }
    const QString reverse = reverseDnsName(target);
    struct Q { QDnsLookup::Type type; const char *name; };
    QList<Q> questions;
    if (!reverse.isEmpty()) questions = {{QDnsLookup::PTR, "PTR"}};
    else questions = {{QDnsLookup::A, "A"}, {QDnsLookup::AAAA, "AAAA"}, {QDnsLookup::CNAME, "CNAME"}, {QDnsLookup::MX, "MX"}, {QDnsLookup::NS, "NS"}, {QDnsLookup::TXT, "TXT"}};
    const QString name = reverse.isEmpty() ? target : reverse;
    auto answers = std::make_shared<QMap<int, QStringList>>();
    auto left = std::make_shared<int>(questions.size());
    auto problem = std::make_shared<QString>();
    for (int i = 0; i < questions.size(); ++i) {
        auto *l = new QDnsLookup(questions[i].type, name, job_);
        if (!server.isNull()) l->setNameserver(server);
        connect(l, &QDnsLookup::finished, job_, [=, this, type = questions[i].type, label = QString::fromLatin1(questions[i].name)] {
            QStringList &out = (*answers)[i];
            const auto line = [&](quint32 ttl, const QString &value) { out << QStringLiteral("%1  %2  %3 s").arg(label.leftJustified(6), value).arg(ttl); };
            // An answer also carries the aliases on the way to it: only what was asked for is listed under its type.
            if (type == QDnsLookup::A || type == QDnsLookup::AAAA) for (const auto &r : l->hostAddressRecords()) line(r.timeToLive(), r.value().toString());
            if (type == QDnsLookup::CNAME) for (const auto &r : l->canonicalNameRecords()) line(r.timeToLive(), r.value());
            for (const auto &r : l->mailExchangeRecords()) line(r.timeToLive(), QStringLiteral("%1 %2").arg(r.preference()).arg(r.exchange()));
            for (const auto &r : l->nameServerRecords()) line(r.timeToLive(), r.value());
            for (const auto &r : l->pointerRecords()) line(r.timeToLive(), r.value());
            for (const auto &r : l->textRecords()) {
                QStringList parts;
                for (const QByteArray &b : r.values()) parts << QString::fromUtf8(b);
                line(r.timeToLive(), QLatin1Char('"') + parts.join(QString()) + QLatin1Char('"'));
            }
            if (l->error() != QDnsLookup::NoError && l->error() != QDnsLookup::NotFoundError) *problem = l->errorString();
            if (--*left > 0) return;
            QStringList all;
            for (const QStringList &part : std::as_const(*answers)) all << part;
            output_->setPlainText(all.join(QLatin1Char('\n')));
            finish(!all.isEmpty() ? tr("%1 record(s) for %2").arg(all.size()).arg(name)
                   : problem->isEmpty() ? tr("No records for %1.").arg(name) : tr("DNS lookup failed: %1").arg(*problem));
        });
        l->lookup();
    }
}

void LookupTab::runPorts(const QString &target) {
    const QList<int> ports = expandPorts(extra_->text(), MAX_PORTS);
    if (ports.isEmpty()) { finish(tr("Name the ports to check: numbers or ranges, at most %1.").arg(MAX_PORTS)); extra_->setFocus(); return; }
    auto results = std::make_shared<QMap<int, QString>>();
    auto total = ports.size();
    auto report = [this, results, total, target](int port, const QString &result) {
        if (results->contains(port)) return;
        results->insert(port, result);
        QStringList lines;
        int open = 0;
        for (auto it = results->cbegin(); it != results->cend(); ++it) {
            lines << QStringLiteral("%1  %2").arg(QString::number(it.key()).rightJustified(5), it.value());
            open += it.value() == tr("open");
        }
        output_->setPlainText(lines.join(QLatin1Char('\n')));
        if (results->size() == total) finish(tr("%1: %2 of %3 port(s) open over TCP").arg(target).arg(open).arg(total));
    };
    for (int port : ports) {
        auto *s = new QTcpSocket(job_);
        s->setProxy(QNetworkProxy::NoProxy);
        connect(s, &QTcpSocket::connected, job_, [=, this] { report(port, tr("open")); s->abort(); });
        connect(s, &QTcpSocket::errorOccurred, job_, [=, this](QAbstractSocket::SocketError e) {
            report(port, e == QAbstractSocket::ConnectionRefusedError ? tr("closed (refused)") : s->errorString());
        });
        QTimer::singleShot(PORT_TIMEOUT_MS, s, [=, this] { report(port, tr("no answer (filtered, or the host is down)")); s->abort(); });
        s->connectToHost(target, quint16(port));
    }
}

void LookupTab::runCommand(const QString &program, const QStringList &args) {
    const QString path = QStandardPaths::findExecutable(program, {QStringLiteral("/usr/bin"), QStringLiteral("/bin"), QStringLiteral("/usr/sbin"), QStringLiteral("/sbin")});
    if (path.isEmpty()) { finish(tr("%1 is not installed.").arg(program)); return; }
    auto *p = new QProcess(job_);
    p->setProcessChannelMode(QProcess::MergedChannels);
    connect(p, &QProcess::readyRead, job_, [this, p] {
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(QString::fromLocal8Bit(p->readAll()));
    });
    connect(p, &QProcess::finished, job_, [this, p](int code, QProcess::ExitStatus) {
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(QString::fromLocal8Bit(p->readAll()));
        finish(code == 0 ? tr("Done.") : tr("Finished with exit code %1.").arg(code));
    });
    connect(p, &QProcess::errorOccurred, job_, [this, program](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish(tr("%1 could not be started.").arg(program));
    });
    p->start(path, args);
}
