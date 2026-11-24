#include "../src/telemetryservice.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while(false)
// The test supplies these symbols instead of linking the real sysfs backend.
namespace sysinfo {
telemetry::Tick presentationClock() { return std::chrono::duration_cast<telemetry::Tick>(std::chrono::steady_clock::now().time_since_epoch()); }
bool PresentationReading::fresh() const { return telemetry::freshAt(quality,captured,ttl,presentationClock()); }
PresentationReading presentationReading(PresentationMetric) { std::abort(); }
}
static bool until(const std::function<bool()> &ready,int timeout=3000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return ready();
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    using M=sysinfo::PresentationMetric;
    std::atomic<int> calls{0}; QSemaphore gate;
    TelemetryService svc(nullptr,[&](M){
        ++calls; sysinfo::PresentationReading r;
        if (!gate.tryAcquire(1,3000)) { r.quality=telemetry::Quality::Failed; return r; }
        r.quality=telemetry::Quality::Fresh; r.captured=sysinfo::presentationClock(); r.ttl=telemetry::Tick(1000);
        r.values.insert(QStringLiteral("cpu_temp_c"),0.); return r;
    });
    QObject a,b; int finished=0; bool good=true;
    auto cb=[&](TelemetryService::Results r){ ++finished; auto reading=r.value(int(M::CpuTemp)); good &= reading.fresh() && reading.values.value(QStringLiteral("cpu_temp_c"),-1.)==0.; };
    svc.request({M::CpuTemp},&a,cb); svc.request({M::CpuTemp},&b,cb);
    CHECK(until([&]{return calls==1;})); gate.release(); CHECK(until([&]{return finished==2;})); CHECK(good && calls==1);
    bool timed=false; svc.request({M::CpuTemp},&a,[&](auto r){timed=r.value(int(M::CpuTemp)).quality==telemetry::Quality::TimedOut;},30);
    CHECK(until([&]{return calls==2;})); CHECK(until([&]{return timed;}));
    int cancelled=0; auto *gone=new QObject;
    svc.request({M::CpuTemp},gone,[&](auto){++cancelled;}); delete gone;
    bool joined=false; svc.request({M::CpuTemp},&b,[&](auto r){joined=r.value(int(M::CpuTemp)).fresh();});
    CHECK(calls==2); gate.release(); CHECK(until([&]{return joined;})); CHECK(cancelled==0 && calls==2);
    QThreadPool::globalInstance()->waitForDone(); return 0;
}
