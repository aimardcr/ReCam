// Link the old scaler as reference_scale_to_i420 for bit-for-bit differential checks.
#include "../src/feed/scale.hpp"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
namespace recam { bool reference_scale_to_i420(const SrcFrame&, uint8_t*, uint32_t, uint32_t); }
int main() {
    std::mt19937 rng(123);
    for(int i=0;i<160;++i){
        int sw=2+2*(rng()%128),sh=2+2*(rng()%128),dw=2+2*(rng()%128),dh=2+2*(rng()%128);
        if(i==0){sw=1920;sh=1080;dw=1440;dh=1080;}
        if(i==1){sw=1280;sh=720;dw=1440;dh=1080;}
        if(i==2){sw=dw=640;sh=dh=480;}
        int ps=1+i%4, ys=sw+12, cs=sw/2*ps+8;
        std::vector<uint8_t> y(ys*sh),u(cs*sh/2),v(cs*sh/2);
        for(auto* plane:{&y,&u,&v})for(auto& b:*plane)b=rng();
        recam::SrcFrame src;src.width=sw;src.height=sh;
        src.y={y.data(),ys,1};src.cb={u.data(),cs,ps};src.cr={v.data(),cs,ps};
        std::vector<uint8_t> a(dw*dh*3/2+32,0xA5),b=a;
        bool r=recam::scale_to_i420(src,a.data(),dw,dh);
        bool expected=recam::reference_scale_to_i420(src,b.data(),dw,dh);
        assert(r==expected && a==b);
    }
    uint8_t pixels[16]{};
    recam::SrcFrame bad; bad.width=bad.height=2;
    bad.y={pixels,2,1};bad.cb=bad.cr={pixels,1,1};
    assert(!recam::scale_to_i420(bad,pixels,3,2));
    bad.y.rowStride=1;
    assert(!recam::scale_to_i420(bad,pixels,2,2));
    bad.y.rowStride=2;bad.cb.pixelStride=-1;
    assert(!recam::scale_to_i420(bad,pixels,2,2));
    puts("160 scaler differential/canary checks and invalid-layout checks passed");
}
