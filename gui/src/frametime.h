#pragma once
// Read-only analysis of explicitly selected logs. No tuning or calibration writes.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string_view>
#include <vector>

namespace frametime {
struct Sample { std::size_t index; double ms; };
struct Log { std::vector<double> ms; std::size_t rejected = 0; bool supported = true; };
inline std::string_view trim(std::string_view s) {
    constexpr auto ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == s.npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}
inline std::string_view field(std::string_view s, std::size_t index) {
    while (index--) { auto p = s.find(','); if (p == s.npos) return {}; s.remove_prefix(p+1); }
    return trim(s.substr(0,s.find(',')));
}
inline bool equals(std::string_view a, std::string_view b) {
    if (a.size()!=b.size()) return false;
    for (std::size_t i=0;i<a.size();++i) { char c=a[i]; if(c>='A'&&c<='Z') c=char(c-'A'+'a'); if(c!=b[i]) return false; }
    return true;
}
inline Log parse(std::string_view text) {
    Log out;
    if(text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        auto end=text.find('\n'); auto line=trim(text.substr(0,end));
        if(!line.empty()&&!line.starts_with('#')) lines.push_back(line);
        if(end==text.npos) break;
        text.remove_prefix(end+1);
    }
    std::size_t col=0, start=0; double scale=1; bool header=false;
    for(std::size_t i=0;i<std::min<std::size_t>(4,lines.size())&&!header;++i) {
        auto line=lines[i]; std::size_t j=0;
        while(true) {
            auto cell=trim(line.substr(0,line.find(',')));
            if(equals(cell,"frametime")||equals(cell,"interval_ns")) { col=j; scale=equals(cell,"interval_ns")?1e-6:1; start=i+1; header=true; break; }
            auto p=line.find(','); if(p==line.npos) break; line.remove_prefix(p+1); ++j;
        }
    }
    if(!header && std::any_of(lines.begin(),lines.end(),[](auto s){return s.find(',')!=s.npos;})) {out.supported=false;return out;}
    for(std::size_t i=start;i<lines.size();++i) {
        const auto cell=field(lines[i],col); double v=0;
        if(cell.empty()) {++out.rejected;continue;}
        const auto result=std::from_chars(cell.data(),cell.data()+cell.size(),v);
        v*=scale;
        if(result.ec!=std::errc{}||result.ptr!=cell.data()+cell.size()||!std::isfinite(v)||v<=0.05||v>=2000) {++out.rejected;continue;}
        out.ms.push_back(v);
    }
    return out;
}
struct Stats { double mean=0, p50=0, p95=0, p99=0, maximum=0, fps=0; };
inline Stats stats(const std::vector<double>& ms) {
    if(ms.empty()) return {};
    auto s=ms;std::sort(s.begin(),s.end());
    auto pct=[&](double p){double i=p*double(s.size()-1); auto k=std::size_t(i); return s[k]+(s[std::min(k+1,s.size()-1)]-s[k])*(i-double(k));};
    const double mean=std::accumulate(s.begin(),s.end(),0.0)/double(s.size());
    return {mean,pct(.5),pct(.95),pct(.99),s.back(),mean>0?1000/mean:0};
}
// Min/max per bucket in original order: a short stutter is never hidden by averaging.
inline std::vector<Sample> envelope(const std::vector<double>& ms,std::size_t buckets=1000) {
    std::vector<Sample> out;
    if(ms.empty()||!buckets)return out;
    const auto size=ms.size(), step=1+(size-1)/buckets;
    for(std::size_t begin=0;begin<size;) {
        const auto end=begin+std::min(step,size-begin);
        auto lo=begin,hi=begin;
        for(auto i=begin+1;i<end;++i){if(ms[i]<ms[lo])lo=i;if(ms[i]>ms[hi])hi=i;}
        if(lo>hi)std::swap(lo,hi);
        out.push_back({lo,ms[lo]});if(hi!=lo)out.push_back({hi,ms[hi]});
        begin=end;
    }
    return out;
}
}
