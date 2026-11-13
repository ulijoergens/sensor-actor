# sensor-actor

ESP-IDF firmware for an ESP32 node that reads sensors, switches relays with threshold rules, talks MQTT (with discovery for openHAB / Home Assistant), and is configured through a built-in web UI with OTA updates.

**Status:** early development. The functional specification and the rule engine are done.

- Specification: [docs/FSD.md](docs/FSD.md)
- Sensors: VL53L0X / VL53L1X, SHT25, SHT35, AHT20, BME280 (I2C), DS18B20 (1-Wire)
- Actors: relays on GPIO pins
- Target: ESP32 with 4 MB flash, ESP-IDF v6.1

## Layout

| Path | Content |
|---|---|
| `main/` | application entry point |
| `components/rules/` | threshold rule engine (pure C), with on-target unit tests in `test_apps/` |
| `partitions.csv` | 4 MB layout: two OTA slots plus a storage partition |
| `docs/FSD.md` | functional specification with requirement IDs (REQ-…) |

## Build and flash

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py -p /dev/ttyUSB0 flash monitor
```

## Tests

Rule engine unit tests (run on the board, results on the serial console):

```sh
idf.py -C components/rules/test_apps -p /dev/ttyUSB0 flash monitor
```

## License

MIT, see [LICENSE](LICENSE).
