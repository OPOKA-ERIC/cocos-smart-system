# CoCIS Smart Control & Monitoring System

ESP32-S3 / ESP-IDF coursework project for the College of Computing and Information Sciences (CoCIS), Makerere University.

A single ESP32-S3 manages the main entrance (dual RFID readers, access control, occupancy tracking, SD-card audit log, buzzer/LED feedback, serial menu) and two lecture rooms (PIR occupancy sensing, automatic lighting with timeout).

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
|-- src/
|   |-- main.c         Application entry point and all system logic
|   \-- rc522.c        Custom MFRC522 SPI driver
|-- include/
|   |-- config.h       Pin definitions and compile-time defaults
|   \-- rc522.h        MFRC522 driver header
|-- diagram.json       Wokwi circuit diagram
|-- platformio.ini     PlatformIO build configuration
|-- wokwi.toml         Wokwi simulator configuration
|-- REPORT.md          Full technical report
|-- DEMO_SCRIPT.md     Step-by-step demo/video script
\-- WOKWI_SETUP.md     Wokwi run and sharing guide
```

## Architecture

All logic runs on a single ESP32-S3. Four FreeRTOS tasks run concurrently:

```
+--------------------------------------------------+
|                   ESP32-S3                       |
|                                                  |
|  rfid_entry task   rfid_exit task                |
|       |                 |                        |
|       +----+  +---------+                        |
|            |  |                                  |
|        handle_entry / handle_exit                |
|            |  |                                  |
|        s_state_lock (mutex)                      |
|            |                                     |
|        occupant list + SD log                    |
|                                                  |
|  room_task  ----  s_room_lock (mutex)            |
|       |           PIR1/PIR2 -> light1/light2     |
|                                                  |
|  menu_task  ----  serial monitor commands        |
+--------------------------------------------------+
```

Three mutexes prevent race conditions between tasks:
- `s_state_lock` — occupant list and inside count
- `s_sd_lock` — SD card file access
- `s_room_lock` — room occupancy and lighting state

## Hardware and pin map

| Function | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| Door-lock indicator LED | 2 | 220 Ω to GND |
| Buzzer | 18 | Positive pin; negative to GND |
| SD card CS | 10 | SPI3 |
| SD card MOSI | 11 | SPI3 |
| SD card MISO | 13 | SPI3 |
| SD card SCK | 12 | SPI3 |
| RFID shared MOSI | 6 | SPI2 |
| RFID shared MISO | 7 | SPI2 |
| RFID shared SCK | 15 | SPI2 |
| Entry reader CS / RST | 5 / 4 | `board-mfrc522` |
| Exit reader CS / RST | 16 / 17 | `board-mfrc522` |
| Room 1 PIR | 38 | `wokwi-pir-motion-sensor` |
| Room 1 light LED | 39 | 220 Ω to GND |
| Room 2 PIR | 40 | `wokwi-pir-motion-sensor` |
| Room 2 light LED | 41 | 220 Ω to GND |

Wokwi part names used by the diagram:
- `board-esp32-s3-devkitc-1`
- `board-mfrc522`
- `wokwi-microsd-card`
- `wokwi-pir-motion-sensor`
- `wokwi-led`
- `wokwi-resistor`
- `wokwi-buzzer`

## Default configuration

| Setting | Default |
| --- | --- |
| Access schedule | `07:00–21:00` |
| Admin PIN | `1234` |
| Capacity | 50 people |
| Room lighting timeout | 15 s without motion |
| Time zone | `EAT-3` (UTC+3) |
| SD log file | `/sdcard/access.log` |

The PIN and schedule are stored in NVS and survive a reboot on real hardware.

## Test cards

| Preset | UID | Policy |
| --- | --- | --- |
| Blue Card | `01:02:03:04` | Allowed — Alice |
| Green Card | `11:22:33:44` | Denied — not on allow-list |
| Yellow Card | `55:66:77:88` | Allowed — Bob |
| Red Card | `AA:BB:CC:DD` | Allowed — Carol |
| NFC Tag (7-byte) | `04:11:22:33:44:55:66` | Allowed — Dave |
| Key Fob | `C0:FF:EE:99` | Denied |

## Build

```bash
cd cocis-smart-system
pio run
```

## Run in Wokwi

1. Install VS Code with the PlatformIO IDE and Wokwi Simulator extensions.
2. Open this folder, build it (`pio run`), then run `Wokwi: Start Simulator`.
3. Open the serial monitor at 115200 baud.
4. Click a `board-mfrc522` to select a card and use **Tap** or **Hold**.
5. Click a PIR sensor to toggle motion in a room.

Full instructions are in `WOKWI_SETUP.md`.

## Serial menu

Type `help` in the serial monitor. All commands are line-based.

| Command | Description |
| --- | --- |
| `status` | Show hub state, rooms, SD, and admin lock |
| `list` | List people currently inside |
| `users` | Show the RFID allow-list |
| `logs [n]` | Show the last `n` lines of the SD access log |
| `login <pin>` | Unlock admin commands for 60 s (default PIN `1234`) |
| `logout` | Lock admin commands |
| `schedule` | Show the access schedule |
| `schedule HH:MM HH:MM` | Change the schedule (admin) |
| `settime YYYY-MM-DD HH:MM:SS` | Set the clock manually (admin) |
| `setpin <old> <new>` | Change the admin PIN, 4–7 digits (admin) |
| `room1 on\|off\|auto\|status` | Control Room 1 lighting (admin) |
| `room2 on\|off\|auto\|status` | Control Room 2 lighting (admin) |
| `reboot` | Restart the controller (admin) |
| `help` | Show this help |

## Access policy

- Entry allowed only for allow-listed UIDs within the schedule window.
- Exit always allowed for a UID currently inside.
- Duplicate entry (UID already inside) is denied.
- Entry denied when inside count reaches 50.
- Unknown or denied cards trigger three buzzer beeps.
- If the clock is unsynchronised the schedule fails open; use `settime` after `login 1234` for deterministic schedule tests.

## Design notes

- **Custom MFRC522 driver:** `rc522.c` implements SPI register access, REQA/anticollision, CRC_A, and cascade levels for 4/7-byte UIDs with no external library.
- **No cJSON:** Payloads and log lines are built with `snprintf` and parsed with small string helpers.
- **NVS persistence:** PIN and schedule survive reboots on real hardware; Wokwi may reset NVS when a simulation is recreated.
- **Wokwi SD card:** The simulated SD card is FAT16 and may reset between runs; the firmware falls back to safe defaults.

## Submission checklist

- [ ] Build the project (`pio run`).
- [ ] Record the demo using `DEMO_SCRIPT.md`.
- [ ] Add the Wokwi share link below.
- [ ] Complete registration numbers and contribution statements in `REPORT.md`.
- [ ] Export `REPORT.md` to PDF if required.
- [ ] Submit the repository link and video link.

| Item | Link / owner |
| --- | --- |
| Wokwi simulation link | _to be added_ |
| Demo video | _to be added_ |
| Report PDF | _to be exported_ |
