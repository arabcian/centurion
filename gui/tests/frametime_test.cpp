#include "../src/frametime.h"
#include <cassert>
int main(){
    using namespace frametime;
    auto a=parse("os,cpu\nLinux,Ryzen\nfps,frametime\n240,4.1\n238,4.2\n0,nan\n0,0\n0,4bad\n");
    assert(a.supported&&a.ms.size()==2&&a.rejected==3);
    auto b=parse("interval_ns,fps\n10000000,100\n20000000,50\n"); assert(b.ms.size()==2&&b.ms[0]==10&&b.ms[1]==20);
    assert(!parse("fps,cpu\n60,50").supported);
    auto c=parse("\xef\xbb\xbf# comment\n10\n20\n30\n"); auto s=stats(c.ms);assert(s.mean==20&&s.p50==20&&s.fps==50&&s.maximum==30);
    std::vector<double> x(10000,10);x[255]=100;auto e=envelope(x,100);assert(e.size()<=200);assert(std::any_of(e.begin(),e.end(),[](auto p){return p.index==255&&p.ms==100;}));
    for(std::size_t i=1;i<e.size();++i)assert(e[i-1].index<e[i].index);
    assert(envelope({},100).empty()&&envelope(x,0).empty());
}
