#include "web_server.h"
#include "roster.h"
#include "mood_engine.h"
#include "llm_task.h"
#include "event_queue.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "pw_web";
static httpd_handle_t s_server = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t roster_html_start[] asm("_binary_roster_html_start");
extern const uint8_t roster_html_end[]   asm("_binary_roster_html_end");
extern const uint8_t settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t settings_html_end[]   asm("_binary_settings_html_end");
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
static esp_err_t handle_roster_page(httpd_req_t *req) { return serve_embedded(req, roster_html_start, roster_html_end, "text/html"); }
static esp_err_t handle_settings_page(httpd_req_t *req) { return serve_embedded(req, settings_html_start, settings_html_end, "text/html"); }
static esp_err_t handle_css(httpd_req_t *req) { return serve_embedded(req, style_css_start, style_css_end, "text/css"); }
static esp_err_t handle_js(httpd_req_t *req) { return serve_embedded(req, app_js_start, app_js_end, "application/javascript"); }

static esp_err_t handle_api_status(httpd_req_t *req)
{
    pw_mood_state_t mood = pw_mood_engine_get_state();
    const char *active = pw_roster_get_active_id();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active_pokemon", active ? active : "");
    cJSON_AddStringToObject(root, "mood", pw_mood_to_string(mood.current_mood));
    cJSON_AddBoolToObject(root, "person_present", mood.person_present);
    cJSON_AddNumberToObject(root, "evolution_seconds", mood.evolution_seconds);
    cJSON_AddStringToObject(root, "last_commentary", pw_llm_get_last_commentary());

    if (active) {
        pw_pokemon_def_t def;
        if (pw_pokemon_load_def(active, &def)) {
            cJSON_AddNumberToObject(root, "evolution_threshold_hours", def.evolution_hours);
            cJSON_AddStringToObject(root, "evolves_to", def.evolves_to);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_roster_get(httpd_req_t *req)
{
    pw_roster_t roster = pw_roster_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active_id", roster.active_id);

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < roster.count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", roster.entries[i].id);
        cJSON_AddNumberToObject(entry, "evolution_seconds", roster.entries[i].evolution_seconds);

        pw_pokemon_def_t def;
        if (pw_pokemon_load_def(roster.entries[i].id, &def)) {
            cJSON_AddStringToObject(entry, "name", def.name);
            cJSON_AddStringToObject(entry, "evolves_to", def.evolves_to);
            cJSON_AddNumberToObject(entry, "evolution_hours", def.evolution_hours);
        }
        cJSON_AddItemToArray(entries, entry);
    }

    char available[20][PW_POKEMON_ID_LEN];
    int avail_count = pw_pokemon_scan_available(available, 20);
    cJSON *avail_arr = cJSON_AddArrayToObject(root, "available");
    for (int i = 0; i < avail_count; i++) {
        bool in_roster = false;
        for (int j = 0; j < roster.count; j++) {
            if (strcmp(roster.entries[j].id, available[i]) == 0) {
                in_roster = true;
                break;
            }
        }
        if (!in_roster) {
            cJSON_AddItemToArray(avail_arr, cJSON_CreateString(available[i]));
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_roster_add(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (!id || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'id'");
        return ESP_FAIL;
    }

    bool ok = pw_roster_add(id->valuestring);
    cJSON_Delete(root);

    if (ok) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to add");
    }
    return ESP_OK;
}

static esp_err_t handle_api_roster_delete(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *id = strrchr(uri, '/');
    if (!id || strlen(id) < 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ID in URI");
        return ESP_FAIL;
    }
    id++;

    bool ok = pw_roster_remove(id);
    if (ok) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not in roster");
    }
    return ESP_OK;
}

static esp_err_t handle_api_roster_active(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (!id || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'id'");
        return ESP_FAIL;
    }

    bool ok = pw_roster_set_active(id->valuestring);
    cJSON_Delete(root);

    if (ok) {
        pw_event_t evt = { .type = PW_EVENT_ROSTER_CHANGE };
        strncpy(evt.data.roster.pokemon_id, id->valuestring, sizeof(evt.data.roster.pokemon_id) - 1);
        pw_event_send(&evt);
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to set active");
    }
    return ESP_OK;
}

static esp_err_t handle_api_settings_get(httpd_req_t *req)
{
    pw_mood_config_t mood_cfg = pw_mood_engine_get_config();
    pw_llm_config_t llm_cfg = pw_llm_get_config();

    cJSON *root = cJSON_CreateObject();
    cJSON *mood = cJSON_AddObjectToObject(root, "mood");
    cJSON_AddNumberToObject(mood, "excited_duration_ms", mood_cfg.excited_duration_ms);
    cJSON_AddNumberToObject(mood, "overjoyed_duration_ms", mood_cfg.overjoyed_duration_ms);
    cJSON_AddNumberToObject(mood, "curious_timeout_ms", mood_cfg.curious_timeout_ms);
    cJSON_AddNumberToObject(mood, "lonely_timeout_ms", mood_cfg.lonely_timeout_ms);

    cJSON *llm = cJSON_AddObjectToObject(root, "llm");
    cJSON_AddStringToObject(llm, "endpoint", llm_cfg.endpoint);
    cJSON_AddStringToObject(llm, "model", llm_cfg.model);
    cJSON_AddBoolToObject(llm, "api_key_set", llm_cfg.api_key[0] != '\0');

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_settings_put(httpd_req_t *req)
{
    char *buf = malloc(1024);
    if (!buf) return ESP_FAIL;
    int ret = httpd_req_recv(req, buf, 1023);
    if (ret <= 0) { free(buf); return ESP_FAIL; }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *mood = cJSON_GetObjectItem(root, "mood");
    if (mood) {
        pw_mood_config_t cfg = pw_mood_engine_get_config();
        cJSON *val;
        if ((val = cJSON_GetObjectItem(mood, "curious_timeout_ms"))) cfg.curious_timeout_ms = (uint32_t)val->valuedouble;
        if ((val = cJSON_GetObjectItem(mood, "lonely_timeout_ms"))) cfg.lonely_timeout_ms = (uint32_t)val->valuedouble;
        if ((val = cJSON_GetObjectItem(mood, "excited_duration_ms"))) cfg.excited_duration_ms = (uint32_t)val->valuedouble;
        if ((val = cJSON_GetObjectItem(mood, "overjoyed_duration_ms"))) cfg.overjoyed_duration_ms = (uint32_t)val->valuedouble;
        pw_mood_engine_set_config(&cfg);
    }

    cJSON *llm = cJSON_GetObjectItem(root, "llm");
    if (llm) {
        pw_llm_config_t cfg = pw_llm_get_config();
        cJSON *val;
        if ((val = cJSON_GetObjectItem(llm, "endpoint"))) strncpy(cfg.endpoint, val->valuestring, PW_LLM_ENDPOINT_LEN - 1);
        if ((val = cJSON_GetObjectItem(llm, "api_key"))) strncpy(cfg.api_key, val->valuestring, PW_LLM_API_KEY_LEN - 1);
        if ((val = cJSON_GetObjectItem(llm, "model"))) strncpy(cfg.model, val->valuestring, PW_LLM_MODEL_LEN - 1);
        pw_llm_set_config(&cfg);
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_api_timeline(httpd_req_t *req)
{
    char *json = pw_llm_get_history_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "[]");
    free(json);
    return ESP_OK;
}

static void register_routes(httpd_handle_t server)
{
    httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,    .handler = handle_index },
        { .uri = "/roster",       .method = HTTP_GET,    .handler = handle_roster_page },
        { .uri = "/settings",     .method = HTTP_GET,    .handler = handle_settings_page },
        { .uri = "/style.css",    .method = HTTP_GET,    .handler = handle_css },
        { .uri = "/app.js",       .method = HTTP_GET,    .handler = handle_js },
        { .uri = "/api/status",        .method = HTTP_GET,    .handler = handle_api_status },
        { .uri = "/api/roster",        .method = HTTP_GET,    .handler = handle_api_roster_get },
        { .uri = "/api/roster",        .method = HTTP_POST,   .handler = handle_api_roster_add },
        { .uri = "/api/roster/*",      .method = HTTP_DELETE, .handler = handle_api_roster_delete },
        { .uri = "/api/roster/active", .method = HTTP_PUT,    .handler = handle_api_roster_active },
        { .uri = "/api/settings",      .method = HTTP_GET,    .handler = handle_api_settings_get },
        { .uri = "/api/settings",      .method = HTTP_PUT,    .handler = handle_api_settings_put },
        { .uri = "/api/timeline",      .method = HTTP_GET,    .handler = handle_api_timeline },
    };

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}

static void init_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("pokewatcher");
    mdns_instance_name_set("PokéWatcher Dashboard");
    mdns_service_add(NULL, "_http", "_tcp", PW_WEB_SERVER_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: pokewatcher.local");
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
