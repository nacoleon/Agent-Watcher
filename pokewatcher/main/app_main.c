#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sensecap-watcher.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "config.h"
#include "event_queue.h"
#include "agent_state.h"
#include "renderer.h"
#include "himax_task.h"
#include "web_server.h"

static const char *TAG = "pokewatcher";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Compile-time defaults — NVS overrides if set via web dashboard or serial
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = PW_WIFI_SSID_DEFAULT,
            .password = PW_WIFI_PASSWORD_DEFAULT,
        },
    };

    // Override with NVS values if they exist (set at runtime)
    nvs_handle_t nvs;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(wifi_config.sta.ssid);
        if (nvs_get_str(nvs, "wifi_ssid", (char *)wifi_config.sta.ssid, &len) == ESP_OK) {
            len = sizeof(wifi_config.sta.password);
            nvs_get_str(nvs, "wifi_pass", (char *)wifi_config.sta.password, &len);
        }
        nvs_close(nvs);
    }

    if (wifi_config.sta.ssid[0] != '\0') {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi connecting to %s...", wifi_config.sta.ssid);
    } else {
        ESP_LOGW(TAG, "No WiFi credentials configured. Web dashboard will not be available until configured.");
    }
}

static sdmmc_card_t *s_sd_card = NULL;

static void init_sdcard(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BSP_SD_SPI_NUM;
    host.max_freq_khz = 400;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BSP_SD_SPI_CS;
    slot_config.host_id = host.slot;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount(PW_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGW(TAG, "Continuing without SD card — sprites won't load");
        s_sd_card = NULL;
    } else {
        ESP_LOGI(TAG, "SD card mounted at %s", PW_SD_MOUNT_POINT);
        sdmmc_card_print_info(stdout, s_sd_card);
    }
}

static void deinit_sdcard(void)
{
    if (s_sd_card) {
        esp_vfs_fat_sdcard_unmount(PW_SD_MOUNT_POINT, s_sd_card);
        s_sd_card = NULL;
        ESP_LOGI(TAG, "SD card unmounted — SPI2 free for Himax");
    }
}

// SSCMA client handle — created before SD card, used by himax task
sscma_client_handle_t s_sscma_client = NULL;

static void on_state_changed(pw_agent_state_t old_state, pw_agent_state_t new_state)
{
    pw_renderer_set_state(new_state);
    // Don't wake display for sleep-related states
    if (new_state != PW_STATE_SLEEPING && new_state != PW_STATE_DOWN) {
        pw_renderer_wake_display();
    }
    ESP_LOGI(TAG, "State changed: %s -> %s",
             pw_agent_state_to_string(old_state),
             pw_agent_state_to_string(new_state));
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Zidane Watcher v2 starting ===");

    // [1/7] NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/7] NVS initialized");

    // [2/7] Event queue
    pw_event_queue_init();
    ESP_LOGI(TAG, "[2/7] Event queue initialized");

    // [3/7] Agent state engine
    pw_agent_state_init();
    ESP_LOGI(TAG, "[3/7] Agent state initialized");

    // [4/7] IO expander + renderer
    bsp_io_expander_init();
    vTaskDelay(pdMS_TO_TICKS(100));
    // Mute speaker to prevent idle amp pops (unmute with bsp_codec_mute_set(false) when audio needed)
    bsp_codec_init();
    bsp_codec_mute_set(true);
    bsp_rgb_init();
    ESP_LOGI(TAG, "[4/7] IO expander ready, LCD powered, speaker muted, RGB LED initialized");

    pw_renderer_init();
    ESP_LOGI(TAG, "[4/7] Renderer initialized");

    // [5/7] SD card — use BSP init (same as monitor example)
    esp_err_t sd_err = bsp_sdcard_init_default();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed: %s — continuing without sprites", esp_err_to_name(sd_err));
    }
    ESP_LOGI(TAG, "[5/7] SD card initialized");

    if (!pw_renderer_load_character("zidane")) {
        ESP_LOGW(TAG, "Failed to load Zidane sprites from SD card");
    }

    // [5b/7] SD card done — unmount and power off to free SPI2 MISO for Himax
    // Root cause: SD card in SPI mode doesn't tri-state MISO when CS is high,
    // causing bus contention with Himax camera on the same SPI2 bus.
    // Fix: power off SD card after loading sprites so it physically releases MISO.
    bsp_sdcard_deinit_default();
    esp_io_expander_set_dir(bsp_io_expander_init(), BSP_PWR_SDCARD, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(bsp_io_expander_init(), BSP_PWR_SDCARD, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    // Hold SD CS high to prevent any stray bus activity
    gpio_config_t sd_cs_conf = {
        .pin_bit_mask = (1ULL << BSP_SD_SPI_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sd_cs_conf);
    gpio_set_level(BSP_SD_SPI_CS, 1);
    // Also power cycle Himax to clear stale inference data from previous session
    esp_io_expander_handle_t io_exp = bsp_io_expander_init();
    esp_io_expander_set_dir(io_exp, BSP_PWR_AI_CHIP, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_exp, BSP_PWR_AI_CHIP, 0);  // Power OFF Himax
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_io_expander_set_level(io_exp, BSP_PWR_AI_CHIP, 1);  // Power ON Himax
    vTaskDelay(pdMS_TO_TICKS(500));  // Wait for Himax boot
    ESP_LOGI(TAG, "[5b/7] SD powered off + Himax power cycled — SPI2 MISO free");

    // [7/7] WiFi + web server
    init_wifi();
    pw_web_server_start();
    ESP_LOGI(TAG, "[7/7] WiFi + web server initialized");

    // Register callbacks
    pw_agent_state_set_change_cb(on_state_changed);

    // Start tasks
    // pw_himax_task_start();  // SPI2 fix works (SD card powered off) but SSCMA library
    // crashes in heap allocator (tlsf.c:266) when processing stale autorun data from
    // Himax's previous session. Need to either: (a) flash Himax with no-autorun firmware,
    // (b) patch SSCMA library's reply parsing to handle unsolicited events safely, or
    // (c) find a way to drain the RX buffer before SSCMA internal tasks start.
    pw_agent_state_task_start();
    pw_renderer_task_start();

    ESP_LOGI(TAG, "=== Zidane Watcher v2 running ===");
    ESP_LOGI(TAG, "Dashboard: http://zidane.local");
}
