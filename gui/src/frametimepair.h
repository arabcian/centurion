#pragma once
#include "frametime.h"
#include <array>
#include <optional>
namespace frametimepair {
// A user-declared pair of logs is descriptive, not a randomized experiment.
// A small sample or materially malformed log is not eligible for a delta.
inline bool eligible(const frametime::Log &log) {
    if (!log.supported || log.ms.size()<300 || log.rejected>log.ms.size()/19) return false;
    return std::all_of(log.ms.begin(),log.ms.end(),[](double v){return std::isfinite(v) && v>0.05 && v<2000;});
}
struct Comparison { frametime::Stats a,b; std::array<double,5> delta; };
inline std::optional<Comparison> compare(const frametime::Log &a,const frametime::Log &b,bool declaredSameContext,bool distinctLogs) {
    if(!declaredSameContext || !distinctLogs || !eligible(a) || !eligible(b)) return {};
    const auto x=frametime::stats(a.ms),y=frametime::stats(b.ms);
    const auto percent=[](double a,double b){return 100.0*(b/a-1.0);};
    return Comparison{x,y,{percent(x.mean,y.mean),percent(x.p50,y.p50),percent(x.p95,y.p95),percent(x.p99,y.p99),percent(x.fps,y.fps)}};
}
}
