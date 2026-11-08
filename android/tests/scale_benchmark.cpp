// Synthetic standalone microbenchmark; not a camera-pipeline latency measurement.
#include "../src/feed/scale.hpp"
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdio>
int main() {
 for (auto size : {1280,1920}) {
  int h=size*9/16;
  std::vector<uint8_t> in(size*h*3/2), out(1440*1080*3/2);
  for (size_t i=0;i<in.size();++i) in[i]=(i*71+i/537)&255;
  recam::SrcFrame s; s.width=size;s.height=h;
  s.y={in.data(),size,1};s.cb={in.data()+size*h,size/2,1};s.cr={in.data()+size*h*5/4,size/2,1};
  std::vector<double> times;
  for(int i=0;i<120;++i){auto a=std::chrono::steady_clock::now();if(!recam::scale_to_i420(s,out.data(),1440,1080))return 1;auto b=std::chrono::steady_clock::now();if(i>=20)times.push_back(std::chrono::duration<double,std::milli>(b-a).count());}
  std::sort(times.begin(),times.end());unsigned long long sum=0;for(auto v:out)sum=sum*131+v;
  printf("%dx%d median=%.3f p95=%.3f ms checksum=%llu\n",size,h,times[50],times[94],sum);
 }
}
