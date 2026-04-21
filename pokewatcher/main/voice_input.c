#include "voice_input.h"
#include "config.h"
#include "dialog.h"
#include "agent_state.h"
#include "sensecap-watcher.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
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
static volatile bool s_stop_requested = false;

// Audio buffer stored in PSRAM — served via GET /api/audio
static uint8_t *s_audio_buf = NULL;
static size_t   s_audio_size = 0;       // total WAV size (header + PCM)
static volatile bool s_audio_ready = false;

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
    s_stop_requested = false;
    ESP_LOGI(TAG, "Recording started (%d ms)", PW_VOICE_RECORD_MS);

    // Save current state to restore later
    pw_agent_state_data_t state_data = pw_agent_state_get();
    pw_agent_state_t prev_state = state_data.current_state;

    // Visual feedback: blue LED + reporting state
    bsp_rgb_set(0, 0, 26);  // blue
    pw_agent_state_set(PW_STATE_REPORTING);

    // Free previous audio buffer if not yet consumed
    if (s_audio_buf) {
        heap_caps_free(s_audio_buf);
        s_audio_buf = NULL;
        s_audio_size = 0;
        s_audio_ready = false;
    }

    // Allocate buffer for WAV header + PCM data in PSRAM
    size_t buf_size = sizeof(wav_header_t) + PW_VOICE_BUF_SIZE;
    uint8_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes in PSRAM for audio", buf_size);
        bsp_rgb_set(26, 0, 0);  // red = error
        vTaskDelay(pdMS_TO_TICKS(500));
        bsp_rgb_set(0, 0, 0);
        pw_agent_state_set(prev_state);
        s_recording = false;
        vTaskDelete(NULL);
        return;
    }

    // Record audio via codec device (write PCM after the header space)
    int16_t *pcm_start = (int16_t *)(buf + sizeof(wav_header_t));
    size_t total_read = 0;
    size_t chunk_size = 1024;  // bytes per read
    int64_t start_time = esp_timer_get_time();
    int64_t end_time = start_time + (int64_t)PW_VOICE_RECORD_MS * 1000;

    while (!s_stop_requested && esp_timer_get_time() < end_time && total_read < PW_VOICE_BUF_SIZE) {
        size_t to_read = chunk_size;
        if (total_read + to_read > PW_VOICE_BUF_SIZE) {
            to_read = PW_VOICE_BUF_SIZE - total_read;
        }
        esp_err_t ret = bsp_get_feed_data(false, (int16_t *)((uint8_t *)pcm_start + total_read), to_read);
        if (ret == ESP_OK) {
            total_read += to_read;
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

    // Amplify: mic signal is very quiet (~64 peak), boost by 16x with clipping
    int16_t *samples = (int16_t *)pcm_start;
    size_t num_samples = total_read / 2;
    for (size_t i = 0; i < num_samples; i++) {
        int32_t val = (int32_t)samples[i] * 4;
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        samples[i] = (int16_t)val;
    }

    if (total_read < 16000) {  // less than 0.5 seconds
        ESP_LOGW(TAG, "Audio too short (%zu bytes), discarding", total_read);
        heap_caps_free(buf);
        bsp_rgb_set(26, 0, 0);  // red flash
        vTaskDelay(pdMS_TO_TICKS(500));
        bsp_rgb_set(0, 0, 0);
        pw_agent_state_set(prev_state);
        s_recording = false;
        vTaskDelete(NULL);
        return;
    }

    // Write WAV header at the start of the buffer
    build_wav_header((wav_header_t *)buf, total_read);

    // Store buffer for serving via /api/audio
    s_audio_buf = buf;
    s_audio_size = sizeof(wav_header_t) + total_read;
    s_audio_ready = true;

    ESP_LOGI(TAG, "Audio ready for pickup (%zu bytes WAV)", s_audio_size);

    // Green flash = recording done
    bsp_rgb_set(0, 26, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    bsp_rgb_set(0, 0, 0);

    pw_agent_state_set(prev_state);  // restore state
    s_recording = false;

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

bool pw_voice_audio_ready(void)
{
    return s_audio_ready;
}

const uint8_t *pw_voice_get_audio(size_t *out_size)
{
    if (!s_audio_ready || !s_audio_buf) {
        *out_size = 0;
        return NULL;
    }
    *out_size = s_audio_size;
    return s_audio_buf;
}

void pw_voice_clear_audio(void)
{
    s_audio_ready = false;
    if (s_audio_buf) {
        heap_caps_free(s_audio_buf);
        s_audio_buf = NULL;
        s_audio_size = 0;
    }
    ESP_LOGI(TAG, "Audio buffer cleared");
}

void pw_voice_init(void)
{
    // Boost mic hardware gain to max (42 dB, default is 27 dB)
    esp_codec_dev_handle_t mic = bsp_codec_microphone_get();
    if (mic) {
        esp_codec_dev_set_in_gain(mic, 42.0);
        ESP_LOGI(TAG, "Mic hardware gain set to 42 dB");
    }
    ESP_LOGI(TAG, "Voice input initialized (double-click knob to record)");
}

bool pw_voice_is_recording(void)
{
    return s_recording;
}

void pw_voice_request_stop(void)
{
    s_stop_requested = true;
}
