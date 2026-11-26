#include "unlock.h"

#include "privileged.h"

#include <QInputDialog>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QWidget>

namespace unlock {

namespace {
bool pinShape(const QString &p) { return QRegularExpression(QStringLiteral("^[0-9]{4}$")).match(p).hasMatch(); }

/// One PIN entry; empty when cancelled.
QString readPin(QWidget *parent, const QString &title, const QString &label) {
    bool ok = false;
    const QString pin = QInputDialog::getText(parent, title, label, QLineEdit::Password, QString(), &ok);
    return ok ? pin.trimmed() : QString();
}
}  // namespace

Manager &Manager::instance() { static Manager m; return m; }

void Manager::setUnlocked(bool on) {
    if (unlocked_ == on) return;
    unlocked_ = on;
    Q_EMIT changed(on);
}

void Manager::requestUnlock(QWidget *parent) {
    if (busy_ || revoking_) return;
    busy_ = true;
    hiddenDuringUnlock_ = false;
    QPointer<QWidget> guard(parent);
    privileged::run(privileged::helperPath(QStringLiteral("tune-profile-helper")), QJsonObject{{QStringLiteral("op"), QStringLiteral("pin_status")}}, this,
                    [this, guard](const privileged::Result &r) {
        if (hiddenDuringUnlock_ || !guard) { busy_ = false; return; }
        if (!r.ok()) { QMessageBox::warning(guard, tr("Unlock"), r.message()); busy_ = false; return; }
        const QJsonObject s = r.json;
        if (!s.value(QStringLiteral("pin_set")).toBool()) {
            if (QMessageBox::question(guard, tr("Set an unlock PIN"),
                    tr("Writing new values (undervolt, curves, power limits, kernel tunables) needs an unlock PIN.\n\n"
                       "Set a 4-digit PIN now? The administrator password is asked once to store it.")) == QMessageBox::Yes
                && guard && !hiddenDuringUnlock_) { setNewPin(guard); return; }
            busy_ = false;
            return;
        }
        if (s.value(QStringLiteral("disabled")).toBool()) {
            if (QMessageBox::warning(guard, tr("Unlock"), tr("Too many wrong PINs: the PIN is disabled. Set a new one (administrator password)?"),
                                     QMessageBox::Yes | QMessageBox::Cancel) == QMessageBox::Yes
                && guard && !hiddenDuringUnlock_) { setNewPin(guard); return; }
            busy_ = false;
            return;
        }
        const int wait = s.value(QStringLiteral("wait_seconds")).toInt();
        if (wait > 0) {
            QMessageBox::warning(guard, tr("Unlock"), tr("Too many wrong PINs: try again in %1 s.").arg(wait));
            busy_ = false;
            return;
        }
        askPin(guard, QString());
    });
}

void Manager::askPin(QWidget *parent, const QString &note) {
    QPointer<QWidget> guard(parent);
    const QString pin = readPin(parent, tr("Unlock Centurion"), (note.isEmpty() ? QString() : note + QStringLiteral("\n\n")) + tr("PIN (4 digits):"));
    if (pin.isEmpty() || !guard || hiddenDuringUnlock_) { busy_ = false; return; }
    if (!pinShape(pin)) { askPin(guard, tr("The PIN is 4 digits.")); return; }
    submitPin(guard, pin, true);
}

void Manager::submitPin(QWidget *parent, const QString &pin, bool retryWrongPin) {
    QPointer<QWidget> guard(parent);
    privileged::runUntilExit(privileged::helperPath(QStringLiteral("tune-profile-helper")),
                    QJsonObject{{QStringLiteral("op"), QStringLiteral("unlock")}, {QStringLiteral("pin"), pin}}, this,
                    [this, guard, retryWrongPin](const privileged::Result &r) {
        if (hiddenDuringUnlock_ || !guard) {
            busy_ = false;
            revokeGrant(true);
            return;
        }
        if (r.ok()) { busy_ = false; setUnlocked(true); return; }
        if (retryWrongPin && r.reached && r.json.value(QStringLiteral("wrong_pin")).toBool()
            && !r.json.value(QStringLiteral("disabled")).toBool()
            && r.json.value(QStringLiteral("wait_seconds")).toInt() == 0) {
            askPin(guard, r.message());
            return;
        }
        QMessageBox::warning(guard, tr("Unlock"), r.message());
        busy_ = false;
    });
}

void Manager::lock(bool keepScenes) {
    hiddenDuringUnlock_ = true;
    if (!unlocked_) return;
    setUnlocked(false);
    revokeGrant(keepScenes);
}

void Manager::revokeGrant(bool keepScenes) {
    if (revoking_) return;
    revoking_ = true;
    privileged::runUntilExit(privileged::helperPath(QStringLiteral("tune-profile-helper")),
                    QJsonObject{{QStringLiteral("op"), QStringLiteral("lock")}, {QStringLiteral("scenes"), keepScenes}}, this,
                    [this](const privileged::Result &) { revoking_ = false; });
}

void Manager::changePin(QWidget *parent) {
    if (busy_ || revoking_) return;
    busy_ = true;
    hiddenDuringUnlock_ = false;
    setNewPin(parent);
}

void Manager::setNewPin(QWidget *parent) {
    QPointer<QWidget> guard(parent);
    const QString a = readPin(parent, tr("Set unlock PIN"), tr("New PIN (4 digits):"));
    if (a.isEmpty() || !guard || hiddenDuringUnlock_) { busy_ = false; return; }
    if (!pinShape(a)) { QMessageBox::warning(parent, tr("Set unlock PIN"), tr("The PIN is 4 digits.")); busy_ = false; return; }
    const QString b = readPin(parent, tr("Set unlock PIN"), tr("The same PIN again:"));
    if (hiddenDuringUnlock_ || !guard || b != a) {
        if (guard && !hiddenDuringUnlock_ && !b.isEmpty()) QMessageBox::warning(guard, tr("Set unlock PIN"), tr("The two PINs differ."));
        busy_ = false;
        return;
    }
    privileged::run(privileged::helperPath(QStringLiteral("tune-admin-helper")),
                    QJsonObject{{QStringLiteral("op"), QStringLiteral("pin_set")}, {QStringLiteral("pin"), a}}, this,
                    [this, guard, a](const privileged::Result &r) {
        if (r.ok()) setUnlocked(false);  // pin_set drops every grant
        if (hiddenDuringUnlock_ || !guard) { busy_ = false; return; }
        if (!r.ok()) { QMessageBox::warning(guard, tr("Set unlock PIN"), r.message()); busy_ = false; return; }
        submitPin(guard, a, false);
    });
}

}  // namespace unlock
