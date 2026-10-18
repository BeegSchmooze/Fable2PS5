#include <rex/ui/presenter.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void be32(std::vector<uint8_t>& out,uint32_t n){for(int i=24;i>=0;i-=8)out.push_back(uint8_t(n>>i));}
bool chunk(FILE* f,const char* type,const std::vector<uint8_t>& data){
  uint32_t crc=~0u;
  auto add=[&](uint8_t b){crc^=b;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));};
  for(unsigned i=0;i<4;++i)add(uint8_t(type[i]));for(auto b:data)add(b);
  std::vector<uint8_t> size;be32(size,uint32_t(data.size()));
  std::vector<uint8_t> checksum;be32(checksum,~crc);
  return std::fwrite(size.data(),1,4,f)==4&&std::fwrite(type,1,4,f)==4&&
    (data.empty()||std::fwrite(data.data(),1,data.size(),f)==data.size())&&std::fwrite(checksum.data(),1,4,f)==4;
}
}
int devloop_write_png(const rex::ui::RawImage& image){
  if(!image.width||!image.height||image.width>8192||image.height>8192||image.stride<image.width*4u||image.data.size()<size_t(image.stride)*image.height)return 1;
  std::vector<uint8_t> raw;raw.reserve((size_t(image.width)*4+1)*image.height);
  uint32_t a=1,b=0;
  auto append=[&](uint8_t v){raw.push_back(v);a=(a+v)%65521;b=(b+a)%65521;};
  for(unsigned y=0;y<image.height;++y){append(0);const auto* row=image.data.data()+size_t(y)*image.stride;for(size_t x=0;x<size_t(image.width)*4;++x)append(row[x]);}
  std::vector<uint8_t> packed{0x78,0x01};packed.reserve(raw.size()+raw.size()/65535*5+16);
  for(size_t at=0;at<raw.size();){uint16_t count=uint16_t(std::min(size_t(65535),raw.size()-at));packed.push_back(at+count==raw.size()?1:0);packed.push_back(uint8_t(count));packed.push_back(uint8_t(count>>8));uint16_t inverse=~count;packed.push_back(uint8_t(inverse));packed.push_back(uint8_t(inverse>>8));packed.insert(packed.end(),raw.begin()+at,raw.begin()+at+count);at+=count;}
  be32(packed,(b<<16)|a);
  const char* folder="/app0/devloop-captures";
  mkdir(folder,0777);chmod(folder,0777);
  static unsigned sequence=0;char name[128];std::snprintf(name,sizeof(name),"%s/frame-%06u.png",folder,++sequence);
  std::string temporary=std::string(name)+".tmp";FILE* f=std::fopen(temporary.c_str(),"wb");if(!f)return 2;
  const uint8_t signature[]{137,80,78,71,13,10,26,10};std::vector<uint8_t> header;be32(header,image.width);be32(header,image.height);header.insert(header.end(),{8,6,0,0,0});
  bool ok=std::fwrite(signature,1,8,f)==8&&chunk(f,"IHDR",header)&&chunk(f,"IDAT",packed)&&chunk(f,"IEND",{});
  if(std::fclose(f))ok=false;
  if(!ok){unlink(temporary.c_str());return 3;}
  chmod(temporary.c_str(),0666);
  if(std::rename(temporary.c_str(),name)){unlink(temporary.c_str());return 4;}
  return 0;
}

