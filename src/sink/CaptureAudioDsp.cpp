#include "veyra/sink/CaptureAudioDsp.h"
extern "C" {
#include <libswresample/swresample.h>
}
namespace veyra::sink {
int CaptureRateCorrection::prepare(SwrContext* context,bool automatic){
    if(!automatic&&!prepared_)return 0;
    const int result=swr_set_compensation(context,0,0);
    if(result>=0)prepared_=true;
    return result;
}
int CaptureRateCorrection::set(SwrContext* context,int delta,int distance){
    // A newly enabled auto mode must prepare before playback, not hot-switch
    // from identity halfway through a continuous PCM stream.
    if(!prepared_)return delta?AVERROR(EINVAL):0;
    return swr_set_compensation(context,delta,delta?distance:0);
}
}
