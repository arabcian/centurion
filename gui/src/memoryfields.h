#pragma once
// Strict, bounded parsing for kernel memory accounting; never invent zero for
// unreadable fields or mix swap capacity, zram logical capacity and RAM usage.
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>
#include <limits>
#include <algorithm>
namespace memoryfields {
inline std::string_view trim(std::string_view s) {
    const auto first=s.find_first_not_of(" \t\r\n");
    return first==s.npos?std::string_view{}:s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
inline std::string_view token(std::string_view &s) {
    s=trim(s); const auto end=s.find_first_of(" \t\r\n");
    const auto t=s.substr(0,end); s=end==s.npos?std::string_view{}:s.substr(end); return t;
}
inline std::optional<std::uint64_t> unsignedInt(std::string_view s) {
    if(s.empty())return {};
    std::uint64_t n=0; const auto result=std::from_chars(s.data(),s.data()+s.size(),n);
    if(result.ec!=std::errc{}||result.ptr!=s.data()+s.size())return {};
    return n;
}
inline std::optional<std::uint64_t> kib(std::string_view s) {
    const auto n=unsignedInt(token(s));
    if(!n||token(s)!="kB"||!trim(s).empty()||*n>std::numeric_limits<std::uint64_t>::max()/1024)return {};
    return *n*1024;
}
struct Memory {
    std::optional<std::uint64_t> total, available, swapTotal, swapFree;
};
inline Memory meminfo(std::string_view s) {
    Memory result;
    if(s.empty()||s.size()>65536)return result;
    const std::array<std::string_view,4> keys{"MemTotal","MemAvailable","SwapTotal","SwapFree"};
    const std::array<std::optional<std::uint64_t>*,4> fields{&result.total,&result.available,&result.swapTotal,&result.swapFree};
    std::array<bool,4> seen{};
    while(!s.empty()) {
        const auto end=s.find('\n'); const auto line=s.substr(0,end);
        s=end==s.npos?std::string_view{}:s.substr(end+1);
        const auto colon=line.find(':'); if(colon==line.npos)continue;
        const auto key=trim(line.substr(0,colon));
        for(std::size_t i=0;i<keys.size();++i) if(key==keys[i]) {
            if(seen[i])fields[i]->reset(); else *fields[i]=kib(line.substr(colon+1));
            seen[i]=true; break;
        }
    }
    return result;
}
struct Zram { std::uint64_t stored, compressed, resident; };
inline std::optional<Zram> zramStats(std::string_view s) {
    if(s.size()>4096)return {};
    const auto orig=unsignedInt(token(s)), compressed=unsignedInt(token(s)), resident=unsignedInt(token(s));
    if(!orig||!compressed||!resident)return {};
    return Zram{*orig,*compressed,*resident};
}
inline bool zramDevice(std::string_view name) {
    if(!name.starts_with("/dev/zram"))return false;
    name.remove_prefix(9);
    return !name.empty()&&name.find_first_not_of("0123456789")==name.npos;
}
struct Swap { std::string_view path,type; std::uint64_t sizeKiB,usedKiB; };
// Result references the input buffer; consume before destroying it. No alias
// resolution or device open: a canonical /dev/zramN name is positive evidence only.
inline std::optional<std::vector<Swap>> swaps(std::string_view s) {
    if(s.empty()||s.size()>65536)return {};
    const auto nl=s.find('\n'); auto header=s.substr(0,nl);
    for(auto key:{"Filename","Type","Size","Used","Priority"})if(token(header)!=key)return {};
    if(!trim(header).empty())return {};
    s=nl==s.npos?std::string_view{}:s.substr(nl+1);
    std::vector<Swap> result;
    while(!trim(s).empty()) {
        const auto end=s.find('\n'); auto line=s.substr(0,end);
        s=end==s.npos?std::string_view{}:s.substr(end+1);
        if(trim(line).empty())continue;
        const auto path=token(line),type=token(line); const auto size=unsignedInt(token(line)),used=unsignedInt(token(line));
        const auto priority=token(line); int p=0;
        if(priority.empty())return {};
        const auto pr=std::from_chars(priority.data(),priority.data()+priority.size(),p);
        if(path.empty()||(type!="partition"&&type!="file")||!size||!used||*used>*size||pr.ec!=std::errc{}||pr.ptr!=priority.data()+priority.size()||!trim(line).empty()||result.size()>=4096)return {};
        if(std::any_of(result.begin(),result.end(),[&](const auto &row){return row.path==path;}))return {};
        result.push_back({path,type,*size,*used});
    }
    return result;
}
}
