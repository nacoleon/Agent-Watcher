#ifndef POKEWATCHER_CONFIG_H
#define POKEWATCHER_CONFIG_H

// Display
#define PW_DISPLAY_WIDTH      412
#define PW_DISPLAY_HEIGHT     412
#define PW_SPRITE_SCALE       4
#define PW_SPRITE_SRC_SIZE    32
#define PW_SPRITE_DST_SIZE    (PW_SPRITE_SRC_SIZE * PW_SPRITE_SCALE)  // 128
#define PW_ANIM_FPS           10

// Mood timers (milliseconds)
#define PW_EXCITED_DURATION_MS     10000   // 10 seconds
#define PW_OVERJOYED_DURATION_MS   15000   // 15 seconds
#define PW_CURIOUS_TIMEOUT_MS      300000  // 5 minutes
#define PW_LONELY_TIMEOUT_MS       900000  // 15 minutes

// Evolution
#define PW_DEFAULT_EVOLUTION_HOURS 24

// LLM
#define PW_LLM_MAX_RESPONSE_LEN   512

// Event queue
#define PW_EVENT_QUEUE_SIZE        16

// Web server
#define PW_WEB_SERVER_PORT         80

// SD card paths
#define PW_SD_MOUNT_POINT          "/sdcard"
#define PW_SD_POKEMON_DIR          "/sdcard/pokemon"
#define PW_SD_BACKGROUND_DIR       "/sdcard/background"

// NVS namespace
#define PW_NVS_NAMESPACE           "pokewatcher"

// WiFi defaults (NVS overrides at runtime via web dashboard)
#define PW_WIFI_SSID_DEFAULT       "YOUR_WIFI_SSID"
#define PW_WIFI_PASSWORD_DEFAULT   "YOUR_WIFI_PASSWORD"

#endif
