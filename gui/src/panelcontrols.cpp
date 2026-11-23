#include "panelcontrols.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QVBoxLayout>

PanelControlsDialog::PanelControlsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Panel Controls"));
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("The built-in screen's brightness and refresh rate. Brightness is the backlight device in "
                                "/sys/class/backlight; the refresh rate goes through the desktop's own tool (kscreen-doctor, "
                                "wlr-randr or xrandr), because the compositor owns it. Nothing needs a password."));
    intro->setWordWrap(true);
    intro->setProperty("role", "muted");
    v->addWidget(intro);

    auto *form = new QFormLayout;
    bright_ = new QSlider(Qt::Horizontal);
    bright_->setRange(1, 100);
    brightValue_ = new QLabel(QStringLiteral("—"));
    setBright_ = new QPushButton(tr("Set brightness"));
    auto *b = new QHBoxLayout;
    b->addWidget(bright_, 1);
    b->addWidget(brightValue_);
    b->addWidget(setBright_);
    form->addRow(tr("Brightness"), b);
    rates_ = new QComboBox;
    setRate_ = new QPushButton(tr("Set refresh rate"));
    auto *r = new QHBoxLayout;
    r->addWidget(rates_, 1);
    r->addWidget(setRate_);
    form->addRow(tr("Refresh rate"), r);
    v->addLayout(form);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setProperty("role", "muted");
    v->addWidget(status_);
    auto *close = new QPushButton(tr("Close"));
    v->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    setBright_->setEnabled(false);
    setRate_->setEnabled(false);
    connect(bright_, &QSlider::valueChanged, this, [this](int p) { brightValue_->setText(QStringLiteral("%1 %").arg(p)); });
    connect(setBright_, &QPushButton::clicked, this, [this] {
        run({QStringLiteral("set-brightness"), QString::number(bright_->value())}, [this](const QJsonObject &o) {
            status_->setText(o.value("ok").toBool() ? o.value("message").toString() : o.value("error").toString(tr("centurion-display failed")));
        });
    });
    connect(setRate_, &QPushButton::clicked, this, [this] {
        run({QStringLiteral("set-refresh"), QString::number(rates_->currentData().toDouble())}, [this](const QJsonObject &o) {
            status_->setText(o.value("ok").toBool() ? o.value("message").toString() : o.value("error").toString(tr("centurion-display failed")));
        });
    });

    run({QStringLiteral("brightness")}, [this](const QJsonObject &o) {
        if (!o.value("ok").toBool()) { status_->setText(tr("Brightness: %1").arg(o.value("error").toString(tr("centurion-display failed")))); return; }
        bright_->setValue(qBound(1, o.value("percent").toInt(), 100));
        setBright_->setEnabled(true);
    });
    run({QStringLiteral("list")}, [this](const QJsonObject &o) {
        if (!o.value("ok").toBool()) { status_->setText(status_->text() + QLatin1Char('\n') + tr("Refresh rate: %1").arg(o.value("error").toString(tr("centurion-display failed")))); return; }
        const double cur = o.value("current").toDouble(-1);
        for (const QJsonValue &hz : o.value("rates").toArray()) {
            rates_->addItem(tr("%1 Hz").arg(hz.toDouble(), 0, 'f', 2), hz.toDouble());
            if (qAbs(hz.toDouble() - cur) < 0.01) rates_->setCurrentIndex(rates_->count() - 1);
        }
        setRate_->setEnabled(rates_->count() > 0);
    });
}

void PanelControlsDialog::run(const QStringList &args, std::function<void(const QJsonObject &)> done) {
    QString tool = QStandardPaths::findExecutable(QStringLiteral("centurion-display"));
    if (tool.isEmpty()) tool = QStringLiteral("/usr/bin/centurion-display");
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [p, done](int, QProcess::ExitStatus) {
        done(QJsonDocument::fromJson(p->readAllStandardOutput().trimmed().split('\n').last()).object());
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [p, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        done(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), tr("centurion-display not found.")}});
        p->deleteLater();
    });
    p->start(tool, args);
}
