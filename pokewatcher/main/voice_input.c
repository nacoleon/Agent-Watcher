#include "voice_input.h"
#include "config.h"
#include "dialog.h"
#include "agent_state.h"
#include "sensecap-watcher.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "pw_voice";

// WAV header for 16kHz 16-bit mono PCM
typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t file_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_tag[4];
    uint32_t data_size;
} wav_header_t;

static volatile bool s_recording = false;

static void build_wav_header(wav_header_t *hdr, uint32_t data_size)
{
    memcpy(hdr->riff, "RIFF", 4);
    hdr->file_size = data_size + 36;
    memcpy(hdr->wave, "WAVE", 4);
    memcpy(hdr->fmt, "fmt ", 4);
    hdr->fmt_size = 16;
    hdr->audio_format = 1;  // PCM
    hdr->num_channels = 1;
    hdr->sample_rate = PW_VOICE_SAMPLE_RATE;
    hdr->byte_rate = PW_VOICE_SAMPLE_RATE * 1 * (PW_VOICE_SAMPLE_BITS / 8);
    hdr->block_align = 1 * (PW_VOICE_SAMPLE_BITS / 8);
    hdr->bits_per_sample = PW_VOICE_SAMPLE_BITS;
    memcpy(hdr->data_tag, "data", 4);
    hdr->data_size = data_size;
}

static void voice_record_task(void *arg)
{
    ESP_LOGI(TAG, "Recording started (%d ms)", PW_VOICE_RECORD_MS);

    // Save current state to restore later
    pw_agent_state_data_t state_data = pw_agent_state_get();
    pw_agent_state_t prev_state = state_data.current_state;

    // Visual feedback: blue LED + working state
    bsp_rgb_set(0, 0, 26);  // blue
    pw_agent_state_set(PW_STATE_WORKING);

    // Allocate audio buffer in PSRAM
    uint8_t *audio_buf = heap_caps_malloc(PW_VOICE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes in PSRAM for audio", PW_VOICE_BUF_SIZE);
        bsp_rgb_set(26, 0, 0);  // red = error
        vTaskDelay(pdMS_TO_TICKS(500));
        bsp_rgb_set(0, 0, 0);
        s_recording = false;
        vTaskDelete(NULL);
        return;
    }

    // Record audio via I2S
    size_t total_read = 0;
    size_t chunk_size = 1024;
    int64_t start_time = esp_timer_get_time();
    int64_t end_time = start_time + (int64_t)PW_VOICE_RECORD_MS * 1000;

    while (esp_timer_get_time() < end_time && total_read < PW_VOICE_BUF_SIZE) {
        size_t bytes_read = 0;
        size_t to_read = chunk_size;
        if (total_read + to_read > PW_VOICE_BUF_SIZE) {
            to_read = PW_VOICE_BUF_SIZE - total_read;
        }
        esp_err_t ret = bsp_i2s_read(audio_buf + total_read, to_read, &bytes_read, 100);
        if (ret == ESP_OK && bytes_read > 0) {
            total_read += bytes_read;
        }

        // Pulse blue LED (toggle brightness every 500ms)
        int64_t elapsed = (esp_timer_get_time() - start_time) / 1000;
        if ((elapsed / 500) % 2 == 0) {
            bsp_rgb_set(0, 0, 26);
        } else {
            bsp_rgb_set(0, 0, 10);
        }
    }

    ESP_LOGI(TAG, "Recording complete: %zu bytes captured", total_read);

    if (total_read < 16000) {  // less than 0.5 seconds
        ESP_LOGW(TAG, "Audio too short (%zu bytes), skipping upload", total_read);
        heap_caps_free(audio_buf);
        bsp_rgb_set(26, 0, 0);  // red flash
        vTaskDelay(pdMS_TO_TICKS(500));
        bsp_rgb_set(0, 0, 0);
        pw_agent_state_set(prev_state);
        s_recording = false;
        vTaskDelete(NULL);
        return;
    }

    // Build WAV header
    wav_header_t hdr;
    build_wav_header(&hdr, total_read);

    // LED yellow = uploading
    bsp_rgb_set(26, 18, 0);

    // HTTP POST to MCP server
    char url[64];
    snprintf(url, sizeof(url), "http://%s:%d/audio", PW_MCP_SERVER_IP, PW_MCP_SERVER_PORT);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,  // whisper transcription can take a few seconds
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

    // Write WAV header + PCM data
    int content_length = sizeof(wav_header_t) + total_read;
    esp_http_client_open(client, content_length);
    esp_http_client_write(client, (const char *)&hdr, sizeof(wav_header_t));
    esp_http_client_write(client, (const char *)audio_buf, total_read);

    int status = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);

    if (status >= 0 && http_status == 200) {
        ESP_LOGI(TAG, "Audio uploaded successfully (HTTP %d)", http_status);
        // Green flash = success
        bsp_rgb_set(0, 26, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGE(TAG, "Audio upload failed (HTTP %d, fetch=%d)", http_status, status);
        // Red flash x3 = error
        for (int i = 0; i < 3; i++) {
            bsp_rgb_set(26, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            bsp_rgb_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(audio_buf);

    bsp_rgb_set(0, 0, 0);  // LED off
    pw_agent_state_set(prev_state);  // restore state
    s_recording = false;

    ESP_LOGI(TAG, "Voice input task complete");
    vTaskDelete(NULL);
}

void pw_voice_tick(void)
{
    if (s_recording) return;  // already recording

    if (pw_dialog_consume_double_click()) {
        s_recording = true;
        ESP_LOGI(TAG, "Double-click detected — starting voice recording");
        xTaskCreate(voice_record_task, "voice_rec", 4096, NULL, 5, NULL);
    }
}

void pw_voice_init(void)
{
    ESP_LOGI(TAG, "Voice input initialized (double-click knob to record)");
}
