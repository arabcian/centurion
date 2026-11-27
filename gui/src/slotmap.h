#pragma once
// The SMU addresses a core by its physical slot; CO Search only knows the position of a core in the
// list of online CPUs. They are the same only while every CPU of the CCD is online and the cores'
// topology ids run without a gap in list order. Anything else would write a core's offset and load
// another CPU with it, so the search result and the crash guard would belong to the wrong core.
#include <optional>
#include <string>
#include <vector>

namespace slotmap {

/// One CCD's physical cores in list order: each one's topology core_id (nullopt = unreadable).
using CoreIds = std::vector<std::optional<int>>;

/// Empty when list position == physical slot for every core; otherwise why the search must not run.
inline std::string problem(const std::vector<CoreIds> &ccds, const std::vector<int> &offline) {
    if (!offline.empty()) {
        std::string cpus;
        for (int c : offline) cpus += (cpus.empty() ? "" : ", ") + std::to_string(c);
        return "CPU " + cpus + " is offline. Core slots are counted over the online cores, so a slot would no "
               "longer match its CPU. Bring every CPU online before searching.";
    }
    for (std::size_t ccd = 0; ccd < ccds.size(); ++ccd) {
        const CoreIds &ids = ccds[ccd];
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::string where = "CCD" + std::to_string(ccd) + ": ";
            if (!ids[i]) return where + "the topology id of a core is unreadable, so its SMU slot cannot be confirmed.";
            if (*ids[i] != *ids[0] + int(i))
                return where + "the cores' physical numbering has a gap or is out of order, so list position and SMU slot "
                               "can differ. The search is not run on this layout.";
        }
    }
    return {};
}

}  // namespace slotmap
