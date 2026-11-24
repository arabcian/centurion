#pragma once
// Comparison of frozen tuning values. Never authorizes applying or restoring them.
#include <charconv>
#include <cstdint>
#include <string_view>
namespace tuningsemantic {
enum class Kind { Integer, Boolean, Choice, Unknown };
enum class Relation { Equal, Different, Unknown, LeftMissing, RightMissing, BothMissing, Invalid };
struct Value { bool present; bool readable; std::string_view text; };
inline bool integer(std::string_view s, std::int64_t &v) {
    if (s.empty()) return false;
    if (s.front()=='+') { s.remove_prefix(1); if(s.empty() || s.front()=='-') return false; }
    const auto r=std::from_chars(s.data(),s.data()+s.size(),v);
    return r.ec==std::errc{} && r.ptr==s.data()+s.size();
}
inline Relation compare(Kind k, Value a, Value b) {
    if (!a.present && !b.present) return Relation::BothMissing;
    if (!a.present) return Relation::LeftMissing;
    if (!b.present) return Relation::RightMissing;
    if (!a.readable || !b.readable || k==Kind::Unknown) return Relation::Unknown;
    if (a.text.empty() || b.text.empty()) return Relation::Invalid;
    if (k==Kind::Integer) {
        std::int64_t x=0,y=0;
        if(!integer(a.text,x)||!integer(b.text,y)) return Relation::Invalid;
        return x==y?Relation::Equal:Relation::Different;
    }
    if(k==Kind::Boolean && ((a.text!="0" && a.text!="1") || (b.text!="0" && b.text!="1"))) return Relation::Invalid;
    return a.text==b.text?Relation::Equal:Relation::Different;
}
}
