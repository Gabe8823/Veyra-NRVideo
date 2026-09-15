#include "veyra/sink/CaptureAudioDsp.h"
#include "veyra/sink/AudioGain.h"
#include "veyra/sink/WasapiAudioSink.h"
#include <iostream>
#include <vector>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <numbers>
extern "C" {
#include <libswresample/swresample.h>
}
using namespace veyra::sink;
namespace {
void checked(int result){if(result<0)throw std::runtime_error("FFmpeg error "+std::to_string(result));}
struct Swr {
    SwrContext* ctx=nullptr;
    Swr(unsigned rate,unsigned channels){
        AVChannelLayout layout{};av_channel_layout_default(&layout,int(channels));
        const int result=swr_alloc_set_opts2(&ctx,&layout,AV_SAMPLE_FMT_FLT,48000,&layout,AV_SAMPLE_FMT_FLT,int(rate),0,nullptr);
        av_channel_layout_uninit(&layout);checked(result);checked(swr_init(ctx));
    }
    ~Swr(){swr_free(&ctx);}
};
// Uses the same correction/sanitization implementation as CaptureAudioSession.
// Reference explicitly holds FFmpeg history; never re-creates on zero crossing.
std::vector<float> render(unsigned rate,unsigned channels,double frequency,unsigned blockFrames,bool production,bool broken=false){
    Swr swr(rate,channels);CaptureRateCorrection correction;
    if(production)checked(correction.prepare(swr.ctx,true));else checked(swr_set_compensation(swr.ctx,0,0));
    const unsigned total=rate*2,period=rate/4;
    const std::array<int,8> deltas={-60,0,60,0,-12,0,12,0};
    std::vector<float> output;
    auto convert=[&](const float* pcm,unsigned frames){
        const int capacity=swr_get_out_samples(swr.ctx,int(frames));checked(capacity);
        std::vector<float> converted(size_t(capacity)*channels);
        uint8_t* dst[]={reinterpret_cast<uint8_t*>(converted.data())};
        const uint8_t* src[]={reinterpret_cast<const uint8_t*>(pcm)};
        const int count=swr_convert(swr.ctx,dst,capacity,pcm?src:nullptr,int(frames));checked(count);
        if(production)inspectCapturePcm(converted.data(),size_t(count)*channels);
        output.insert(output.end(),converted.begin(),converted.begin()+size_t(count)*channels);
        return count;
    };
    for(unsigned at=0;at<total;){
        if(at%period==0){
            const int delta=deltas[(at/period)%deltas.size()];
            if(broken&&!delta){swr_close(swr.ctx);checked(swr_init(swr.ctx));}
            if(production)checked(correction.set(swr.ctx,delta));
            else checked(swr_set_compensation(swr.ctx,delta,delta?48000:0));
        }
        const unsigned frames=std::min({blockFrames,total-at,period-at%period});
        std::vector<float> input(size_t(frames)*channels);
        for(unsigned i=0;i<frames;++i)for(unsigned c=0;c<channels;++c)
            input[size_t(i)*channels+c]=float(.5*std::sin(2*std::numbers::pi*frequency*(at+i)/rate+.71+c*.2));
        convert(input.data(),frames);at+=frames;
    }
    while(convert(nullptr,0)>0){}
    return output;
}
double difference(const std::vector<float>& a,const std::vector<float>& b){
    if(a.size()!=b.size())return 999;
    double error=0;for(size_t i=0;i<a.size();++i)error=std::max(error,std::abs(double(a[i])-b[i]));return error;
}
}
int main(int argc,char** argv){
    if(argc>1&&(std::string(argv[1])=="--endpoint"||std::string(argv[1])=="--endpoint-gap")){
        const bool gapTest=std::string(argv[1])=="--endpoint-gap";
        struct PausedLiveSource:AudioPcmSource {
            size_t remaining=240;
            bool padUnderruns()const override{return false;}
            size_t pull(float* out,size_t requested,double* pts)override{
                const auto count=std::min(remaining,requested);remaining-=count;*pts=0;
                std::fill(out,out+count*2,.25f);return count;
            }
        } source;
        AudioRenderer renderer;renderer.setGain(0);
        // The endpoint is paused after prefill: queued PCM provably cannot
        // have run out, even if this test is descheduled. No audible playback.
        bool ok=renderer.start({},20)&&renderer.startAnchored(source,!gapTest);
        double pts=0;
        if(gapTest){
            // Keep the source dry over multiple endpoint periods. Some drivers
            // freeze IAudioClock at the last submitted frame during starvation.
            for(unsigned i=0;ok&&i<8;++i){Sleep(10);ok=renderer.pumpOnce(source,&pts,false);}
            source.remaining=240;
            if(ok)ok=renderer.pumpOnce(source,&pts,false)&&renderer.underruns()>0&&renderer.recoveryFades()>0;
            std::cout<<(ok?"PASS ":"FAIL ")<<"REAL_WASAPI true starvation emptyPulls="<<renderer.emptyPulls()
                     <<" gaps="<<renderer.underruns()<<" fades="<<renderer.recoveryFades()<<" muted=true\n";
            renderer.shutdown();return ok?0:1;
        }
        if(ok)ok=renderer.pumpOnce(source,&pts,false)&&renderer.emptyPulls()==1&&
                 renderer.underruns()==0&&renderer.underrunFrames()==0&&renderer.recoveryFades()==0;
        source.remaining=240;
        if(ok)ok=renderer.pumpOnce(source,&pts,false)&&renderer.recoveryFades()==0;
        std::cout<<(ok?"PASS ":"FAIL ")<<"REAL_WASAPI queued empty pull emptyPulls="<<renderer.emptyPulls()
                 <<" gaps="<<renderer.underruns()<<" fades="<<renderer.recoveryFades()<<" paused=true muted=true\n";
        renderer.shutdown();return ok?0:1;
    }
    unsigned checks=0,failures=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok)++failures;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';};
    try{
        for(unsigned rate:{44100u,48000u})for(unsigned channels:{2u,6u})for(double hz:{1000.,4000.,8000.,16000.}){
            const auto reference=render(rate,channels,hz,480,false);
            const auto product=render(rate,channels,hz,480,true);
            const auto repartitioned=render(rate,channels,hz,127,true);
            const double error=difference(product,reference),blockError=difference(product,repartitioned);
            std::cout<<"rate="<<rate<<" channels="<<channels<<" hz="<<hz<<" frames="<<product.size()/channels<<" referenceError="<<error<<" blockError="<<blockError<<'\n';
            check(error<1e-6,"production correction matches continuous reference across positive/zero/negative ratios");
            check(blockError<1e-6,"changing callback partitions preserves waveform and frame count");
        }
        const auto reference=render(48000,2,4000,480,false);
        const auto broken=render(48000,2,4000,480,false,true);
        std::cout<<"referenceFrames="<<reference.size()/2<<" brokenResetFrames="<<broken.size()/2<<'\n';
        check(difference(reference,broken)>.1,"negative control: resetting FIR history fails waveform/frame test");
        std::vector<float> samples={1.05f,1.05f,-1.1f,.9f,.5f};const auto original=samples;
        const auto stats=inspectCapturePcm(samples.data(),samples.size());
        check(samples==original&&stats.overRange==3&&stats.nonFinite==0,"finite over-unity samples survive without clipping or AGC");
        float gain=0.5f;applyPcmGain(samples.data(),samples.size(),1,.5f,gain);
        check(std::abs(samples[0]-.525f)<1e-6,"user attenuation applies after preserving float headroom");
        samples={std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-.5f};
        const auto invalid=inspectCapturePcm(samples.data(),samples.size());
        check(invalid.nonFinite==2&&samples[0]==0&&samples[1]==0&&samples[2]==-.5f,"non-finite values sanitized without changing finite PCM");
        LiveAudioGapTracker gaps;
        auto observation=gaps.observe(480,480,960);
        check(!observation.frames&&!observation.began,"empty pull with queued PCM does not trigger recovery fade");
        observation=gaps.observe(0,960,960);
        check(!observation.frames&&!observation.began,"exact endpoint boundary is not evidence of a gap");
        observation=gaps.observe(0,1000,960);
        check(observation.frames==40&&observation.began,"actual device clock gap starts one event");
        observation=gaps.observe(0,1020,1000);
        check(observation.frames==20&&!observation.began,"ongoing gap counts only new frames, not writable capacity");
        gaps.submitted(480);observation=gaps.observe(0,1600,1500);
        check(observation.frames==100&&observation.began,"real PCM closes a gap before a new interruption");
        gaps.reset();observation=gaps.observe(480,0,480);
        check(!observation.frames,"reset clears previous gap state");
        check(!gaps.observeEmpty(480,0,0,10),"queued empty observation never starts a starvation timer");
        check(!gaps.observeEmpty(0,0,10,10)&&!gaps.observeEmpty(0,480,15,10),"short empty observation does not arm a fade");
        check(!gaps.observeEmpty(0,0,20,10)&&gaps.observeEmpty(0,0,31,10),"sustained empty endpoint detected even when device clock freezes");
        check(!gaps.observeEmpty(0,0,50,10),"frozen-clock starvation is counted once");
        Swr native(48000,2);CaptureRateCorrection manual;checked(manual.prepare(native.ctx,false));
        check(manual.set(native.ctx,60)<0,"unprepared live identity cannot hot-switch into compensation");
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
    std::cout<<"AUDIO_WAVEFORM checks="<<checks<<" failures="<<failures<<" (offline production DSP; no speaker claim)\n";
    return failures?1:0;
}
