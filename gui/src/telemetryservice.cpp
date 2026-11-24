#include "telemetryservice.h"
#include <QCoreApplication>
#include <QThreadPool>
#include <QTimer>
#include <QThread>
#include <algorithm>
#include <utility>

TelemetryService::TelemetryService(QObject *parent, Reader reader) : QObject(parent), reader_(std::move(reader)) {}
TelemetryService &TelemetryService::instance() {
    // Called exclusively from the GUI thread, after QApplication construction.
    static auto *service = new TelemetryService(QCoreApplication::instance());
    return *service;
}
void TelemetryService::request(const QList<Metric> &metrics, QObject *owner, Callback callback, int timeoutMs) {
    Q_ASSERT(thread() == QThread::currentThread());
    if (!owner || !callback) return;
    QSet<int> wanted;
    for (auto metric : metrics) { const int i = int(metric); if (i >= 0 && i < 6) wanted.insert(i); }
    if (waiters_.size() >= 64 || wanted.isEmpty()) {
        Results results;
        for (int i : wanted) { sysinfo::PresentationReading r; r.quality = telemetry::Quality::Pending; results.insert(i,r); }
        QTimer::singleShot(0, owner, [callback = std::move(callback), results = std::move(results)]() mutable { callback(std::move(results)); });
        return;
    }
    const auto id = ++next_;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true); timer->setTimerType(Qt::PreciseTimer);
    const int limit = std::clamp(timeoutMs,1,30000);
    waiters_.insert(id, Waiter{owner,wanted,{},std::move(callback),{},timer,sysinfo::presentationClock()+telemetry::Tick(limit)});
    // Context lifetime is pruned immediately; the shared flight remains active.
    waiters_[id].ownerConnection = connect(owner,&QObject::destroyed,this,[this,id]{
        auto it = waiters_.find(id); if (it == waiters_.end()) return;
        it->timer->stop(); it->timer->deleteLater(); waiters_.erase(it);
    });
    connect(timer,&QTimer::timeout,this,[this,id]{ finish(id,true); }); timer->start(limit);
    for (int i : wanted) launch(i);
}
void TelemetryService::finish(quint64 id, bool timedOut) {
    auto it = waiters_.find(id);
    if (it == waiters_.end()) return;
    Waiter w = std::move(it.value()); waiters_.erase(it);
    disconnect(w.ownerConnection); w.timer->stop(); w.timer->deleteLater();
    if (!w.owner) return;
    if (!timedOut && sysinfo::presentationClock() >= w.deadline) {
        // A queued completion arriving after the deadline cannot count as success.
        for (auto i=w.results.begin(); i!=w.results.end(); ++i) {
            *i = sysinfo::PresentationReading{}; i->quality = telemetry::Quality::TimedOut;
        }
        timedOut = true;
    }
    for (int i : w.pending) { sysinfo::PresentationReading r; r.quality = timedOut ? telemetry::Quality::TimedOut : telemetry::Quality::Unavailable; w.results.insert(i,r); }
    // Check freshness again at actual delivery, including time in the GUI queue.
    for (auto i = w.results.begin(); i != w.results.end(); ++i) {
        if (i->quality == telemetry::Quality::Fresh && !i->fresh()) {
            i->quality = telemetry::Quality::Expired; i->text.reset(); i->values.clear(); i->metrics.clear();
        }
    }
    w.callback(std::move(w.results));
}
void TelemetryService::launch(int index) {
    const auto token = flights_.begin(std::size_t(index));
    if (!token) return;
    auto reader = reader_;
    QPointer<TelemetryService> self(this);
    QThreadPool::globalInstance()->start([self,index,token=*token,reader=std::move(reader)] {
        sysinfo::PresentationReading reading;
        try { reading = reader(static_cast<Metric>(index)); } catch (...) { reading.quality = telemetry::Quality::Failed; }
        QMetaObject::invokeMethod(QCoreApplication::instance(),[self,index,token,reading=std::move(reading)] {
            if (!self || !self->flights_.finish(std::size_t(index),token)) return;
            QList<quint64> ready;
            for (auto it=self->waiters_.begin(); it!=self->waiters_.end(); ++it) {
                if (!it->pending.remove(index)) continue;
                it->results.insert(index,reading);
                if (it->pending.isEmpty()) ready.append(it.key());
            }
            // A callback may issue a new request or destroy its owner.
            for (auto id : ready) { if (!self) break; self->finish(id,false); }
        },Qt::QueuedConnection);
    });
}
