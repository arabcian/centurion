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
    text_->setTextFormat(Qt::PlainText);
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
        text_->setText(tr("Testing — back to stock in %1 s unless you keep it.").arg(left_));
    });
}

bool TrialBar::wanted() const { return on_->isChecked(); }
bool TrialBar::running() const { return timer_->isActive(); }

void TrialBar::offer(std::function<void()> revert) {
    if (!wanted()) return;
    revertFn_ = std::move(revert);
    left_ = TRIAL_SECONDS;
    theme::setLiveSheet(text_, [] { return QStringLiteral("color:%1;font-weight:700;").arg(theme::WARN); });
    text_->setText(tr("Testing — back to stock in %1 s unless you keep it.").arg(left_));
    keep_->show();
    revert_->show();
    timer_->start();
}

void TrialBar::cancel() {
    if (!timer_->isActive()) return;  // nothing running (keeps the last result/request note visible)
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
    if (keep) {
        theme::setLiveSheet(text_, [] { return QStringLiteral("color:%1;").arg(theme::OK); });
        text_->setText(tr("Kept.")); return;
    }
    // The reset callback may complete asynchronously or fail. Requesting it is
    // not evidence of successful restoration; the owning curve panel reports results.
    theme::setLiveSheet(text_, [] { return QStringLiteral("color:%1;").arg(theme::WARN); });
    text_->setText(fn ? tr("Revert requested; see the curve panel for the result.")
                      : tr("Revert unavailable: no reset callback."));
    if (fn) fn();
}
