#include "setup_mode.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define BUTTON_GPIO     GPIO_NUM_0
#define LED_GPIO        GPIO_NUM_2
#define WIFI_SSID       "ESP32-Setup"
#define WIFI_PASS       ""
#define SETUP_IP        "192.168.4.1"

#define NVS_NAMESPACE   "bsm_cfg"
#define NVS_KEY_PHONE   "phone"
#define NVS_KEY_EC1     "ec1"
#define NVS_KEY_EC2     "ec2"
#define NVS_KEY_EC3     "ec3"
#define NVS_KEY_EMERG   "emerg_alerts"

#define MAX_PHONE_LEN   32

static const char *TAG = "setup_mode";

// ─── State ─────────────────────────────────────────────────────────────
static volatile bool setup_confirmed = false;
static httpd_handle_t server = NULL;
static TaskHandle_t dns_task_handle = NULL;
static esp_netif_t *s_ap_netif = NULL;
static uint8_t s_setup_count = 0;
static bool s_netif_inited = false;
static bool s_eventloop_inited = false;

// ─── NVS Helpers ───────────────────────────────────────────────────────

typedef struct {
    char phone[MAX_PHONE_LEN];
    char ec1[MAX_PHONE_LEN];
    char ec2[MAX_PHONE_LEN];
    char ec3[MAX_PHONE_LEN];
    bool emerg_alerts;
} bsm_config_t;

static void nvs_load_config(bsm_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->emerg_alerts = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    size_t len;

    len = sizeof(cfg->phone);
    nvs_get_str(h, NVS_KEY_PHONE, cfg->phone, &len);

    len = sizeof(cfg->ec1);
    nvs_get_str(h, NVS_KEY_EC1, cfg->ec1, &len);

    len = sizeof(cfg->ec2);
    nvs_get_str(h, NVS_KEY_EC2, cfg->ec2, &len);

    len = sizeof(cfg->ec3);
    nvs_get_str(h, NVS_KEY_EC3, cfg->ec3, &len);

    uint8_t ea = 0;
    nvs_get_u8(h, NVS_KEY_EMERG, &ea);
    cfg->emerg_alerts = (ea == 1);

    nvs_close(h);
}

static void nvs_save_config(const bsm_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(h, NVS_KEY_PHONE, cfg->phone));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(h, NVS_KEY_EC1,   cfg->ec1));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(h, NVS_KEY_EC2,   cfg->ec2));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(h, NVS_KEY_EC3,   cfg->ec3));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(h,  NVS_KEY_EMERG, cfg->emerg_alerts ? 1 : 0));

    err = nvs_commit(h);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));

    nvs_close(h);
}

// ─── URL Decode ────────────────────────────────────────────────────────

static char hex_to_char(char hi, char lo)
{
    auto unhex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    return (char)((unhex(hi) << 4) | unhex(lo));
}

static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    size_t si = 0;
    size_t src_len = strlen(src);

    while (si < src_len && di < dst_size - 1) {
        if (src[si] == '%' && si + 2 < src_len) {
            dst[di++] = hex_to_char(src[si+1], src[si+2]);
            si += 3;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
            si++;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
}

// Extract a named field from an application/x-www-form-urlencoded body.
// Writes decoded value into `out` (up to out_size-1 chars).
static void form_get_field(const char *body, const char *key, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *p = body;

    while (*p) {
        // find '=' separator
        const char *eq = strchr(p, '=');
        if (!eq) break;

        size_t field_len = (size_t)(eq - p);
        const char *amp = strchr(eq + 1, '&');
        const char *val_end = amp ? amp : (eq + 1 + strlen(eq + 1));

        if (field_len == key_len && strncmp(p, key, key_len) == 0) {
            size_t val_len = (size_t)(val_end - (eq + 1));
            char raw[128] = {};
            if (val_len >= sizeof(raw)) val_len = sizeof(raw) - 1;
            strncpy(raw, eq + 1, val_len);
            raw[val_len] = '\0';
            url_decode(raw, out, out_size);
            return;
        }

        p = amp ? amp + 1 : val_end;
    }
}

// ─── HTML ──────────────────────────────────────────────────────────────

// We build the portal HTML dynamically to inject saved values.
// Max size: ~5 KB stack-allocated in the handler.
#define PORTAL_HTML_MAXLEN 5120

static void build_portal_html(char *buf, size_t buf_size, const bsm_config_t *cfg)
{
    snprintf(buf, buf_size,
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta charset='UTF-8'>"
        "<title>Backseat Minder Setup</title>"
        "<link rel='preconnect' href='https://fonts.googleapis.com'>"
        "<link rel='preconnect' href='https://fonts.gstatic.com' crossorigin>"
        "<link href='https://fonts.googleapis.com/css2?family=DM+Mono:wght@400;500&family=DM+Sans:wght@400;500;700&display=swap' rel='stylesheet'>"
        "<style>"
        ":root{"
          "--bg:#0a0a0a;--surface:#141414;--border:#2a2a2a;--border-focus:#4ecca3;"
          "--accent:#4ecca3;--accent-dim:#1a3d35;--text:#f0f0f0;--muted:#666;"
          "--danger:#ff6b6b;--radius:12px;"
        "}"
        "*{box-sizing:border-box;margin:0;padding:0;}"
        "body{"
          "background:var(--bg);color:var(--text);"
          "font-family:'DM Sans',sans-serif;"
          "min-height:100vh;display:flex;flex-direction:column;"
          "align-items:center;justify-content:flex-start;"
          "padding:32px 20px 48px;"
        "}"
        ".card{"
          "background:var(--surface);border:1px solid var(--border);"
          "border-radius:20px;padding:32px 28px;width:100%%;max-width:420px;"
          "box-shadow:0 0 60px rgba(78,204,163,0.04);"
        "}"
        ".header{display:flex;align-items:center;gap:12px;margin-bottom:8px;}"
        ".dot{"
          "width:10px;height:10px;border-radius:50%%;background:var(--accent);"
          "flex-shrink:0;"
          "animation:pulse 1.8s ease-in-out infinite;"
        "}"
        "@keyframes pulse{0%%,100%%{opacity:1;box-shadow:0 0 0 0 rgba(78,204,163,0.4);}50%%{opacity:0.6;box-shadow:0 0 0 6px rgba(78,204,163,0);}}"
        "h1{font-size:1.25rem;font-weight:700;letter-spacing:-0.01em;}"
        ".subtitle{color:var(--muted);font-size:0.85rem;margin-bottom:28px;margin-top:6px;line-height:1.5;}"
        ".divider{border:none;border-top:1px solid var(--border);margin:24px 0;}"
        ".section-label{"
          "font-family:'DM Mono',monospace;font-size:0.7rem;letter-spacing:0.12em;"
          "text-transform:uppercase;color:var(--muted);margin-bottom:12px;"
        "}"
        ".field{margin-bottom:14px;}"
        ".field label{display:block;font-size:0.8rem;color:#aaa;margin-bottom:5px;font-weight:500;}"
        ".field input[type=tel]{"
          "width:100%%;background:#1a1a1a;border:1px solid var(--border);"
          "border-radius:var(--radius);padding:11px 14px;"
          "color:var(--text);font-family:'DM Sans',sans-serif;font-size:0.95rem;"
          "outline:none;transition:border-color 0.2s,box-shadow 0.2s;"
          "appearance:none;"
        "}"
        ".field input[type=tel]:focus{"
          "border-color:var(--border-focus);"
          "box-shadow:0 0 0 3px rgba(78,204,163,0.12);"
        "}"
        ".field input[type=tel]::placeholder{color:#3a3a3a;}"
        ".optional-tag{"
          "font-family:'DM Mono',monospace;font-size:0.65rem;color:var(--muted);"
          "letter-spacing:0.05em;margin-left:6px;vertical-align:middle;"
        "}"
        ".toggle-row{"
          "display:flex;align-items:flex-start;gap:14px;"
          "background:#111;border:1px solid var(--border);"
          "border-radius:var(--radius);padding:14px 16px;cursor:pointer;"
          "transition:border-color 0.2s;"
        "}"
        ".toggle-row:has(input:checked){border-color:var(--accent-dim);background:var(--accent-dim);}"
        ".toggle-row input[type=checkbox]{accent-color:var(--accent);width:18px;height:18px;flex-shrink:0;margin-top:1px;cursor:pointer;}"
        ".toggle-text .label{font-size:0.9rem;font-weight:600;margin-bottom:3px;}"
        ".toggle-text .desc{font-size:0.78rem;color:var(--muted);line-height:1.45;}"
        ".toggle-text .desc em{color:var(--danger);font-style:normal;font-weight:600;}"
        "button[type=submit]{"
          "width:100%%;margin-top:28px;"
          "background:var(--accent);color:#0a0a0a;"
          "border:none;border-radius:var(--radius);"
          "padding:15px;font-family:'DM Sans',sans-serif;"
          "font-size:0.95rem;font-weight:700;letter-spacing:0.01em;"
          "cursor:pointer;transition:opacity 0.15s,transform 0.1s;"
        "}"
        "button[type=submit]:hover{opacity:0.9;}"
        "button[type=submit]:active{transform:scale(0.98);}"
        ".required-note{font-size:0.72rem;color:var(--muted);margin-top:18px;text-align:center;}"
        "</style></head><body>"
        "<div class='card'>"
          "<div class='header'><div class='dot'></div><h1>Backseat Minder</h1></div>"
          "<p class='subtitle'>Configure your device. Settings are saved on-board<br>and restored after reboot.</p>"

          "<form action='/confirm' method='POST'>"

          "<p class='section-label'>Your number</p>"
          "<div class='field'>"
            "<label for='phone'>Mobile number <span style='color:#ff6b6b'>*</span></label>"
            "<input type='tel' id='phone' name='phone' placeholder='+1 555 000 0000' value='%s'>"
          "</div>"

          "<hr class='divider'>"

          "<p class='section-label'>Emergency contacts <span class='optional-tag'>optional</span></p>"
          "<div class='field'>"
            "<label for='ec1'>Contact 1</label>"
            "<input type='tel' id='ec1' name='ec1' placeholder='+1 555 000 0001' value='%s'>"
          "</div>"
          "<div class='field'>"
            "<label for='ec2'>Contact 2</label>"
            "<input type='tel' id='ec2' name='ec2' placeholder='+1 555 000 0002' value='%s'>"
          "</div>"
          "<div class='field'>"
            "<label for='ec3'>Contact 3</label>"
            "<input type='tel' id='ec3' name='ec3' placeholder='+1 555 000 0003' value='%s'>"
          "</div>"

          "<hr class='divider'>"

          "<p class='section-label'>Alerts</p>"
          "<label class='toggle-row'>"
            "<input type='checkbox' name='emerg_alerts' value='1' %s>"
            "<div class='toggle-text'>"
              "<div class='label'>Emergency alerts</div>"
              "<div class='desc'><em>ON</em> = device may contact emergency services</div>"
            "</div>"
          "</label>"

          "<button type='submit'>Confirm &amp; Start</button>"
          "<p class='required-note'>* required</p>"

          "</form>"
        "</div>"
        "</body></html>",
        cfg->phone,
        cfg->ec1,
        cfg->ec2,
        cfg->ec3,
        cfg->emerg_alerts ? "checked" : ""
    );
}

static const char *CONFIRM_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='UTF-8'>"
    "<title>All Set</title>"
    "<link href='https://fonts.googleapis.com/css2?family=DM+Sans:wght@400;700&display=swap' rel='stylesheet'>"
    "<style>"
    "body{margin:0;min-height:100vh;display:flex;flex-direction:column;"
    "align-items:center;justify-content:center;"
    "background:#0a0a0a;color:#f0f0f0;font-family:'DM Sans',sans-serif;"
    "text-align:center;padding:24px;box-sizing:border-box;}"
    ".icon{font-size:3rem;margin-bottom:16px;color:#4ecca3;}"
    "h1{font-size:1.5rem;margin-bottom:8px;color:#4ecca3;}"
    "p{color:#666;font-size:0.9rem;line-height:1.6;}"
    "</style></head><body>"
    "<div class='icon'>&#10003;</div>"
    "<h1>All Set!</h1>"
    "<p>Backseat Minder is now active.<br>Your settings have been saved.<br>You can close this page.</p>"
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

        buffer[2] |= 0x80;
        buffer[7] = buffer[5];

        int idx = recv_len;

        buffer[idx++] = 0xC0; buffer[idx++] = 0x0C;
        buffer[idx++] = 0x00; buffer[idx++] = 0x01;
        buffer[idx++] = 0x00; buffer[idx++] = 0x01;
        buffer[idx++] = 0x00; buffer[idx++] = 0x00;
        buffer[idx++] = 0x00; buffer[idx++] = 0x3C;
        buffer[idx++] = 0x00; buffer[idx++] = 0x04;
        buffer[idx++] = 192;  buffer[idx++] = 168;
        buffer[idx++] = 4;    buffer[idx++] = 1;

        sendto(sock, buffer, idx, 0,
               (struct sockaddr *)&client_addr, len);
    }
}

// ─── HTTP HANDLERS ────────────────────────────────────────────────

static esp_err_t root_get_handler(httpd_req_t *req)
{
    bsm_config_t cfg;
    nvs_load_config(&cfg);

    char *html = (char *)malloc(PORTAL_HTML_MAXLEN);
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    build_portal_html(html, PORTAL_HTML_MAXLEN, &cfg);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);

    free(html);
    return ESP_OK;
}

static esp_err_t confirm_post_handler(httpd_req_t *req)
{
    // Read POST body
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 2048) {
        // Nothing useful, still confirm
        setup_confirmed = true;
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, CONFIRM_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char *body = (char *)malloc(total_len + 1);
    if (!body) {
        setup_confirmed = true;
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, CONFIRM_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, body + received, total_len - received);
        if (ret <= 0) break;
        received += ret;
    }
    body[received] = '\0';

    ESP_LOGI(TAG, "POST body: %s", body);

    bsm_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    form_get_field(body, "phone", cfg.phone, sizeof(cfg.phone));
    form_get_field(body, "ec1",   cfg.ec1,   sizeof(cfg.ec1));
    form_get_field(body, "ec2",   cfg.ec2,   sizeof(cfg.ec2));
    form_get_field(body, "ec3",   cfg.ec3,   sizeof(cfg.ec3));

    char ea_val[4] = {};
    form_get_field(body, "emerg_alerts", ea_val, sizeof(ea_val));
    cfg.emerg_alerts = (strcmp(ea_val, "1") == 0);

    free(body);

    nvs_save_config(&cfg);
    ESP_LOGI(TAG, "Saved -> phone='%s' ec1='%s' ec2='%s' ec3='%s' emerg=%d",
             cfg.phone, cfg.ec1, cfg.ec2, cfg.ec3, cfg.emerg_alerts);

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
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.max_uri_handlers = 20;

    httpd_handle_t hdl = NULL;
    if (httpd_start(&hdl, &config) != ESP_OK) return NULL;

    // ── Core handlers ──────────────────────────────────────────
    httpd_uri_t root    = { "/",       HTTP_GET,  root_get_handler,     NULL };
    httpd_uri_t confirm = { "/confirm",HTTP_POST, confirm_post_handler, NULL };

    // ── Captive portal detection endpoints ─────────────────────
    // Android / Chrome
    httpd_uri_t and1 = { "/generate_204",                    HTTP_GET, redirect_handler, NULL };
    httpd_uri_t and2 = { "/gen_204",                         HTTP_GET, redirect_handler, NULL };
    httpd_uri_t and3 = { "/connectivitycheck.gstatic.com",   HTTP_GET, redirect_handler, NULL };
    // iOS / macOS
    httpd_uri_t ios1 = { "/hotspot-detect.html",             HTTP_GET, redirect_handler, NULL };
    httpd_uri_t ios2 = { "/library/test/success.html",       HTTP_GET, redirect_handler, NULL };
    httpd_uri_t ios3 = { "/bag",                             HTTP_GET, redirect_handler, NULL };
    // Windows
    httpd_uri_t win1 = { "/connecttest.txt",                 HTTP_GET, redirect_handler, NULL };
    httpd_uri_t win2 = { "/ncsi.txt",                        HTTP_GET, redirect_handler, NULL };
    httpd_uri_t win3 = { "/redirect",                        HTTP_GET, redirect_handler, NULL };
    // Firefox
    httpd_uri_t ff1  = { "/canonical.html",                  HTTP_GET, redirect_handler, NULL };
    httpd_uri_t ff2  = { "/success.txt",                     HTTP_GET, redirect_handler, NULL };

    // ── Catch-all wildcard — MUST be registered last ───────────
    // Redirects any URL not matched above, so the portal always
    // pops up regardless of which probe URL the OS tries first.
    httpd_uri_t wild = { "/*",         HTTP_GET,  redirect_handler,     NULL };

    httpd_register_uri_handler(hdl, &root);
    httpd_register_uri_handler(hdl, &confirm);
    httpd_register_uri_handler(hdl, &and1);
    httpd_register_uri_handler(hdl, &and2);
    httpd_register_uri_handler(hdl, &and3);
    httpd_register_uri_handler(hdl, &ios1);
    httpd_register_uri_handler(hdl, &ios2);
    httpd_register_uri_handler(hdl, &ios3);
    httpd_register_uri_handler(hdl, &win1);
    httpd_register_uri_handler(hdl, &win2);
    httpd_register_uri_handler(hdl, &win3);
    httpd_register_uri_handler(hdl, &ff1);
    httpd_register_uri_handler(hdl, &ff2);
    httpd_register_uri_handler(hdl, &wild);

    return hdl;
}

// ─── AP START ────────────────────────────────────────────────

static void start_ap(void)
{
    // esp_netif_init and esp_event_loop_create_default are one-time inits —
    // calling them again after teardown causes silent failures on ESP-IDF v5.
    // We track whether they have been called and skip if already done.
    if (!s_netif_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        s_netif_inited = true;
    }
    if (!s_eventloop_inited) {
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_eventloop_inited = true;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *ap_netif = s_ap_netif;

    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr      = ipaddr_addr(SETUP_IP);
    ip_info.gw.addr      = ipaddr_addr(SETUP_IP);
    ip_info.netmask.addr = ipaddr_addr("255.255.255.0");

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Use a rolling SSID so the phone always sees a "new" network,
    // forcing it to re-run captive portal detection on every entry.
    char ssid_buf[32];
    s_setup_count++;
    snprintf(ssid_buf, sizeof(ssid_buf), "%s-%u", WIFI_SSID, s_setup_count);

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.ap.ssid,     ssid_buf, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, WIFI_PASS, sizeof(wifi_config.ap.password));

    wifi_config.ap.ssid_len       = strlen(ssid_buf);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode       = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

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

    if (dns_task_handle) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }

    if (server) {
        httpd_stop(server);
        server = NULL;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    // Do NOT call esp_event_loop_delete_default() or esp_netif_deinit() here.
    // These are one-time inits on ESP-IDF v5 and cannot be safely re-initialized
    // in the same session. The event loop and netif stack stay up between
    // setup mode sessions; only the wifi driver and AP netif are torn down.

    while (gpio_get_level(BUTTON_GPIO) == 0)
        vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Exiting setup mode");
}