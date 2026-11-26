#include "unlock.h"

#include "privileged.h"

#include <QCoreApplication>
#include <QInputDialog>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>

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

void Manager::requestUnlock(QWidget *parent, std::function<void(bool)> done) {
    if (!done) done = [](bool) {};
    if (asking_) { done(false); return; }
    asking_ = true;
    QPointer<QWidget> guard(parent);
    privileged::run(privileged::helperPath(QStringLiteral("tune-profile-helper")), QJsonObject{{QStringLiteral("op"), QStringLiteral("pin_status")}}, this,
                    [this, guard, done](const privileged::Result &r) {
        asking_ = false;
        if (!r.reached) { QMessageBox::warning(guard, tr("Unlock"), r.message()); done(false); return; }
        const QJsonObject s = r.json;
        if (s.value(QStringLiteral("unlocked")).toBool() && !s.value(QStringLiteral("scenes_only")).toBool()) { setUnlocked(true); done(true); return; }
        if (!s.value(QStringLiteral("pin_set")).toBool()) {
            if (QMessageBox::question(guard, tr("Set an unlock PIN"),
                    tr("Writing new values (undervolt, curves, power limits, kernel tunables) needs an unlock PIN.\n\n"
                       "Set a 4-digit PIN now? The administrator password is asked once to store it.")) != QMessageBox::Yes) { done(false); return; }
            changePin(guard, done);
            return;
        }
        if (s.value(QStringLiteral("disabled")).toBool()) {
            if (QMessageBox::warning(guard, tr("Unlock"), tr("Too many wrong PINs: the PIN is disabled. Set a new one (administrator password)?"),
                                     QMessageBox::Yes | QMessageBox::Cancel) == QMessageBox::Yes) { changePin(guard, done); return; }
            done(false);
            return;
        }
        const int wait = s.value(QStringLiteral("wait_seconds")).toInt();
        if (wait > 0) { QMessageBox::warning(guard, tr("Unlock"), tr("Too many wrong PINs: try again in %1 s.").arg(wait)); done(false); return; }
        askPin(guard, QString(), done);
    });
}

void Manager::askPin(QWidget *parent, const QString &note, std::function<void(bool)> done) {
    const QString pin = readPin(parent, tr("Unlock Centurion"), (note.isEmpty() ? QString() : note + QStringLiteral("\n\n")) + tr("PIN (4 digits):"));
    if (pin.isEmpty()) { done(false); return; }
    if (!pinShape(pin)) { askPin(parent, tr("The PIN is 4 digits."), done); return; }
    QPointer<QWidget> guard(parent);
    privileged::run(privileged::helperPath(QStringLiteral("tune-profile-helper")),
                    QJsonObject{{QStringLiteral("op"), QStringLiteral("unlock")}, {QStringLiteral("pin"), pin}}, this,
                    [this, guard, done](const privileged::Result &r) {
        if (r.ok()) { setUnlocked(true); done(true); return; }
        if (r.reached && r.json.value(QStringLiteral("wrong_pin")).toBool() && !r.json.value(QStringLiteral("disabled")).toBool()
            && r.json.value(QStringLiteral("wait_seconds")).toInt() == 0) {
            askPin(guard, r.message(), done);
            return;
        }
        QMessageBox::warning(guard, tr("Unlock"), r.message());
        done(false);
    });
}

void Manager::lock(bool keepScenes) {
    setUnlocked(false);
    privileged::run(privileged::helperPath(QStringLiteral("tune-profile-helper")),
                    QJsonObject{{QStringLiteral("op"), QStringLiteral("lock")}, {QStringLiteral("scenes"), keepScenes}}, this,
                    [](const privileged::Result &) {});
}

void Manager::changePin(QWidget *parent, std::function<void(bool)> done) {
    if (!done) done = [](bool) {};
    const QString a = readPin(parent, tr("Set unlock PIN"), tr("New PIN (4 digits):"));
    if (a.isEmpty()) { done(false); return; }
    if (!pinShape(a)) { QMessageBox::warning(parent, tr("Set unlock PIN"), tr("The PIN is 4 digits.")); done(false); return; }
    const QString b = readPin(parent, tr("Set unlock PIN"), tr("The same PIN again:"));
    if (b != a) { if (!b.isEmpty()) QMessageBox::warning(parent, tr("Set unlock PIN"), tr("The two PINs differ.")); done(false); return; }
    QPointer<QWidget> guard(parent);
    privileged::run(privileged::helperPath(QStringLiteral("tune-admin-helper")),
                    QJsonObject{{QStringLiteral("op"), QStringLiteral("pin_set")}, {QStringLiteral("pin"), a}}, this,
                    [this, guard, a, done](const privileged::Result &r) {
        if (!r.ok()) { QMessageBox::warning(guard, tr("Set unlock PIN"), r.message()); done(false); return; }
        setUnlocked(false);  // pin_set drops every grant
        privileged::run(privileged::helperPath(QStringLiteral("tune-profile-helper")),
                        QJsonObject{{QStringLiteral("op"), QStringLiteral("unlock")}, {QStringLiteral("pin"), a}}, this,
                        [this, guard, done](const privileged::Result &u) {
            if (u.ok()) { setUnlocked(true); done(true); return; }
            QMessageBox::warning(guard, tr("Unlock"), u.message());
            done(false);
        });
    });
}

}  // namespace unlock
