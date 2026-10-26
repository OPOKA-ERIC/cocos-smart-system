# CoCIS Smart Control & Monitoring System

ESP32-S3 / ESP-IDF coursework project for the College of Computing and Information Sciences (CoCIS), Makerere University.

The system uses three independent Wokwi terminals that communicate through MQTT:

- **Entrance hub** - dual MFRC522 RFID readers, entry/exit validation, occupancy count, access schedule, admin PIN, SD-card audit log, buzzer/LED feedback, serial menu, and MQTT hub.
- **Room 1 controller** - PIR occupancy sensing, automatic lighting with timeout, remote ON/OFF/AUTO commands, MQTT state publishing.
- **Room 2 controller** - same room-controller behaviour as Room 1, on its own MQTT topics.

Wokwi does not support ESP-NOW or multi-MCU projects, so the terminals are separate simulations connected through the public HiveMQ broker.

## Team

| Name | Contribution |
| --- | --- |
| OPOKA ERIC | _to be completed_ |
| OJOK ERICK | _to be completed_ |
| OPETO ISAAC | _to be completed_ |
| KIZITO RYAN | _to be completed_ |
| AUMA DILIS | _to be completed_ |

Registration numbers and final contribution statements are completed in `REPORT.md` before submission.

## Repository layout

```text
cocis-smart-system/
|-- entrance/          Entrance hub (ESP-IDF project)
|   |-- src/main.c
|   |-- src/mqtt_bus.c
|   |-- src/rc522.c
|   |-- include/config.h
|   |-- include/mqtt_bus.h
|   |-- include/rc522.h
|   |-- diagram.json
|   |-- platformio.ini
|   \-- wokwi.toml
|-- room1/             Room 1 controller (ESP-IDF project)
|-- room2/             Room 2 controller (ESP-IDF project)
|-- REPORT.md          Full technical report
|-- DEMO_SCRIPT.md     Step-by-step demo/video script
|-- WOKWI_SETUP.md     Wokwi run and sharing guide
\-- README.md
```

Each terminal is a self-contained PlatformIO project. The MQTT bus module is duplicated in each project because Wokwi simulates one project at a time.

## Architecture

```text
                    +---------------------------+
                    |  MQTT broker              |
                    |  broker.hivemq.com:1883   |
                    +-------------+-------------+
                                  |
        +-------------------------+-------------------------+
        |                         |                         |
+-------+--------+       +--------+-------+        +--------+-------+
| Entrance hub   |       | Room 1       |        | Room 2       |
| ESP32-S3       |       | ESP32-S3     |        | ESP32-S3     |
| 2x MFRC522     |       | PIR + light  |        | PIR + light  |
| microSD, buzzer|       |              |        |              |
| schedule + PIN |       |              |        |              |
+----------------+       +--------------+        +--------------+
```

The entrance is the authority for access control and the monitoring point for room state. Rooms publish their state; the entrance subscribes and can send lighting commands back to either room.

## Hardware and pin map

### Entrance hub

| Function | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| Door-lock indicator LED | 2 | 220 ohm resistor to GND |
| Buzzer | 18 | Buzzer pin 2 (positive); pin 1 to GND |
| SD card CS | 10 | SPI3 |
| SD card DI (MOSI) | 11 | SPI3 |
| SD card DO (MISO) | 13 | SPI3 |
| SD card SCK | 12 | SPI3 |
| RFID shared MOSI | 6 | SPI2 |
| RFID shared MISO | 7 | SPI2 |
| RFID shared SCK | 15 | SPI2 |
| Entry reader CS / RST | 5 / 4 | `board-mfrc522` |
| Exit reader CS / RST | 16 / 17 | `board-mfrc522` |

### Room 1 and Room 2

| Function | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| PIR output | 4 | `wokwi-pir-motion-sensor` |
| Light LED | 2 | 220 ohm resistor to GND |

Wokwi part names used by the diagrams:

- `board-esp32-s3-devkitc-1`
- `board-mfrc522` (not `wokwi-rfid-rc522`)
- `wokwi-microsd-card`
- `wokwi-pir-motion-sensor`
- `wokwi-led`
- `wokwi-resistor`
- `wokwi-buzzer`

The buzzer pin names are `1` (negative/GND) and `2` (positive/signal).

## Default configuration

| Setting | Default |
| --- | --- |
| Wi-Fi | `Wokwi-GUEST` (open) |
| MQTT broker | `broker.hivemq.com:1883` |
| Topic prefix | `cocis/2026/opoka-eric` |
| Admin PIN | `1234` |
| Access schedule | `07:00-21:00` |
| Capacity | 50 people |
| Room lighting timeout | 15 s without motion |
| Time zone | `EAT-3` (UTC+3) |
| SD mount point | `/sdcard` |
| SD log file | `/sdcard/access.log` |

The entrance stores the PIN and schedule in NVS, so changes survive a restart on real hardware. Wokwi may reset NVS when a simulation is recreated.

## MQTT topics

All topics use the prefix `cocis/2026/opoka-eric`.

| Topic | Direction | Retained | Purpose |
| --- | --- | --- | --- |
| `/entrance/event` | Entrance -> subscribers | No | Entry/exit/auth event |
| `/entrance/state` | Entrance -> subscribers | Yes | Occupancy, schedule, time, SD status |
| `/room1/state` | Room 1 -> subscribers | Yes | Occupancy, light, mode, sequence |
| `/room2/state` | Room 2 -> subscribers | Yes | Occupancy, light, mode, sequence |
| `/room1/cmd` | Any -> Room 1 | No | `ON`, `OFF`, `AUTO`, `STATUS` |
| `/room2/cmd` | Any -> Room 2 | No | `ON`, `OFF`, `AUTO`, `STATUS` |
| `/status/entrance` | Entrance LWT/online | Yes | `online` / `offline` |
| `/status/room1` | Room 1 LWT/online | Yes | `online` / `offline` |
| `/status/room2` | Room 2 LWT/online | Yes | `online` / `offline` |

Example entrance event:

```json
{
  "term": "entrance",
  "type": "ENTRY",
  "uid": "01:02:03:04",
  "name": "Blue Card (Alice)",
  "result": "GRANTED",
  "inside": 1,
  "seq": 7,
  "ts": "2026-10-26 18:42:10"
}
```

Example room state:

```json
{
  "term": "room1",
  "occupied": true,
  "light": true,
  "mode": "AUTO",
  "seq": 12,
  "uptime_ms": 345678
}
```

`seq` is a monotonically increasing per-terminal sequence number. Retained state plus `seq` lets a late subscriber recover the latest state and detect ordering.

## Test cards

Wokwi provides these built-in MFRC522 card presets:

| Preset | UID | Policy |
| --- | --- | --- |
| Blue Card | `01:02:03:04` | Allowed - Alice |
| Green Card | `11:22:33:44` | Denied - not on allow-list |
| Yellow Card | `55:66:77:88` | Allowed - Bob |
| Red Card | `AA:BB:CC:DD` | Allowed - Carol |
| NFC Tag (7-byte UID) | `04:11:22:33:44:55:66` | Allowed - Dave (admin tag) |
| Key Fob | `C0:FF:EE:99` | Denied |

The NFC tag exercises the 7-byte UID cascade path in the custom MFRC522 driver.

## Build

`pio` may not be on `PATH`. Use the PlatformIO executable directly if needed.

```powershell
cd D:\cocis-smart-system\entrance
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run

cd D:\cocis-smart-system\room1
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run

cd D:\cocis-smart-system\room2
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
```

All three projects currently build successfully:

| Project | RAM | Flash |
| --- | --- | --- |
| Entrance | 13.0% (42,480 / 327,680 B) | 85.6% (897,217 / 1,048,576 B) |
| Room 1 | 11.8% (38,640 / 327,680 B) | 73.7% (772,625 / 1,048,576 B) |
| Room 2 | 11.8% (38,640 / 327,680 B) | 73.7% (772,625 / 1,048,576 B) |

## Run in Wokwi

1. Install VS Code with the PlatformIO IDE and Wokwi Simulator extensions.
2. Open the `entrance` folder, build it, then run `Wokwi: Start Simulator`.
3. Open `room1` and `room2` in separate VS Code windows (or separate Wokwi simulations), build and start each.
4. Open the serial monitor for each terminal at 115200 baud.
5. In the entrance simulation, click each `board-mfrc522` to select a card and use **Tap** or **Hold**.
6. In the room simulations, click the PIR sensor to toggle motion.

Full instructions and troubleshooting are in `WOKWI_SETUP.md`.

## Serial menu (entrance)

Type `help` in the entrance serial monitor. The menu is line-based.

| Command | Description |
| --- | --- |
| `status` | Show hub state, rooms, MQTT, SD, and admin lock |
| `list` | List people currently inside |
| `users` | Show the RFID allow-list |
| `logs [n]` | Show the last `n` lines of the SD access log |
| `login <pin>` | Unlock admin commands for 60 s (default PIN `1234`) |
| `logout` | Lock admin commands |
| `schedule` | Show the access schedule |
| `schedule HH:MM HH:MM` | Change the schedule (admin) |
| `settime YYYY-MM-DD HH:MM:SS` | Set the clock manually and stop NTP until reboot (admin) |
| `setpin <old> <new>` | Change the admin PIN (4-7 digits) |
| `room1 on\|off\|auto\|status` | Control Room 1 lighting |
| `room2 on\|off\|auto\|status` | Control Room 2 lighting |
| `mqtt` | Show MQTT connection state |
| `reboot` | Restart the controller (admin) |

Access policy:

- Entry is allowed only for allow-listed UIDs during the schedule window.
- Exit is always allowed for a UID that is currently inside.
- A second entry for a UID already inside is denied as a duplicate.
- Entry is denied when the inside count reaches 50.
- A Key Fob or any other unknown card is denied.
- If the clock is unsynchronised, the schedule fails open so the demo still works; use `settime` after `login 1234` to make schedule tests deterministic.

## Demo

See `DEMO_SCRIPT.md` for a complete video walkthrough covering:

- Boot, Wi-Fi, MQTT, and SD mount.
- Allowed and denied RFID cards.
- Duplicate entry, exit, and occupancy count.
- Schedule enforcement and admin authentication.
- Room PIR lighting and MQTT state.
- Remote room control from the entrance menu.
- SD log reading.
- LWT online/offline monitoring.

## Design notes and limitations

- **MQTT instead of ESP-NOW:** Wokwi has one MCU per project and does not emulate ESP-NOW. MQTT over the Wokwi internet gateway provides the required inter-terminal synchronization.
- **Custom MQTT client:** The PlatformIO ESP-IDF 6.0.1 package does not include the `esp-mqtt` component (only its test apps), so `mqtt_bus.c` implements the minimal MQTT 3.1.1 client directly over lwIP sockets. It supports QoS 0 publish/subscribe, retained messages, LWT, keepalive, and reconnect.
- **Custom MFRC522 driver:** No external RFID library is used. `rc522.c` implements SPI register access, REQA/anticollision, CRC_A, cascade levels for 4/7/10-byte UIDs, and SELECT.
- **No cJSON:** The ESP-IDF package has no JSON component, so payloads are built with `snprintf` and parsed with small, controlled string helpers.
- **Entrance as hub:** The entrance terminal owns the SD card, schedule, PIN, and access log, so it is the natural authority and monitoring point. Rooms remain independent controllers and keep working locally if MQTT is offline.
- **Public broker:** `broker.hivemq.com` is used for a zero-configuration demo. Client IDs include a MAC-address suffix to reduce collisions between simultaneous runs.
- **Wokwi limitations:** The simulated SD card is FAT16 and may reset between runs; NVS persistence is emulated and may also reset. The firmware falls back to safe defaults.

## Submission checklist

- [ ] Build all three projects (`pio run`).
- [ ] Record the demo using `DEMO_SCRIPT.md`.
- [ ] Add the three Wokwi share links (or video links) below.
- [ ] Complete registration numbers and contribution statements in `REPORT.md`.
- [ ] Export `REPORT.md` to PDF if required by the course.
- [ ] Submit the repository link and video link.

| Item | Link / owner |
| --- | --- |
| Entrance Wokwi link | _to be added_ |
| Room 1 Wokwi link | _to be added_ |
| Room 2 Wokwi link | _to be added_ |
| Demo video | _to be added_ |
| Report PDF | _to be exported_ |
