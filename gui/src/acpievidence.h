#pragma once
// Read-only lexical evidence for kernel-exposed ACPI/WMI objects. A mention
// is not proof of causation, a method invocation, or the historical binding.
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace acpievidence {
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline bool word(char c) { return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-'; }
inline std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return out;
}
inline bool uuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !hex(s[i])) return false;
    }
    return true;
}
// Accept an exact GUID or the kernel's GUID-N instance suffix only.
inline std::string wmiGuid(std::string_view s) {
    if (s.size() < 36 || s.size() > 64 || !uuid(s.substr(0,36))) return {};
    if (s.size() > 36 && (s[36] != '-' || s.size() == 37 ||
        !std::all_of(s.begin()+37, s.end(), digit))) return {};
    return lower(s.substr(0,36));
}
inline bool sameBoot(std::string_view a, std::string_view b) {
    return uuid(a) && uuid(b) && lower(a) == lower(b);
}
// Keep instance identifiers (PNP0C14:00) and dotted names whole, but let a
// trailing colon or period delimit a kernel log prefix. Do not use substrings.
inline std::vector<std::string> tokens(std::string_view line) {
    std::vector<std::string> out;
    if (line.size() > 8192) return out;
    for (std::size_t i = 0; i < line.size();) {
        if (!word(line[i])) { ++i; continue; }
        const auto begin = i++;
        while (i < line.size() && (word(line[i]) ||
            ((line[i] == ':' || line[i] == '.') && i+1 < line.size() && word(line[i+1])))) ++i;
        if (i-begin <= 128) out.push_back(lower(line.substr(begin,i-begin)));
        else out.emplace_back(); // preserve prefix position; never interpret a later word as the driver prefix
    }
    return out;
}
enum class Kind { Instance, Guid, Driver, ClassOnly, None };
struct Object { std::string instance, guid, driver; bool wmi = false; };
struct Match { Kind kind = Kind::None; std::vector<std::size_t> objects; std::size_t count = 0; };
class Index {
    using Map = std::unordered_map<std::string,std::vector<std::size_t>>;
    Map instances_, guids_, drivers_;
    static void add(Map &map, const std::string &key, std::size_t i) {
        if (!key.empty()) map[lower(key)].push_back(i);
    }
    // Log prefixes use KBUILD_MODNAME (underscores); driver directories often use dashes.
    static std::string driverKey(std::string s) {
        std::replace(s.begin(), s.end(), '-', '_');
        return s;
    }
public:
    explicit Index(const std::vector<Object> &objects) {
        for (std::size_t i=0;i<objects.size();++i) {
            const auto &o=objects[i];
            // A bare WMI GUID is not a unique instance when other instances share it.
            if (!o.wmi || (!o.guid.empty() && lower(o.instance)!=lower(o.guid))) add(instances_,o.instance,i);
            if (o.wmi && uuid(o.guid)) add(guids_,o.guid,i);
            add(drivers_,driverKey(o.driver),i);
        }
    }
    Match match(std::string_view line) const {
        auto ts=tokens(line);
        if (ts.empty()) return {};
        // The WMI bus device is named "wmi_bus-<ACPI instance>".
        for (auto &t:ts) if (t.rfind("wmi_bus-",0)==0) t.erase(0,8);
        auto find = [&](const Map &map, Kind kind, bool prefixOnly) {
            Match m; std::unordered_set<std::size_t> seen;
            for (std::size_t j=0;j<(prefixOnly?1:ts.size());++j) {
                const auto it=map.find(ts[j]);
                if (it==map.end()) continue;
                for (auto i:it->second) if (seen.insert(i).second) {
                    ++m.count;
                    if (m.objects.size()<16) m.objects.push_back(i);
                }
            }
            if (m.count) m.kind=kind;
            return m;
        };
        // Preserve all explicit mentions, even if a line names both a specific
        // instance and a different GUID. Never silently narrow contradictory evidence.
        auto direct=find(instances_,Kind::Instance,false);
        auto guid=find(guids_,Kind::Guid,false);
        if (direct.count && !guid.count) return direct;
        if (guid.count && !direct.count) return guid;
        if (direct.count && guid.count) {
            // Recompute the union without the display cap. A mixed mention is
            // labelled GUID evidence (not a uniquely resolved instance).
            std::unordered_set<std::size_t> seen;
            Match m; m.kind=Kind::Guid;
            for (const auto &t:ts) for (const auto *map:{&instances_,&guids_}) {
                const auto it=map->find(t); if(it==map->end()) continue;
                for(auto i:it->second) if(seen.insert(i).second) {
                    ++m.count; if(m.objects.size()<16)m.objects.push_back(i);
                }
            }
            return m;
        }
        // Driver prefix: the first token, or the second after an "ACPI:"/"WMI:" class prefix.
        const bool classPrefix = ts.front()=="acpi" || ts.front()=="wmi" || ts.front()=="acpica";
        if (classPrefix) ts.erase(ts.begin());
        if (!ts.empty()) {
            ts.front()=driverKey(ts.front());
            if (auto driver=find(drivers_,Kind::Driver,true); driver.count) return driver;
        }
        if (classPrefix) return {Kind::ClassOnly,{},0};
        return {};
    }
};
} // namespace acpievidence
