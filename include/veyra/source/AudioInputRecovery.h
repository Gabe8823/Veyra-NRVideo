#pragma once
#include <algorithm>
#include <cstdint>
namespace veyra::source {
// Observe PCM packet progress, including silence packets. Never treat quiet
// audio as a lost input. Times are monotonic milliseconds on the owner thread.
class AudioInputRecovery {
    uint64_t blocks_=0,progressAt_=0,retryAt_=0;unsigned attempts_=0;
public:
    void reset(uint64_t now){blocks_=0;progressAt_=now;retryAt_=0;attempts_=0;}
    bool due(uint64_t blocks,uint64_t now){
        if(blocks!=blocks_){blocks_=blocks;progressAt_=now;attempts_=0;retryAt_=0;return false;}
        if(now<progressAt_||now-progressAt_<3000||now<retryAt_)return false;
        attempts_=std::min(attempts_+1,5u);retryAt_=now+attempts_*1000;return true;
    }
};
}
