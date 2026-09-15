#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
namespace veyra::sink {
// 5 ms full-scale ramp at 48 kHz, independent of PTS and renderer clock.
inline void applyPcmGain(float* samples,size_t frames,unsigned channels,float target,float& current){
    target=std::isfinite(target)?std::clamp(target,0.0f,1.0f):0;
    for(size_t i=0;i<frames;++i){current+=std::clamp(target-current,-1.0f/240,1.0f/240);for(unsigned c=0;c<channels;++c)samples[i*channels+c]*=current;}
}
// Full transition duration is fixed even when the current user gain is low.
inline void fadePcmTail(float* samples,size_t frames,unsigned channels,float gain){
    if(!frames)return;
    gain=std::isfinite(gain)?std::clamp(gain,0.0f,1.0f):0;
    for(size_t i=0;i<frames;++i){const float envelope=frames>1?gain*float(frames-1-i)/float(frames-1):0;
        for(unsigned c=0;c<channels;++c)samples[channels*i+c]*=envelope;}
}
// Restart the waveform at zero after a live endpoint gap.  This is applied
// after the user gain, so it only changes the short edge envelope and never
// changes the requested volume.
inline void fadePcmHead(float* samples,size_t frames,unsigned channels,size_t& remaining){
    constexpr size_t total=240; // 5 ms at 48 kHz
    for(size_t i=0;i<frames&&remaining;++i){
        const size_t consumed=total-remaining;
        const float envelope=float(consumed+1)/float(total);
        for(unsigned c=0;c<channels;++c)samples[i*channels+c]*=envelope;
        --remaining;
    }
}
inline void applyStereoGain(float* p,size_t n,float target,float& current){applyPcmGain(p,n,2,target,current);}
inline void fadeStereoTail(float* p,size_t n,float gain){fadePcmTail(p,n,2,gain);}
}
