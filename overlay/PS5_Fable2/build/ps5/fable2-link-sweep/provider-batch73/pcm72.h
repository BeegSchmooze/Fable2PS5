#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
namespace fable2_ps5_audio {
inline float load_be_float(const float* source) {
  uint32_t bits; std::memcpy(&bits, source, sizeof(bits));
  bits=__builtin_bswap32(bits);
  float value; std::memcpy(&value,&bits,sizeof(value));return value;
}
inline int16_t pcm16(float value) {
  if(!std::isfinite(value))return 0;
  if(value>=1.0f)return 32767;
  if(value<=-1.0f)return -32767;
  return static_cast<int16_t>(value*32767.0f);
}
inline void stereo(const float* input,int16_t* output,size_t samples,
                   float center,float surround,float lfe,float scale,float gain) {
  for(size_t i=0;i<samples;++i) {
    const float mid=load_be_float(input+2*samples+i)*center+load_be_float(input+3*samples+i)*lfe;
    output[2*i]=pcm16((load_be_float(input+i)+mid+load_be_float(input+4*samples+i)*surround)*scale*gain);
    output[2*i+1]=pcm16((load_be_float(input+samples+i)+mid+load_be_float(input+5*samples+i)*surround)*scale*gain);
  }
}
}
