#include "llm_task.h"
#include "event_queue.h"
#include "mood_engine.h"
#include "roster.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "pw_llm";

static pw_llm_config_t s_config = {};
static char s_last_commentary[PW_LLM_MAX_RESPONSE_LEN] = "";

#define PW_LLM_HISTORY_SIZE 10
static char s_history[PW_LLM_HISTORY_SIZE][PW_LLM_MAX_RESPONSE_LEN];
static int s_history_index = 0;
static int s_history_count = 0;


static void save_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "llm_cfg", &s_config, sizeof(pw_llm_config_t));
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(pw_llm_config_t);
        esp_err_t err = nvs_get_blob(handle, "llm_cfg", &s_config, &len);
        nvs_close(handle);
        if (err != ESP_OK || len != sizeof(pw_llm_config_t)) {
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "LLM config blob size mismatch, resetting");
            }
            memset(&s_config, 0, sizeof(pw_llm_config_t));
        }
    }
}

static void add_to_history(const char *commentary)
{
    strncpy(s_history[s_history_index], commentary, PW_LLM_MAX_RESPONSE_LEN - 1);
    s_history_index = (s_history_index + 1) % PW_LLM_HISTORY_SIZE;
    if (s_history_count < PW_LLM_HISTORY_SIZE) {
        s_history_count++;
    }
}

typedef struct {
    char *buf;
    int len;
    int capacity;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp->len + evt->data_len < resp->capacity) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static bool call_llm(const char *prompt, char *response, int response_len)
{
    if (s_config.endpoint[0] == '\0' || s_config.api_key[0] == '\0') {
        ESP_LOGW(TAG, "LLM not configured");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", s_config.model);
    cJSON_AddNumberToObject(root, "max_tokens", 150);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *system_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content",
        "You are a Pokemon companion living on someone's desk. "
        "Respond in character as the Pokemon species given. "
        "Keep responses to 1-2 short sentences. Be cute and expressive. "
        "React to the mood transition described.");
    cJSON_AddItemToArray(messages, system_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", prompt);
    cJSON_AddItemToArray(messages, user_msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;

    http_response_t resp = {
        .buf = heap_caps_malloc(2048, MALLOC_CAP_DEFAULT),
        .len = 0,
        .capacity = 2048,
    };
    if (!resp.buf) { free(body); return false; }
    resp.buf[0] = '\0';

    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_config.api_key);

    esp_http_client_config_t http_config = {
        .url = s_config.endpoint,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    free(body);

    bool success = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            cJSON *resp_json = cJSON_Parse(resp.buf);
            if (resp_json) {
                cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
                if (choices && cJSON_GetArraySize(choices) > 0) {
                    cJSON *first = cJSON_GetArrayItem(choices, 0);
                    cJSON *message = cJSON_GetObjectItem(first, "message");
                    cJSON *content = cJSON_GetObjectItem(message, "content");
                    if (content && cJSON_IsString(content)) {
                        strncpy(response, content->valuestring, response_len - 1);
                        success = true;
                    }
                }
                cJSON_Delete(resp_json);
            }
        } else {
            ESP_LOGW(TAG, "LLM API returned status %d", status);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    heap_caps_free(resp.buf);
    return success;
}

void pw_llm_init(void)
{
    load_config();
    ESP_LOGI(TAG, "LLM engine initialized (endpoint=%s)",
             s_config.endpoint[0] ? s_config.endpoint : "not configured");
}

void pw_llm_set_config(const pw_llm_config_t *config)
{
    s_config = *config;
    save_config();
    ESP_LOGI(TAG, "LLM config updated");
}

pw_llm_config_t pw_llm_get_config(void)
{
    return s_config;
}

const char *pw_llm_get_last_commentary(void)
{
    return s_last_commentary;
}

char *pw_llm_get_history_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    int start = (s_history_count < PW_LLM_HISTORY_SIZE) ? 0 : s_history_index;
    for (int i = 0; i < s_history_count; i++) {
        int idx = (start + i) % PW_LLM_HISTORY_SIZE;
        if (s_history[idx][0] != '\0') {
            cJSON_AddItemToArray(arr, cJSON_CreateString(s_history[idx]));
        }
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

static void llm_task(void *arg)
{
    ESP_LOGI(TAG, "LLM task started");
    pw_mood_t last_known_mood = PW_MOOD_SLEEPY;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        pw_mood_state_t state = pw_mood_engine_get_state();
        if (state.current_mood == last_known_mood) {
            continue;
        }

        pw_mood_t old_mood = last_known_mood;
        last_known_mood = state.current_mood;

        const char *active = pw_roster_get_active_id();
        if (!active) continue;

        char prompt[256];
        snprintf(prompt, sizeof(prompt),
            "You are %s. Your mood just changed from %s to %s. "
            "You've been the active companion for %lu hours. "
            "Express how you feel about this change.",
            active,
            pw_mood_to_string(old_mood),
            pw_mood_to_string(state.current_mood),
            (unsigned long)(state.evolution_seconds / 3600));

        char response[PW_LLM_MAX_RESPONSE_LEN] = "";
        if (call_llm(prompt, response, sizeof(response))) {
            strncpy(s_last_commentary, response, sizeof(s_last_commentary) - 1);
            add_to_history(response);
            ESP_LOGI(TAG, "LLM commentary: %s", response);
        }
    }
}

void pw_llm_task_start(void)
{
    xTaskCreate(llm_task, "llm", 8192, NULL, 3, NULL);
}
