#pragma once

#include <QDateTime>
#include <optional>
#include <time.h>

namespace kernelwalltime {

// /dev/kmsg timestamps use the monotonic clock, which stops during suspend.
// This is an estimate at capture time. It cannot reconstruct earlier clock jumps or sleeps.
struct Anchor {
    qint64 monotonicUs;
    qint64 wallMs;

    static std::optional<Anchor> capture() {
        timespec now{};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return std::nullopt;
        return Anchor{qint64(now.tv_sec) * 1000000 + now.tv_nsec / 1000,
                      QDateTime::currentMSecsSinceEpoch()};
    }

    qint64 toWallMs(qint64 eventUs) const {
        if (eventUs < 0 || eventUs > monotonicUs) return 0;
        const qint64 result = wallMs - (monotonicUs - eventUs) / 1000;
        return result > 0 && result < 253402300800000LL ? result : 0;
    }
};

} // namespace kernelwalltime
