#include <cassert>
#include <cmath>
#include <cstdio>
#include "alert_workflow.h"
using namespace alert;
static Workflow make() {
    Workflow w; w.config.emergency_call_enabled=true;
    for(unsigned i=0;i<4;++i) std::snprintf(w.config.numbers[i],32,"+1555123456%u",i);
    return w;
}
static unsigned drain(Workflow &w, Kind kind) {
    unsigned n=0; Action a;
    while(w.next(a)) { assert(a.kind==kind); assert(cellular::valid_message(a.message)); w.submitted(a.kind); ++n; }
    return n;
}
int main() {
    auto w=make(); Action a;
    w.tick(0,false,true,true); assert(w.state==State::Armed);
    w.tick(0,false,false,true); assert(w.state==State::Armed); // Unknown beacon isn't AWAY.
    w.tick(1000,true,false,true); assert(w.state==State::Active);
    assert(drain(w,Kind::Text)==1);
    for(uint64_t t=0;t<=177000;t+=3000) { w.sample(1000+t,80+float(t)/60000,true); w.tick(1000+t,true,false,true); }
    assert(w.forecast_valid && std::fabs(w.slope_f_per_min-1)<.001);
    assert(!w.next(a));
    w.sample(181000,83,true); w.tick(181000,true,false,true);
    assert(w.stage==2 && drain(w,Kind::Text)==4);
    assert(std::fabs(w.forecast_f-90)<.001);
    w.tick(300999,true,false,true); assert(!w.next(a));
    w.tick(301000,true,false,true); assert(w.next(a)&&a.kind==Kind::Horn);
    w.submitted(a.kind); assert(drain(w,Kind::Text)==4);
    w.tick(600000,true,false,false); assert(w.state==State::Active); // Sensor loss alone cannot cancel.
    w.tick(600001,false,true,false); w.tick(605002,false,true,false); assert(w.state==State::Suppressed);
    assert(!w.next(a)); w.tick(610003,false,true,false); w.tick(615004,false,true,false); assert(w.state==State::Armed);

    w=make(); w.tick(1000,true,false,true); drain(w,Kind::Text);
    for(uint64_t t=0;t<=180000;t+=3000) w.sample(1000+t,90+2*float(t)/60000,true);
    w.tick(181000,true,false,true);
    assert(std::fabs(w.forecast_f-110)<.001);
    assert(w.next(a)&&a.kind==Kind::Call&&!std::strcmp(a.number,"+15129622126"));
    w.submitted(a.kind); assert(w.call_attempted); drain(w,Kind::Text);
    for(uint64_t t=183000;t<240000;t+=3000) {w.sample(1000+t,110,true);w.tick(1000+t,true,false,true);assert(!w.next(a));}
    w.cancel(); assert(w.state==State::Suppressed&&!w.next(a));
    w.tick(400000,true,false,true); assert(w.state==State::Suppressed);

    w=make(); w.tick(0,true,false,true); drain(w,Kind::Text);
    for(uint64_t t=0;t<=180000;t+=3000) w.sample(t,83+float(t)/60000*2,true);
    w.tick(180000,true,false,true); assert(std::fabs(w.forecast_f-103)<.001); assert(drain(w,Kind::Text)==4);
    w.sample(183000,NAN,false); w.tick(183000,true,false,true); assert(!w.forecast_valid&&!w.next(a));
    w.sample(186000,INFINITY,true); assert(!w.forecast_valid);
    w.sample(200000,90,true); w.tick(211000,true,false,true); assert(!w.forecast_valid);
    w=make(); w.config.emergency_call_enabled=false;w.tick(0,true,false,true);drain(w,Kind::Text);
    for(uint64_t t=0;t<=180000;t+=3000) w.sample(t,100+float(t)/60000,true);
    w.tick(180000,true,false,true);assert(drain(w,Kind::Text)==4);assert(!w.call_attempted);
    w=make();w.tick(0,true,false,true);drain(w,Kind::Text);
    for(uint64_t t=0;t<=180000;t+=3000) w.sample(t,84+float(t)/60000*2,true);
    w.tick(180000,true,false,true);assert(w.forecast_f==104.0f);assert(drain(w,Kind::Text)==4);
    assert(stop_text(" STOP\r\n")&&stop_text("sToP")&&!stop_text("stop now")&&!stop_text(nullptr)&&!stop_text("st"));
    assert(cellular::valid_call_number("933")&&!cellular::valid_number("933"));
    puts("PASS: alert timeline, beacon gating, rolling device regression, forecast threshold, one call, cancel/suppression/rearm");
}
