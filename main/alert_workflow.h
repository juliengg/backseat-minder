#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "cellular_protocol.h"

namespace alert {
constexpr float FORECAST_THRESHOLD_F = 104.0f;
constexpr char EMERGENCY_SERVICES_NUMBER[] = "+13464421981";
enum class State { NotArmed, Armed, Active, Suppressed };
enum class Kind { Text, Call, Horn };
struct Action { Kind kind; char number[32] = {}; char message[161] = {}; };
struct Checkpoint {
    uint32_t version = 1;
    uint32_t mode = 0; // 0: ordinary boot, 1: active incident, 2: explicit STOP.
    uint64_t elapsed_ms = 0;
    uint32_t stage = 0, call_attempted = 0, queued = 0;
    Action pending[9] = {};
};
struct Config {
    char numbers[4][32] = {}; // Driver, then three emergency contacts.
    bool emergency_call_enabled = false;
};
// Fixed-size, allocation-free controller. All times are monotonic milliseconds.
class Workflow {
public:
    // Ordinary boots wait for the driver; a persisted active incident overrides this.
    State state = State::Suppressed;
    Config config;
    uint64_t started = 0;
    unsigned stage = 0;
    bool call_attempted = false, forecast_valid = false;
    float slope_f_per_min = 0, forecast_f = 0;
    uint64_t elapsed(uint64_t now) const { return state == State::Active ? restored_elapsed + now - started : 0; }
    Checkpoint checkpoint(uint64_t now, bool stopped) const {
        Checkpoint saved;
        saved.mode = state == State::Active ? 1 : stopped ? 2 : 0;
        if (saved.mode == 1) {
            saved.elapsed_ms = elapsed(now); saved.stage = stage;
            saved.call_attempted = call_attempted; saved.queued = queued;
            for (size_t i=0;i<queued;++i) saved.pending[i] = queue[i];
        }
        return saved;
    }
    bool restore(const Checkpoint &saved, uint64_t now) {
        if (saved.version != 1 || saved.mode > 2) return false;
        if (saved.mode != 1) { cancel(); return true; }
        if (saved.stage < 1 || saved.stage > 3 || saved.call_attempted > 1 || saved.queued > 9)
            return false;
        for (size_t i=0;i<saved.queued;++i) {
            const auto &a=saved.pending[i];
            if (a.kind!=Kind::Text || !memchr(a.number,0,sizeof(a.number)) ||
                !memchr(a.message,0,sizeof(a.message))) return false;
        }
        cancel(); state=State::Active; started=now; restored_elapsed=saved.elapsed_ms;
        stage=saved.stage; call_attempted=saved.call_attempted; queued=saved.queued;
        count=0; // Temperature forecasts require fresh post-boot measurements.
        for (size_t i=0;i<queued;++i) queue[i]=saved.pending[i];
        horn_pending=stage>=3; // Reassert the idempotent alarm command after reboot.
        return true;
    }
    void restore_suppressed() { state = State::Suppressed; }
    void cancel() {
        state = State::Suppressed; queued = 0; forecast_valid = false;
        call_pending = horn_pending = false; clear_since = 0;
        return_tracking = false;
    }
    void rearm(bool presence) {
        cancel();
        state = presence ? State::Armed : State::NotArmed;
    }
    const char *state_name() const {
        switch (state) {
        case State::NotArmed: return "not_armed";
        case State::Armed: return "armed";
        case State::Active: return "active";
        default: return "suppressed";
        }
    }
    bool tick(uint64_t now, bool away, bool near, bool presence) {
        if (state == State::Suppressed) {
            if (near) { rearm(presence); return true; }
            return false;
        }
        // Once a driver has been confirmed, occupancy determines readiness.
        // An active incident keeps running through brief sensor dropouts.
        if (state != State::Active)
            state = presence ? State::Armed : State::NotArmed;
        if (state == State::Armed && away && presence) {
            state = State::Active; started = now; restored_elapsed = 0; stage = 1;
            count = queued = 0; call_attempted = call_pending = horn_pending = false;
            forecast_valid = false; clear_since = 0; return_tracking = false; enqueue_texts(1,now);
        }
        if (state != State::Active) return false;
        // Qualify continuous driver return independently of passenger presence.
        if (!near) return_tracking = false;
        else if (!return_tracking) { return_tracking = true; return_since = now; }
        // Invalidate stale forecasts before either cancellation or call decisions.
        if (forecast_valid && now - newest > 10000) forecast_valid = false;
        if (return_tracking && now - return_since >= 15000 &&
            forecast_valid && forecast_f < FORECAST_THRESHOLD_F) {
            rearm(presence);
            return true;
        }
        if (near && !presence) {
            if (!clear_since) clear_since = now;
            else if (now - clear_since >= 5000) rearm(false);
        } else clear_since = 0;
        if (state != State::Active) return false;
        if (elapsed(now) >= 180000 && stage < 2) { stage = 2; enqueue_texts(2,now); }
        if (elapsed(now) >= 300000 && stage < 3) { stage = 3; enqueue_texts(3,now); horn_pending = true; }
        if (elapsed(now) >= 180000 && forecast_valid && forecast_f > FORECAST_THRESHOLD_F &&
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
            out={}; out.kind=Kind::Call; std::strcpy(out.number,EMERGENCY_SERVICES_NUMBER); return true;
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
    uint64_t restored_elapsed=0;
    uint64_t return_since=0;
    bool return_tracking=false;
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
