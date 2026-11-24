#include "../src/telemetrycache.h"
#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
using namespace telemetry;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"Failed line %d: %s\n",__LINE__,#x); return 1; } } while(false)
int main() {
    std::atomic<long long> clock{100};
    auto now = [&] { return Tick(clock.load()); };
    Slot<double> slot;
    int calls=0;
    auto read = [&]() -> std::optional<double> { ++calls; return 0.0; };
    CHECK(slot.read(Tick(10),now,read).quality==Quality::Fresh);
    CHECK(slot.read(Tick(10),now,read).value==0.0 && calls==1);
    clock=110;
    CHECK(slot.read(Tick(10),now,read).value==0.0 && calls==2);
    slot.invalidate();
    CHECK(slot.read(Tick(10),now,[]()->std::optional<double>{return {};}).quality==Quality::Unavailable);
    CHECK(!slot.read(Tick(10),now,read).value && calls==2);
    slot.invalidate();
    CHECK(slot.read(Tick(10),now,[]()->std::optional<double>{throw std::runtime_error("failure");}).quality==Quality::Failed);
    slot.invalidate();
    std::promise<void> started, release;
    auto ready=release.get_future();
    Sample<double> result;
    std::thread worker([&]{ result=slot.read(Tick(10),now,[&]()->std::optional<double>{started.set_value();ready.wait();return 42.;}); });
    started.get_future().wait();
    const auto pending=slot.read(Tick(10),now,read);
    slot.invalidate();
    const auto afterInvalidate=slot.read(Tick(10),now,read);
    release.set_value(); worker.join();
    CHECK(pending.quality==Quality::Pending && !pending.value);
    CHECK(afterInvalidate.quality==Quality::Pending && !afterInvalidate.value);
    CHECK(result.quality==Quality::Expired && !result.value && calls==2);
    CHECK(slot.read(Tick(10),now,read).value==0.0 && calls==3);
    slot.invalidate();
    CHECK(slot.read(Tick(10),now,[&]()->std::optional<double>{clock+=15;return 90.;}).quality==Quality::Expired);
    CHECK(slot.read(Tick(10),now,read).value==0.0 && calls==4);
    return 0;
}
