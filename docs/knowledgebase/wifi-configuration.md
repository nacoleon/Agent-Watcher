# WiFi Configuration

## How It Works

WiFi credentials are set as compile-time defaults in `pokewatcher/main/config.h`. At boot, the firmware loads these defaults, then checks NVS for runtime overrides (set via the web dashboard). If NVS has credentials, they take precedence.

## Changing WiFi Credentials

### Edit config.h
`pokewatcher/main/config.h` — change these two lines:

```c
#define PW_WIFI_SSID_DEFAULT       "YourNetworkName"
#define PW_WIFI_PASSWORD_DEFAULT   "YourPassword"
```

### Build and Flash
```bash
# Copy changed file to build dir
cp "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/main/config.h" /tmp/pokewatcher-build/main/config.h

# Build and flash (no need for full rebuild — config.h triggers recompile of dependents)
bash -c 'export IDF_PATH="/Users/nacoleon/esp/esp-idf" && . "$IDF_PATH/export.sh" 2>/dev/null && cd /tmp/pokewatcher-build && idf.py build 2>&1 | tail -3 && idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash 2>&1 | tail -3'
```

### Verify Connection
Check the serial log for:
```
WiFi connecting to YourNetworkName...
WiFi connected, IP: 192.168.x.x
```

If you see `WiFi disconnected, reconnecting...` looping, the password is wrong or the router is rejecting the connection.

## How the Code Works

`pokewatcher/main/app_main.c` in `init_wifi()`:

```c
// 1. Start with compile-time defaults from config.h
wifi_config_t wifi_config = {
    .sta = {
        .ssid = PW_WIFI_SSID_DEFAULT,
        .password = PW_WIFI_PASSWORD_DEFAULT,
    },
};

// 2. Override with NVS values if they exist (set via web dashboard)
nvs_handle_t nvs;
if (nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
    size_t len = sizeof(wifi_config.sta.ssid);
    if (nvs_get_str(nvs, "wifi_ssid", (char *)wifi_config.sta.ssid, &len) == ESP_OK) {
        len = sizeof(wifi_config.sta.password);
        nvs_get_str(nvs, "wifi_pass", (char *)wifi_config.sta.password, &len);
    }
    nvs_close(nvs);
}

// 3. Connect if SSID is not empty
if (wifi_config.sta.ssid[0] != '\0') {
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}
```

## Why Not Kconfig?

We considered using `CONFIG_PW_WIFI_SSID` in `sdkconfig.defaults`, but that requires creating a Kconfig menu file to declare the options. ESP-IDF silently ignores unknown `CONFIG_*` keys in sdkconfig.defaults. Using `#define` in `config.h` is simpler and works the same way.

## Security Note

The WiFi password is compiled into the firmware binary in plaintext. Don't commit `config.h` with real credentials to a public repository. For production, use NVS-only credentials set via the web dashboard or serial provisioning.

## Future: Runtime WiFi Configuration

Once WiFi is connected, the web dashboard at `pokewatcher.local/settings` can update credentials at runtime (writes to NVS). This is not yet implemented — the settings page has LLM config and mood timers but no WiFi fields. Adding WiFi to the settings page requires:

1. Add SSID/password fields to `pokewatcher/main/web/settings.html`
2. Handle WiFi config in `handle_api_settings_put()` in `pokewatcher/main/web_server.c`
3. Write new credentials to NVS and reconnect WiFi

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `No WiFi credentials configured` | SSID is empty string | Set credentials in config.h |
| `WiFi connecting to ...` then `disconnected, reconnecting` loop | Wrong password, or router rejecting ESP32 | Double-check credentials, check router logs |
| `auth -> init (200)` in log | Authentication timeout | Router might be using WPA3-only — ESP32-S3 supports WPA2, ensure router has WPA2 enabled |
| `assoc -> init (2700)` in log | Association rejected | Router may have MAC filtering or too many clients |
| WiFi connects but no IP | DHCP issue | Check router DHCP pool |
| `WiFi connected, IP: x.x.x.x` but dashboard unreachable | mDNS not resolving | Try the IP address directly instead of `pokewatcher.local` |
