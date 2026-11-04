#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "cellular_protocol.h"

namespace alert {
enum class State { Armed, Active, Suppressed };
enum class Kind { Text, Call, Horn };
struct Action { Kind kind; char number[32] = {}; char message[161] = {}; };
struct Config {
    char numbers[4][32] = {}; // Driver, then three emergency contacts.
    bool emergency_call_enabled = false;
};
// Fixed-size, allocation-free controller. All times are monotonic milliseconds.
class Workflow {
public:
    State state = State::Armed;
    Config config;
    uint64_t started = 0;
    unsigned stage = 0;
    bool call_attempted = false, forecast_valid = false;
    float slope_f_per_min = 0, forecast_f = 0;
    uint64_t elapsed(uint64_t now) const { return state == State::Active ? now - started : 0; }
    void restore_suppressed() { state = State::Suppressed; }
    void cancel() {
        state = State::Suppressed; queued = 0; forecast_valid = false;
        call_pending = horn_pending = false; clear_since = 0;
    }
    bool tick(uint64_t now, bool away, bool near, bool presence) {
        if (state == State::Suppressed) {
            if (!near || presence) clear_since = 0;
            else if (!clear_since) clear_since = now;
            else if (now - clear_since >= 5000) { state = State::Armed; return true; }
            return false;
        }
        if (state == State::Armed && away && presence) {
            state = State::Active; started = now; stage = 1;
            count = queued = 0; call_attempted = call_pending = horn_pending = false;
            forecast_valid = false; clear_since = 0; enqueue_texts(1,now);
        }
        if (state != State::Active) return false;
        if (near && !presence) {
            if (!clear_since) clear_since = now;
            else if (now - clear_since >= 5000) cancel();
        } else clear_since = 0;
        if (state != State::Active) return false;
        if (elapsed(now) >= 180000 && stage < 2) { stage = 2; enqueue_texts(2,now); }
        if (elapsed(now) >= 300000 && stage < 3) { stage = 3; enqueue_texts(3,now); horn_pending = true; }
        // A stale/failed sensor sample cannot authorize a forecast call.
        if (forecast_valid && now - newest > 10000) forecast_valid = false;
        if (elapsed(now) >= 180000 && forecast_valid && forecast_f > 104.0f &&
            config.emergency_call_enabled && !call_attempted) call_pending = true;
        return false;
    }
    void sample(uint64_t now, float f, bool valid) {
        if (state != State::Active) return;
        forecast_valid = false;
        if (!valid || !std::isfinite(f) || f < -40 || f > 176) return;
        size_t kept = 0;
        for (size_t i = 0; i < count; ++i)
            if (now - samples[i].time <= 180000) samples[kept++] = samples[i];
        count = kept;
        if (count == 64) { for (size_t i = 1; i < count; ++i) samples[i-1] = samples[i]; --count; }
        samples[count++] = {now, f}; newest = now;
        if (count < 3 || now - samples[0].time < 30000) return;
        double sx=0, sy=0, sxx=0, sxy=0;
        for (size_t i=0; i<count; ++i) {
            const double x = -static_cast<double>(now - samples[i].time) / 60000.0;
            const double y = samples[i].f;
            sx+=x; sy+=y; sxx+=x*x; sxy+=x*y;
        }
        const double denominator=count*sxx-sx*sx;
        if (denominator <= 1e-9) return;
        slope_f_per_min=static_cast<float>((count*sxy-sx*sy)/denominator);
        forecast_f=f+7.0f*slope_f_per_min;
        forecast_valid=std::isfinite(forecast_f) && std::isfinite(slope_f_per_min);
    }
    bool next(Action &out) {
        if (state != State::Active) return false;
        // Call has priority over texts waiting for the modem. Horn start is brief.
        if (horn_pending) { out = {}; out.kind=Kind::Horn; return true; }
        if (call_pending && !call_attempted) {
            out={}; out.kind=Kind::Call; std::strcpy(out.number,"933"); return true;
        }
        if (!queued) return false;
        out=queue[0]; return true;
    }
    void submitted(Kind kind) {
        if (kind==Kind::Horn) horn_pending=false;
        else if (kind==Kind::Call) { call_attempted=true; call_pending=false; }
        else if (queued) { for(size_t i=1;i<queued;++i) queue[i-1]=queue[i]; --queued; }
    }
private:
    struct Sample { uint64_t time; float f; } samples[64] = {};
    size_t count=0, queued=0;
    Action queue[9] = {};
    uint64_t newest=0, clear_since=0;
    bool call_pending=false, horn_pending=false;
    void enqueue_texts(unsigned level,uint64_t now) {
        for (unsigned i=0;i<(level==1?1u:4u);++i) {
            if (!cellular::valid_number(config.numbers[i])) continue;
            bool duplicate=false;
            for(unsigned j=0;j<i;++j) duplicate |= !std::strcmp(config.numbers[i],config.numbers[j]);
            if(duplicate || queued==9) continue;
            auto &a=queue[queued++]; a={}; a.kind=Kind::Text;
            std::strcpy(a.number,config.numbers[i]);
            const char *message=level==1 ?
                "Backseat Minder: Presence detected; driver away. Check vehicle now." :
                level==2 ? "Backseat Minder: Presence alert active for 3 min. Check vehicle immediately." :
                "Backseat Minder: Presence alert active for 5 min. Horn alarm requested.";
            if(count && now-newest<=10000)
                std::snprintf(a.message,sizeof(a.message),"%s Last temp: %.1f F. Reply STOP to cancel.",message,samples[count-1].f);
            else std::snprintf(a.message,sizeof(a.message),"%s Reply STOP to cancel.",message);
        }
    }
};
inline bool stop_text(const char *text) {
    if(!text) return false;
    while(*text==' '||*text=='\r'||*text=='\n'||*text=='\t') ++text;
    for(const char *p="stop";*p;++p,++text) {
        char c=*text; if(c>='A'&&c<='Z') c+=32;
        if(c!=*p) return false;
    }
    while(*text==' '||*text=='\r'||*text=='\n'||*text=='\t') ++text;
    return !*text;
}
} // namespace alert
