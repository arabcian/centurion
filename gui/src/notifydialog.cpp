#include "notifydialog.h"

#include "notifyprefs.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

NotifyDialog::NotifyDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Notifications"));
    auto *v = new QVBoxLayout(this);

    auto *about = new QLabel(tr("Choose which alerts reach the desktop. Hidden alerts are still listed where they came from "
                                "(Health, Network, Curve, Backup)."));
    about->setWordWrap(true);
    v->addWidget(about);

    const char *const labels[] = {
        QT_TR_NOOP("Hardware and driver errors (Health)"),
        QT_TR_NOOP("Blocked connections (Network guard)"),
        QT_TR_NOOP("Backup reminders and finished backups"),
        QT_TR_NOOP("Curve guard (suspect or reset curves)"),
        QT_TR_NOOP("Disk health (SMART)"),
        QT_TR_NOOP("Lid closed while the machine stays awake"),
    };
    const notifyprefs::Prefs p = notifyprefs::load();
    for (int i = 0; i < 6; ++i) {
        categories_[i] = new QCheckBox(tr(labels[i]));
        categories_[i]->setChecked(!p.off.contains(notifyprefs::ALL_CATEGORIES.at(i)));
        v->addWidget(categories_[i]);
    }

    criticalOnly_ = new QCheckBox(tr("Hardware and driver alerts: critical only (hide plain errors)"));
    criticalOnly_->setChecked(p.criticalOnly);
    v->addWidget(criticalOnly_);
    deferGame_ = new QCheckBox(tr("While a game runs, hold these alerts and send one summary when it ends"));
    deferGame_->setChecked(p.deferGame);
    v->addWidget(deferGame_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { save(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

void NotifyDialog::save() {
    notifyprefs::Prefs p;
    for (int i = 0; i < 6; ++i) if (!categories_[i]->isChecked()) p.off << notifyprefs::ALL_CATEGORIES.at(i);
    p.criticalOnly = criticalOnly_->isChecked();
    p.deferGame = deferGame_->isChecked();
    notifyprefs::save(p);
}
