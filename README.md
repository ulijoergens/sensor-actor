# sensor-actor

ESP-IDF firmware for an ESP32 node that reads sensors, switches relays with threshold rules, talks MQTT (with discovery for openHAB / Home Assistant), and is configured through a built-in web UI with OTA updates.

**Status:** early development.

Working:
- settings storage;
- WiFi with the reconnect schedule and access-point fallback;
- web UI: Home, Settings, Status and error log;
- MQTT: status with last will, info every 60 s, discovery for the diagnostics;
- serial console;
- OTA self-check and rollback guard;
- rule engine.

- sensors: I2C scan, VL53L1X driver, per-sensor settings, median, error state, MQTT values and discovery, Sensors pages.

- actors: GPIO relays (active low, power-on off/on/last, inactive = high-Z), two-threshold rules with start-up and fault behaviour, MQTT switch with discovery, Actors pages with rule editor.

Not yet: drivers for SHT2x/SHT3x/AHT20/BME280/VL53L0X/DS18B20, firmware upload.

- Specification: [docs/FSD.md](docs/FSD.md)
- Sensors: VL53L0X / VL53L1X, SHT25, SHT35, AHT20, BME280 (I2C), DS18B20 (1-Wire)
- Actors: relays on GPIO pins
- Target: ESP32 with 4 MB flash, ESP-IDF v6.1

## Layout

| Path | Content |
|---|---|
| `main/` | start-up sequence, OTA self-check, serial console |
| `components/sa_config/` | device settings in NVS, validation |
| `components/wifi_mgr/` | station mode with the reconnect schedule, access-point fallback |
| `components/webui/` | HTTP server, REST API, single-page UI (`www/`) |
| `components/sa_mqtt/` | MQTT client: status/last will, info, discovery, actor commands |
| `components/sa_i2c/` | I2C bus and sensor detection (address + chip-ID register) |
| `components/sensors/` | sensor framework (storage, read task, median, MQTT) and drivers (VL53L1X) |
| `components/sa_crash/` | crash report: core dump summary, download, erase |
| `components/sa_log/` | ring buffer of warnings and errors for the Status page |
| `components/actors/` | GPIO relays, power-on state, rule evaluation, MQTT switch |
| `components/rules/` | threshold rule engine (pure C) |
| `components/*/test/` | Unity unit tests, collected by `test/unit` |
| `partitions.csv` | 4 MB layout: two OTA slots plus a storage partition |
| `docs/FSD.md` | functional specification with requirement IDs (REQ-…) |

## Build and flash

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py -p /dev/ttyUSB0 flash monitor
```

cJSON and the MQTT client are fetched from the ESP component registry on the first build; the versions are pinned in the components' `idf_component.yml` files.

openHAB picks the node up automatically if the MQTT binding is connected to the same broker and the **Home Assistant Binding** add-on is installed.

## First start

Without WiFi settings the node opens the access point **SensAct-XXXX** (no password).
1. Connect to it.
2. Open http://192.168.4.1.
3. Under *Settings*, enter your network and save. The node restarts and joins your network.

Alternatively, use the serial console (115200 baud):

```
sa> set ssid MyNetwork
sa> set pass my-wifi-password
sa> reboot
```

Other console commands: `status`, `show`, `log`, `scan` (I2C bus), `net`, `ping <ip>`, `factory-reset`, `help`.

## Crash reports

After a crash (including watchdog timeouts) the firmware keeps a core dump in flash. Its summary appears in the error log; to analyse it:

```sh
curl -o coredump.elf http://<device>/api/crash/dump
idf.py coredump-info -c coredump.elf      # needs the matching build/sensor-actor.elf
curl -X DELETE http://<device>/api/crash
```

`crash-test abort|intwdt` on the console crashes on purpose to check this.

## Tests

Unit tests run on the board; the results appear on the serial console:

```sh
idf.py -C test/unit -p /dev/ttyUSB0 flash monitor
```

To see the WiFi reconnect and access-point fallback in about a minute instead of 65, build with short timers:

```sh
idf.py -B build-test -D SDKCONFIG=build-test/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test-timing" -p /dev/ttyUSB0 flash monitor
```

## License

MIT, see [LICENSE](LICENSE).
