#include "../src/feed/flv.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>

int main(int argc, char** argv) {
    using namespace recam::flv;
    std::vector<uint8_t> hdr={'F','L','V',1,1,0,0,0,9,0,0,0,0};
    size_t consumed=0;
    for(size_t n=0;n<hdr.size();++n) assert(header(hdr.data(),n,consumed)==Result::More);
    assert(header(hdr.data(),hdr.size(),consumed)==Result::Ready && consumed==13);
    hdr[8]=8; assert(header(hdr.data(),hdr.size(),consumed)==Result::Invalid);
    std::vector<uint8_t> badTag(15,0);
    Tag tagOut{};
    badTag[0]=9; badTag[14]=11;
    assert(tag(badTag.data(),badTag.size(),tagOut)==Result::Ready);
    badTag[14]=12;
    assert(tag(badTag.data(),badTag.size(),tagOut)==Result::Invalid);
    badTag[1]=255;
    assert(tag(badTag.data(),11,tagOut)==Result::Invalid);
    unsigned width=0;std::vector<uint8_t> sps,pps,out;
    const std::vector<uint8_t> cfg={1,66,0,30,255,225,0,2,0x67,1,1,0,2,0x68,1};
    for(size_t n=0;n<cfg.size();++n) assert(!config(cfg.data(),n,width,sps,pps));
    assert(config(cfg.data(),cfg.size(),width,sps,pps) && width==4);
    auto bad=cfg;bad[4]=254;assert(!config(bad.data(),bad.size(),width,sps,pps));
    const std::vector<uint8_t> pic={0,0,0,2,0x65,0x80,0,0,0,2,0x65,0x40};
    size_t vcl=0;
    assert(picture(pic.data(),pic.size(),4,out,vcl) && vcl==4 && out.size()==12);
    bad=pic;bad[11]=0x80;assert(!picture(bad.data(),bad.size(),4,out,vcl));
    for(size_t n=0;n<6;++n)assert(!picture(pic.data(),n,4,out,vcl));
    // Truncation after a complete first NAL is a valid single-slice picture;
    // partial subsequent lengths and payloads must be rejected.
    for(size_t n=7;n<pic.size();++n)assert(!picture(pic.data(),n,4,out,vcl));
    std::mt19937 rng(123);
    for(int i=0;i<10000;++i){
        std::vector<uint8_t> bytes(rng()%512);
        for(auto& b:bytes)b=rng();
        Tag t{};header(bytes.data(),bytes.size(),consumed);tag(bytes.data(),bytes.size(),t);
        config(bytes.data(),bytes.size(),width,sps,pps);
        picture(bytes.data(),bytes.size(),(rng()%4)+1,out,vcl);
    }
    if(argc>1){
        std::ifstream f(argv[1],std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        assert(header(bytes.data(),bytes.size(),consumed)==Result::Ready);
        size_t pos=consumed;unsigned pictures=0;
        while(pos<bytes.size()){
            Tag t{};assert(tag(bytes.data()+pos,bytes.size()-pos,t)==Result::Ready);
            for(size_t n=0;n<t.bytes;++n){Tag partial{};assert(tag(bytes.data()+pos,n,partial)==Result::More);}
            if(t.type==9){
                assert(t.size>=5);
                if(t.data[1]==0)assert(config(t.data+5,t.size-5,width,sps,pps));
                if(t.data[1]==1){assert(picture(t.data+5,t.size-5,width,out,vcl));++pictures;}
            }
            pos+=t.bytes;
        }
        assert(pictures==30);
        printf("parsed %u complete FFmpeg pictures\n",pictures);
    }
    puts("FLV bounds, fragmentation, and picture checks passed");
}
