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
#include "mood_engine.h"
#include "roster.h"
#include "renderer.h"
#include "himax_task.h"
#include "llm_task.h"
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

static void on_mood_changed(pw_mood_t old_mood, pw_mood_t new_mood)
{
    ESP_LOGI(TAG, "Mood changed: %s -> %s", pw_mood_to_string(old_mood), pw_mood_to_string(new_mood));
    pw_renderer_set_mood(new_mood);
}

static void on_roster_change(const char *pokemon_id)
{
    ESP_LOGI(TAG, "Roster change: loading %s", pokemon_id);
    pw_renderer_load_pokemon(pokemon_id);
}

static void on_evolution_triggered(void)
{
    const char *active = pw_roster_get_active_id();
    if (active) {
        pw_pokemon_def_t def;
        if (pw_pokemon_load_def(active, &def) && def.evolves_to[0] != '\0') {
            pw_roster_evolve_active();
            pw_renderer_play_evolution(def.evolves_to);
            ESP_LOGI(TAG, "Evolution complete: %s -> %s", active, def.evolves_to);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== PokéWatcher v1 starting ===");

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/8] NVS initialized");

    // 2. Initialize event queue
    pw_event_queue_init();
    ESP_LOGI(TAG, "[2/8] Event queue initialized");

    // 3. Initialize roster (loads from NVS, SD card not needed yet)
    pw_roster_init();
    ESP_LOGI(TAG, "[3/8] Roster initialized");

    // 4. Initialize mood engine
    pw_mood_engine_init();
    ESP_LOGI(TAG, "[4/8] Mood engine initialized");

    // 5. Initialize LLM
    pw_llm_init();
    ESP_LOGI(TAG, "[5/8] LLM engine initialized");

    // 6. Initialize display & renderer
    // IO expander must be up first — it powers the LCD (BSP_PWR_LCD) and SD card (BSP_PWR_SDCARD).
    // bsp_lvgl_init sends SPI init commands to the LCD controller; if the LCD has no power,
    // those commands are lost and the display stays off.
    // NOTE: Do NOT call bsp_spi_bus_init() here — it must run inside bsp_lvgl_init() in
    // the correct order relative to backlight and panel init. The idempotent guard would
    // cause it to skip the second call, breaking the init sequence.
    bsp_io_expander_init();
    vTaskDelay(pdMS_TO_TICKS(100));  // Let LCD power stabilize
    ESP_LOGI(TAG, "[6/8] IO expander ready, LCD powered");

    pw_renderer_init();
    ESP_LOGI(TAG, "[6/8] Renderer initialized");

    // 7. Mount SD card (after renderer, so BSP SPI bus is fully set up)
    init_sdcard();
    ESP_LOGI(TAG, "[7/8] SD card initialized");

    // Auto-add Pikachu if roster is empty (first boot with SD card)
    const char *active = pw_roster_get_active_id();
    if (!active) {
        if (pw_roster_add("pikachu")) {
            active = pw_roster_get_active_id();
            ESP_LOGI(TAG, "Auto-added Pikachu to roster");
        }
    }

    // Load active Pokemon sprites
    if (active) {
        pw_renderer_load_pokemon(active);
    }

    // 8. Initialize WiFi & start web server
    init_wifi();
    pw_web_server_start();
    ESP_LOGI(TAG, "[8/8] WiFi + web server initialized");

    // Register callbacks before starting tasks
    pw_mood_engine_set_change_cb(on_mood_changed);
    pw_mood_engine_set_roster_cb(on_roster_change);
    pw_mood_engine_set_evolution_cb(on_evolution_triggered);

    // Start all tasks
    pw_himax_task_start();
    pw_mood_engine_task_start();
    pw_llm_task_start();
    pw_renderer_task_start();

    ESP_LOGI(TAG, "=== PokéWatcher v1 running ===");
    ESP_LOGI(TAG, "Active Pokemon: %s", active ? active : "none");
    ESP_LOGI(TAG, "Dashboard: http://pokewatcher.local");
}
