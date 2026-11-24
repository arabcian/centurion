#pragma once
#include "sysinfo.h"
#include "telemetrybroker.h"
#include <QObject>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QList>
#include <QTimer>
#include <functional>

// Presentation only. Never use this service for safety gates or write readback.
// One physical acquisition per metric, shared by all requesting views.
class TelemetryService : public QObject {
public:
    using Metric = sysinfo::PresentationMetric;
    using Results = QHash<int, sysinfo::PresentationReading>;
    using Callback = std::function<void(Results)>;
    using Reader = std::function<sysinfo::PresentationReading(Metric)>;
    static TelemetryService &instance();
    explicit TelemetryService(QObject *parent, Reader reader = sysinfo::presentationReading);
    // Owner destruction cancels delivery, not physical I/O. Callback is queued.
    void request(const QList<Metric> &metrics, QObject *owner, Callback callback, int timeoutMs = 5000);
private:
    struct Waiter { QPointer<QObject> owner; QSet<int> pending; Results results; Callback callback; QMetaObject::Connection ownerConnection; QTimer *timer = nullptr; telemetry::Tick deadline{}; };
    void finish(quint64 id, bool timedOut);
    void launch(int index);
    Reader reader_;
    telemetry::Flights<6> flights_;
    QHash<quint64, Waiter> waiters_;
    quint64 next_ = 0;
};
