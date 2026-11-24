#pragma once
#include "tunereceipt.h"
#include <set>
#include <string>
#include <vector>
namespace bootreceiptcoverage {
struct Row { std::string key; tunereceipt::Status status; };
struct Coverage { bool consistent=true; std::size_t matched=0, requested=0; std::vector<std::string> missing; };
inline Coverage assess(const std::vector<std::string> &requested, const std::vector<Row> &rows) {
    Coverage c; std::set<std::string> expected, covered;
    if(requested.size()>1024 || rows.size()>1024) { c.consistent=false; return c; }
    for(const auto &key:requested) if(key.empty() || key.size()>256 || !expected.insert(key).second) c.consistent=false;
    c.requested=expected.size();
    for(const auto &row:rows) {
        if(!expected.contains(row.key) || !covered.insert(row.key).second) { c.consistent=false; continue; }
        if(row.status==tunereceipt::Status::Match) ++c.matched;
    }
    for(const auto &key:expected) if(!covered.contains(key)) c.missing.push_back(key);
    if(!c.consistent) c.matched=0;
    return c;
}
}
