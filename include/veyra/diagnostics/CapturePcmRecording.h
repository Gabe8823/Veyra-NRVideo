#pragma once
#include <windows.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>
#include <cstdint>
#include "veyra/Log.h"
namespace veyra::diagnostics {
// Explicit diagnostic opt-in only. Keep at most 20 seconds per stage in
// memory; write after capture stops, never perform disk I/O in a callback.
class CapturePcmRecording {
    std::wstring prefix_;
    size_t rawLimit_=0,floatLimit_=0;
    unsigned rate_=0,channels_=0,bits_=0,valid_=0;bool floating_=false;
    std::vector<uint8_t> raw_;
    std::vector<float> converted_,pulled_,rendered_;
    struct Write {uint64_t frame;unsigned count,padding;int64_t qpc;};
    std::vector<Write> writes_;unsigned renderedChannels_=0;
public:
    void configure(unsigned rate,unsigned channels,unsigned bits,unsigned valid,bool floating){
        wchar_t path[32768]{};const DWORD n=GetEnvironmentVariableW(L"VEYRA_DIAGNOSTIC_CAPTURE_PCM_PREFIX",path,DWORD(std::size(path)));
        if(!n||n>=std::size(path)||!std::filesystem::path(path).is_absolute())return;
        prefix_=path;rate_=rate;channels_=channels;bits_=bits;valid_=valid;floating_=floating;
        rawLimit_=size_t(rate)*channels*(bits/8)*20;floatLimit_=size_t(48000)*channels*20;
        raw_.reserve(rawLimit_);converted_.reserve(floatLimit_);pulled_.reserve(floatLimit_);rendered_.reserve(size_t(48000)*8*20);writes_.reserve(20000);
    }
    void raw(const void* bytes,size_t count){if(prefix_.empty())return;const auto* p=static_cast<const uint8_t*>(bytes);count=std::min(count,rawLimit_-raw_.size());raw_.insert(raw_.end(),p,p+count);}
    void converted(const float* samples,size_t count){if(prefix_.empty())return;count=std::min(count,floatLimit_-converted_.size());converted_.insert(converted_.end(),samples,samples+count);}
    void pulled(const float* samples,size_t count){if(prefix_.empty())return;count=std::min(count,floatLimit_-pulled_.size());pulled_.insert(pulled_.end(),samples,samples+count);}
    void rendered(const float* samples,size_t frames,unsigned channels,unsigned padding){
        if(prefix_.empty()||!channels||channels>8)return;
        if(renderedChannels_&&renderedChannels_!=channels)return;
        renderedChannels_=channels;const size_t limit=size_t(48000)*channels*20;
        const size_t count=std::min(frames*channels,limit-rendered_.size());if(!count)return;
        LARGE_INTEGER qpc;QueryPerformanceCounter(&qpc);
        if(writes_.size()<20000)writes_.push_back({rendered_.size()/channels,unsigned(count/channels),padding,qpc.QuadPart});
        rendered_.insert(rendered_.end(),samples,samples+count);
    }
    void finish(){
        if(prefix_.empty())return;
        try{
        bool saved=true;
        auto write=[&](const wchar_t* suffix,const void* data,size_t bytes){std::ofstream file(std::filesystem::path(prefix_+suffix),std::ios::binary);if(bytes)file.write(static_cast<const char*>(data),std::streamsize(bytes));file.close();saved=saved&&bool(file);};
        write(L".raw.pcm",raw_.data(),raw_.size());write(L".converted.f32",converted_.data(),converted_.size()*sizeof(float));write(L".pulled.f32",pulled_.data(),pulled_.size()*sizeof(float));
        write(L".rendered.f32",rendered_.data(),rendered_.size()*sizeof(float));
        std::ofstream events(std::filesystem::path(prefix_+L".writes.csv"));LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
        events<<"frame,count,padding,qpc,frequency,channels\n";for(const auto& w:writes_)events<<w.frame<<','<<w.count<<','<<w.padding<<','<<w.qpc<<','<<frequency.QuadPart<<','<<renderedChannels_<<'\n';
        std::ofstream meta(std::filesystem::path(prefix_+L".json"));meta<<"{\"inputRate\":"<<rate_<<",\"channels\":"<<channels_<<",\"containerBits\":"<<bits_<<",\"validBits\":"<<valid_<<",\"floating\":"<<(floating_?"true":"false")<<",\"outputRate\":48000,\"rawBytes\":"<<raw_.size()<<",\"convertedSamples\":"<<converted_.size()<<",\"pulledSamples\":"<<pulled_.size()<<"}";
        events.close();meta.close();
        if(!saved||!events||!meta)log::warn("capture-pcm-recording","Diagnostic recording write failed; capture state is unaffected");
        }catch(const std::exception&){log::warn("capture-pcm-recording","Diagnostic recording could not be saved");}
        prefix_.clear();raw_.clear();converted_.clear();pulled_.clear();rendered_.clear();writes_.clear();renderedChannels_=0;
    }
};
}
