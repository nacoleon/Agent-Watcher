#include "web_server.h"
#include "agent_state.h"
#include "sensecap-watcher.h"
#include "dialog.h"
#include "event_queue.h"
#include "renderer.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mdns.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "pw_web";
static httpd_handle_t s_server = NULL;

static int recv_full_body(httpd_req_t *req, char *buf, int buf_size)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len >= buf_size) {
        return -1;
    }
    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) {
            return -1;
        }
        received += ret;
    }
    buf[received] = '\0';
    return received;
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[]   asm("_binary_app_js_end");

static esp_err_t serve_embedded(httpd_req_t *req, const uint8_t *start, const uint8_t *end, const char *content_type)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_send(req, (const char *)start, end - start);
    return ESP_OK;
}

static esp_err_t handle_index(httpd_req_t *req) { return serve_embedded(req, index_html_start, index_html_end, "text/html"); }
static esp_err_t handle_css(httpd_req_t *req) { return serve_embedded(req, style_css_start, style_css_end, "text/css"); }
static esp_err_t handle_js(httpd_req_t *req) { return serve_embedded(req, app_js_start, app_js_end, "application/javascript"); }

static esp_err_t handle_api_status(httpd_req_t *req)
{
    pw_agent_state_data_t state = pw_agent_state_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "agent_state", pw_agent_state_to_string(state.current_state));
    cJSON_AddBoolToObject(root, "person_present", state.person_present);
    cJSON_AddStringToObject(root, "last_message", pw_dialog_get_last_text());
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "background", pw_renderer_get_background());

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_agent_state(httpd_req_t *req)
{
    char body[128];
    int ret = recv_full_body(req, body, sizeof(body));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *state_json = cJSON_GetObjectItem(root, "state");
    if (!state_json || !cJSON_IsString(state_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'state' field");
        return ESP_FAIL;
    }

    pw_agent_state_t state = pw_agent_state_from_string(state_json->valuestring);
    pw_agent_state_set(state);
    cJSON_Delete(root);

    pw_renderer_wake_display();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "state", pw_agent_state_to_string(state));
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_message(httpd_req_t *req)
{
    char body[1280];
    int ret = recv_full_body(req, body, sizeof(body));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *text_json = cJSON_GetObjectItem(root, "text");
    if (!text_json || !cJSON_IsString(text_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'text' field");
        return ESP_FAIL;
    }

    pw_msg_level_t level = PW_MSG_LEVEL_INFO;
    cJSON *level_json = cJSON_GetObjectItem(root, "level");
    if (level_json && cJSON_IsString(level_json)) {
        const char *lvl = level_json->valuestring;
        if (strcmp(lvl, "success") == 0) level = PW_MSG_LEVEL_SUCCESS;
        else if (strcmp(lvl, "warning") == 0) level = PW_MSG_LEVEL_WARNING;
        else if (strcmp(lvl, "error") == 0) level = PW_MSG_LEVEL_ERROR;
    }

    pw_renderer_show_message(text_json->valuestring, level);
    pw_renderer_wake_display();
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_background(httpd_req_t *req)
{
    char body[128];
    int ret = recv_full_body(req, body, sizeof(body));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *idx_json = cJSON_GetObjectItem(root, "index");
    if (!idx_json || !cJSON_IsNumber(idx_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'index' field");
        return ESP_FAIL;
    }

    int idx = (int)idx_json->valuedouble;
    cJSON_Delete(root);

    pw_renderer_set_background(idx);
    pw_renderer_wake_display();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "background", idx);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_reboot(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "Rebooting...");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);

    // Power cycle LCD and AI chip for a clean hardware-like reset
    vTaskDelay(pdMS_TO_TICKS(500));
    bsp_exp_io_set_level(BSP_PWR_LCD | BSP_PWR_AI_CHIP, 0);  // power off
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static void register_routes(httpd_handle_t server)
{
    httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,    .handler = handle_index },
        { .uri = "/style.css",    .method = HTTP_GET,    .handler = handle_css },
        { .uri = "/app.js",       .method = HTTP_GET,    .handler = handle_js },
        { .uri = "/api/status",   .method = HTTP_GET,    .handler = handle_api_status },
    };

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    httpd_uri_t agent_state_uri = { .uri = "/api/agent-state", .method = HTTP_PUT, .handler = handle_api_agent_state };
    httpd_register_uri_handler(server, &agent_state_uri);

    httpd_uri_t message_uri = { .uri = "/api/message", .method = HTTP_POST, .handler = handle_api_message };
    httpd_register_uri_handler(server, &message_uri);

    httpd_uri_t reboot_uri = { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_api_reboot };
    httpd_register_uri_handler(server, &reboot_uri);

    httpd_uri_t bg_uri = { .uri = "/api/background", .method = HTTP_PUT, .handler = handle_api_background };
    httpd_register_uri_handler(server, &bg_uri);
}

static void init_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("zidane");
    mdns_instance_name_set("Zidane Watcher");
    mdns_service_add(NULL, "_http", "_tcp", PW_WEB_SERVER_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: zidane.local");
}

void pw_web_server_start(void)
{
    init_mdns();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = PW_WEB_SERVER_PORT;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
        return;
    }

    register_routes(s_server);
    ESP_LOGI(TAG, "Web server started on port %d", PW_WEB_SERVER_PORT);
}

void pw_web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
