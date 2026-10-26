#include "trialbar.h"
#include "theme.h"
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

static constexpr int TRIAL_SECONDS = 30;

static QSettings guiIni() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"),
                     QSettings::IniFormat);
}

TrialBar::TrialBar(const QString &key, QWidget *parent) : QFrame(parent), key_(key) {
    setObjectName("trialBar");
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(6, 2, 6, 2);
    on_ = new QCheckBox(QStringLiteral("Trial apply"));
    on_->setToolTip(QStringLiteral("After every successful apply, revert to stock in %1 s unless you press Keep.\n"
                                   "For testing a new curve: if the screen glitches or a game hangs, just wait.").arg(TRIAL_SECONDS));
    on_->setChecked(guiIni().value(key_, false).toBool());
    connect(on_, &QCheckBox::toggled, this, [this](bool v) { guiIni().setValue(key_, v); if (!v) cancel(); });
    h->addWidget(on_);
    text_ = new QLabel;
    text_->setTextFormat(Qt::RichText);
    h->addWidget(text_, 1);
    keep_ = new QPushButton(QStringLiteral("Keep"));
    keep_->setObjectName("btnAccent");
    revert_ = new QPushButton(QStringLiteral("Revert now"));
    for (QPushButton *b : {keep_, revert_}) { b->setObjectName(b == keep_ ? "btnAccent" : "btnMini"); b->hide(); h->addWidget(b); }
    connect(keep_, &QPushButton::clicked, this, [this] { finish(true); });
    connect(revert_, &QPushButton::clicked, this, [this] { finish(false); });
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (--left_ <= 0) { finish(false); return; }
        text_->setText(QStringLiteral("<b style='color:%1'>Testing — back to stock in %2 s unless you keep it.</b>").arg(theme::WARN).arg(left_));
    });
}

bool TrialBar::wanted() const { return on_->isChecked(); }
bool TrialBar::running() const { return timer_->isActive(); }

void TrialBar::offer(std::function<void()> revert) {
    if (!wanted()) return;
    revertFn_ = std::move(revert);
    left_ = TRIAL_SECONDS;
    text_->setText(QStringLiteral("<b style='color:%1'>Testing — back to stock in %2 s unless you keep it.</b>").arg(theme::WARN).arg(left_));
    keep_->show();
    revert_->show();
    timer_->start();
}

void TrialBar::cancel() {
    if (!timer_->isActive()) return;  // nothing running (keeps a "Kept" / "Reverted" note visible)
    timer_->stop();
    revertFn_ = nullptr;
    keep_->hide();
    revert_->hide();
    text_->clear();
}

void TrialBar::finish(bool keep) {
    auto fn = std::move(revertFn_);
    timer_->stop();
    keep_->hide();
    revert_->hide();
    if (keep) { text_->setText(QStringLiteral("<span style='color:%1'>Kept.</span>").arg(theme::OK)); return; }
    text_->setText(QStringLiteral("<span style='color:%1'>Reverted to stock.</span>").arg(theme::FG_DIM));
    if (fn) fn();
}
