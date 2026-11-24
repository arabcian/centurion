#pragma once
// Pure presentation math: not a controller timing model and never a write path.
#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <cstddef>
#include "canvasviewport.h"
namespace lightingpreview {
struct Frame { double hue, brightness; std::size_t color; };
inline Frame frame(int type, int speed, int direction, int rotation, double time,
                   double position, std::size_t colorCount) {
    if (!std::isfinite(time)) time=0;
    if (!std::isfinite(position)) position=0;
    position=std::clamp(position,0.,1.);
    const double rate=0.25*std::clamp(speed,1,4);
    const auto wrap=[](double v){return v-std::floor(v);};
    double phase=wrap(std::fmod(time,3600.)*rate);
    const double p=(direction==3 || direction==1 || rotation==2)?1-position:position;
    double hue=position, level=1;
    switch(type) {
    case 1: case 2: hue=wrap(phase+p);break; // rotating/rainbow approximation
    case 3: case 6: hue=phase;break;
    case 4: hue=phase;level=0.2+0.8*(0.5-0.5*std::cos(phase*2*std::numbers::pi));break;
    case 5: hue=wrap(phase-p);break;
    // Host/reactive effects are intentionally not fabricated by the caller.
    default: break;
    }
    const auto color=colorCount?std::min(colorCount-1,static_cast<std::size_t>(hue*static_cast<double>(colorCount))):0;
    return {hue,level,color};
}
inline bool animated(int type) {return type>=1 && type<=6;}
inline std::optional<int> zoneAt(const canvasviewport::View &view,canvasviewport::Point p) {
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.x>=1||p.y<0||p.y>=1)return {};
    p=view.inverse(p);
    if(p.x<0||p.x>=1||p.y<0||p.y>=1)return {};
    return std::min(3,static_cast<int>(p.x*4));
}
}
