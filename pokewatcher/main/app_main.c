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

static void init_sdcard(void)
{

    // Try to mount regardless of detect pin — some cards don't trigger it
    // Mount SD card via SPI (same as bsp_sdcard_init but without ESP_ERROR_CHECK)
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BSP_SD_SPI_NUM;
    host.max_freq_khz = 400;  // Slow down for old SD v1 cards
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BSP_SD_SPI_CS;
    slot_config.host_id = host.slot;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(PW_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGW(TAG, "Continuing without SD card — sprites won't load");
    } else {
        ESP_LOGI(TAG, "SD card mounted at %s", PW_SD_MOUNT_POINT);
        sdmmc_card_print_info(stdout, card);
    }
}

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

    // Init Himax SSCMA client AFTER renderer — renderer allocates DMA bounce buffer
    // from unfragmented heap. SSCMA registration on SPI2 can fragment DMA memory.
    pw_himax_early_init();

    // [5/7] SD card
    init_sdcard();
    ESP_LOGI(TAG, "[5/7] SD card initialized");

    // [6/7] Load Zidane character
    if (!pw_renderer_load_character("zidane")) {
        ESP_LOGW(TAG, "Failed to load Zidane sprites from SD card");
    }

    // [7/7] WiFi + web server
    init_wifi();
    pw_web_server_start();
    ESP_LOGI(TAG, "[7/7] WiFi + web server initialized");

    // Register callbacks
    pw_agent_state_set_change_cb(on_state_changed);

    // Start tasks
    pw_himax_task_start();  // Auto-flashes firmware + person model from SD card if Himax not responding
    pw_agent_state_task_start();
    pw_renderer_task_start();

    ESP_LOGI(TAG, "=== Zidane Watcher v2 running ===");
    ESP_LOGI(TAG, "Dashboard: http://zidane.local");
}
