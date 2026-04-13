# Rebooting the SenseCAP Watcher

## Via Serial (Software Reset)

The fastest way — doesn't require unplugging anything:

```bash
bash -c 'export IDF_PATH="/Users/nacoleon/esp/esp-idf" && . "$IDF_PATH/export.sh" 2>/dev/null && python3 -c "
import serial, time
port = serial.Serial(\"/dev/cu.usbmodem5A8A0533623\", 115200)
port.setDTR(False)
port.setRTS(True)
time.sleep(0.1)
port.setRTS(False)
port.close()
print(\"Watcher rebooted\")
"'
```

This toggles the RTS line which is wired to the ESP32-S3's EN (reset) pin. The device resets immediately and boots the firmware from flash.

## Via Serial with Log Capture

Reboot and read the boot log in one go:

```bash
bash -c 'export IDF_PATH="/Users/nacoleon/esp/esp-idf" && . "$IDF_PATH/export.sh" 2>/dev/null && python3 -c "
import serial, time, sys
port = serial.Serial(\"/dev/cu.usbmodem5A8A0533623\", 115200, timeout=1)
port.setDTR(False)
port.setRTS(True)
time.sleep(0.1)
port.setRTS(False)
time.sleep(5)
start = time.time()
while time.time() - start < 15:
    data = port.read(4096)
    if data:
        sys.stdout.write(data.decode(\"utf-8\", errors=\"replace\"))
        sys.stdout.flush()
port.close()
"'
```

## Physical Reset

- **Unplug and replug** the USB-C cable from the bottom port
- There is no physical reset button on the Watcher

## After Flashing

`idf.py app-flash` automatically resets the device via RTS after flashing completes (`Hard resetting via RTS pin...` in the output). No manual reboot needed.

## Notes

- Serial port: `/dev/cu.usbmodem5A8A0533623` (use the higher-numbered port)
- Baud rate: 115200
- The first ~0.5 seconds of boot output is garbled (ROM bootloader runs at a different baud rate) — this is normal
- Boot takes about 2-3 seconds to reach `=== PokéWatcher v1 running ===`
