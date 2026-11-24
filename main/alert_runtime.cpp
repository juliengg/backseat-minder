#include "alert_runtime.h"
#include "setup_mode.h"
#include "cellular_link.h"
#include "cellular_diagnostics.h"
#include "telemetry_test_commands.h"
#include "esp_timer.h"
#include "nvs.h"
#include <atomic>
#include <cstring>

namespace {
alert::Workflow workflow;
std::atomic<bool> stop_requested{false};
bool persistence_ok = true;
bool stop_suppressed = false;
uint64_t last_checkpoint_ms = 0;
constexpr uint64_t CHECKPOINT_INTERVAL_MS = 5000;
constexpr char TEST_SMS_MESSAGE[] = "Backseat Minder test message. No action is needed.";
char test_recipients[4][cellular::PHONE_CAPACITY];
unsigned test_recipient_count = 0, test_recipient_next = 0;

const char *phone_digits(const char *number) { return *number == '+' ? number + 1 : number; }

void load_test_recipients()
{
    test_recipient_next = test_recipient_count = 0;
    for (unsigned i = 0; i < 4; ++i) {
        char *number = test_recipients[test_recipient_count];
        if (!setup_mode_get_contact(i, number, sizeof(test_recipients[0]))) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < test_recipient_count; ++j) {
            duplicate |= !std::strcmp(phone_digits(number), phone_digits(test_recipients[j]));
        }
        if (!duplicate) ++test_recipient_count;
    }
    if (!test_recipient_count) cellular_diagnostics_record("TEST_NO_CONTACTS");
}

uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time()/1000); }
bool persist() {
    nvs_handle_t h;
    if(nvs_open("bsm_alert",NVS_READWRITE,&h)!=ESP_OK) return false;
    const auto saved=workflow.checkpoint(now_ms(),stop_suppressed);
    esp_err_t err=nvs_set_blob(h,"incident_v1",&saved,sizeof(saved));
    if(err==ESP_OK) err=nvs_set_u8(h,"stop_suppressed",stop_suppressed?1:0);
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h); return err==ESP_OK;
}
void save_checkpoint() {
    persistence_ok=persist();
    last_checkpoint_ms=now_ms();
    if(!persistence_ok) cellular_diagnostics_record("ALERT_STORAGE_ERROR");
}
void incoming(const char *sender, const char *text) {
    if(!alert::stop_text(text)) return;
    const char *digits=phone_digits(sender);
    for(unsigned i=0;i<4;++i) {
        char saved[32];
        if(!setup_mode_get_contact(i,saved,sizeof(saved))) continue;
        const char *expected=phone_digits(saved);
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
    test_recipient_next = test_recipient_count = 0;
    stop_suppressed=false;
    last_checkpoint_ms=now_ms();
    nvs_handle_t h;
    if(nvs_open("bsm_alert",NVS_READONLY,&h)==ESP_OK) {
        alert::Checkpoint saved; size_t length=sizeof(saved);
        const bool restored=nvs_get_blob(h,"incident_v1",&saved,&length)==ESP_OK &&
            length==sizeof(saved) && workflow.restore(saved,now_ms());
        if(restored) stop_suppressed=saved.mode==2;
        else {
            uint8_t value=0; nvs_get_u8(h,"stop_suppressed",&value);
            stop_suppressed=value!=0;
            if(value) workflow.restore_suppressed();
        }
        nvs_close(h);
        if(workflow.state==alert::State::Active) cellular_diagnostics_record("ALERT_RESUMED");
    }
    cellular_link_set_sms_handler(incoming);
}
void alert_runtime_cancel() {
    workflow.cancel();
    test_recipient_next = test_recipient_count = 0;
    stop_suppressed=true;
    save_checkpoint();
    cellular_link_cancel_alert();
    cellular_diagnostics_record("ALERT_CANCELLED");
}
void alert_runtime_enter_setup() {
    workflow.cancel();
    test_recipient_next = test_recipient_count = 0;
    // Setup exits by rebooting: retain settings, then confirm driver/occupancy.
    stop_suppressed=false;
    save_checkpoint();
    cellular_link_cancel_alert();
    cellular_diagnostics_record("SETUP_ALERTS_PAUSED");
}
void alert_runtime_exit_setup() {
    workflow.cancel(); // Recheck driver/occupancy on the next monitoring poll.
    stop_suppressed=false;
    save_checkpoint();
    refresh_config();
    cellular_diagnostics_record("SETUP_ALERTS_PAUSED");
}
void alert_runtime_poll(bool presence,const BeaconStatus &driver) {
    if(stop_requested.exchange(false)) alert_runtime_cancel();
    const auto before=workflow.state;
    const auto previous_stage=workflow.stage;
    const bool near=driver.available && !driver.checking && driver.driver_present;
    const bool away=driver.available && !driver.checking && !driver.driver_present;
    workflow.tick(now_ms(),away,near,presence);
    if(before!=alert::State::Active && workflow.state==alert::State::Active) {
        test_recipient_next = test_recipient_count = 0;
        cellular_link_cancel_test();
        cellular_diagnostics_record("ALERT_STARTED");
    }
    if(before==alert::State::Active && workflow.state!=alert::State::Active) {
        stop_suppressed=false;
        cellular_link_cancel_alert();
        cellular_diagnostics_record("ALERT_CANCELLED");
        refresh_config();
    }
    if(workflow.state!=alert::State::Suppressed && before==alert::State::Suppressed) {
        stop_suppressed=false; refresh_config();
        cellular_diagnostics_record("ALERT_REARMED");
    }
    if((before!=workflow.state && (before==alert::State::Active ||
        workflow.state==alert::State::Active || before==alert::State::Suppressed)) ||
        previous_stage!=workflow.stage ||
        (workflow.state==alert::State::Active && now_ms()-last_checkpoint_ms>=CHECKPOINT_INTERVAL_MS))
        save_checkpoint();
    if(!persistence_ok) {
        workflow.config.emergency_call_enabled=false;
        static bool reported=false;
        if(!reported) { cellular_diagnostics_record("ALERT_STORAGE_ERROR"); reported=true; }
    }
    alert::Action action;
    if(!workflow.next(action)) return;
    if(action.kind==alert::Kind::Horn) {
        workflow.submitted(action.kind); save_checkpoint(); cellular_link_start_alarm(); return;
    }
    if(cellular_link_busy()) return;
    // Never repeat a submitted action, even on unknown modem outcome.
    workflow.submitted(action.kind);
    save_checkpoint();
    // Commit the attempted call before dispatch so a reset cannot redial it.
    if(action.kind==alert::Kind::Call && !persistence_ok) return;
    if(action.kind==alert::Kind::Text) cellular_link_send_sms(action.number,action.message);
    else if(action.kind==alert::Kind::Call) cellular_link_send_call(action.number);
    else cellular_link_start_alarm();
}

bool alert_runtime_test_sms_pending() { return test_recipient_next < test_recipient_count; }

void alert_runtime_poll_tests(uint8_t requests)
{
    if (alert_runtime_active()) {
        if (requests) cellular_diagnostics_record("TEST_ALERT_ACTIVE");
        return;
    }
    const bool busy = cellular_link_busy();
    if (requests) {
        if (busy || alert_runtime_test_sms_pending()) {
            cellular_diagnostics_record("TEST_BUSY");
        } else if (requests == telemetry_test::SMS) {
            load_test_recipients();
        } else if (requests == telemetry_test::CALL) {
            if (setup_mode_emergency_alerts_enabled()) {
                cellular_link_send_call(alert::EMERGENCY_SERVICES_NUMBER, true);
            } else {
                cellular_diagnostics_record("TEST_CALL_DISABLED");
            }
        } else if (requests == telemetry_test::HORN) {
            cellular_link_send_horn(true);
        } else {
            cellular_diagnostics_record("TEST_BUSY");
        }
    }
    if (!busy && alert_runtime_test_sms_pending()) {
        cellular_link_send_sms(test_recipients[test_recipient_next++], TEST_SMS_MESSAGE, true);
    }
}

void alert_runtime_sample(float f,bool valid) { workflow.sample(now_ms(),f,valid); }
bool alert_runtime_active() { return workflow.state==alert::State::Active; }
bool alert_runtime_suppressed() { return workflow.state==alert::State::Suppressed; }
const alert::Workflow &alert_runtime_workflow() { return workflow; }
