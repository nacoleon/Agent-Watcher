#ifndef POKEWATCHER_CONFIG_H
#define POKEWATCHER_CONFIG_H

// Display
#define PW_DISPLAY_WIDTH      412
#define PW_DISPLAY_HEIGHT     412
#define PW_SPRITE_SCALE       4
#define PW_SPRITE_SRC_SIZE    32
#define PW_SPRITE_DST_SIZE    (PW_SPRITE_SRC_SIZE * PW_SPRITE_SCALE)  // 128
#define PW_ANIM_FPS           10

// Agent state timeouts (auto-revert to IDLE)
#define PW_GREETING_TIMEOUT_MS   10000   // 10 seconds
#define PW_ALERT_TIMEOUT_MS      60000   // 60 seconds

// Dialog
#define PW_DIALOG_MAX_TEXT     1001  // paginated at 80 chars, knob scrolls pages
#define PW_DIALOG_DISPLAY_MS   10000   // 10 seconds
#define PW_DIALOG_FADE_MS      500

// Event queue
#define PW_EVENT_QUEUE_SIZE    16

// Web server
#define PW_WEB_SERVER_PORT     80

// SD card paths
#define PW_SD_MOUNT_POINT      "/sdcard"
#define PW_SD_CHARACTER_DIR    "/sdcard/characters"

// Backgrounds sheet (Pictlogica FF battle backgrounds)
#define PW_BG_SHEET_COLS       6
#define PW_BG_SHEET_ROWS       14
#define PW_BG_CELL_W           240
#define PW_BG_CELL_H           170
#define PW_BG_SHEET_W          1447
#define PW_BG_SHEET_H          2453
#define PW_BG_SEPARATOR        1      // 1px white separator between cells

// NVS namespace
#define PW_NVS_NAMESPACE       "pokewatcher"

// Display sleep
#define PW_DISPLAY_SLEEP_TIMEOUT_MS  180000  // 3 minutes
#define PW_SLEEP_STATE_BEFORE_MS      15000  // trigger sleeping state 15s before display off
#define PW_SLEEP_AFTER_STATE_MS       15000  // display off 15s after sleeping state is set

// WiFi defaults (NVS overrides at runtime)
#define PW_WIFI_SSID_DEFAULT       "YOUR_WIFI_SSID"
#define PW_WIFI_PASSWORD_DEFAULT   "YOUR_WIFI_PASSWORD"

// LLM (kept for potential future use)
#define PW_LLM_MAX_RESPONSE_LEN   512

// Voice input (push-to-talk)
#define PW_VOICE_RECORD_MS     10000          // 10 seconds recording
#define PW_VOICE_SAMPLE_RATE   16000
#define PW_VOICE_SAMPLE_BITS   16
#define PW_VOICE_BUF_SIZE      (PW_VOICE_SAMPLE_RATE * (PW_VOICE_SAMPLE_BITS / 8) * (PW_VOICE_RECORD_MS / 1000))  // 160000 bytes

#endif
