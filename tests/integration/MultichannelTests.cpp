#include "veyra/sink/WasapiAudioSink.h"
#include "veyra/sink/CaptureAudioSession.h"
#include "veyra/sink/AudioGain.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <chrono>
#include <thread>
using namespace veyra::sink;
static void wave(const std::filesystem::path& path,unsigned rate,uint32_t mask,bool isolated=false){
    auto w=floatWave({6,mask});w.Format.nSamplesPerSec=rate;w.Format.nAvgBytesPerSec=rate*24;
    std::ofstream out(path,std::ios::binary);auto number=[&](uint32_t n){out.write(reinterpret_cast<const char*>(&n),4);};
    const uint32_t size=rate*24;out.write("RIFF",4);number(60+size);out.write("WAVEfmt ",8);number(40);out.write(reinterpret_cast<const char*>(&w),40);out.write("data",4);number(size);
    for(unsigned i=0;i<rate;++i)for(unsigned c=0;c<6;++c){const float value=isolated?(i/(rate/10)==c?float(.2*sin(2*3.141592653589793*(c==3?60:300+c*100)*i/rate)):0):.04f*(c+1);out.write(reinterpret_cast<const char*>(&value),4);}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    unsigned failures=0,checks=0;auto check=[&](bool ok,const char* name){++checks;if(!ok)++failures;std::cout<<name<<" pass="<<ok<<std::endl;};
    for(auto mask:{0x3fu,0x60fu})for(unsigned bits:{16,24,32}){
        auto w=floatWave({6,mask});w.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;w.Format.wBitsPerSample=WORD(bits);w.Format.nBlockAlign=WORD(bits/8*6);w.Format.nAvgBytesPerSec=w.Format.nBlockAlign*48000;w.Samples.wValidBitsPerSample=WORD(bits);
        WavePcmFormat parsed;check(parseWavePcm(&w,sizeof(w),parsed)&&parsed.layout.mask==mask&&parsed.layout.channels==6,"PCM extended layout");
        check(!parseWavePcm(&w,sizeof(WAVEFORMATEX),parsed),"truncated extended format rejected");
        w.dwChannelMask=3;check(!parseWavePcm(&w,sizeof(w),parsed),"six channels with stereo mask rejected");
    }
    {
        auto w=floatWave({2,0x3});w.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;w.Format.wBitsPerSample=32;w.Format.nBlockAlign=8;w.Format.nAvgBytesPerSec=48000*8;w.Samples.wValidBitsPerSample=24;
        WavePcmFormat parsed;check(parseWavePcm(&w,sizeof(w),parsed)&&parsed.validBits==24&&parsed.wave.wBitsPerSample==32,"24 valid bits in 32-bit PCM container accepted");
        WAVEFORMATEX low{};low.wFormatTag=WAVE_FORMAT_PCM;low.nChannels=2;low.nSamplesPerSec=44100;low.wBitsPerSample=16;low.nBlockAlign=4;low.nAvgBytesPerSec=44100*4;
        WAVEFORMATEX native=low;native.nSamplesPerSec=48000;native.nAvgBytesPerSec=48000*4;
        WavePcmFormat lowParsed{},nativeParsed{};parseWavePcm(&low,sizeof(low),lowParsed);parseWavePcm(&native,sizeof(native),nativeParsed);
        check(preferCaptureAudioFormat(nativeParsed,lowParsed)&&!preferCaptureAudioFormat(lowParsed,nativeParsed),"capture format preference chooses native 48 kHz over enumeration order");
    }
    for(auto mask:{0x3fu,0x60fu})for(unsigned rate:{44100u,48000u,96000u}){
        const auto path=root/(std::to_wstring(mask)+L"-"+std::to_wstring(rate)+L".wav");wave(path,rate,mask);
        AudioPipeline pipe;bool ok=pipe.open(path.wstring())&&pipe.pcmFormat()==AudioFormat{6,mask};
        if(ok){pipe.startThread(nullptr);const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(pipe.bufferedMs()<500&&std::chrono::steady_clock::now()<until)Sleep(2);pipe.stopThread();
            std::vector<float> pcm(10000*6+16,-9);double pts=-1;const auto n=pipe.pull(pcm.data(),10000,&pts);ok=n==10000;
            double error=0;for(size_t i=100;i<n;++i)for(unsigned c=0;c<6;++c)error=std::max(error,std::abs(double(pcm[i*6+c]-.04f*(c+1))));
            ok=ok&&error<.0001&&pipe.overruns()==0&&std::all_of(pcm.begin()+60000,pcm.end(),[](float v){return v==-9;});
            double next=-1;pipe.pull(pcm.data(),1,&next);ok=ok&&std::abs(next-pts-10000.0/48)<.01;
            std::cout<<"rate="<<rate<<" mask="<<mask<<" amplitudeError="<<error<<" pts="<<pts<<" next="<<next<<std::endl;
        }
        check(ok,"file six channels independent and PTS in audio frames");
    }
    {
        const auto path=root/L"six-speaker-isolation.wav";wave(path,48000,0x60f,true);
        AudioPipeline pipe;bool ok=pipe.open(path.wstring());
        if(ok){pipe.startThread(nullptr);const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(pipe.bufferedMs()<650&&std::chrono::steady_clock::now()<until)Sleep(2);pipe.stopThread();
            std::vector<float> samples(28800*6);const auto n=pipe.pull(samples.data(),28800,nullptr);ok=n==28800;
            for(unsigned speaker=0;speaker<6;++speaker){double active=0,leak=0;
                for(unsigned i=speaker*4800;i<(speaker+1)*4800;++i)for(unsigned c=0;c<6;++c)
                    if(c==speaker)active+=samples[i*6+c]*samples[i*6+c];else leak+=std::abs(samples[i*6+c]);
                ok=ok&&active>90&&active<100&&leak<.00001;
                std::cout<<"speaker="<<speaker<<" energy="<<active<<" otherChannelLeak="<<leak<<std::endl;
            }
        }
        check(ok,"six independent speaker bursts including center and LFE");
    }
    std::vector<float> pcm(240*6,1);float gain=0;applyPcmGain(pcm.data(),240,6,1,gain);bool equal=true;
    for(unsigned i=0;i<240;++i)for(unsigned c=1;c<6;++c)equal&=pcm[i*6+c]==pcm[i*6];
    fadePcmTail(pcm.data(),240,6,gain);check(equal&&std::all_of(pcm.end()-6,pcm.end(),[](float v){return v==0;}),"six-channel simultaneous gain and fade");
    CaptureAudioSession capture;auto format=floatWave({6,0x60f});check(capture.configure(format.Format,sizeof(format)),"capture accepts true 5.1 mask");capture.setGain(0);capture.setSync(2,0);
    bool started=capture.start();std::vector<float> silence(480*6,0);const auto begin=std::chrono::steady_clock::now();
    for(unsigned i=0;started&&i<80;++i){std::this_thread::sleep_until(begin+std::chrono::milliseconds(i*10));started=capture.push(silence.data(),silence.size()*4,i*10.0,i==0);}
    const auto state=capture.snapshot();capture.stop();
    std::cout<<"capture input="<<state.inputChannels<<" output="<<state.outputChannels<<" mask="<<state.outputChannelMask<<" blocks="<<state.inputBlocks<<std::endl;
    check(started&&state.available&&state.running&&state.inputChannels==6&&state.outputChannels>0&&state.overflows==0&&state.inputValidBits==32,"5.1 capture to actual WASAPI endpoint (muted; no speaker claim)");
    {
        auto format24=floatWave({2,0x3});format24.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;format24.Format.wBitsPerSample=32;format24.Format.nBlockAlign=8;format24.Format.nAvgBytesPerSec=48000*8;format24.Samples.wValidBitsPerSample=24;
        CaptureAudioSession capture24;bool ok=capture24.configure(format24.Format,sizeof(format24))&&capture24.start();capture24.setGain(0);capture24.setSync(2,0);
        std::vector<int32_t> samples(480*2);for(size_t i=0;i<samples.size();++i){const int32_t value=int32_t(0.25*2147483647.0);samples[i]=value&~0xFF;}
        const auto begin24=std::chrono::steady_clock::now();for(unsigned i=0;ok&&i<80;++i){std::this_thread::sleep_until(begin24+std::chrono::milliseconds(i*10));ok=capture24.push(samples.data(),samples.size()*sizeof(int32_t),i*10.0,i==0);}
        const auto state24=capture24.snapshot();capture24.stop();
        check(ok&&state24.inputContainerBits==32&&state24.inputValidBits==24&&state24.nonFiniteSamples==0&&state24.inputPeak>.20&&state24.inputPeak<.30&&state24.invalidPaddingSamples==0,"24-in-32 PCM is left-aligned and finite after conversion");
    }
    std::cout<<"MULTICHANNEL checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;
}
