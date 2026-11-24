#pragma once
// Classify the observed bind address, not external reachability. No network I/O.
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace socketbind {
enum class Scope { Unknown, Loopback, Wildcard, LinkLocal, Private, Shared, Multicast, Specific };
struct Bind { int family = 0; Scope scope = Scope::Unknown; bool mapped = false; };
inline Scope ipv4(const std::uint8_t *b) {
    if (b[0] == 127) return Scope::Loopback;
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) return Scope::Wildcard;
    if (b[0] == 169 && b[1] == 254) return Scope::LinkLocal;
    if (b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168)) return Scope::Private;
    if (b[0] == 100 && b[1] >= 64 && b[1] <= 127) return Scope::Shared;
    if (b[0] >= 224 && b[0] <= 239) return Scope::Multicast;
    return Scope::Specific;
}
inline Bind classify(std::string_view text) {
    if (text.empty() || text.size() > 128 || text.find('\0') != text.npos) return {};
    const auto zone = text.find('%');
    if (zone != text.npos) {
        const auto suffix = text.substr(zone + 1);
        if (suffix.empty() || suffix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-:") != suffix.npos) return {};
        text = text.substr(0, zone);
    }
    const std::string address(text);
    std::array<std::uint8_t, 16> b{};
    if (zone == std::string_view::npos && inet_pton(AF_INET, address.c_str(), b.data()) == 1) return {4, ipv4(b.data()), false};
    if (inet_pton(AF_INET6, address.c_str(), b.data()) != 1) return {};
    const bool zero10 = std::all_of(b.begin(), b.begin()+10, [](auto n){return n == 0;});
    if (zero10 && b[10] == 255 && b[11] == 255) {
        // IPv4-mapped 0.0.0.0 is not the IPv6 wildcard.
        const auto s = ipv4(b.data()+12);
        return {6, s == Scope::Wildcard ? Scope::Specific : s, true};
    }
    const bool zero15 = zero10 && std::all_of(b.begin()+10, b.begin()+15, [](auto n){return n == 0;});
    if (zero15 && b[15] == 0) return {6, Scope::Wildcard, false};
    if (zero15 && b[15] == 1) return {6, Scope::Loopback, false};
    if (b[0] == 0xff) return {6, Scope::Multicast, false};
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return {6, Scope::LinkLocal, false};
    if ((b[0] & 0xfe) == 0xfc) return {6, Scope::Private, false};
    return {6, Scope::Specific, false};
}
inline bool potentiallyNonlocal(Bind b) { return b.scope != Scope::Unknown && b.scope != Scope::Loopback; }
} // namespace socketbind
