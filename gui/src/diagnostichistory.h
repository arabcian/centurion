#pragma once
// Archived capture provenance, not the boot/device of every individual finding.
#include <string>
#include <string_view>
namespace diagnostichistory {
inline std::string fullBoot(std::string_view value) {
    if(value.size()!=36) return {};
    std::string result; result.reserve(36);
    bool nonzero=false;
    for(std::size_t i=0;i<value.size();++i) {
        char c=value[i];
        if(i==8 || i==13 || i==18 || i==23) {if(c!='-') return {};}
        else {
            if(c>='A' && c<='F') c=char(c-'A'+'a');
            if(!((c>='0' && c<='9') || (c>='a' && c<='f'))) return {};
            nonzero |= c!='0';
        }
        result+=c;
    }
    return nonzero?result:std::string{};
}
enum class Relation { Unknown, SameCaptureBoot, DifferentCaptureBoot };
inline Relation relation(std::string_view a,std::string_view b) {
    const auto x=fullBoot(a),y=fullBoot(b);
    if(x.empty() || y.empty()) return Relation::Unknown;
    return x==y?Relation::SameCaptureBoot:Relation::DifferentCaptureBoot;
}
}
