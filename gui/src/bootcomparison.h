#pragma once
// Frozen, saved observations only. A missing category is not a measured zero.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
namespace bootcomparison {
struct Event { std::string message, category; bool operator==(const Event &) const = default; };
struct Boot {
    std::map<uint64_t, Event> records;
    uint64_t duplicates=0, conflicts=0;
    bool capped=false;
    // Never count an ambiguous sequence twice or retain a conflicting first version.
    void add(uint64_t seq, Event event) {
        const auto found=records.find(seq);
        if (found!=records.end()) {
            if (found->second.category.empty() && found->second.message.empty()) { ++duplicates; return; }
            if (found->second==event) ++duplicates;
            else { ++conflicts; found->second={}; }
        } else if (records.size()>=20000) capped=true;
        else records.emplace(seq,std::move(event));
    }
    std::map<std::string,uint64_t> counts() const {
        std::map<std::string,uint64_t> result;
        for (const auto &[seq,event]:records) if (!event.category.empty()) ++result[event.category];
        return result;
    }
};
inline std::optional<uint64_t> observed(const std::map<std::string,uint64_t>& counts, const std::string& key) {
    const auto it=counts.find(key);
    if (it==counts.end()) return {};
    return it->second;
}
}
