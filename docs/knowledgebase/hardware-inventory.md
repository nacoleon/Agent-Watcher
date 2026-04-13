# SenseCap Watcher Hardware Inventory

Complete hardware peripheral inventory for the SenseCAP Watcher, derived from the firmware source code and board support package. Documents what's available, what's enabled, and what's untapped.

## MCU

- **Chip**: ESP32-S3, dual-core Xtensa LX7 @ 240 MHz
- **Flash**: 32 MB (quad-mode SPI)
- **PSRAM**: External, OctoSPI @ 80 MHz, DMA capable
- **Internal SRAM**: 8 MB

## Display

- **Resolution**: 412x412 pixels (square)
- **Controller**: SPD2010 (combined LCD + touch controller)
- **Interface**: QSPI (SPI3_HOST) @ 40 MHz pixel clock
- **Backlight**: PWM via LEDC channel 1 on GPIO 8, 10-bit duty resolution
- **Power**: Controlled via IO expander pin 9
- **Graphics**: LVGL 8.x with double-buffered rendering

### Touch

- **Controller**: SPD2010 (same chip as display)
- **Interface**: I2C_1 (SDA=GPIO 39, SCL=GPIO 38) @ 400 kHz, address 0x53
- **Interrupt**: IO expander pin 5

### QSPI Pins

| Signal | GPIO |
|--------|------|
| PCLK   | 7    |
| DATA0  | 9    |
| DATA1  | 1    |
| DATA2  | 14   |
| DATA3  | 13   |
| CS     | 45   |

## Camera

- **Module**: Himax (AI vision module, separate from ESP32-S3)
- **Interface**: SPI (SPI2_HOST) @ 12 MHz
- **CS**: GPIO 21
- **Sync/Reset**: IO expander pins 6 and 7
- **Flashing UART**: UART_1 @ 921600 baud (TX=GPIO 17, RX=GPIO 18)
- **Software**: SSCMA client library for object/person detection
- **Status**: Working — used for person detection in mood engine

## Audio

Microphone and speaker share the same codec and I2S bus.

### Codec

- **Chips supported**: ES8311, ES7243, ES7243E (plus TAS5805M, AW88298, ES8156, ES8374, ES8388 in driver lib)
- **I2C Addresses**: ES8311=0x30, ES7243=0x13, ES7243E=0x14
- **PA Power**: IO expander pin 12

### I2S Bus (I2S_NUM_0)

| Signal | GPIO |
|--------|------|
| MCLK   | 10   |
| SCLK   | 11   |
| LRCK   | 12   |
| Din    | 15   |
| Dout   | 16   |

### Microphone

- **Sample Rate**: 16000 Hz
- **Bit Depth**: 16-bit
- **Channels**: Mono (1)
- **Mic Gain**: 27.0 dB
- **Status**: Working

### Speaker

- **Output**: Via same codec, I2S Dout on GPIO 16
- **PA Power Control**: IO expander pin 12
- **Status**: Working

## Rotary Wheel (Encoder)

- **Type**: Physical rotary encoder with push button
- **Channel A**: GPIO 41
- **Channel B**: GPIO 42
- **Button**: IO expander pin 3 (supports long-press/long-release)
- **Driver**: `iot_knob` (espressif__button component)
- **Integration**: LVGL encoder input device for UI navigation
- **Status**: Working

## WiFi

- **Standard**: 802.11 b/g/n (2.4 GHz only)
- **Mode**: Station (STA)
- **Features**: AMPDU TX/RX, WPA3-SAE, NVS credential storage, mDNS
- **mDNS Hostname**: `pokewatcher.local`
- **Use**: Web dashboard on HTTP port 80
- **Status**: Working, enabled in sdkconfig

## Bluetooth

- **Hardware**: BLE supported by ESP32-S3 SoC
- **Firmware Status**: **DISABLED** (`CONFIG_BT_ENABLED` is not set)
- **Potential**: Phone connectivity, BLE beacons, peripheral pairing
- **To enable**: Set `CONFIG_BT_ENABLED=y` in sdkconfig and configure BLE stack

## RGB LED

- **Type**: WS2812 (Neopixel), single LED
- **Pin**: GPIO 40
- **Protocol**: RMT (Remote Control Transducer)
- **Pixel Format**: GRB
- **Status**: Working

## Battery

- **Voltage Monitoring**: ADC channel 2 (GPIO 3), attenuation 2.5 dB
- **Voltage Divider**: (62+20)/20 = 4.1x ratio
- **ADC Power**: IO expander pin 15
- **Charge Detection**: IO expander pin 0
- **Standby Detection**: IO expander pin 1
- **Battery Present**: IO expander pin 13
- **VBUS In Detection**: IO expander pin 2
- **Power Management**: Deep sleep and system shutdown supported
- **Status**: Working

## Real-Time Clock (RTC)

- **Chip**: PCF8563
- **I2C Address**: 0x51
- **Bus**: I2C_0 (general bus)
- **Status**: Working

## Storage

### SD Card

- **Interface**: SPI (SPI2_HOST, shared with camera)
- **CS**: GPIO 46
- **Detection**: IO expander pin 4
- **Clock**: 400 kHz
- **Mount Point**: `/sdcard`
- **Power**: IO expander pin 8
- **Status**: Working

### SPIFFS

- **Mount Point**: `/spiffs`
- **Use**: Internal flash filesystem, minimal usage

## IO Expander

- **Chip**: PCA9535 (16-bit I/O expander)
- **Bus**: I2C_0 (general bus)
- **Interrupt**: GPIO 2
- **Update Interval**: 1 second

### Power Control Map

| Pin | Function |
|-----|----------|
| P0.0 | Charge detection |
| P0.1 | Standby detection |
| P0.2 | VBUS detection |
| P0.3 | Wheel button |
| P0.4 | SD card detection |
| P0.5 | Touch interrupt |
| P0.6 | Camera sync |
| P0.7 | Camera reset |
| P1.0 (8) | SD card power |
| P1.1 (9) | LCD power |
| P1.4 (12) | Codec PA power |
| P1.5 (13) | Battery present |
| P1.6 (14) | Grove port power |
| P1.7 (15) | Battery ADC power |

## I2C Buses

| Bus | SDA | SCL | Speed | Devices |
|-----|-----|-----|-------|---------|
| I2C_0 | GPIO 47 | GPIO 48 | 400 kHz | IO expander, RTC, audio codec |
| I2C_1 | GPIO 39 | GPIO 38 | 400 kHz | Touch controller (SPD2010) |

## Grove Expansion Port

- **Power Control**: IO expander pin 14
- **Status**: Available but unused — no external sensors connected in current firmware

## USB

- **Type**: USB Serial JTAG
- **Status**: Debugging only, not configured for data transfer

## Untapped Capabilities

These are hardware features present but not enabled or used in the current firmware:

1. **Bluetooth (BLE)** — just needs `CONFIG_BT_ENABLED=y` in sdkconfig
2. **Temperature Sensor** — built into ESP32-S3 SoC, supported but unused
3. **Capacitive Touch Pads** — 15 channels available on SoC, unused (screen touch is separate I2C)
4. **USB Data Transfer** — USB Serial JTAG hardware present, only used for debug
5. **Grove Sensors** — port is powered but no external sensors driven

## Key Source Files

- `SenseCAP-Watcher-Firmware/components/sensecap-watcher/include/sensecap-watcher.h` — all pin definitions
- `SenseCAP-Watcher-Firmware/components/sensecap-watcher/sensecap-watcher.c` — BSP init and power control
- `pokewatcher/sdkconfig` — enabled/disabled features
- `pokewatcher/main/app_main.c` — initialization sequence
