#pragma once
// GUI-thread-only fanout bookkeeping. A timed-out waiter does not free the
// physical acquisition slot; only its matching completion can release it.
#include <array>
#include <cstdint>
#include <optional>
#include <cstddef>
namespace telemetry {
template<std::size_t N> class Flights {
public:
    using Token = std::uint64_t;
    std::optional<Token> begin(std::size_t index) {
        if (index >= N || active_[index]) return {};
        active_[index] = ++next_;
        return active_[index];
    }
    bool finish(std::size_t index, Token token) {
        if (index >= N || !active_[index] || *active_[index] != token) return false;
        active_[index].reset(); return true;
    }
private:
    std::array<std::optional<Token>, N> active_{};
    Token next_ = 0;
};
}
