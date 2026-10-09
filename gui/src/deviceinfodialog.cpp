#include "deviceinfodialog.h"
#include "privileged.h"
#include "theme.h"
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

DeviceInfoDialog::DeviceInfoDialog(const QString &helper, QWidget *parent) : QDialog(parent), helper_(helper) {
    setWindowTitle("Device information");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(700, 620);
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel("What this machine's firmware reports about itself. Centurion offers an optional "
                             "feature only where the firmware lists it here. Nothing on this page changes a setting.");
    intro->setWordWrap(true);
    v->addWidget(intro);

    text_ = new QPlainTextEdit;
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(text_, 1);

    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::RichText);
    v->addWidget(status_);

    auto *h = new QHBoxLayout;
    auto *reload = new QPushButton("Reload");
    auto *copy = new QPushButton("Copy report");
    copy->setToolTip("Copy the text — e.g. to attach to a bug report for a model other than the developer's.");
    auto *close = new QPushButton("Close");
    h->addWidget(reload);
    h->addWidget(copy);
    h->addStretch(1);
    h->addWidget(close);
    v->addLayout(h);
    connect(reload, &QPushButton::clicked, this, &DeviceInfoDialog::load);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(text_->toPlainText()); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    load();
}

void DeviceInfoDialog::load() {
    status_->setText(QStringLiteral("<span style='color:%1'>Reading…</span>").arg(theme::MUTED));
    privileged::run(helper_, QJsonObject{{"machine", "report"}}, this, [this](const privileged::Result &r) {
        if (!r.ok()) {
            status_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::DANGER, r.message().toHtmlEscaped()));
            return;
        }
        const QJsonObject &j = r.json;
        QStringList out;
        auto line = [&out](const QString &k, const QString &val) { if (!val.isEmpty()) out << QStringLiteral("%1 %2").arg(k + ':', -22).arg(val); };
        auto strList = [](const QJsonValue &a) { QStringList l; for (const QJsonValue &x : a.toArray()) l << x.toString(); return l; };
        auto num = [&j](const char *k) { return j.value(QLatin1String(k)).isDouble() ? QString::number(j.value(QLatin1String(k)).toInt()) : QString(); };
        line("Model", j.value("model").toString());
        line("Machine type", j.value("machine_type").toString());
        line("Series", j.value("series").toString());
        line("Generation", j.value("generation").toInt() > 0 ? QString::number(j.value("generation").toInt()) : QStringLiteral("unknown"));
        line("BIOS", j.value("bios").toString());
        line("acpi_call", j.value("acpi_call").toBool() ? QStringLiteral("loaded") : QStringLiteral("not loaded (firmware queries unavailable)"));
        line("Lenovo WMI", strList(j.value("wmi")).join(QStringLiteral(", ")));
        line("VPC2004", j.value("vpc2004").toString());
        line("SmartFan version", num("smartfan_version"));
        line("Legion Zone version", num("legion_zone_version"));
        line("Thermal mode (raw)", num("thermal_mode"));
        line("Power modes", strList(j.value("power_modes")).join(QStringLiteral(", ")));
        line("Note", j.value("note").toString());
        const QJsonArray caps = j.value("capabilities").toArray();
        out << QString();
        if (caps.isEmpty()) {
            out << QStringLiteral("Capability list: not available on this firmware (features that depend on it are not offered).");
        } else {
            out << QStringLiteral("Capability list (%1 entries)").arg(caps.size());
            out << QStringLiteral("  %1 %2 %3 %4").arg(QStringLiteral("ID"), -11).arg(QStringLiteral("flags"), -6).arg(QStringLiteral("default"), -8).arg(QStringLiteral("name"));
            for (const QJsonValue &cv : caps) {
                const QJsonObject c = cv.toObject();
                const QString flags = QStringLiteral("%1%2%3").arg(c.value("valid").toBool() ? 'v' : '-')
                    .arg(c.value("get").toBool() ? 'r' : '-').arg(c.value("set").toBool() ? 'w' : '-');
                out << QStringLiteral("  %1 %2 %3 %4").arg(c.value("id").toString(), -11).arg(flags, -6)
                           .arg(QString::number(qint64(c.value("default").toDouble())), -8).arg(c.value("name").toString());
            }
            out << QString() << QStringLiteral("flags: v = valid on this machine, r = readable, w = writable");
        }
        text_->setPlainText(out.join('\n'));
        status_->clear();
    });
}
