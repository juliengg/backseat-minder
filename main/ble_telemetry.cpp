#include "ble_telemetry.h"
#include "ble_telemetry_gap.h"
#include "ble_telemetry_protocol.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace {
const char *TAG="ble_telemetry";
// Nordic UART UUIDs, little endian as required by the ESP-IDF attribute table.
const uint8_t service_uuid[16]={0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e};
const uint8_t tx_uuid[16]={0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e};
const uint8_t rx_uuid[16]={0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e};
enum {SERVICE,TX_DECL,TX_VALUE,TX_CCCD,RX_DECL,RX_VALUE,ATTR_COUNT};
const uint16_t primary_uuid=ESP_GATT_UUID_PRI_SERVICE, declaration_uuid=ESP_GATT_UUID_CHAR_DECLARE,cccd_uuid=ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
const uint8_t notify_property=ESP_GATT_CHAR_PROP_BIT_NOTIFY,write_property=ESP_GATT_CHAR_PROP_BIT_WRITE;
const uint8_t zero=0,cccd[2]={0,0};
#define ATTR(uuid_len,uuid,perm,max_len,len,value) {{ESP_GATT_AUTO_RSP},{uuid_len,(uint8_t*)uuid,perm,max_len,len,(uint8_t*)value}}
const esp_gatts_attr_db_t attributes[ATTR_COUNT]={
 ATTR(ESP_UUID_LEN_16,&primary_uuid,ESP_GATT_PERM_READ,16,16,service_uuid),
 ATTR(ESP_UUID_LEN_16,&declaration_uuid,ESP_GATT_PERM_READ,1,1,&notify_property),
 ATTR(ESP_UUID_LEN_128,tx_uuid,ESP_GATT_PERM_READ_ENCRYPTED,512,1,&zero),
 ATTR(ESP_UUID_LEN_16,&cccd_uuid,ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE_ENCRYPTED,2,2,cccd),
 ATTR(ESP_UUID_LEN_16,&declaration_uuid,ESP_GATT_PERM_READ,1,1,&write_property),
 ATTR(ESP_UUID_LEN_128,rx_uuid,ESP_GATT_PERM_WRITE_ENCRYPTED,32,1,&zero),
};
#undef ATTR
std::atomic<bool> stopping{false},registered{false},registration_pending{false},adv_stop_done{true};
std::atomic<bool> active{false},connected{false},subscribed{false},encrypted{false},congested{false},sms_request{false};
std::atomic<uint32_t> generation{0};
std::atomic<uint16_t> mtu{23};
esp_gatt_if_t interface_id=ESP_GATT_IF_NONE;
uint16_t connection_id=0,handles[ATTR_COUNT]={};
esp_bd_addr_t peer={};
SemaphoreHandle_t queue_mutex=nullptr,io_mutex=nullptr;
TaskHandle_t sender_task=nullptr;
struct Packet {uint8_t *bytes=nullptr;size_t size=0;};
Packet pending[6];
int64_t last_media[3]={};
uint16_t next_id=0;
uint8_t adv_done=0;
bool service_started=false;
const char *types[6]={"BSMP","BSMT","BSMC","BSMD","BSMH","BSMF"};
void discard_pending(){
 if(!queue_mutex)return;
 xSemaphoreTake(queue_mutex,portMAX_DELAY);
 for(auto &p:pending){free(p.bytes);p={};}
 xSemaphoreGive(queue_mutex);
}
void advertise(){
 if(!active || stopping || adv_done!=3 || !service_started || connected)return;
 esp_ble_adv_params_t params={};
 params.adv_int_min=0x30;params.adv_int_max=0x60;
 params.adv_type=ADV_TYPE_IND;params.own_addr_type=BLE_ADDR_TYPE_PUBLIC;
 params.channel_map=ADV_CHNL_ALL;params.adv_filter_policy=ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
 const auto err=esp_ble_gap_start_advertising(&params);
 if(err!=ESP_OK)ESP_LOGW(TAG,"Advertising could not start: %s",esp_err_to_name(err));
}
void sender(void*){
 uint8_t fragment[ESP_GATT_MAX_MTU_SIZE];
 for(;;){
  ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
  Packet p;
  if(!ble_telemetry_connected())continue;
  xSemaphoreTake(queue_mutex,portMAX_DELAY);
  for(auto &slot:pending)if(slot.bytes){p=slot;slot={};break;}
  xSemaphoreGive(queue_mutex);
  if(!p.bytes)continue;
  const auto epoch=generation.load();const uint16_t id=++next_id;
  size_t offset=0;const int64_t started=esp_timer_get_time();
  const bool media=!memcmp(p.bytes,"BSMF",4)||!memcmp(p.bytes,"BSMH",4);
  const int64_t budget=media?5000000:8000000;
  while(offset<p.size && ble_telemetry_connected() && generation==epoch &&
        esp_timer_get_time()-started<budget){
   if(congested){vTaskDelay(1);continue;}
   size_t capacity=mtu.load()-3;
   if(capacity>sizeof(fragment))capacity=sizeof(fragment);
   if(capacity<=ble_wire::HEADER_SIZE)break;
   const size_t count=(p.size-offset<capacity-ble_wire::HEADER_SIZE)?p.size-offset:capacity-ble_wire::HEADER_SIZE;
   ble_wire::header(fragment,id,offset,p.size);
   memcpy(fragment+ble_wire::HEADER_SIZE,p.bytes+offset,count);
   if(xSemaphoreTake(io_mutex,pdMS_TO_TICKS(10))!=pdTRUE)continue;
   esp_err_t err=ESP_ERR_INVALID_STATE;
   if(ble_telemetry_connected() && generation==epoch)
    err=esp_ble_gatts_send_indicate(interface_id,connection_id,handles[TX_VALUE],count+ble_wire::HEADER_SIZE,fragment,false);
   xSemaphoreGive(io_mutex);
   if(err==ESP_OK)offset+=count;
   // Bound load on the shared scanner/advertiser/controller and yield to alerts.
   vTaskDelay(1);
  }
  free(p.bytes);
  // A frame timeout is safe: each next frame starts with its own id/offset.
  xTaskNotifyGive(sender_task);
 }
}
void gatts(esp_gatts_cb_event_t event,esp_gatt_if_t gatts_if,esp_ble_gatts_cb_param_t *p){
 if(!active)return;
 switch(event){
 case ESP_GATTS_REG_EVT:{
  if(p->reg.status!=ESP_GATT_OK){registration_pending=false;ESP_LOGW(TAG,"GATT registration failed");return;}
  registration_pending=false;registered=true;interface_id=gatts_if;
  esp_ble_gap_set_device_name("Backseat Minder");
  esp_ble_adv_data_t adv={};adv.service_uuid_len=16;adv.p_service_uuid=(uint8_t*)service_uuid;
  adv.flag=ESP_BLE_ADV_FLAG_GEN_DISC|ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
  esp_ble_gap_config_adv_data(&adv);
  esp_ble_adv_data_t response={};response.set_scan_rsp=true;response.include_name=true;
  esp_ble_gap_config_adv_data(&response);
  esp_ble_gatts_create_attr_tab(attributes,gatts_if,ATTR_COUNT,0);
  break;}
 case ESP_GATTS_UNREG_EVT:registered=false;interface_id=ESP_GATT_IF_NONE;break;
 case ESP_GATTS_CREAT_ATTR_TAB_EVT:
  if(p->add_attr_tab.status==ESP_GATT_OK && p->add_attr_tab.num_handle==ATTR_COUNT){
   memcpy(handles,p->add_attr_tab.handles,sizeof(handles));esp_ble_gatts_start_service(handles[SERVICE]);
  } else ESP_LOGW(TAG,"GATT table creation failed");
  break;
 case ESP_GATTS_START_EVT:service_started=p->start.status==ESP_GATT_OK;advertise();break;
 case ESP_GATTS_CONNECT_EVT:{
  connected=true;subscribed=false;encrypted=false;congested=false;mtu=23;++generation;
  connection_id=p->connect.conn_id;memcpy(peer,p->connect.remote_bda,sizeof(peer));
  discard_pending();
  if(stopping){esp_ble_gatts_close(gatts_if,connection_id);break;}
  esp_ble_set_encryption(peer,ESP_BLE_SEC_ENCRYPT_NO_MITM);
  esp_ble_conn_update_params_t params={};memcpy(params.bda,peer,sizeof(peer));
  params.min_int=12;params.max_int=24;params.latency=0;params.timeout=400;
  esp_ble_gap_update_conn_params(&params);
  ESP_LOGI(TAG,"Viewer connected; waiting for encrypted notification subscription");break;}
 case ESP_GATTS_DISCONNECT_EVT:
  connected=subscribed=encrypted=false;++generation;discard_pending();advertise();break;
 case ESP_GATTS_MTU_EVT:mtu=p->mtu.mtu;break;
 case ESP_GATTS_CONGEST_EVT:congested=p->congest.congested;break;
 case ESP_GATTS_WRITE_EVT:
  if(stopping || p->write.is_prep || p->write.offset || !encrypted)break;
  if(p->write.handle==handles[TX_CCCD] && p->write.len==2){
   subscribed=p->write.value[0]==1 && p->write.value[1]==0;
  } else if(p->write.handle==handles[RX_VALUE] &&
    ((p->write.len==9 && !memcmp(p->write.value,"SEND_SMS\n",9)) ||
     (p->write.len==10 && !memcmp(p->write.value,"SEND_SMS\r\n",10)))) sms_request=true;
  break;
 default:break;
 }
}
}
void ble_telemetry_init(){
 if(sender_task)return;
 queue_mutex=xSemaphoreCreateMutex();io_mutex=xSemaphoreCreateMutex();
 if(!queue_mutex || !io_mutex){ESP_LOGW(TAG,"Telemetry mutex allocation failed");return;}
 if(xTaskCreatePinnedToCore(sender,"ble_telemetry",4096,nullptr,1,&sender_task,1)!=pdPASS)
  ESP_LOGW(TAG,"Telemetry task allocation failed");
}
bool ble_telemetry_start(){
 if(!sender_task)return false;
 active=true;stopping=false;registered=false;registration_pending=true;adv_stop_done=true;connected=subscribed=encrypted=congested=false;adv_done=0;service_started=false;mtu=23;++generation;
 uint8_t auth=ESP_LE_AUTH_REQ_SC_BOND,key_size=16,keys=ESP_BLE_ENC_KEY_MASK|ESP_BLE_ID_KEY_MASK;
 uint8_t io=ESP_IO_CAP_NONE;
 esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,&auth,sizeof(auth));
 esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,&io,sizeof(io));
 esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,&key_size,sizeof(key_size));
 esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,&keys,sizeof(keys));
 esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,&keys,sizeof(keys));
 esp_ble_gatt_set_local_mtu(247);
 if(esp_ble_gatts_register_callback(gatts)!=ESP_OK || esp_ble_gatts_app_register(0xb50)!=ESP_OK){registration_pending=false;active=false;return false;}
 ESP_LOGI(TAG,"BLE telemetry starting (Backseat Minder); beacon scan remains enabled");return true;
}
bool ble_telemetry_stop(){
 if(!active){discard_pending();return true;}
 // Keep callbacks alive while closing the link/unregistering the server. Host
 // disable with a live GATT connection can stall; never clear its state early.
 stopping=true;subscribed=false;sms_request=false;++generation;
 if(io_mutex && xSemaphoreTake(io_mutex,pdMS_TO_TICKS(200))!=pdTRUE)return false;
 if(io_mutex)xSemaphoreGive(io_mutex);
 auto waitUntil=[](auto predicate)->bool {
  const int64_t started=esp_timer_get_time();
  while(!predicate()) {
   if(esp_timer_get_time()-started>700000)return false;
   vTaskDelay(1);
  }
  return true;
 };
 if(!waitUntil([]{return !registration_pending.load();}))return false;
 adv_stop_done=false;
 if(esp_ble_gap_stop_advertising()!=ESP_OK)adv_stop_done=true;
 if(connected && interface_id!=ESP_GATT_IF_NONE)esp_ble_gatts_close(interface_id,connection_id);
 if(!waitUntil([]{return !connected.load() && adv_stop_done.load();}))return false;
 if(registered && interface_id!=ESP_GATT_IF_NONE) {
  if(esp_ble_gatts_app_unregister(interface_id)!=ESP_OK)return false;
  if(!waitUntil([]{return !registered.load();}))return false;
 }
 active=false;encrypted=false;discard_pending();return true;
}
void ble_telemetry_gap_event(esp_gap_ble_cb_event_t event,esp_ble_gap_cb_param_t *p){
 if(!active)return;
 switch(event){
 case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
  if(p->adv_data_cmpl.status==ESP_BT_STATUS_SUCCESS){adv_done|=1;advertise();}break;
 case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
  if(p->scan_rsp_data_cmpl.status==ESP_BT_STATUS_SUCCESS){adv_done|=2;advertise();}break;
 case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
  if(p->adv_start_cmpl.status!=ESP_BT_STATUS_SUCCESS)ESP_LOGW(TAG,"Advertising start rejected");
  break;
 case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:adv_stop_done=true;break;
 case ESP_GAP_BLE_SEC_REQ_EVT:esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr,!stopping.load());break;
 case ESP_GAP_BLE_AUTH_CMPL_EVT:
  if(connected && !memcmp(peer,p->ble_security.auth_cmpl.bd_addr,sizeof(peer)))
   encrypted=p->ble_security.auth_cmpl.success;
  if(!p->ble_security.auth_cmpl.success)ESP_LOGW(TAG,"Viewer encryption/pairing failed");
  break;
 default:break;
 }
}
bool ble_telemetry_client_connected(){return active && !stopping && connected;}
bool ble_telemetry_connected(){return active && !stopping && connected && encrypted && subscribed;}
bool ble_telemetry_take_sms_request(){return sms_request.exchange(false);}
bool ble_telemetry_send_packet(const char magic[4],const uint8_t *data,size_t size){
 if(!data || !ble_telemetry_connected() || size>200000 || !queue_mutex)return false;
 unsigned type=0;while(type<6 && memcmp(magic,types[type],4))++type;
 if(type==6)return false;
 if(type>=3){
  const unsigned index=type-3;const int64_t now=esp_timer_get_time();
  const int64_t interval=type==5?2000000:1000000;
  if(now-last_media[index]<interval)return false;
  if(type==5 && (size>40000 || mtu<100))return false;
  last_media[index]=now;
 }
 const size_t length=size+8;
 auto *copy=(uint8_t*)heap_caps_malloc(length,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!copy && length<=10008)copy=(uint8_t*)heap_caps_malloc(length,MALLOC_CAP_8BIT);
 if(!copy)return false;
 memcpy(copy,magic,4);for(unsigned i=0;i<4;++i)copy[4+i]=size>>(24-8*i);memcpy(copy+8,data,size);
 if(xSemaphoreTake(queue_mutex,0)!=pdTRUE){free(copy);return false;}
 if(!ble_telemetry_connected()){xSemaphoreGive(queue_mutex);free(copy);return false;}
 free(pending[type].bytes);pending[type]={copy,length};
 xSemaphoreGive(queue_mutex);xTaskNotifyGive(sender_task);return true;
}
