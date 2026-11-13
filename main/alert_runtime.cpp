#include "alert_runtime.h"
#include "setup_mode.h"
#include "cellular_link.h"
#include "cellular_diagnostics.h"
#include "esp_timer.h"
#include "nvs.h"
#include <atomic>
#include <cstring>

namespace {
alert::Workflow workflow;
std::atomic<bool> stop_requested{false};
bool persistence_ok = true;
uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time()/1000); }
bool latch(bool value) {
    nvs_handle_t h;
    if(nvs_open("bsm_alert",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t err=nvs_set_u8(h,"latched",value?1:0);
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h); return err==ESP_OK;
}
void incoming(const char *sender, const char *text) {
    if(!alert::stop_text(text)) return;
    const char *digits=sender[0]=='+' ? sender+1 : sender;
    for(unsigned i=0;i<4;++i) {
        char saved[32];
        if(!setup_mode_get_contact(i,saved,sizeof(saved))) continue;
        const char *expected=saved[0]=='+' ? saved+1 : saved;
        if(!std::strcmp(expected,digits)) {
            stop_requested.store(true); cellular_link_cancel_alert(); return;
        }
    }
    cellular_diagnostics_record("STOP_UNAUTHORIZED");
}
void refresh_config() {
    for(unsigned i=0;i<4;++i)
        setup_mode_get_contact(i,workflow.config.numbers[i],sizeof(workflow.config.numbers[i]));
    workflow.config.emergency_call_enabled=setup_mode_emergency_alerts_enabled();
}
}
void alert_runtime_init() {
    refresh_config();
    nvs_handle_t h;
    if(nvs_open("bsm_alert",NVS_READONLY,&h)==ESP_OK) {
        uint8_t value=0; nvs_get_u8(h,"latched",&value); nvs_close(h);
        // An interrupted incident cannot redial after a reset. Rearm on return/clear.
        if(value) workflow.restore_suppressed();
    }
    cellular_link_set_sms_handler(incoming);
}
void alert_runtime_cancel() {
    workflow.cancel();
    persistence_ok = latch(true);
    cellular_link_cancel_alert();
    cellular_diagnostics_record("ALERT_CANCELLED");
}
void alert_runtime_poll(bool presence,const BeaconStatus &driver) {
    if(stop_requested.exchange(false)) alert_runtime_cancel();
    const auto before=workflow.state;
    const bool near=driver.available && !driver.checking && driver.driver_present;
    const bool away=driver.available && !driver.checking && !driver.driver_present;
    workflow.tick(now_ms(),away,near,presence);
    if(before==alert::State::Armed && workflow.state==alert::State::Active) {
        persistence_ok=latch(true); cellular_diagnostics_record("ALERT_STARTED");
    }
    if(before==alert::State::Active && workflow.state!=alert::State::Active) {
        persistence_ok=latch(workflow.state==alert::State::Suppressed); cellular_link_cancel_alert();
        cellular_diagnostics_record("ALERT_CANCELLED");
        if(workflow.state==alert::State::Armed) refresh_config();
    }
    if(workflow.state==alert::State::Armed && before==alert::State::Suppressed) {
        persistence_ok=latch(false); refresh_config();
        cellular_diagnostics_record("ALERT_REARMED");
    }
    if(!persistence_ok) {
        workflow.config.emergency_call_enabled=false;
        static bool reported=false;
        if(!reported) { cellular_diagnostics_record("ALERT_STORAGE_ERROR"); reported=true; }
    }
    alert::Action action;
    if(!workflow.next(action)) return;
    if(action.kind==alert::Kind::Horn) {
        workflow.submitted(action.kind); cellular_link_start_alarm(); return;
    }
    if(cellular_link_busy()) return;
    // Never repeat a submitted action, even on unknown modem outcome.
    workflow.submitted(action.kind);
    if(action.kind==alert::Kind::Text) cellular_link_send_sms(action.number,action.message);
    else if(action.kind==alert::Kind::Call) cellular_link_send_call(action.number);
    else cellular_link_start_alarm();
}
void alert_runtime_sample(float f,bool valid) { workflow.sample(now_ms(),f,valid); }
bool alert_runtime_active() { return workflow.state==alert::State::Active; }
bool alert_runtime_suppressed() { return workflow.state==alert::State::Suppressed; }
const alert::Workflow &alert_runtime_workflow() { return workflow; }
