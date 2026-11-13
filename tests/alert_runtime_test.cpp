#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "../main/alert_runtime.cpp"
static int64_t now=0;
static uint8_t savedLatch=0;
static bool failed=false, linkBusy=false;
static unsigned cancels=0;
static std::vector<std::string> texts,calls;
static void (*handler)(const char *,const char *)=nullptr;
int64_t esp_timer_get_time(){return now;}
esp_err_t nvs_open(const char *,int,nvs_handle_t *h){*h=1;return failed?ESP_FAIL:ESP_OK;}
esp_err_t nvs_set_u8(nvs_handle_t,const char *,uint8_t v){savedLatch=v;return ESP_OK;}
esp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *v){*v=savedLatch;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t){return ESP_OK;}
void nvs_close(nvs_handle_t){}
bool setup_mode_get_contact(unsigned i,char *out,size_t cap){std::snprintf(out,cap,"+1555123456%u",i);return true;}
bool setup_mode_emergency_alerts_enabled(){return true;}
void cellular_link_set_sms_handler(void (*h)(const char *,const char *)){handler=h;}
void cellular_link_cancel_alert(){++cancels;}
bool cellular_link_busy(){return linkBusy;}
esp_err_t cellular_link_send_sms(const char *n,const char *m){texts.emplace_back(std::string(n)+":"+m);return ESP_OK;}
esp_err_t cellular_link_send_call(const char *n){calls.emplace_back(n);return ESP_OK;}
esp_err_t cellular_link_start_alarm(){return ESP_OK;}
void cellular_diagnostics_record(const char *){}
int main(){
    BeaconStatus away{true,false,false,0,false},near{true,true,true,-60,false};
    alert_runtime_init();assert(handler);
    alert_runtime_poll(true,away);assert(texts.size()==1&&savedLatch==1);
    handler("+15559999999","STOP");alert_runtime_poll(true,away);assert(alert_runtime_active()&&cancels==0);
    handler("+15551234561","sToP");alert_runtime_poll(true,away);assert(alert_runtime_suppressed()&&cancels);
    alert_runtime_poll(true,away);assert(texts.size()==1);
    now=1000000;alert_runtime_poll(false,near);now=7000000;alert_runtime_poll(false,near);
    assert(!alert_runtime_suppressed()&&!savedLatch);
    now=8000000;alert_runtime_poll(true,away);
    for(int i=0;i<=60;++i){now=8000000+i*3000000LL;alert_runtime_sample(90+float(i)/10,true);alert_runtime_poll(true,away);}
    assert(calls.size()==1&&calls[0]=="+13464421981");
    // A physical setup request cancels with presence detected and the driver away,
    // even while the modem is busy. Monitoring cannot restart the same incident.
    const auto text_count = texts.size();
    const auto cancel_count = cancels;
    linkBusy = true;
    alert_runtime_cancel();
    assert(alert_runtime_suppressed() && savedLatch == 1 && cancels == cancel_count + 1);
    now += 400000000;
    alert_runtime_poll(true, away);
    assert(texts.size() == text_count && calls.size() == 1);
    linkBusy = false;
    // Driver-return cancellation clears the persistence latch and dispatches
    // stop even with passenger presence and a busy modem.
    workflow=alert::Workflow{};
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
    assert(alert_runtime_active() && savedLatch==1 && texts.size()==return_texts+1);
    alert_runtime_cancel();
    // Reboot of a latched incident suppresses rather than placing a second call.
    workflow=alert::Workflow{};alert_runtime_init();assert(alert_runtime_suppressed());
    assert(calls.size()==1);
    puts("PASS: saved-contact STOP authorization, persistence, runtime dispatch and one-call reset latch");
}
