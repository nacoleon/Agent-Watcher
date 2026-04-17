# Himax WE2 Camera — Communication & API Reference

## Hardware

- **Chip:** Himax WE2 (HX6538), firmware v2024.08.16
- **Bus:** SPI2 at 12 MHz (MOSI=GPIO5, MISO=GPIO6, SCLK=GPIO4, CS=GPIO21)
- **Sync pin:** IO expander pin 6 (Himax asserts HIGH when data ready)
- **Reset pin:** IO expander pin 7 (active LOW, 100ms pulse + 200ms wait)
- **Power:** IO expander pin 11 (BSP_PWR_AI_CHIP)
- **Person detection model:** `swift_yolo_nano_person_192` at flash address 0x400000

## SPI Transport Protocol

ESP32 is SPI master. Communication uses the FEATURE_TRANSPORT protocol:
- **Write:** Header `0x10 0x02` + 2-byte length + payload + `0xFF 0xFF` padding
- **Read:** Header `0x10 0x01` + 2-byte length + `0xFF 0xFF`, response in same buffer
- **Available:** Header `0x10 0x03`, returns 2-byte available count
- **Flush/Reset:** Header `0x10 0x06`

**CRITICAL:** The transport uses a 2ms inter-operation delay (`pdMS_TO_TICKS(2)`). This requires `CONFIG_FREERTOS_HZ=1000`. At 100Hz, the delay rounds to 0 and the camera cannot respond.

## AT Command Protocol

- Commands: `AT+<CMD>[?|=<params>]\r\n`
- Responses: `\r{"type":<0|1|2>,"name":"<CMD>","code":<0-10>,"data":{...}}\n`
- Type 0 = command response, Type 1 = event, Type 2 = log
- Timeout: 2000ms (`CMD_WAIT_DELAY`)

## Boot Sequence (what works)

```
sscma_client_init(client)     → hardware reset, starts process/monitor tasks
sscma_client_get_info(...)    → ID, name, firmware version
sscma_client_set_model(1)     → select person detection model
sscma_client_set_sensor(1,1,true) → enable sensor, opt_id=1
sscma_client_invoke(-1,false,true) → start continuous inference with images
```

No delays needed between commands (at 1000Hz ticks). The `on_connect` callback fires ~500ms after init when the camera boots.

## Available Commands

### Device Info
| Command | API | Returns |
|---------|-----|---------|
| `AT+ID?` | `sscma_client_get_info()` | Device ID string (e.g., "360779f5") |
| `AT+NAME?` | `sscma_client_get_info()` | Device name (e.g., "SenseCAP Watcher") |
| `AT+VER?` | `sscma_client_get_info()` | hw_ver, sw_ver, fw_ver |

### Model Management
| Command | API | Description |
|---------|-----|-------------|
| `AT+MODEL=<id>` | `sscma_client_set_model(client, id)` | Select model (1=person detection) |
| `AT+MODEL?` | `sscma_client_get_model(client, &model, cached)` | Get current model info (name, UUID, classes, URL) |
| `AT+INFO="<b64>"` | `sscma_client_set_model_info(client, json)` | Set model metadata (base64 JSON) |

### Inference
| Command | API | Description |
|---------|-----|-------------|
| `AT+INVOKE=<times>,<filter>,<show>` | `sscma_client_invoke(client, times, filter, show)` | Start inference |
| `AT+SAMPLE=<times>` | `sscma_client_sample(client, times)` | Capture single frames |
| `AT+BREAK` | `sscma_client_break(client)` | Stop inference/sampling |

**Invoke parameters:**
- `times`: -1 = continuous, N = run N times
- `filter`: true = filter results (NMS), false = raw
- `show`: true = include base64 JPEG in events, false = boxes only
  - **WARNING:** Parameter is inverted in the library! `show=true` sends `0` to camera (= include images)

### Thresholds
| Command | API | Description |
|---------|-----|-------------|
| `AT+TSCORE=<val>` | `sscma_client_set_confidence_threshold()` | Min confidence score (0-100) |
| `AT+TSCORE?` | `sscma_client_get_confidence_threshold()` | Get current threshold |
| `AT+TIOU=<val>` | `sscma_client_set_iou_threshold()` | NMS IoU threshold |
| `AT+TIOU?` | `sscma_client_get_iou_threshold()` | Get current IoU |

### Sensor
| Command | API | Description |
|---------|-----|-------------|
| `AT+SENSOR=<id>,<opt>,<en>` | `sscma_client_set_sensor(client, id, opt_id, enable)` | Configure sensor |
| `AT+SENSOR?` | `sscma_client_get_sensor(client, &sensor)` | Get sensor config |

Sensor opt_id values (resolution):
- 0: 240x240
- 1: 416x416 (used by monitor example)
- 2: 480x480
- 3: 640x480

### System
| Command | API | Description |
|---------|-----|-------------|
| `AT+RST` | `sscma_client_reset()` | Software reset (commented out, use GPIO reset) |
| `AT+OTA` | `sscma_client_ota_start/write/finish/abort()` | Flash firmware over SPI |

## Detection Data Types

### Bounding Box (`sscma_client_box_t`)
```c
{ uint16_t x, y, w, h; uint8_t score; uint8_t target; }
```
- `target=0` = person (for person detection model)
- `score` = confidence 0-100

### Classification (`sscma_client_class_t`)
```c
{ uint8_t target; uint8_t score; }
```

### Point (`sscma_client_point_t`)
```c
{ uint16_t x, y, z; uint8_t score; uint8_t target; }
```

### Keypoint (`sscma_client_keypoint_t`)
```c
{ sscma_client_box_t box; uint8_t points_num; sscma_client_point_t points[80]; }
```

### Image
Base64-encoded JPEG, extracted with `sscma_utils_fetch_image_from_reply()`. Only present when `show=true` in invoke.

## Event Callbacks

Register with `sscma_client_register_callback()`:
- `on_connect` — Camera booted (INIT@STAT event)
- `on_event` — Inference result (INVOKE/SAMPLE event with boxes, classes, points, keypoints, image)
- `on_log` — Camera debug log
- `on_response` — Unmatched command response
- `on_disconnect` — Camera disconnected

## Flash Memory Map

| Address | Size | Content |
|---------|------|---------|
| 0x000000 | 1MB | Himax firmware (.img) |
| 0x400000 | 2MB | Person detection model (slot 1) |
| 0x600000 | 2MB | Pet detection model (slot 2) |
| 0x800000 | 2MB | Gesture detection model (slot 3) |
| 0xA00000 | 4MB | Custom model (slot 4) |

Flash with: `sscma_client_ota_start(client, flasher, offset)` → write 256-byte chunks → `ota_finish()`

## Available Models (verified 2026-04-17)

| Slot | Model | UUID | Classes |
|------|-------|------|---------|
| 1 | Person Detection | f2b99229ba108c82de9379c4b6ad6354 | person |
| 2 | Pet Detection | 60084 | cat, dog, person |
| 3 | Gesture Detection | 91331a9db811ed5cfb5cdba2e419e507 | paper, rock, scissors |
| 4 | (empty — returns 0x102) | — | — |

All three models are pre-flashed from factory. Slot 4 available for custom .tflite upload via OTA.

## Key Config Requirements

**sdkconfig MUST have:**
```
CONFIG_FREERTOS_HZ=1000
```
Without this, SPI transport delays round to 0 and the camera cannot respond.

**Recommended SSCMA config (matches factory firmware):**
```
CONFIG_SSCMA_RX_BUFFER_SIZE=98304
CONFIG_SSCMA_PROCESS_TASK_STACK_SIZE=4096
CONFIG_SSCMA_PROCESS_TASK_AFFINITY=-1
CONFIG_SSCMA_EVENT_QUEUE_SIZE=2
```
