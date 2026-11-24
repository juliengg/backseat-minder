#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "../main/alert_runtime.cpp"
static int64_t now=0;
static uint8_t savedLatch=0;
static std::vector<uint8_t> savedIncident;
static bool failed=false, linkBusy=false;
static unsigned cancels=0, alarms=0;
static std::vector<std::string> texts,calls;
static std::vector<std::string> testTexts, testCalls;
static unsigned testHorns=0, testStops=0;
static bool testLinkBusy=false, callEnabled=true, duplicateContact=false;
static void (*handler)(const char *,const char *)=nullptr;
int64_t esp_timer_get_time(){return now;}
esp_err_t nvs_open(const char *,int,nvs_handle_t *h){*h=1;return failed?ESP_FAIL:ESP_OK;}
esp_err_t nvs_set_u8(nvs_handle_t,const char *key,uint8_t v){assert(std::string(key)=="stop_suppressed");savedLatch=v;return ESP_OK;}
esp_err_t nvs_get_u8(nvs_handle_t,const char *key,uint8_t *v){assert(std::string(key)=="stop_suppressed");*v=savedLatch;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t,const char *key,const void *data,size_t size){
    assert(std::string(key)=="incident_v1");
    if(failed) return ESP_FAIL;
    const auto *bytes=static_cast<const uint8_t *>(data);savedIncident.assign(bytes,bytes+size);return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t,const char *key,void *data,size_t *size){
    assert(std::string(key)=="incident_v1");
    if(savedIncident.empty() || *size<savedIncident.size()) return ESP_FAIL;
    *size=savedIncident.size();std::memcpy(data,savedIncident.data(),*size);return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t){return ESP_OK;}
void nvs_close(nvs_handle_t){}
bool setup_mode_get_contact(unsigned i,char *out,size_t cap){
    if (duplicateContact && i == 3) return false;
    if (duplicateContact && i == 1) std::snprintf(out,cap,"15551234560");
    else std::snprintf(out,cap,"+1555123456%u",i);
    return true;
}
bool setup_mode_emergency_alerts_enabled(){return callEnabled;}
void cellular_link_set_sms_handler(void (*h)(const char *,const char *)){handler=h;}
void cellular_link_cancel_alert(){++cancels;}
void cellular_link_cancel_test(){if(testLinkBusy) ++testStops;}
bool cellular_link_busy(){return linkBusy || testLinkBusy;}
esp_err_t cellular_link_send_sms(const char *n,const char *m,bool test){
    (test ? testTexts : texts).emplace_back(std::string(n)+":"+m);return ESP_OK;
}
esp_err_t cellular_link_send_call(const char *n,bool test){(test ? testCalls : calls).emplace_back(n);return ESP_OK;}
esp_err_t cellular_link_send_horn(bool test){assert(test);++testHorns;return ESP_OK;}
esp_err_t cellular_link_start_alarm(){++alarms;return ESP_OK;}
void cellular_diagnostics_record(const char *){}
int main(){
    BeaconStatus away{true,false,false,0,false},near{true,true,true,-60,false};
    alert_runtime_init();assert(handler);
    alert_runtime_poll(true,away);assert(alert_runtime_suppressed()&&texts.empty());
    alert_runtime_poll(false,near);assert(workflow.state==alert::State::NotArmed);
    alert_runtime_poll(true,near);assert(workflow.state==alert::State::Armed);
    alert_runtime_poll(true,away);assert(texts.size()==1&&savedLatch==0);
    handler("+15559999999","STOP");alert_runtime_poll(true,away);assert(alert_runtime_active()&&cancels==0);
    handler("+15551234561","sToP");alert_runtime_poll(true,away);assert(alert_runtime_suppressed()&&cancels);
    alert_runtime_poll(true,away);assert(texts.size()==1);
    now=1000000;alert_runtime_poll(true,near);
    assert(!alert_runtime_suppressed()&&!savedLatch);
    now=8000000;alert_runtime_poll(true,away);
    for(int i=0;i<=60;++i){now=8000000+i*3000000LL;alert_runtime_sample(90+float(i)/10,true);alert_runtime_poll(true,away);}
    assert(calls.size()==1&&calls[0]=="+13464421981");
    const auto elapsed_before_call_reboot=workflow.elapsed(now_ms());
    workflow=alert::Workflow{};now=0;alert_runtime_init();
    assert(alert_runtime_active() && workflow.call_attempted && !workflow.forecast_valid);
    assert(workflow.elapsed(now_ms())==elapsed_before_call_reboot);
    for(int i=0;i<=10;++i){now=i*3000000LL;alert_runtime_sample(110,true);alert_runtime_poll(true,away);}
    assert(calls.size()==1); // A call already dispatched cannot redial after reboot.
    // Setup pauses alerts without persisting suppression, even with a busy modem.
    const auto text_count = texts.size();
    const auto cancel_count = cancels;
    linkBusy = true;
    alert_runtime_enter_setup();
    assert(alert_runtime_suppressed() && savedLatch == 0 && cancels == cancel_count + 1);
    assert(texts.size() == text_count && calls.size() == 1);
    alert_runtime_exit_setup();assert(alert_runtime_suppressed() && savedLatch==0);
    alert_runtime_poll(false,near);assert(workflow.state==alert::State::NotArmed);
    alert_runtime_enter_setup();
    workflow=alert::Workflow{};alert_runtime_init();
    assert(alert_runtime_suppressed());
    alert_runtime_poll(true,away);assert(alert_runtime_suppressed()&&texts.size()==text_count);
    alert_runtime_poll(true,near);assert(workflow.state==alert::State::Armed);
    linkBusy = false;
    // Driver-return cancellation clears the persistence latch and dispatches
    // stop even with passenger presence and a busy modem.
    workflow=alert::Workflow{};
    alert_runtime_poll(true,near);
    now=500000000; alert_runtime_poll(true,away);
    for(int i=0;i<=10;++i){now=500000000+i*3000000LL;alert_runtime_sample(80,true);}
    alert_runtime_poll(true,near);
    now+=14999000;alert_runtime_sample(80,true);alert_runtime_poll(true,near);
    assert(alert_runtime_active());
    const auto return_cancels=cancels;
    linkBusy=true; now+=1000;alert_runtime_poll(true,near);
    assert(workflow.state==alert::State::Armed && savedLatch==0 && cancels==return_cancels+1);
    linkBusy=false;
    const auto return_texts=texts.size();
    now+=1000;alert_runtime_poll(true,away);
    assert(alert_runtime_active() && savedLatch==0 && texts.size()==return_texts+1);
    // Restarting an active incident resumes without waiting for the driver.
    const auto saved_elapsed=workflow.elapsed(now_ms());
    workflow=alert::Workflow{};now=0;alert_runtime_init();assert(alert_runtime_active());
    assert(workflow.elapsed(now_ms())==saved_elapsed);
    const auto reboot_texts=texts.size();
    alert_runtime_poll(true,away);assert(alert_runtime_active() && texts.size()==reboot_texts);
    alert_runtime_cancel();
    // Only explicit STOP persists across reboot, until confirmed driver return.
    workflow=alert::Workflow{};alert_runtime_init();assert(alert_runtime_suppressed());
    alert_runtime_poll(true,away);assert(alert_runtime_suppressed());
    alert_runtime_poll(true,near);assert(workflow.state==alert::State::Armed && savedLatch==0);
    assert(calls.size()==1);
    // Resume just before the horn deadline, without repeating prior text rounds.
    texts.clear();calls.clear();savedIncident.clear();savedLatch=0;workflow=alert::Workflow{};
    now=0;alert_runtime_init();alert_runtime_poll(true,near);alert_runtime_poll(true,away);
    now=180000000;alert_runtime_poll(true,away);
    for(unsigned i=0;i<4;++i) alert_runtime_poll(true,away);
    now=295000000;alert_runtime_poll(true,away);
    now=297000000;alert_runtime_poll(true,away); // Uncheckpointed time can lose <5 seconds.
    workflow=alert::Workflow{};now=0;alert_runtime_init();
    assert(alert_runtime_active() && workflow.elapsed(now_ms())==295000 && workflow.stage==2);
    const auto milestone_texts=texts.size();
    alert_runtime_poll(true,away);assert(texts.size()==milestone_texts);
    now=5000000;alert_runtime_poll(true,away);assert(workflow.stage==3);
    // A post-five-minute reboot reasserts the horn; it never starts a new timeline.
    workflow=alert::Workflow{};now=0;alert_runtime_init();
    assert(alert_runtime_active() && workflow.elapsed(now_ms())==300000 && workflow.stage==3);
    alert::Action action;assert(workflow.next(action) && action.kind==alert::Kind::Horn);
    const auto prior_alarms=alarms;
    alert_runtime_poll(true,away);assert(alarms==prior_alarms+1);
    // STOP and setup both remove the saved active incident.
    alert_runtime_cancel();workflow=alert::Workflow{};alert_runtime_init();assert(alert_runtime_suppressed());
    alert_runtime_enter_setup();workflow=alert::Workflow{};alert_runtime_init();assert(alert_runtime_suppressed());
    assert(savedLatch==0);

    alert_runtime_poll(true,near);
    const auto before_tests = savedIncident;
    const auto before_cancels = cancels;
    const auto before_alarms = alarms;
    alert_runtime_poll_tests(telemetry_test::SMS);
    assert(testTexts.size()==1 && alert_runtime_test_sms_pending());
    linkBusy=true;
    alert_runtime_poll_tests(telemetry_test::HORN);
    assert(testTexts.size()==1 && testHorns==0);
    linkBusy=false;
    for(unsigned i=0; i<4; ++i) alert_runtime_poll_tests(0);
    assert(testTexts.size()==4 && !alert_runtime_test_sms_pending());
    for(unsigned i=0; i<4; ++i) {
        assert(testTexts[i]=="+1555123456"+std::to_string(i)+":"+TEST_SMS_MESSAGE);
    }
    duplicateContact=true;
    alert_runtime_poll_tests(telemetry_test::SMS);
    for(unsigned i=0; i<4; ++i) alert_runtime_poll_tests(0);
    assert(testTexts.size()==6 && testTexts[4]==testTexts[0] && testTexts[5]==testTexts[2]);
    duplicateContact=false;
    callEnabled=false;
    alert_runtime_poll_tests(telemetry_test::CALL);
    assert(testCalls.empty());
    callEnabled=true;
    alert_runtime_poll_tests(telemetry_test::CALL);
    alert_runtime_poll_tests(telemetry_test::HORN);
    assert(testHorns==1 && testCalls==std::vector<std::string>{alert::EMERGENCY_SERVICES_NUMBER});
    assert(workflow.state==alert::State::Armed && !workflow.call_attempted);
    assert(savedIncident==before_tests && cancels==before_cancels && alarms==before_alarms);

    alert_runtime_poll_tests(telemetry_test::SMS);
    assert(testTexts.size()==7 && alert_runtime_test_sms_pending());
    testLinkBusy=true;
    const auto real_text_count=texts.size();
    alert_runtime_poll(true,away);
    assert(alert_runtime_active() && !alert_runtime_test_sms_pending() && testStops==1);
    assert(workflow.stage==1 && texts.size()==real_text_count && cancels==before_cancels);
    testLinkBusy=false;
    alert_runtime_poll(true,away);
    alert_runtime_poll_tests(telemetry_test::SMS | telemetry_test::CALL | telemetry_test::HORN);
    assert(texts.size()==real_text_count+1 && testTexts.size()==7 && testCalls.size()==1 && testHorns==1);
    puts("PASS: saved-contact STOP persistence, driver-return rearm, setup exit and runtime dispatch");
}
