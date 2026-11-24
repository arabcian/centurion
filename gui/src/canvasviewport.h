#pragma once
// View-only normalized pan/zoom. No key map, device or hardware state.
#include <algorithm>
#include <cmath>
namespace canvasviewport {
struct Point { double x=0,y=0; };
class View {
public:
    double zoom() const { return zoom_; }
    Point offset() const { return offset_; }
    void reset() { zoom_=1; offset_={}; }
    Point map(Point p) const { return {0.5+(p.x-0.5)*zoom_+offset_.x,0.5+(p.y-0.5)*zoom_+offset_.y}; }
    Point inverse(Point p) const { return {0.5+(p.x-0.5-offset_.x)/zoom_,0.5+(p.y-0.5-offset_.y)/zoom_}; }
    void zoomAt(double factor, Point anchor) {
        if(!std::isfinite(factor)||factor<=0||!std::isfinite(anchor.x)||!std::isfinite(anchor.y)) return;
        const auto original=inverse(anchor);
        zoom_=std::clamp(zoom_*factor,1.0,4.0);
        offset_={anchor.x-0.5-(original.x-0.5)*zoom_,anchor.y-0.5-(original.y-0.5)*zoom_};
        clamp();
    }
    void pan(Point delta) {
        if(!std::isfinite(delta.x)||!std::isfinite(delta.y)) return;
        offset_.x+=delta.x; offset_.y+=delta.y; clamp();
    }
private:
    void clamp() { const auto max=(zoom_-1)/2; offset_.x=std::clamp(offset_.x,-max,max); offset_.y=std::clamp(offset_.y,-max,max); }
    double zoom_=1; Point offset_{};
};
}
