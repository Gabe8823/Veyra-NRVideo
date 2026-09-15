#pragma once
#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <bit>
#include <cstdint>
#include <cstring>
namespace veyra::sink {
// Interleaved float PCM exchanged by the software pipeline, always 48 kHz.
// WAVE and FFmpeg native channel masks share the standard speaker bit order.
struct AudioFormat {
    unsigned channels=2;
    uint32_t mask=SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT;
    bool valid()const{return channels>=1&&channels<=8&&std::popcount(mask)==int(channels)&&(mask&~0x3ffffu)==0;}
    bool operator==(const AudioFormat&)const=default;
};
struct WavePcmFormat {
    WAVEFORMATEX wave{};
    AudioFormat layout;
    unsigned validBits=0;
    bool floating=false;
};
inline bool parseWavePcm(const void* bytes,size_t size,WavePcmFormat& out){
    if(!bytes||size<sizeof(WAVEFORMATEX))return false;
    WAVEFORMATEX w{};memcpy(&w,bytes,sizeof(w));
    if(w.cbSize>size-sizeof(w)||w.nChannels<1||w.nChannels>8||w.nSamplesPerSec<8000||w.nSamplesPerSec>192000||
       (w.wBitsPerSample!=16&&w.wBitsPerSample!=24&&w.wBitsPerSample!=32)||
       w.nBlockAlign!=w.nChannels*(w.wBitsPerSample/8)||w.nAvgBytesPerSec!=w.nBlockAlign*w.nSamplesPerSec)return false;
    WavePcmFormat result;result.wave=w;result.layout.channels=w.nChannels;result.validBits=w.wBitsPerSample;
    if(w.wFormatTag==WAVE_FORMAT_EXTENSIBLE){
        if(w.cbSize<22||size<sizeof(WAVEFORMATEXTENSIBLE))return false;
        WAVEFORMATEXTENSIBLE e{};memcpy(&e,bytes,sizeof(e));
        if(e.SubFormat!=KSDATAFORMAT_SUBTYPE_PCM&&e.SubFormat!=KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)return false;
        result.floating=e.SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        result.validBits=e.Samples.wValidBitsPerSample;
        result.layout.mask=e.dwChannelMask;
    }else{
        if(w.wFormatTag!=WAVE_FORMAT_PCM&&w.wFormatTag!=WAVE_FORMAT_IEEE_FLOAT)return false;
        result.floating=w.wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
        // More than stereo without a speaker mask is ambiguous (side/back).
        if(w.nChannels>2)return false;
        result.layout.mask=w.nChannels==1?SPEAKER_FRONT_CENTER:SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT;
    }
    if(!result.layout.valid()||!result.validBits||result.validBits>w.wBitsPerSample||
       (result.floating&&(w.wBitsPerSample!=32||result.validBits!=32)))return false;
    out=result;return true;
}
// Capture devices frequently enumerate a low-rate compatibility format before
// their native 48 kHz format.  Do not let DirectShow enumeration order choose
// the format: the whole live path and the WASAPI renderer use 48 kHz PCM.
inline bool preferCaptureAudioFormat(const WavePcmFormat& lhs,const WavePcmFormat& rhs){
    const auto rateDistance=[](const WAVEFORMATEX& f)->uint32_t{
        const auto rate=f.nSamplesPerSec;return rate>48000?rate-48000:48000-rate;
    };
    const bool lhs48=lhs.wave.nSamplesPerSec==48000,rhs48=rhs.wave.nSamplesPerSec==48000;
    if(lhs48!=rhs48)return lhs48;
    if(const auto l=rateDistance(lhs.wave),r=rateDistance(rhs.wave);l!=r)return l<r;
    if(lhs.layout.channels!=rhs.layout.channels)return lhs.layout.channels>rhs.layout.channels;
    const bool lhs16=!lhs.floating&&lhs.wave.wBitsPerSample==16&&lhs.validBits==16;
    const bool rhs16=!rhs.floating&&rhs.wave.wBitsPerSample==16&&rhs.validBits==16;
    if(lhs16!=rhs16)return lhs16;
    const bool lhsFull=lhs.validBits==lhs.wave.wBitsPerSample;
    const bool rhsFull=rhs.validBits==rhs.wave.wBitsPerSample;
    if(lhsFull!=rhsFull)return lhsFull;
    if(lhs.validBits!=rhs.validBits)return lhs.validBits>rhs.validBits;
    if(lhs.floating!=rhs.floating)return !lhs.floating;
    return lhs.wave.wBitsPerSample<rhs.wave.wBitsPerSample;
}
inline WAVEFORMATEXTENSIBLE floatWave(AudioFormat layout){
    WAVEFORMATEXTENSIBLE out{};out.Format={WAVE_FORMAT_EXTENSIBLE,WORD(layout.channels),48000,48000*layout.channels*4,WORD(layout.channels*4),32,22};
    out.Samples.wValidBitsPerSample=32;out.dwChannelMask=layout.mask;out.SubFormat=KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;return out;
}
}
