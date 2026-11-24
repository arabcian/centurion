#pragma once
// Shared selection/order for SceneEngine and its read-only preview. No I/O.
#include <algorithm>
#include <vector>
namespace sceneplan {
enum class Step { FanBoost, Power, FanBoostCheck, Firmware, CpuRyzen, CpuIntel, CpuUnsupported,
                  Gpu, Optimization, FanCurve, Spectrum, Zone4, Keyboard, PanelOd, Display, Command, Brightness };
enum class Cpu { None, Ryzen, Intel };
struct Inputs {
    bool fanBoost=false,power=false,firmware=false,cpu=false,gpu=false,optimization=false,fanCurve=false,
         spectrum=false,zone4=false,keyboard=false,panelOd=false,display=false,command=false,brightness=false;
    Cpu cpuBackend=Cpu::None;
};
inline std::vector<Step> steps(const Inputs &i) {
    std::vector<Step> out;
    auto add=[&](bool yes,Step step){if(yes)out.push_back(step);};
    add(i.fanBoost,Step::FanBoost); add(i.power,Step::Power); add(i.fanBoost&&i.power,Step::FanBoostCheck);
    add(i.firmware,Step::Firmware);
    if(i.cpu)out.push_back(i.cpuBackend==Cpu::Ryzen?Step::CpuRyzen:i.cpuBackend==Cpu::Intel?Step::CpuIntel:Step::CpuUnsupported);
    add(i.gpu,Step::Gpu);add(i.optimization,Step::Optimization);add(i.fanCurve,Step::FanCurve);
    add(i.spectrum,Step::Spectrum);add(i.zone4,Step::Zone4);add(i.keyboard,Step::Keyboard);add(i.panelOd,Step::PanelOd);
    add(i.display,Step::Display);add(i.command,Step::Command);add(i.brightness,Step::Brightness);
    return out;
}
inline bool contains(const std::vector<Step>& plan,Step s){return std::find(plan.begin(),plan.end(),s)!=plan.end();}
// Gates are evaluated by the caller in this order, so live reads can short circuit.
enum class SourceAction { Paused, CalibrationDeferred, GameDeferred, NoTarget, Coalesce, Queue, Apply };
inline SourceAction sourceAction(bool paused,bool calibrating,bool game,bool hasTarget,bool busy,bool sameTarget) {
    if(paused)return SourceAction::Paused;
    if(calibrating)return SourceAction::CalibrationDeferred;
    if(game)return SourceAction::GameDeferred;
    if(!hasTarget)return SourceAction::NoTarget;
    if(busy)return sameTarget?SourceAction::Coalesce:SourceAction::Queue;
    return SourceAction::Apply;
}
} // namespace sceneplan
