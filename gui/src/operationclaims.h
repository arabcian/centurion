#pragma once
// Explicitly audited privileged operations. Unclassified helpers continue to
// use their original backend locks; this is not a system-wide hardware lock.
#include <string_view>
#include <vector>
namespace operations {
enum class Resource { CpuCurve, GpuCurve, GpuProfiles, Tuning };
enum class Access { Read, Write };
struct Claim { Resource resource; Access access; };
struct RequestClaims { std::vector<Claim> claims; bool recoveryOverride=false; };
inline bool conflicts(const Claim &a,const Claim &b){return a.resource==b.resource&&(a.access==Access::Write||b.access==Access::Write);}
inline RequestClaims classify(std::string_view helper,std::string_view op){
    using R=Resource;using A=Access;
    if(helper=="ryzen-co-helper"){
        if(op=="probe")return {{{R::CpuCurve,A::Read}},false};
        if(op=="set_coall"||op=="set_coper_batch"||op=="reset")return {{{R::CpuCurve,A::Write}},op=="reset"};
    }else if(helper=="intel-uv-helper"){
        if(op=="status")return {{{R::CpuCurve,A::Read}},false};
        if(op=="apply"||op=="boot"||op=="reset"||op=="set_boot"||op=="clear_boot")return {{{R::CpuCurve,A::Write}},op=="reset"};
    }else if(helper=="nvcurve-root-helper"){
        if(op=="read_gpu_curve"||op=="read_sensors")return {{{R::GpuCurve,A::Read}},false};
        if(op=="write_nvcurve_profile"||op=="delete_nvcurve_profile"||op=="set_default_gpu_profile")return {{{R::GpuProfiles,A::Write}},false};
        if(op=="apply_named_profile"||op=="run_gpu_autoload")return {{{R::GpuCurve,A::Write},{R::GpuProfiles,A::Read}},false};
        if(op=="apply_gpu_offsets"||op=="set_powermizer"||op=="set_vram_memlock"||op=="set_gpu_clocklock")return {{{R::GpuCurve,A::Write}},false};
        if(op=="reset_gpu_curve"||op=="reset_all"||op=="reset_vram_memlock"||op=="reset_gpu_clocklock")return {{{R::GpuCurve,A::Write}},true};
    }else if(helper=="tune-helper"||helper=="tune-profile-helper"){
        if(op=="apply"||op=="apply_preset"||op=="restore"||op=="restore_keys"||op=="restore_defaults"||op=="boot"||op=="set_boot"||op=="snapshot"||op=="preset_save"||op=="preset_delete"||op=="boost"||op=="isolate_join"||op=="set_wireless_pm_lock")return {{{R::Tuning,A::Write}},false};
        // Cleanup must remain callable if a prior operation timed out. The backend still serializes it.
        if(op=="release"||op=="prune")return {{{R::Tuning,A::Write}},true};
    }
    return {};
}
} // namespace operations
