#pragma once
// Bounded parsing of read-only kernel/firmware metadata. No strings/serials from DMI records.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
namespace hardwarefields {
inline std::optional<unsigned> number(std::string_view s) {
    if(s.empty())return std::nullopt;
    unsigned value=0;
    const auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),value);
    if(ec!=std::errc{}||end!=s.data()+s.size())return std::nullopt;
    return value;
}
inline std::optional<std::vector<unsigned>> cpuList(std::string_view s) {
    if(s.empty()||s.size()>65536)return std::nullopt;
    std::vector<unsigned> cpus; constexpr unsigned cap=65535;
    while(!s.empty()) {
        const auto sep=s.find(','); const auto part=s.substr(0,sep); const auto dash=part.find('-');
        auto lo=number(part.substr(0,dash)); auto hi=dash==part.npos?lo:number(part.substr(dash+1));
        if(!lo||!hi||*lo>*hi||*hi>cap||cpus.size()+(*hi-*lo+1)>cap+1)return std::nullopt;
        for(unsigned i=*lo;i<=*hi;i++)cpus.push_back(i);
        if(sep==s.npos)break;
        s.remove_prefix(sep+1); if(s.empty())return std::nullopt;
    }
    std::sort(cpus.begin(),cpus.end()); if(std::adjacent_find(cpus.begin(),cpus.end())!=cpus.end())return std::nullopt;
    return cpus;
}
inline std::uint32_t le(std::span<const std::uint8_t> b,std::size_t off,std::size_t count){
    std::uint32_t v=0;for(std::size_t i=0;i<count;i++)v|=std::uint32_t(b[off+i])<<(8*i);return v;
}
struct Dimm { bool empty=false; std::optional<std::uint64_t> bytes; std::optional<unsigned> speed, configured; };
inline std::optional<Dimm> memoryDevice(std::span<const std::uint8_t> b) {
    if(b.size()<0x0e||b[0]!=17||b[1]<0x0e)return std::nullopt;
    b=b.first(std::min<std::size_t>(b[1],b.size()));
    Dimm result; const auto size=le(b,0x0c,2);
    if(size==0){result.empty=true;return result;}
    if(size==0x7fff){if(b.size()>=0x20){auto ext=le(b,0x1c,4);if(ext&&!(ext&0x80000000u))result.bytes=std::uint64_t(ext)*1024*1024;}}
    else if(size!=0xffff){const auto amount=size&0x7fff;if(amount)result.bytes=std::uint64_t(amount)*1024*((size&0x8000)?1:1024);}
    auto speed=[&](std::size_t off)->std::optional<unsigned>{if(b.size()<off+2)return {};auto v=le(b,off,2);return v&&v!=0xffff?std::optional<unsigned>(v):std::nullopt;};
    result.speed=speed(0x15);result.configured=speed(0x20);return result;
}
struct AcpiHeader { std::uint32_t length; unsigned revision; };
inline std::optional<AcpiHeader> acpiHeader(std::span<const std::uint8_t> b){
    if(b.size()<36)return std::nullopt;
    for(int i=0;i<4;i++)if(!((b[i]>='A'&&b[i]<='Z')||(b[i]>='0'&&b[i]<='9')||b[i]=='_'))return std::nullopt;
    auto len=le(b,4,4);if(len<36||len>64u*1024*1024)return std::nullopt;
    return AcpiHeader{len,b[8]};
}
inline std::optional<double> milli(std::string_view s,std::string_view unit){
    if(!s.ends_with(unit))return std::nullopt;
    auto n=number(s.substr(0,s.size()-unit.size()));if(!n||!*n)return std::nullopt;
    return *n/1000.0;
}
} // namespace hardwarefields
