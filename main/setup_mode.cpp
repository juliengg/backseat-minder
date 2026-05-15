#include "setup_mode.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <string.h>

#define BUTTON_GPIO     GPIO_NUM_0
#define LED_GPIO        GPIO_NUM_2
#define WIFI_SSID       "ESP32-Setup"
#define WIFI_PASS       ""
#define SETUP_IP        "192.168.4.1"

static const char *TAG = "setup_mode";

// ─── State ─────────────────────────────────────────────────────────────
static volatile bool setup_confirmed = false;
static httpd_handle_t server = NULL;
static TaskHandle_t dns_task_handle = NULL;

// ─── HTML ──────────────────────────────────────────────────────────────

static const char *PORTAL_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Backseat Minder Setup</title>"
    "<style>"
    "body{margin:0;min-height:100vh;display:flex;flex-direction:column;"
    "align-items:center;justify-content:center;"
    "background:#0f0f0f;color:#f0f0f0;font-family:sans-serif;text-align:center;padding:24px;box-sizing:border-box;}"
    "h1{font-size:1.6rem;margin-bottom:8px;}"
    ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;"
    "background:#4ecca3;margin-right:8px;animation:pulse 1.5s infinite;}"
    "@keyframes pulse{0%,100%{opacity:1;}50%{opacity:0.3;}}"
    "p{color:#888;margin-bottom:40px;font-size:0.95rem;}"
    "button{background:#4ecca3;color:#0f0f0f;border:none;padding:16px 48px;"
    "font-size:1rem;font-weight:bold;border-radius:10px;cursor:pointer;width:100%;max-width:280px;}"
    "</style></head><body>"
    "<h1><span class='dot'></span>Backseat Minder</h1>"
    "<p>Your device is connected and ready.<br>Tap confirm to begin monitoring.</p>"
    "<form action='/confirm' method='POST'>"
    "<button type='submit'>Confirm &amp; Start</button>"
    "</form>"
    "</body></html>";

static const char *CONFIRM_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>All Set</title>"
    "<style>"
    "body{margin:0;min-height:100vh;display:flex;flex-direction:column;"
    "align-items:center;justify-content:center;"
    "background:#0f0f0f;color:#f0f0f0;font-family:sans-serif;text-align:center;padding:24px;box-sizing:border-box;}"
    ".check{font-size:3rem;margin-bottom:16px;}"
    "h1{font-size:1.6rem;margin-bottom:8px;color:#4ecca3;}"
    "p{color:#888;font-size:0.95rem;}"
    "</style></head><body>"
    "<div class='check'>&#10003;</div>"
    "<h1>All Set!</h1>"
    "<p>Backseat Minder is now active.<br>You can close this page.</p>"
    "</body></html>";

// ─── DNS HIJACK TASK ────────────────────────────────────────────────

static void dns_server_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

    uint8_t buffer[512];

    while (true)
    {
        struct sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        int recv_len = recvfrom(sock, buffer, sizeof(buffer), 0,
                                (struct sockaddr *)&client_addr, &len);

        if (recv_len < 12) continue;

        // convert request → response
        buffer[2] |= 0x80;
        buffer[7] = buffer[5];

        int idx = recv_len;

        buffer[idx++] = 0xC0;
        buffer[idx++] = 0x0C;

        buffer[idx++] = 0x00;
        buffer[idx++] = 0x01;

        buffer[idx++] = 0x00;
        buffer[idx++] = 0x01;

        buffer[idx++] = 0x00;
        buffer[idx++] = 0x00;
        buffer[idx++] = 0x00;
        buffer[idx++] = 0x3C;

        buffer[idx++] = 0x00;
        buffer[idx++] = 0x04;

        buffer[idx++] = 192;
        buffer[idx++] = 168;
        buffer[idx++] = 4;
        buffer[idx++] = 1;

        sendto(sock, buffer, idx, 0,
               (struct sockaddr *)&client_addr, len);
    }
}

// ─── HTTP HANDLERS ────────────────────────────────────────────────

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t confirm_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIRM_HTML, HTTPD_RESP_USE_STRLEN);
    setup_confirmed = true;
    return ESP_OK;
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ─── HTTP SERVER ────────────────────────────────────────────────

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t hdl = NULL;
    if (httpd_start(&hdl, &config) != ESP_OK) return NULL;

    httpd_uri_t root = { "/", HTTP_GET, root_get_handler, NULL };
    httpd_uri_t confirm = { "/confirm", HTTP_POST, confirm_post_handler, NULL };

    // captive portal detection endpoints
    httpd_uri_t android = { "/generate_204", HTTP_GET, redirect_handler, NULL };
    httpd_uri_t apple   = { "/hotspot-detect.html", HTTP_GET, redirect_handler, NULL };
    httpd_uri_t windows = { "/connecttest.txt", HTTP_GET, redirect_handler, NULL };
    httpd_uri_t ncsi    = { "/ncsi.txt", HTTP_GET, redirect_handler, NULL };

    httpd_register_uri_handler(hdl, &root);
    httpd_register_uri_handler(hdl, &confirm);

    httpd_register_uri_handler(hdl, &android);
    httpd_register_uri_handler(hdl, &apple);
    httpd_register_uri_handler(hdl, &windows);
    httpd_register_uri_handler(hdl, &ncsi);

    return hdl;
}

// ─── AP START ────────────────────────────────────────────────

static void start_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = ipaddr_addr(SETUP_IP);
    ip_info.gw.addr = ipaddr_addr(SETUP_IP);
    ip_info.netmask.addr = ipaddr_addr("255.255.255.0");

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.ap.ssid, WIFI_SSID, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, WIFI_PASS, sizeof(wifi_config.ap.password));

    wifi_config.ap.ssid_len = strlen(WIFI_SSID);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // start DNS hijack
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);

    ESP_LOGI(TAG, "AP + DNS started");
}

// ─── PUBLIC API ────────────────────────────────────────────────

void setup_mode_init()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);
}

bool setup_mode_button_pressed()
{
    if (gpio_get_level(BUTTON_GPIO) != 0) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
    return gpio_get_level(BUTTON_GPIO) == 0;
}

void enter_setup_mode()
{
    ESP_LOGI(TAG, "Entering setup mode");

    setup_confirmed = false;
    start_ap();
    server = start_webserver();

    bool led_state = false;

    while (!setup_confirmed)
    {
        led_state = !led_state;
        gpio_set_level(LED_GPIO, led_state);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (dns_task_handle)
    {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }

    if (server)
    {
        httpd_stop(server);
        server = NULL;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();
    esp_netif_deinit();

    while (gpio_get_level(BUTTON_GPIO) == 0)
        vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Exiting setup mode");
}