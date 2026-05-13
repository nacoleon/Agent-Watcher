#include "time_sync.h"

#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "time_sync";

#define PW_REBOOT_HOUR_LOCAL    3       // 03:00 local
#define PW_REBOOT_GUARD_S       3600    // skip reboot if we've been up < 1h
#define PW_FALLBACK_REBOOT_S    (25 * 3600)  // 25h uptime fallback if never synced
#define PW_SCHEDULER_PERIOD_MS  60000   // check once per minute

// Pacific time with DST transitions per US rules
#define PW_POSIX_TZ "PST8PDT,M3.2.0,M11.1.0"

static atomic_bool s_synced = ATOMIC_VAR_INIT(false);

static void on_sntp_sync(struct timeval *tv)
{
    atomic_store(&s_synced, true);
    char buf[32];
    time_t now = tv->tv_sec;
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local);
    ESP_LOGI(TAG, "Time synchronized: %s %s", buf, tzname[tm_local.tm_isdst > 0 ? 1 : 0]);
}

static void init_sntp(void)
{
    setenv("TZ", PW_POSIX_TZ, 1);
    tzset();

    if (esp_sntp_enabled()) return;

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (TZ=%s)", PW_POSIX_TZ);
}

bool pw_time_is_synced(void)
{
    return atomic_load(&s_synced);
}

time_t pw_time_next_reboot_epoch(void)
{
    if (!atomic_load(&s_synced)) return 0;

    time_t now = time(NULL);
    struct tm next;
    localtime_r(&now, &next);
    next.tm_hour = PW_REBOOT_HOUR_LOCAL;
    next.tm_min = 0;
    next.tm_sec = 0;
    time_t today_3am = mktime(&next);
    if (today_3am <= now) {
        // Already past 3am today — next reboot is tomorrow's 3am
        today_3am += 24 * 3600;
    }
    return today_3am;
}

static void reboot_scheduler_task(void *arg)
{
    const int64_t boot_us = esp_timer_get_time();

    // Give WiFi + SNTP a moment to come online
    vTaskDelay(pdMS_TO_TICKS(10000));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PW_SCHEDULER_PERIOD_MS));

        int64_t uptime_s = (esp_timer_get_time() - boot_us) / 1000000;

        if (pw_time_is_synced()) {
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);

            if (timeinfo.tm_hour == PW_REBOOT_HOUR_LOCAL &&
                timeinfo.tm_min == 0 &&
                uptime_s > PW_REBOOT_GUARD_S) {
                ESP_LOGW(TAG, "Scheduled 03:00 %s reboot (uptime %llds)",
                         tzname[timeinfo.tm_isdst > 0 ? 1 : 0],
                         (long long)uptime_s);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else if (uptime_s > PW_FALLBACK_REBOOT_S) {
            ESP_LOGW(TAG, "Fallback reboot — no SNTP sync after %lldh uptime",
                     (long long)(uptime_s / 3600));
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}

void pw_time_sync_init(void)
{
    init_sntp();
    xTaskCreate(reboot_scheduler_task, "reboot_scheduler", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "Reboot scheduler started (03:00 local daily, fallback at 25h uptime)");
}
