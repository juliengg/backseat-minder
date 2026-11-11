#include "ble_telemetry.h"
#include "ble_telemetry_gap.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
const char *TAG = "ble_telemetry";
// 8d7c0001-3e91-4d8b-9f2a-5b6c7d8e9010; TX=0002, command RX=0003.
uint8_t service_uuid[] = {0x10,0x90,0x8e,0x7d,0x6c,0x5b,0x2a,0x9f,
                          0x8b,0x4d,0x91,0x3e,0x01,0x00,0x7c,0x8d};
uint8_t tx_uuid[] = {0x10,0x90,0x8e,0x7d,0x6c,0x5b,0x2a,0x9f,
                     0x8b,0x4d,0x91,0x3e,0x02,0x00,0x7c,0x8d};
uint8_t rx_uuid[] = {0x10,0x90,0x8e,0x7d,0x6c,0x5b,0x2a,0x9f,
                     0x8b,0x4d,0x91,0x3e,0x03,0x00,0x7c,0x8d};
enum { SERVICE, TX_DECL, TX_VALUE, TX_CCCD, RX_DECL, RX_VALUE, ATTR_COUNT };
uint16_t handles[ATTR_COUNT] = {};
uint16_t primary_uuid = ESP_GATT_UUID_PRI_SERVICE;
uint16_t declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
uint16_t cccd_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
uint8_t notify_property = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
uint8_t write_property = ESP_GATT_CHAR_PROP_BIT_WRITE;
uint8_t cccd[2] = {};
uint8_t empty_value[1] = {};
const esp_gatts_attr_db_t attributes[] = {
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&primary_uuid),
        ESP_GATT_PERM_READ, 16, 16, service_uuid}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declaration_uuid),
        ESP_GATT_PERM_READ, 1, 1, &notify_property}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, tx_uuid, 0, 244, 0, empty_value}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&cccd_uuid),
        ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED, 2, 2, cccd}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declaration_uuid),
        ESP_GATT_PERM_READ, 1, 1, &write_property}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, rx_uuid,
        ESP_GATT_PERM_WRITE_ENCRYPTED, 9, 0, empty_value}},
};

constexpr size_t SLOT_COUNT = 6;
const char *markers[SLOT_COUNT] = {"BSMP", "BSMT", "BSMC", "BSMD", "BSMH", "BSMF"};
const size_t limits[SLOT_COUNT] = {512, 512, 10000, 5, 8000, 200000};
struct Packet {
    uint8_t *data = nullptr;
    size_t length = 0, offset = 0;
    uint32_t sequence = 0, session = 0;
    int64_t started = 0;
};
Packet pending[SLOT_COUNT];
SemaphoreHandle_t mutex = nullptr;
TaskHandle_t worker = nullptr;
bool enabled = false, connected = false, subscribed = false, congested = false;
bool sms_pending = false, adv_ready = false, name_ready = false, service_ready = false;
uint16_t connection_id = 0, mtu = 23;
esp_gatt_if_t interface_id = ESP_GATT_IF_NONE;
uint32_t session = 0, next_sequence = 0;
int64_t last_camera = 0;
bool driver_in_flight = false;

// Caller holds mutex. Epochs prevent queued data crossing sleep/reconnections.
void clear_connection()
{
    connected = subscribed = congested = false;
    mtu = 23;
    ++session;
    for (auto &packet : pending) { free(packet.data); packet = {}; }
    last_camera = 0;
}

void advertise()
{
    if (!enabled || connected || !adv_ready || !name_ready || !service_ready) return;
    esp_ble_adv_params_t params = {};
    params.adv_int_min = 0x100;
    params.adv_int_max = 0x120;
    params.adv_type = ADV_TYPE_IND;
    params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    params.channel_map = ADV_CHNL_ALL;
    params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    const auto err = esp_ble_gap_start_advertising(&params);
    if (err != ESP_OK) ESP_LOGW(TAG, "Advertising unavailable: %s", esp_err_to_name(err));
}

void put_u32(uint8_t *out, uint32_t value)
{
    for (int i = 3; i >= 0; --i) { out[i] = value & 0xff; value >>= 8; }
}

void transmit_task(void *)
{
    Packet active[SLOT_COUNT];
    size_t image_turn = 4;
    while (true) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < SLOT_COUNT; ++i) {
            auto &packet = active[i];
            if (packet.data && (!enabled || !subscribed || packet.session != session ||
                                now - packet.started > 10000000)) {
                free(packet.data); packet = {};
            }
            if (!packet.data && pending[i].data) {
                packet = pending[i]; pending[i] = {};
                packet.started = now;
                packet.sequence = ++next_sequence;
            }
        }
        driver_in_flight = active[0].data != nullptr;
        size_t slot = SLOT_COUNT;
        // Driver/sensor/command receipts take priority over image fragments.
        for (size_t i = 0; i < 4; ++i) if (active[i].data) { slot = i; break; }
        if (slot == SLOT_COUNT) {
            if (active[image_turn].data) slot = image_turn;
            else if (active[9 - image_turn].data) slot = 9 - image_turn;
            image_turn = 9 - image_turn;
        }
        if (enabled && connected && subscribed && !congested && slot < SLOT_COUNT) {
            auto &packet = active[slot];
            uint8_t fragment[244];
            // Each notification identifies its packet and offset. Interrupted
            // images cannot corrupt subsequent telemetry or another image type.
            memcpy(fragment, markers[slot], 4);
            put_u32(fragment + 4, packet.sequence);
            put_u32(fragment + 8, packet.offset);
            put_u32(fragment + 12, packet.length);
            const size_t capacity = std::min<size_t>(mtu - 3, sizeof(fragment)) - 16;
            const size_t count = std::min(capacity, packet.length - packet.offset);
            memcpy(fragment + 16, packet.data + packet.offset, count);
            if (esp_ble_gatts_send_indicate(interface_id, connection_id, handles[TX_VALUE],
                                            count + 16, fragment, false) == ESP_OK) {
                packet.offset += count;
                if (packet.offset == packet.length) { free(packet.data); packet = {}; }
                driver_in_flight = active[0].data != nullptr;
            }
        }
        xSemaphoreGive(mutex);
        // Pace notifications and yield to the beacon scanner and sensor tasks.
        vTaskDelay(1);
    }
}

void gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                 esp_ble_gatts_cb_param_t *param)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!enabled) { xSemaphoreGive(mutex); return; }
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status == ESP_GATT_OK) {
            interface_id = gatts_if;
            if (esp_ble_gatts_create_attr_tab(attributes, gatts_if, ATTR_COUNT, 0) != ESP_OK)
                ESP_LOGW(TAG, "Could not create telemetry service");
        } else ESP_LOGW(TAG, "Telemetry registration failed: %d", param->reg.status);
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK && param->add_attr_tab.num_handle == ATTR_COUNT) {
            memcpy(handles, param->add_attr_tab.handles, sizeof(handles));
            esp_ble_gatts_start_service(handles[SERVICE]);
        } else ESP_LOGW(TAG, "Telemetry attribute table failed");
        break;
    case ESP_GATTS_START_EVT:
        service_ready = param->start.status == ESP_GATT_OK;
        advertise();
        break;
    case ESP_GATTS_CONNECT_EVT: {
        clear_connection();
        connected = true;
        connection_id = param->connect.conn_id;
        esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
        esp_ble_conn_update_params_t params = {};
        memcpy(params.bda, param->connect.remote_bda, sizeof(params.bda));
        params.min_int = 12; params.max_int = 24; // 15-30 ms, subject to host agreement.
        params.latency = 0; params.timeout = 400;
        esp_ble_gap_update_conn_params(&params);
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT:
        clear_connection();
        advertise();
        break;
    case ESP_GATTS_MTU_EVT:
        mtu = std::max<uint16_t>(23, param->mtu.mtu);
        break;
    case ESP_GATTS_CONGEST_EVT:
        congested = param->congest.congested;
        break;
    case ESP_GATTS_READ_EVT: {
        esp_gatt_rsp_t response = {};
        response.attr_value.handle = param->read.handle;
        response.attr_value.len = 2;
        response.attr_value.value[0] = subscribed ? 1 : 0;
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                    ESP_GATT_OK, &response);
        break;
    }
    case ESP_GATTS_WRITE_EVT: {
        esp_gatt_status_t status = ESP_GATT_INVALID_ATTR_LEN;
        if (!param->write.is_prep && param->write.offset == 0) {
            if (param->write.handle == handles[TX_CCCD] && param->write.len == 2 &&
                param->write.value[1] == 0 && param->write.value[0] <= 1) {
                subscribed = param->write.value[0] == 1;
                status = ESP_GATT_OK;
                ESP_LOGI(TAG, "Viewer %s (MTU %u)", subscribed ? "subscribed" : "unsubscribed", mtu);
            } else if (param->write.handle == handles[RX_VALUE] && param->write.len == 9 &&
                       memcmp(param->write.value, "SEND_SMS\n", 9) == 0) {
                sms_pending = true; // Main task uses the existing SMS request path.
                status = ESP_GATT_OK;
            }
        }
        if (param->write.need_rsp)
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, status, nullptr);
        break;
    }
    default: break;
    }
    xSemaphoreGive(mutex);
}
} // namespace

bool ble_telemetry_start()
{
    if (!mutex) mutex = xSemaphoreCreateMutex();
    if (!mutex) return false;
    if (!worker && xTaskCreate(transmit_task, "ble_telemetry", 4096, nullptr, 3, &worker) != pdPASS)
        return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    clear_connection();
    sms_pending = false;
    adv_ready = name_ready = service_ready = false;
    memset(handles, 0, sizeof(handles));
    enabled = true;
    xSemaphoreGive(mutex);

    uint8_t auth = ESP_LE_AUTH_BOND, io = ESP_IO_CAP_NONE;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, sizeof(io));
    esp_ble_gatt_set_local_mtu(247);
    // UUID in advertisements, name in scan response: each fits legacy 31 bytes.
    esp_ble_adv_data_t advertising = {};
    advertising.service_uuid_len = sizeof(service_uuid);
    advertising.p_service_uuid = service_uuid;
    advertising.flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    esp_ble_adv_data_t scan_response = {};
    scan_response.set_scan_rsp = true;
    scan_response.include_name = true;
    const bool ok = esp_ble_gap_set_device_name("Backseat Minder") == ESP_OK &&
        esp_ble_gap_config_adv_data(&advertising) == ESP_OK &&
        esp_ble_gap_config_adv_data(&scan_response) == ESP_OK &&
        esp_ble_gatts_register_callback(gatts_event) == ESP_OK &&
        esp_ble_gatts_app_register(0x4253) == ESP_OK;
    if (!ok) ble_telemetry_stop();
    return ok;
}

void ble_telemetry_stop()
{
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    enabled = false;
    clear_connection();
    xSemaphoreGive(mutex);
}

void ble_telemetry_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (enabled) {
        if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT)
            adv_ready = param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS;
        else if (event == ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT)
            name_ready = param->scan_rsp_data_cmpl.status == ESP_BT_STATUS_SUCCESS;
        else if (event == ESP_GAP_BLE_SEC_REQ_EVT)
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        else if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT &&
                 param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            ESP_LOGW(TAG, "Telemetry advertising failed: %d", param->adv_start_cmpl.status);
        if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT ||
            event == ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT) advertise();
    }
    xSemaphoreGive(mutex);
}

bool ble_telemetry_connected()
{
    if (!mutex) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool ready = enabled && connected && subscribed;
    xSemaphoreGive(mutex);
    return ready;
}

bool ble_telemetry_take_sms_request()
{
    if (!mutex) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool requested = sms_pending;
    sms_pending = false;
    xSemaphoreGive(mutex);
    return requested;
}

void ble_telemetry_flush()
{
    if (!mutex) return;
    const int64_t deadline = esp_timer_get_time() + 200000;
    while (esp_timer_get_time() < deadline) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        const bool waiting = enabled && subscribed && (pending[0].data || driver_in_flight);
        xSemaphoreGive(mutex);
        if (!waiting) return;
        vTaskDelay(1);
    }
}

bool ble_telemetry_send_packet(const char magic[4], const uint8_t *payload, size_t length)
{
    if (!mutex || !payload || !length || xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    size_t slot = SLOT_COUNT;
    for (size_t i = 0; i < SLOT_COUNT; ++i)
        if (memcmp(markers[i], magic, 4) == 0) { slot = i; break; }
    const int64_t now = esp_timer_get_time();
    if (!enabled || !connected || !subscribed || slot == SLOT_COUNT || length > limits[slot] ||
        (slot == 5 && last_camera && now - last_camera < 1000000)) {
        xSemaphoreGive(mutex); return false;
    }
    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) { xSemaphoreGive(mutex); return false; }
    memcpy(copy, payload, length);
    free(pending[slot].data);
    pending[slot] = {copy, length, 0, 0, session, 0};
    if (slot == 5) last_camera = now;
    xSemaphoreGive(mutex);
    return true;
}
