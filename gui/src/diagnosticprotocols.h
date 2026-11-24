#pragma once
// Presentation-only protocol validation. No filesystem or command execution.
#include "hardwarefields.h"
#include "memoryfields.h"
#include <map>
#include <set>
#include <string>
#include <cmath>
#include <utility>
namespace diagnosticprotocols {
struct Pdo {
    std::optional<double> low, high, current, watts;
};
inline Pdo pdo(std::string_view type, std::string_view voltage, std::string_view minimum,
               std::string_view maximum, std::string_view current, std::string_view power,
               std::string_view limited) {
    Pdo result;
    if (type == "fixed_supply") result.low = result.high = hardwarefields::milli(voltage,"mV");
    else if (type == "variable_supply" || type == "battery" || type == "programmable_supply") {
        result.low=hardwarefields::milli(minimum,"mV"); result.high=hardwarefields::milli(maximum,"mV");
    } else return result;
    if (!result.low || !result.high || *result.low > *result.high) return {};
    if (type == "battery") result.watts=hardwarefields::milli(power,"mW");
    else {
        result.current=hardwarefields::milli(current,"mA");
        // A PPS power-limited source need not supply its maximum current at maximum voltage.
        // Missing flag is also insufficient to claim a maximum.
        if (result.current && (type != "programmable_supply" || limited == "0"))
            result.watts=*result.high * *result.current;
    }
    return result;
}
struct Cache {
    unsigned cpu; std::string level,type,size; std::vector<unsigned> members;
};
inline bool sameCache(const Cache &a,const Cache &b) {
    return a.level==b.level && a.type==b.type && a.size==b.size && a.members==b.members &&
        std::binary_search(a.members.begin(),a.members.end(),a.cpu) &&
        std::binary_search(a.members.begin(),a.members.end(),b.cpu);
}
// Each member must report exactly one matching cache; competing declarations
// for the same level/type and member make the relationship unverified.
inline bool verifiedCache(const Cache &a,const std::map<unsigned,std::vector<Cache>> &all) {
    if(a.members.empty()||a.members.size()>4096||a.size.empty()||a.level.empty()||a.type.empty()||
       !std::is_sorted(a.members.begin(),a.members.end())||std::adjacent_find(a.members.begin(),a.members.end())!=a.members.end()||!sameCache(a,a))return false;
    for(auto id:a.members) {
        const auto it=all.find(id);if(it==all.end())return false;
        unsigned matches=0;
        for(const auto &b:it->second)if(b.level==a.level&&b.type==a.type){
            if(!sameCache(a,b)||b.cpu!=id)return false;
            ++matches;
        }
        if(matches!=1)return false;
    }
    return true;
}
struct MemoryRow { std::string name; std::vector<std::uint64_t> bytes; };
inline std::optional<std::vector<MemoryRow>> freeBytes(const std::vector<std::string>& lines) {
    if(lines.size()!=3)return {};
    std::string_view header=lines[0];
    for(auto key:{"total","used","free","shared","buff/cache","available"})if(memoryfields::token(header)!=key)return {};
    if(!memoryfields::trim(header).empty())return {};
    std::vector<MemoryRow> rows;
    for(unsigned j=1;j<=2;++j){
        std::string_view line=lines[j]; const auto name=memoryfields::token(line);
        if(name!=(j==1?"Mem:":"Swap:"))return {};
        MemoryRow row{std::string(name.substr(0,name.size()-1)),{}};
        for(unsigned i=0;i<(j==1?6u:3u);++i){auto n=memoryfields::unsignedInt(memoryfields::token(line));if(!n)return {};row.bytes.push_back(*n);}
        if(!memoryfields::trim(line).empty()||row.bytes[1]>row.bytes[0]||row.bytes[2]>row.bytes[0]||
            (j==1&&row.bytes[5]>row.bytes[0]))return {};
        rows.push_back(std::move(row));
    }
    return rows;
}
inline bool parameterName(std::string_view s) {
    return !s.empty()&&s.size()<=128&&s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")==s.npos;
}
struct Parameter { std::string name,description; };
inline std::optional<std::vector<Parameter>> parameters(const std::vector<std::string>& lines) {
    if(lines.empty()||lines.size()>4096)return {};
    std::set<std::string> names;std::vector<Parameter> result;
    for(const auto& line:lines){
        const auto colon=line.find(':');if(colon==line.npos||line.size()>4096)return {};
        const auto name=line.substr(0,colon),desc=line.substr(colon+1);
        if(!parameterName(name)||desc.find_first_not_of(" \t")==desc.npos||!names.insert(name).second)return {};
        for(unsigned char c:desc)if(c<32&&c!='\t')return {};
        result.push_back({name,desc});
    }
    return result;
}
} // namespace diagnosticprotocols
