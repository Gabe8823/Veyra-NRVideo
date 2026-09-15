#include "veyra/sink/CaptureAudioSession.h"
#include "veyra/sink/AudioFormat.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string_view>
int main(int argc,char** argv){
    using namespace veyra::sink;using Clock=std::chrono::steady_clock;
    struct Pacer {
        HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        ~Pacer(){if(timer)CloseHandle(timer);}
        void until(Clock::time_point due){
            const auto ticks=std::chrono::duration_cast<std::chrono::nanoseconds>(due-Clock::now()).count()/100;
            if(ticks<=0)return;
            LARGE_INTEGER when{};when.QuadPart=-ticks;
            if(timer&&SetWaitableTimer(timer,&when,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,100);
            else std::this_thread::sleep_until(due);
        }
    } pacer;
    const bool endpointTest=argc>=2&&std::string_view(argv[1])=="--endpoint-loss";
    const bool jitterTest=argc>=2&&std::string_view(argv[1])=="--jitter";
    const bool transientTest=argc>=2&&std::string_view(argv[1])=="--transient";
    const bool compensatedTransientTest=argc>=2&&std::string_view(argv[1])=="--transient-comp";
    const bool fastDrift=argc>=2&&std::string_view(argv[1])=="--drift-fast";
    const bool driftTest=fastDrift||(argc>=2&&std::string_view(argv[1])=="--drift-slow");
    if(endpointTest)SetEnvironmentVariableW(L"VEYRA_TEST_CAPTURE_AUDIO_ENDPOINT_LOSS",L"1");
    const bool multichannel=argc>=3&&std::string_view(argv[2])=="--5.1";
    CaptureAudioSession audio;WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=2;f.nSamplesPerSec=transientTest?44100:48000;f.wBitsPerSample=16;f.nBlockAlign=4;f.nAvgBytesPerSec=f.nSamplesPerSec*f.nBlockAlign;
    auto extended=floatWave({6,0x60f});extended.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;extended.Format.wBitsPerSample=16;extended.Samples.wValidBitsPerSample=16;extended.Format.nBlockAlign=12;extended.Format.nAvgBytesPerSec=576000;
    if(!(multichannel?audio.configure(extended.Format,sizeof(extended)):audio.configure(f))||!audio.start())return 2;audio.setGain(0);
    const unsigned channels=multichannel?6:2;std::vector<int16_t> pcm(480*channels);
    for(size_t i=0;i<pcm.size()/channels;++i)for(unsigned c=0;c<channels;++c)pcm[i*channels+c]=int16_t(2000*std::sin((i*channels+c)*0.031));
    const auto start=Clock::now();
    if(transientTest){
        const size_t frames=441;std::vector<int16_t> block(frames*2);
        for(unsigned blockIndex=0;blockIndex<30;++blockIndex){
            pacer.until(start+std::chrono::milliseconds(blockIndex*10));
            std::fill(block.begin(),block.end(),blockIndex>=10&&blockIndex<20?int16_t(32767):int16_t(0));
            if(!audio.push(block.data(),block.size()*sizeof(int16_t),blockIndex*10.0,blockIndex==0))return 10;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto state=audio.snapshot();audio.stop();
        const bool pass=state.inputSampleRate==44100&&state.inputBlocks==30&&state.nonFiniteSamples==0&&state.clippedSamples==0&&state.overRangeSamples>0&&state.inputPeak>1.0;
        std::cout<<(pass?"PASS ":"FAIL ")<<"44.1k sharp transient conversion clipped="<<state.clippedSamples<<" peakProtected="<<state.peakProtectedSamples<<" nonFinite="<<state.nonFiniteSamples<<" peak="<<state.inputPeak<<" blocks="<<state.inputBlocks<<'\n';
        return pass?0:1;
    }
    if(compensatedTransientTest){
        const size_t frames=480;std::vector<int16_t> block(frames*2);
        for(unsigned blockIndex=0;blockIndex<500;++blockIndex){
            pacer.until(start+std::chrono::milliseconds(blockIndex*10));
            std::fill(block.begin(),block.end(),blockIndex>=300&&blockIndex<310?int16_t(32767):int16_t(0));
            if(!audio.push(block.data(),block.size()*sizeof(int16_t),blockIndex*10.0,blockIndex==0))return 11;
            const auto host=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()/100;
            audio.videoPresented(blockIndex*10.0-80,host);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto state=audio.snapshot();audio.stop();
        const bool pass=state.inputSampleRate==48000&&state.inputBlocks==500&&state.nonFiniteSamples==0&&state.clippedSamples==0&&state.overRangeSamples>0&&state.inputPeak>1.0;
        std::cout<<(pass?"PASS ":"FAIL ")<<"48k compensated sharp transient conversion clipped="<<state.clippedSamples<<" peakProtected="<<state.peakProtectedSamples<<" nonFinite="<<state.nonFiniteSamples<<" peak="<<state.inputPeak<<" correctionPpm="<<state.driftCorrectionPpm<<" blocks="<<state.inputBlocks<<'\n';
        return pass?0:1;
    }
    if(jitterTest){
        uint64_t settledResets=0,settledUnderruns=0;unsigned missing=0;std::vector<double> errors;
        for(unsigned i=0;i<500;++i){
            pacer.until(start+std::chrono::milliseconds(i*10));
            if(!audio.push(pcm.data(),pcm.size()*2,i*10.0,i==0))return 9;
            const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()/100;
            // Callback every 10ms; video updates at ~30fps with 30/35ms
            // software delay. Both share the original input PTS time base.
            if(i>=4&&i%3==0)audio.videoPresented(i*10.0-((i/3)%2?30:35),now);
            const auto s=audio.snapshot();
            if(i==100){settledResets=s.resets;settledUnderruns=s.underruns;}
            if(i>100){if(s.running&&s.skewMs)errors.push_back(std::abs(*s.skewMs));else ++missing;}
        }
        const auto s=audio.snapshot();audio.stop();std::sort(errors.begin(),errors.end());
        const double p95=errors.empty()?999:errors[(errors.size()*95+99)/100-1];
        const bool inputTelemetry=s.inputBlocks==500&&std::abs(s.inputBlockMs-10)<.001&&s.inputIntervalMs>=0&&s.inputIntervalMs<100&&s.inputPeak>.01&&s.nonFiniteSamples==0&&s.clippedSamples==0;
        const bool pass=inputTelemetry&&missing==0&&s.resets==settledResets&&s.underruns==settledUnderruns&&s.overflows==0&&p95<35;
        std::cout<<(pass?"PASS ":"FAIL ")<<"CAPTURE_JITTER additionalResets="<<s.resets-settledResets<<" additionalUnderruns="<<s.underruns-settledUnderruns<<" missing="<<missing<<" p95SkewMs="<<p95<<" inputBlocks="<<s.inputBlocks<<" inputBlockMs="<<s.inputBlockMs<<" inputIntervalMs="<<s.inputIntervalMs<<" peak="<<s.inputPeak<<" nonFinite="<<s.nonFiniteSamples<<" clipped="<<s.clippedSamples<<'\n';
        return pass?0:1;
    }
    if(driftTest){
        std::vector<double> errors;unsigned missing=0;
        const double speed=fastDrift?1.001:.999;
        for(unsigned i=0;i<11980;++i){
            pacer.until(start+std::chrono::microseconds(int64_t(i*10000/speed)));
            const auto host=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()/100;
            if(!audio.push(pcm.data(),pcm.size()*2,i*10.0,i==0))return 6;
            if(i>=8)audio.videoPresented(i*10.0-80,host);
            const auto s=audio.snapshot();if(i>1000){if(s.running&&s.skewMs)errors.push_back(std::abs(*s.skewMs));else ++missing;}
            if(i&&i%2000==0)std::cout<<"DRIFT seconds="<<i/100<<" skewMs="<<s.skewMs.value_or(-999)<<" resets="<<s.resets<<" queueMs="<<s.bufferedMs<<" correctionPpm="<<s.driftCorrectionPpm<<std::endl;
        }
        const auto state=audio.snapshot();audio.stop();std::sort(errors.begin(),errors.end());
        const double p95=errors.empty()?999:errors[(errors.size()*95+99)/100-1];
        const bool pass=p95<=30&&missing<100&&state.resets==1&&state.overflows==0&&state.bufferHighWaterMs<=500;
        std::cout<<(pass?"PASS ":"FAIL ")<<"120s capture clock speed="<<speed<<" p95SkewMs="<<p95<<" missing="<<missing<<" resets="<<state.resets<<" highWaterMs="<<state.bufferHighWaterMs<<'\n';return pass?0:1;
    }
    double sum=0;unsigned count=0;bool bounded=true,sawReconnecting=false;
    for(unsigned i=0;i<(endpointTest?350u:160u);++i){
        const auto due=start+std::chrono::milliseconds(i*10);pacer.until(due);
        const auto time=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()/100;
        if(!audio.push(pcm.data(),pcm.size()*2,i*10.0,i==0))return 3;
        if(i>=8)audio.videoPresented(i*10.0-80,time);
        const auto s=audio.snapshot();bounded&=s.bufferedMs<=520;
        sawReconnecting|=!s.error.empty();
        if(i>(endpointTest?240u:70u)&&s.running&&s.skewMs){sum+=std::abs(*s.skewMs);++count;}
    }
    const auto state=audio.snapshot();
    std::cout<<"capture_audio meanAbsSkewMs="<<(count?sum/count:-1)<<" samples="<<count<<" compensationMs="<<state.compensationMs<<" queueMs="<<state.bufferedMs<<" resets="<<state.resets<<" overflows="<<state.overflows<<" underruns="<<state.underruns<<'\n';
    bool ok=count>=60&&sum/count<25&&state.compensationMs>=65&&state.compensationMs<=95&&bounded&&state.overflows==0;
    if(endpointTest){ok=ok&&sawReconnecting&&state.error.empty()&&state.endpointRetries>=2&&state.running;audio.stop();std::cout<<(ok?"PASS ":"FAIL ")<<"owned WASAPI endpoint recovered without reopening capture retries="<<state.endpointRetries<<'\n';return ok?0:1;}
    std::cout<<(ok?"PASS":"FAIL")<<" synthetic capture PTS with 80ms video delay; real WASAPI, no physical capture\n";
    unsigned sequence=160;
    auto phase=[&](unsigned mode,int offset,double delay,double expectedComp,double expectedSkew,double commonInputMs=0,double maxSkewError=28){
        audio.setSync(mode,offset);double error=0;unsigned samples=0;bool boundedPhase=true;
        const unsigned steps=delay>500?300:150;
        for(unsigned j=0;j<steps;++j,++sequence){
            pacer.until(start+std::chrono::milliseconds(sequence*10));
            const auto host=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()/100;
            if(!audio.push(pcm.data(),pcm.size()*2,sequence*10.0-commonInputMs,j==0&&commonInputMs!=0))return false;
            audio.videoPresented(sequence*10.0-commonInputMs-delay,host);const auto s=audio.snapshot();
            boundedPhase&=s.bufferedMs<=2000;
            if(j>steps-80&&s.running&&s.skewMs){error+=std::abs(*s.skewMs-expectedSkew);++samples;}
        }
        const auto s=audio.snapshot();const bool pass=samples>=60&&error/samples<maxSkewError&&std::abs(s.compensationMs-expectedComp)<15&&boundedPhase&&s.overflows==0;
        std::cout<<(pass?"PASS ":"FAIL ")<<"capture phase mode="<<mode<<" delay="<<delay<<" commonInputMs="<<commonInputMs<<" compensation="<<s.compensationMs<<" meanSkewError="<<(samples?error/samples:-1)<<" resets="<<s.resets<<" queueMs="<<s.bufferedMs<<" endpointMs="<<s.endpointBufferedMs<<" underruns="<<s.underruns<<" correctionPpm="<<s.driftCorrectionPpm<<'\n';return pass;
    };
    ok=phase(0,0,160,160,0)&&ok;
    ok=phase(0,0,400,400,0)&&ok;
    ok=phase(0,0,400,400,0,900)&&ok;
    ok=phase(0,0,900,900,0,900)&&ok;
    // Start a fresh ingress epoch after the shared-input offset fixture.
    audio.stop();if(!audio.start())return 7;
    ok=phase(0,0,80,80,0)&&ok;
    const auto beforeGraphReset=audio.snapshot();
    audio.videoReset(false);
    for(unsigned i=0;i<35;++i,++sequence){
        pacer.until(start+std::chrono::milliseconds(sequence*10));
        // This models a video graph rebuild only.  The audio callback remains
        // on the same DirectShow epoch; a true audio discontinuity is tested
        // separately through the normal reset/re-anchor paths.
        if(!audio.push(pcm.data(),pcm.size()*2,sequence*10.0,false))return 8;
    }
    const auto afterGraphReset=audio.snapshot();
    const bool heldForGraph=afterGraphReset.running&&afterGraphReset.resets==beforeGraphReset.resets;
    std::cout<<(heldForGraph?"PASS ":"FAIL ")<<"video graph reset preserves running audio clock\n";ok=heldForGraph&&ok;
    ok=phase(0,0,80,80,0)&&ok;
    ok=phase(1,100,80,100,-20)&&ok;
    // With the shared endpoint's 20 ms safety window and no requested
    // compensation, the observed software skew has a wider scheduling
    // envelope than the auto/manual-delay phases. This is timing tolerance,
    // not permission to insert or drop PCM.
    ok=phase(2,0,80,0,80,0,45)&&ok;
    ok=phase(1,-100,80,0,80,0,45)&&ok;
    // Keep the SAME sync mode before and after the stall. Otherwise the
    // explicit mode-switch reset can falsely satisfy this recovery assertion.
    ok=phase(0,0,80,80,0)&&ok;
    const auto beforeStall=audio.snapshot().resets;
    std::this_thread::sleep_for(std::chrono::milliseconds(350));sequence+=35;
    const bool reanchoredWhileDry=audio.snapshot().resets>beforeStall;
    std::cout<<(reanchoredWhileDry?"PASS ":"FAIL ")<<"persistent dry input recovers without a sync setting change\n";ok=reanchoredWhileDry&&ok;
    ok=phase(0,0,80,80,0)&&ok;
    const bool recovered=audio.snapshot().resets>beforeStall;
    std::cout<<(recovered?"PASS ":"FAIL ")<<"audio reanchors real PCM after ingress stall\n";ok=recovered&&ok;
    audio.stop();
    const auto stopped=audio.snapshot();const bool stoppedClean=!stopped.available&&!stopped.running&&!stopped.skewMs&&stopped.bufferedMs==0;
    std::cout<<(stoppedClean?"PASS ":"FAIL ")<<"stopped audio has no stale clock or queued state\n";ok=stoppedClean&&ok;
    if(!audio.start())return 4;
    ok=phase(0,0,80,80,0)&&ok;
    audio.stop();
    if(!audio.start())return 5;
    audio.setSync(0,0);
    bool burstAccepted=true;
    for(unsigned i=0;i<600;++i)burstAccepted=audio.push(pcm.data(),pcm.size()*2,i*10.0,i==0)&&burstAccepted;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto burst=audio.snapshot();
    const bool burstBound=burstAccepted&&burst.bufferHighWaterMs<=2000&&burst.overflows>0;
    std::cout<<(burstBound?"PASS ":"FAIL ")<<"combined raw/converting/PCM burst budget highWaterMs="<<burst.bufferHighWaterMs<<" overflows="<<burst.overflows<<'\n';ok=burstBound&&ok;
    audio.stop();
    return ok?0:1;
}
