#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace veyra::sink {
// Capture callbacks and presentation use the same host clock. Cross-stream
// PTS may include a small hardware offset, but are not trusted without bounds.
class CaptureSyncTarget {
public:
    struct Result {double targetMs=0,rawMs=0,localMs=0;bool fallback=false,limited=false;};
    void reset(){*this={};}
    void invalidate(){count_=cursor_=0;recoverySince_.reset();}
    Result observe(double rawMs,double hostMs,std::optional<double> arrivalMs){
        if(!arrivalMs){reset();return {std::isfinite(rawMs)?rawMs:0,rawMs,0,false};}
        const bool valid=std::isfinite(hostMs)&&std::isfinite(*arrivalMs)&&*arrivalMs>0&&*arrivalMs<=hostMs;
        const double local=valid?hostMs-*arrivalMs:lastLocal_;
        // 80 ms is a confidence bound, not measured hardware latency. Larger
        // upstream A/V offsets require manual calibration, not blind waiting.
        const double residual=rawMs-local;
        const bool inconsistent=!valid||!std::isfinite(rawMs)||std::abs(residual)>80;
        if(inconsistent){fallback_=true;recoverySince_.reset();}
        else if(fallback_){
            if(std::abs(residual)<=40){
                if(!recoverySince_)recoverySince_=hostMs;
                if(hostMs-*recoverySince_>=2000){fallback_=false;recoverySince_.reset();}
            }else recoverySince_.reset();
        }
        if(valid)lastLocal_=local;
        const double requested=fallback_?local:rawMs;
        const double candidate=std::clamp(requested,0.0,1500.0);
        // Filter actual video observations, never repeated audio-thread polls.
        samples_[cursor_]=candidate;cursor_=(cursor_+1)%samples_.size();
        count_=std::min(count_+1,samples_.size());
        auto sorted=samples_;std::sort(sorted.begin(),sorted.begin()+count_);
        return {sorted[count_/2],rawMs,valid?local:-1,fallback_,requested<0||requested>1500};
    }
private:
    std::array<double,5> samples_{};
    size_t count_=0,cursor_=0;
    double lastLocal_=0;
    bool fallback_=false;
    std::optional<double> recoverySince_;
};
}
