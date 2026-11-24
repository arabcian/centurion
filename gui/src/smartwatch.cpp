#include "smartwatch.h"
#include "privileged.h"
#include <QDate>
#include <QCoreApplication>
#include <QPointer>
#include <utility>
#include <QTimer>
#include <grp.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

namespace {
constexpr int CHECK_EVERY_MS = 60 * 60 * 1000;
constexpr int FIRST_CHECK_MS = 60 * 1000;
QString todayIso() { return QDate::currentDate().toString(Qt::ISODate); }
// Only a wheel/sudo admin gets the helper without a password, so only they get the unattended sample.
bool adminAtMachine() {
    const int count = getgroups(0, nullptr);
    std::vector<gid_t> gids(static_cast<size_t>(std::max(count, 0)));
    // A size-zero getgroups call returns a count rather than filling storage.
    // Never turn that count or a failed read into zero-initialized group IDs.
    const int filled = count > 0 ? getgroups(count, gids.data()) : 0;
    gids.resize(filled >= 0 && filled <= count ? static_cast<size_t>(filled) : 0);
    gids.push_back(getgid());
    for (const char *name : {"wheel", "sudo"}) {
        const group *g = getgrnam(name);
        if (g && std::find(gids.begin(), gids.end(), g->gr_gid) != gids.end()) return true;
    }
    return false;
}

} // namespace

// The page and unattended watch share one acquisition, including its real
// terminal lifetime. Destroyed pages cannot receive a callback. No hardware
// write or retry is initiated by this broker.
void smart::requestSample(QObject *owner, SampleCallback callback, std::function<void()> onTimeout) {
    struct Waiter {
        QPointer<QObject> owner;
        SampleCallback callback;
        std::function<void()> onTimeout;
    };
    struct Flight { bool active = false, timedOut = false; std::vector<Waiter> waiters; };
    static Flight flight;
    if (!owner || !callback) return;
    std::erase_if(flight.waiters, [](const Waiter &w) { return w.owner.isNull(); });
    flight.waiters.push_back({owner, std::move(callback), std::move(onTimeout)});
    if (flight.active) {
        if (flight.timedOut && flight.waiters.back().onTimeout) {
            // Copy before invoking: a callback may re-enter this function.
            const auto notify = flight.waiters.back().onTimeout;
            notify();
        }
        return;
    }
    flight.active = true;
    flight.timedOut = false;
    privileged::runUntilExit(privileged::helperPath(QStringLiteral("tune-helper")),
        QJsonObject{{"op", "tool"}, {"tool", "smart_json"}}, QCoreApplication::instance(),
        [](const privileged::Result &r) {
            auto waiters = std::exchange(flight.waiters, {});
            flight.active = false;
            flight.timedOut = false;
            RecordResult result;
            const bool hasOwner = std::any_of(waiters.begin(), waiters.end(), [](const Waiter &w) { return !w.owner.isNull(); });
            if (hasOwner && r.ok() && r.json.value("disks").isArray() && r.json.value("missing").toString().isEmpty())
                result = record(r.json.value("disks").toArray());
            for (const auto &w : waiters) if (w.owner) w.callback(r, result);
        }, 120000, [] {
            flight.timedOut = true;
            // Copy callbacks to allow owner destruction/reentry during notification.
            const auto waiters = flight.waiters;
            for (const auto &w : waiters) if (w.owner && w.onTimeout) w.onTimeout();
        });
}

SmartWatch::SmartWatch(QObject *parent) : QObject(parent), timer_(new QTimer(this)) {
    timer_->setInterval(CHECK_EVERY_MS);
    connect(timer_, &QTimer::timeout, this, &SmartWatch::check);
    timer_->start();
    QTimer::singleShot(FIRST_CHECK_MS, this, &SmartWatch::check);
}

void SmartWatch::check() {
    if (busy_ || !smart::automaticSamplingEnabled() || !adminAtMachine()) return;
    if (smart::loadHistory().value(QStringLiteral("sampled")).toString() == todayIso()) return;
    busy_ = true;
    smart::requestSample(this, [this](const privileged::Result &r, const smart::RecordResult &result) {
        busy_ = false;
        if (r.ok() && !result.alerts.isEmpty()) Q_EMIT alert(result.alerts);
    });
}
