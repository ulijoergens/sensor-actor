# Sensor-Actor Node: Functional Specification (draft v0.1)

Source: the owner's project idea document.
Decisions confirmed by the owner: VL53L0X/VL53L1X (not "VL530X/VL531X"); DS18B20 on 1-Wire;
BME280 on I2C.
Items marked **[PROPOSED]** are defaults chosen for this draft and still need the owner's confirmation.

## 1. Scope

The node is ESP-IDF firmware for a plain ESP32 (4 MB flash). It:
- reads sensors;
- evaluates simple threshold rules that switch GPIO relays;
- publishes sensor and actor state over MQTT and accepts actor commands over MQTT;
- is configured through a built-in web UI;
- supports OTA updates.

## 2. Hardware interfaces

| Bus | Devices | Default pins [PROPOSED] |
|---|---|---|
| I2C (100 kHz; 400 kHz selectable) | VL53L0X, VL53L1X, SHT25, SHT35, AHT20, BME280 | SDA 21, SCL 22 |
| 1-Wire | DS18B20 (several on one bus) | GPIO 4 (4.7 kΩ pull-up) |
| GPIO outputs | relay channels | any output-capable pin |

- **REQ-HW-1:** the I2C and 1-Wire pins are configurable in Settings. A change takes effect after a reboot.
- **REQ-HW-2:** pins that cannot be used for relays are rejected:
  - GPIO 6–11 (flash);
  - GPIO 34–39 (input-only);
  - the pins already used by I2C or 1-Wire.

  Strapping pins (0, 2, 5, 12, 15) are accepted with a warning.
- **REQ-HW-3:** each relay has an "active low" option, because many relay boards switch on LOW.

### 2.1 Sensor detection

- **REQ-DET-1:** "Scan" in the web UI probes the I2C bus and the 1-Wire bus. Every device found is identified by address and chip ID:

  | Device | Address | Identification |
  |---|---|---|
  | AHT20 | 0x38 | status register, calibrated bit |
  | SHT25 | 0x40 | user register read |
  | SHT35 | 0x44 / 0x45 | status register with CRC |
  | BME280 | 0x76 / 0x77 | register 0xD0 = 0x60 (0x58 = BMP280, reported as unsupported) |
  | VL53L0X | 0x29 | register 0xC0 = 0xEE |
  | VL53L1X | 0x29 | model ID register 0x010F = 0xEA |
  | DS18B20 | 1-Wire ROM | family code 0x28 |

- **REQ-DET-2:** found devices that are not yet configured are offered for adding. Configured sensors that are not found are shown as "missing".
- ⚠ **VL53L0X and VL53L1X both use address 0x29.** Only one of them can sit on the bus unless the others are held off with their XSHUT pins and given new addresses at boot.

  **Version 1 supports one VL53Lx sensor** (confirmed by the owner). Supporting several is a later extension, with one XSHUT GPIO per sensor. A scan that finds a second VL53Lx reports it as a conflict.

## 3. Sensors

- **REQ-SEN-1:** each sensor has:
  - a label of up to 32 characters;
  - a short ID of up to 8 characters, `[a-z0-9_-]`, unique on the device and used in MQTT topics;
  - an active flag and a favourite flag;
  - a read interval of 1–3600 s (default 10 s);
  - a median window of 3–31 samples (default 15).
- **REQ-SEN-2:** per-type parameters:

  | Type | Parameters |
  |---|---|
  | SHT3x | repeatability (low / medium / high) |
  | BME280 | oversampling (T / P / H), IIR filter |
  | VL53Lx | timing budget, distance mode (L1X short / long), mounting height (mm, optional) |
  | DS18B20 | resolution 9–12 bit |
  | All types | per-quantity offset for calibration |

- **REQ-SEN-3:** quantities per type:

  | Type | Quantities |
  |---|---|
  | temperature/humidity sensors (SHT25, SHT35, AHT20) | temperature (°C), humidity (%RH) |
  | BME280 | temperature, humidity, pressure (hPa) |
  | VL53Lx | distance (mm); level (mm) when a mounting height is set |
  | DS18B20 | temperature |

- **REQ-SEN-4:** for each quantity the node keeps the last value, a timestamp and the median of the last N valid samples.
- **REQ-SEN-5:** a read failure is logged. After 3 failures in a row the sensor is flagged "error", and its last value is kept but marked stale.
- **REQ-SEN-6:** inactive sensors are not read and publish nothing.
- **REQ-SEN-7:** if a VL53Lx sensor has a mounting height h, it also provides the derived quantity `level` = h − distance (mm). `level` has its own last value and median, and can be published and used in rules like any other quantity.

## 4. Actors

- **REQ-ACT-1:** each actor has:
  - a GPIO pin;
  - a label of up to 32 characters and an ID of up to 8 characters (same rules as sensors);
  - an active flag and a favourite flag;
  - an active-low flag;
  - a power-on state: OFF / ON / last [PROPOSED default OFF].
- **REQ-ACT-2:** the state can be set from the web UI, from MQTT and from rules. An inactive actor keeps its pin as an input (high-Z) and ignores commands.

### 4.1 Rules (confirmed by the owner)

- **REQ-RUL-1:** each actor can have one rule. A rule watches one quantity of one sensor, for example `tank.distance` or `room.temperature`. The source value v is either the last value or the median [PROPOSED default: median].
- **REQ-RUL-2:** a rule has up to **two thresholds, T1 and T2**. Each threshold is independent and has two actions:

  | Crossing | Condition | Action |
  |---|---|---|
  | rising | previous v ≤ T, new v > T | ON / OFF / none |
  | falling | previous v > T, new v ≤ T | ON / OFF / none |

- **REQ-RUL-3:** rules are edge-triggered. They act only when the value crosses a threshold. A manual command (web UI or MQTT) therefore keeps its effect until the next crossing.
- **REQ-RUL-4:** if one sample crosses both thresholds, both actions run in the order of the movement: the lower threshold first when rising, the higher one first when falling. The last action wins. The order depends only on the threshold values, not on which slot (T1/T2) holds them.
- **REQ-RUL-5:** after boot, and after a sensor recovers, the first valid sample has no previous value. For each threshold, the rule applies the action of the crossing that would have brought v to its current side: the rising action if v > T, the falling action if v ≤ T. The thresholds are applied in ascending order of value, and the last action wins. Thresholds with action "none" are skipped. If no action applies, the actor keeps its power-on state.

  This makes the pump example safe after a power cut with an empty tank.
- **REQ-RUL-6:** a rule whose sensor is in error or stale does nothing, and this is logged once.

- **REQ-RUL-8:** each actor has a **fault action** (none / ON / OFF, default none). It is applied once when the rule's sensor goes into error or becomes stale. With it, a pump can be switched OFF when its level sensor fails.
- **REQ-RUL-7:** the web UI checks the rule for plausibility and shows it in words, e.g. "OFF when temperature rises above 30.0; ON when it falls to 28.0 or below". A rule where neither threshold has an action is rejected.

**Examples:**

| Use | T1 | T2 | Effect |
|---|---|---|---|
| Overheat protection without flicker (fan heater, temperature) | 28.0: rising none, falling **ON** | 30.0: rising **OFF**, falling none | the heater switches OFF above 30 °C and back ON only once the temperature is ≤ 28 °C |
| Pump dry-run protection (tank level in cm) | 20: rising none, falling **OFF** | 30: rising **ON**, falling none | the pump stops when the level drops to ≤ 20 cm and restarts once the tank has refilled above 30 cm |

With a VL53Lx mounted above the water, the sensor measures the distance to the surface, so a low water level means a large distance. To allow rules on the water level instead, each VL53Lx sensor has an optional **mounting height** (REQ-SEN-7).

Extension idea, not part of version 1: several conditions combined with AND/OR, and time windows.

## 5. MQTT (topic scheme confirmed by the owner)

Base topic: `sensact/<device-id>/`. Retained where marked (R).

| Topic | Direction | Payload |
|---|---|---|
| `…/status` (R) | out | `online` / `offline` (last will) |
| `…/sensor/<sid>/<quantity>` (R) | out | plain number, e.g. `21.4` (includes derived quantities such as `level`) |
| `…/sensor/<sid>/state` (R) | out | JSON `{"temperature":21.4,"temperature_median":21.3,"ts":…,"ok":true}` |
| `…/actor/<aid>/state` (R) | out | `ON` / `OFF` |
| `…/actor/<aid>/set` | in | `ON` / `OFF` / `TOGGLE` |
| `…/info` (R) | out | JSON `{"firmware","ip","rssi","uptime","heap"}` (on connect and every 60 s) |

- **REQ-MQ-1:** values are published on every read.
- **REQ-MQ-2:** actor state is published on every change, and again after each reconnect.
- **REQ-MQ-3:** commands to unknown or inactive actors are logged and ignored.
- **REQ-MQ-4:** the node publishes Home-Assistant-style discovery messages (confirmed by the owner). openHAB's MQTT binding understands these, so the sensors and relays appear in openHAB without manual setup.
  - Topics: `<prefix>/sensor/<device-id>_<sid>_<quantity>/config` and `<prefix>/switch/<device-id>_<aid>/config`. The prefix is `homeassistant` by default and can be changed in Settings.
  - Discovery is on by default and can be switched off in Settings.
  - Messages are retained and re-sent after every MQTT reconnect.
  - A sensor, quantity or actor that is removed or deactivated gets an empty retained message, which removes it from openHAB.
  - All entities share one device record (name, ID, firmware version, MAC), and they use `…/status` as their availability topic.
  - Diagnostic entities (from `…/info`): WiFi signal, uptime, IP address, free memory, firmware.
  - openHAB 5 needs the **Home Assistant Binding** add-on (separate from the MQTT binding) to discover these.

## 6. Connectivity

- **REQ-WIFI-1:** station mode with the configured SSID and password. If the connection is lost:
  1. retry every **10 s for 5 min**;
  2. then every **5 min for 1 h**;
  3. then start **access-point mode**: SSID `SensAct-<last 4 hex digits of MAC>`, web UI at 192.168.4.1.
- **REQ-WIFI-2 [PROPOSED]:** while in AP mode, the node retries the configured network every 15 min. On success it switches back to station mode.
- **REQ-WIFI-3:** a node without WiFi credentials starts directly in AP mode.
- **REQ-MQ-5:** the MQTT client runs only while WiFi has an IP address, and reconnects on its own (every 10 s) after WiFi or the broker returns. Each outage is logged once, with the reason. The broker IP and port are configurable; username and password are optional [PROPOSED].
- **REQ-NET-1 [PROPOSED]:** the node is reachable by mDNS as `<device-name>.local`.

### 6.1 Serial console

- **REQ-CON-1:** a console on UART0 (115200 baud) offers:
  - `status` and `show` (settings, with passwords hidden);
  - `set <key> <value>`, which validates and saves one setting;
  - `log`, `reboot` and `factory-reset`;
  - `scan`, which runs the I2C bus scan (REQ-DET-1) and lists each device with its identification;
  - `net` (IP, gateway, DNS, access point) and `ping <ip>` for network diagnostics.

  It is used to set up the node and for bench tests.

## 7. Web UI

The UI is plain HTML, CSS and JavaScript, gzipped and built into the firmware so that an OTA update also updates the UI. It talks to a JSON REST API under `/api/…`.

| Page | Content |
|---|---|
| Home | system status (WiFi, MQTT, uptime, errors); favourite sensors (value + median) and favourite actors (with switch); menu |
| Settings | device name, device ID, WiFi SSID and password, MQTT broker IP and port (user and password), discovery on/off and prefix; I2C and 1-Wire pins; firmware upload (OTA); reboot |
| Sensors | list with value, status and an active toggle; "Scan bus" button; link to the detail page |
| Sensor detail | type, bus address, parameters, label, ID, last value, median, favourite; all fields editable. API: `GET/POST /api/sensors`, `GET/POST/DELETE /api/sensors/<id>`, `GET /api/scan` |
| Actors | list with state, active toggle and switch; "Add actor"; link to the detail page |
| Actor detail | API: `GET/POST /api/actors`, `GET/POST/DELETE /api/actors/<id>`, `POST /api/actors/<id>/set`. GPIO, label, ID, state, favourite, active-low, power-on state, fault action; rule editor (sensor + quantity, last or median, T1 and T2 each with rising and falling actions) with a plain-language summary; all fields editable |
| Status | error log (newest first), WiFi (SSID, RSSI, IP, mode), MQTT (connected, broker, last error), uptime, firmware version, free heap |

- **REQ-UI-1:** values refresh without reloading the page (polling every 2 s [PROPOSED]).
- **REQ-UI-2:** all input is validated in the browser and again on the device.
- **REQ-UI-3:** the web UI has no login (decided by the owner). The node is meant for a trusted home network. Anyone who can reach it can change its settings and upload firmware.

## 8. OTA

- **REQ-OTA-1:** a firmware image uploaded in Settings (`POST /api/ota`, raw image) is written to the inactive OTA slot and checked: ESP32 application header, same project name, ESP-IDF image validation (checksum, SHA-256). The node then reboots into it. `GET /api/ota` shows both slots with version, build time and state.
- **REQ-OTA-2:** the new image must confirm it works within 60 s of boot: WiFi or AP up, and the web server running. Otherwise the bootloader rolls back to the previous image.

### 8.1 Partition table for 4 MB flash [PROPOSED]

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB |
| otadata | data/ota | 0xF000 | 8 KB |
| phy_init | data/phy | 0x11000 | 4 KB |
| ota_0 | app | 0x20000 | 1.75 MB |
| ota_1 | app | 0x1E0000 | 1.75 MB |
| storage | data/littlefs | 0x3A0000 | 320 KB (persistent error log, spare) |
| coredump | data/coredump | 0x3F0000 | 64 KB (crash report, REQ-LOG-2) |

## 9. Persistence and logging

- **REQ-PER-1:** the configuration (device, network, sensors, actors, rules) is stored in NVS as versioned records. "Factory reset": hold BOOT for 10 s, or press the button in Settings.
- **REQ-LOG-2:** after a crash or watchdog panic, a core dump (ELF) is kept in the `coredump` partition. On the next boot a one-line summary (task, PC, panic reason) goes into the error log; `GET /api/crash` returns the summary with the backtrace, `GET /api/crash/dump` downloads the dump, `DELETE /api/crash` erases it.
- **REQ-LOG-1:** the error log is a RAM ring buffer of 100 entries (time, level, source, text). The last 20 warnings and errors are copied to flash so they survive a reboot. The time comes from SNTP, or from uptime while no time is available.

## 10. Testing (Universal-ESP32-Workbench)

- **Unit tests:** rule and hysteresis logic, median, config validation and MQTT topic building. These run on the host and in CI.
- **On the bench:**
  - flash over RFC2217;
  - check WiFi reconnect and fallback using the bench's test access point (shortened timers through a test build flag);
  - check MQTT round trips against a broker on the bench;
  - check OTA upload and rollback;
  - check web API smoke tests;
  - with the sensors fitted to the bench: scan and identify each type.

## 11. Open questions

1. Rules (§4.1): settled. The owner confirmed the two-threshold model, the start-up behaviour, the fault action and the VL53Lx `level` quantity.
2. MQTT (§5): settled. The scheme is confirmed and discovery is on.
3. Web UI login (§7): settled. There is none.
4. VL53Lx (§2.1): settled. Version 1 supports one sensor.
5. Workbench: is a bench Pi already running? (Its IP is needed, because `.local` names don't resolve in this container.)
6. GitHub repository name and visibility; GitHub access for this container.
