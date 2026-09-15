#pragma once
#include "veyra/pipeline/FramePacket.h"
#include <algorithm>
#include <cmath>
namespace veyra::pipeline {
struct HdrToneMapPeak {float nits;const char* source;};
inline HdrToneMapPeak hdrToneMapPeak(const ColorDescription& c){
    // HLG's existing reference OOTF defines a 1000-nit reference display.
    if(c.transfer==TransferFunction::HLG)return {1000,"HLG-reference"};
    const auto valid=[](float n){return std::isfinite(n)&&n>0&&n<=10000;};
    if(valid(c.hdrMaxCllNits)&&(!valid(c.hdrMaxFallNits)||c.hdrMaxFallNits<=c.hdrMaxCllNits))
        return {std::max(203.0f,c.hdrMaxCllNits),"MaxCLL"};
    // Mastering display capability is only an explicit fallback, not a
    // measurement of the content. Never amplify a dim source up to the target.
    if(valid(c.hdrMasteringPeakNits))return {std::max(203.0f,c.hdrMasteringPeakNits),"mastering-fallback"};
    return {1000,"assumed-fallback"};
}
}
