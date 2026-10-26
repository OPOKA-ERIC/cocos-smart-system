# CoCIS Smart Control & Monitoring System

**Course:** Smart Control and Monitoring Systems  
**Institution:** College of Computing and Information Sciences (CoCIS), Makerere University  
**Academic year:** 2025/2026  
**Submission date:** 2 November 2026

## Team

| Name | Registration number | Contribution |
| --- | --- | --- |
| OPOKA ERIC | _to be added_ | _to be completed_ |
| OJOK ERICK | _to be added_ | _to be completed_ |
| OPETO ISAAC | _to be added_ | _to be completed_ |
| KIZITO RYAN | _to be added_ | _to be completed_ |
| AUMA DILIS | _to be added_ | _to be completed_ |

## Abstract

This project implements a three-terminal smart building control and monitoring system on the ESP32-S3 using ESP-IDF and FreeRTOS, simulated in Wokwi. The entrance terminal performs RFID access control with two MFRC522 readers, validates cards against an allow-list, enforces a configurable schedule, tracks the number of people inside, logs every decision to a microSD card, and exposes a serial administration menu. Two room terminals use PIR sensors to detect occupancy and control lighting automatically, with remote ON/OFF/AUTO commands. Because Wokwi does not support ESP-NOW or multi-MCU simulations, the terminals are separate ESP-IDF projects synchronized through MQTT over the Wokwi internet gateway. The firmware uses a custom minimal MQTT 3.1.1 client over lwIP sockets and a custom MFRC522 SPI driver, avoiding unavailable or incompatible ESP-IDF components. All three projects compile successfully for the ESP32-S3 DevKitC-1.

## 1. Introduction

Buildings on campus need controlled entry, knowledge of how many people are inside, and lighting that responds to actual occupancy rather than staying on all day. A useful system must also keep an audit trail of access decisions and allow an administrator to change the access window without rebuilding the firmware.

The objective of this coursework is to design and implement a three-terminal smart control and monitoring system that demonstrates:

- RFID-based entry and exit control.
- Occupancy-aware automatic lighting in two rooms.
- Persistent access logging on an SD card.
- Serial monitoring and configuration menus.
- A configurable access schedule protected by authentication.
- Synchronization between terminals.
- A complete Wokwi simulation using ESP-IDF and FreeRTOS in C.

The implementation targets the ESP32-S3 DevKitC-1 and uses the ESP-IDF framework through PlatformIO. The final repository contains three independent projects (`entrance`, `room1`, and `room2`) and the supporting documentation.

## 2. Requirements and mapping

| Requirement | Implementation | Terminal |
| --- | --- | --- |
| Three terminals | Three separate ESP-IDF projects and Wokwi diagrams | All |
| RFID access control | Two MFRC522 readers, allow-list, entry/exit state | Entrance |
| Entry and exit validation | Separate entry/exit readers; exit always allowed for occupants | Entrance |
| In-building count | Occupant registry capped at 50 | Entrance |
| Automated lighting for two rooms | PIR + 15 s timeout + ON/OFF/AUTO modes | Room 1, Room 2 |
| SD-card storage | FATFS append-only CSV access log | Entrance |
| Serial menus | Line-based admin/monitoring console | Entrance |
| Configurable schedule with auth | NVS-backed start/end times, PIN session | Entrance |
| Synchronization | MQTT retained state, events, commands, LWT | All |
| Wokwi simulation | Three diagrams with correct Wokwi part IDs | All |

## 3. System architecture

### 3.1 Physical/logical topology

```text
                 +-----------------------------+
                 | MQTT broker                 |
                 | broker.hivemq.com:1883      |
                 +--------------+--------------+
                                |
        +-----------------------+-----------------------+
        |                       |                       |
+-------+--------+      +-------+--------+      +-------+--------+
| Entrance hub   |      | Room 1       |      | Room 2       |
| ESP32-S3       |      | ESP32-S3     |      | ESP32-S3     |
| 2x MFRC522     |      | PIR          |      | PIR          |
| microSD        |      | Light        |      | Light        |
| Buzzer + LED   |      |              |      |              |
+----------------+      +--------------+      +--------------+
```

Each terminal is an independent Wokwi simulation with its own ESP32-S3. The broker is the only shared component. This satisfies the one-MCU-per-Wokwi-project constraint while still allowing the terminals to exchange state and commands.

### 3.2 Role of the entrance hub

The entrance terminal is the authority for access control because it already owns the RFID readers, the SD card, the schedule, and the admin PIN. It:

- Reads cards from the entry and exit readers.
- Decides whether entry or exit is allowed.
- Maintains the list of people currently inside.
- Writes a CSV line for every decision.
- Publishes access events and retained hub state.
- Subscribes to room state and room online/offline status.
- Sends remote lighting commands to the rooms from its serial menu.

The rooms do not depend on the entrance for local lighting. If MQTT or Wi-Fi is unavailable, each room continues to run its PIR/light state machine and republishes state when the connection returns.

### 3.3 Why MQTT rather than ESP-NOW

The original plan used ESP-NOW. Wokwi does not emulate ESP-NOW and does not support multiple MCUs in one project (`wokwi-features` issue #1015). MQTT over TCP was selected because:

- It works through the Wokwi internet gateway (`Wokwi-GUEST`).
- It provides publish/subscribe decoupling, retained state, and last-will messages.
- It allows the three simulations to run independently.
- It is closer to a real building deployment than a simulated peer-to-peer link.

## 4. Hardware design

### 4.1 Components

| Component | Quantity | Purpose |
| --- | --- | --- |
| ESP32-S3 DevKitC-1 | 3 | One controller per terminal |
| MFRC522 RFID reader | 2 | Entry and exit card readers |
| microSD card module | 1 | Access log storage |
| PIR motion sensor | 2 | Room occupancy detection |
| LED + 220 ohm resistor | 3 | Door-lock indicator and room lights |
| Piezoelectric buzzer | 1 | Audible grant/deny feedback |

### 4.2 Entrance pin assignment

| Signal | GPIO | Notes |
| --- | --- | --- |
| Door-lock LED | 2 | Active high, 220 ohm to GND |
| Buzzer | 18 | Positive pin 2; pin 1 to GND |
| SD CS | 10 | SPI3 |
| SD DI (MOSI) | 11 | SPI3 |
| SD DO (MISO) | 13 | SPI3 |
| SD SCK | 12 | SPI3 |
| RFID MOSI | 6 | Shared SPI2 |
| RFID MISO | 7 | Shared SPI2 |
| RFID SCK | 15 | Shared SPI2 |
| Entry RC522 CS | 5 | `board-mfrc522` SDA |
| Entry RC522 RST | 4 | Active low |
| Exit RC522 CS | 16 | `board-mfrc522` SDA |
| Exit RC522 RST | 17 | Active low |

### 4.3 Room pin assignment

| Signal | GPIO | Notes |
| --- | --- | --- |
| PIR output | 4 | Digital input |
| Light LED | 2 | Active high, 220 ohm to GND |

### 4.4 Bus design

The two RFID readers share one SPI bus (`SPI2_HOST`) and use separate chip-select lines. The SD card uses a separate bus (`SPI3_HOST`) to avoid contention and to keep the card at a conservative 4 MHz. The ESP32-S3 GPIO matrix allows both SPI buses to use the selected pins.

Wokwi part identifiers are important. The RFID part is `board-mfrc522`, not `wokwi-rfid-rc522`. Its supply pin is named `3.3V`. The buzzer's pin 1 is the negative terminal and pin 2 is the positive terminal. These details were corrected against the official Wokwi part references.

## 5. Software design

### 5.1 Platform and project structure

| Item | Value |
| --- | --- |
| Framework | ESP-IDF 6.0.1 |
| Build system | PlatformIO Core 6.2.0 |
| Board | `esp32-s3-devkitc-1` |
| Language | C |
| RTOS | FreeRTOS |
| Network stack | lwIP |
| Storage | FATFS over SDSPI |
| Configuration | NVS |

Each project contains its own copy of `mqtt_bus.c` and `mqtt_bus.h`. This duplication is deliberate: Wokwi runs one project at a time and the projects must remain self-contained.

### 5.2 Entrance firmware

#### 5.2.1 Startup sequence

1. Create the state and SD mutexes.
2. Initialise NVS, then load the schedule and admin PIN (defaults: `07:00-21:00`, `1234`).
3. Configure the door LED and buzzer outputs.
4. Mount the SD card at `/sdcard` (format on mount failure, 4 MHz SPI).
5. Initialise the shared RFID SPI bus.
6. Initialise the entry and exit MFRC522 readers.
7. Start Wi-Fi in station mode and wait up to 30 s for an IP address.
8. Start SNTP with timezone `EAT-3`.
9. Start the MQTT client, subscribe to room state and room status topics.
10. Start the RFID tasks, serial menu task, and heartbeat task.

If Wi-Fi is unavailable, the hub still performs local access control and logs to SD. MQTT reconnects in the background.

#### 5.2.2 RFID task and card detection

There are two independent reader tasks, one per MFRC522. Each task:

- Calls `rc522_read_uid()`.
- Converts the UID to uppercase colon-separated text.
- Uses a presence latch to process a card once per presentation.
- Clears the latch only after eight consecutive failed reads (about 2 s of absence).
- Processes a different UID immediately if it replaces the previous UID.

The field is switched off and on after a successful read. This resets the card to the IDLE state, so a card that remains on the reader is still detected, while the presence latch prevents repeated access decisions. This is important for a stable demo: a held card produces one event, not one event every few seconds.

#### 5.2.3 Access decision algorithm

Entry:

```text
if UID is not on the allow-list:
    result = DENIED_UNKNOWN
else if the access window is closed:
    result = DENIED_SCHEDULE
else if the UID is already inside:
    result = DENIED_DUPLICATE
else if inside_count >= 50:
    result = DENIED_CAPACITY
else:
    add the occupant
    result = GRANTED
```

Exit:

```text
if the UID is inside:
    remove the occupant
    result = GRANTED
else:
    result = DENIED_NOT_INSIDE
```

The schedule gates entry only. Exit is always allowed for an occupant so that nobody is trapped inside after the access window closes.

Every decision produces:

- A serial log line.
- A CSV line in `/sdcard/access.log` (when mounted).
- A non-retained MQTT event on `/entrance/event`.
- A retained hub state update on `/entrance/state`.
- Visual and audible feedback: the door LED and one long beep for a grant, three short beeps for a denial.

#### 5.2.4 Schedule, authentication, and NVS

The schedule is stored as two minute-of-day values in NVS. The admin PIN is stored as a string. The serial menu supports:

- `login <pin>` to open a 60-second admin session.
- `schedule HH:MM HH:MM` to change the access window.
- `settime YYYY-MM-DD HH:MM:SS` to set the clock manually when NTP is unavailable; this also stops SNTP until reboot so the manual time is not overwritten.
- `setpin <old> <new>` to rotate the PIN (4-7 digits).

Schedule changes, manual time changes, and reboot require an authenticated session. The PIN comparison is deliberately simple for the coursework; a production system would store a salted hash and add rate limiting.

If the clock has not synchronized and no manual time has been set, the schedule fails open. This keeps the local access system usable and is reported in the hub state as `"synced": false`.

#### 5.2.5 SD logging

The log is append-only CSV with a header:

```text
timestamp,event,uid,name,inside,result
2026-10-26 18:42:10,ENTRY,01:02:03:04,Blue Card (Alice),1,GRANTED
```

A mutex serialises file access. `logs [n]` reads up to the last 4 KB of the file and prints the last `n` lines. The file is kept open only for the duration of each write, which reduces the risk of corruption if the simulation stops.

#### 5.2.6 Serial menu

The menu runs in its own task and uses `fgets(stdin)`. It provides status, occupant list, allow-list, log tail, login/logout, schedule, time, PIN, room commands, MQTT status, and reboot. The full command table is in Appendix B.

### 5.3 Room firmware

Both room controllers run the same code with compile-time configuration for the terminal name, MQTT client ID, and topics. The control task samples the PIR every 200 ms:

```text
if PIR is high:
    last_motion = now
    occupied = true
else if occupied and (now - last_motion) > 15 s:
    occupied = false

if mode == ON:
    light = true
elif mode == OFF:
    light = false
else:  # AUTO
    light = occupied
```

State changes are published as retained JSON. A heartbeat republishes the state every 10 s. The room subscribes to its command topic and accepts plain text or JSON:

- `ON` - force the light on.
- `OFF` - force the light off.
- `AUTO` - follow occupancy.
- `STATUS` - publish the current state immediately.

Commands are executed in the MQTT callback, which updates the shared state under a mutex, applies the GPIO output, and publishes the new state.

### 5.4 MQTT communication

#### 5.4.1 Custom client

The PlatformIO ESP-IDF 6.0.1 package does not include the `esp-mqtt` component (the `components/mqtt` directory contains only test applications). A minimal MQTT 3.1.1 client was therefore implemented in `mqtt_bus.c` over lwIP sockets.

Supported features:

- CONNECT with clean session, keepalive 60 s, and last-will message.
- CONNACK validation.
- QoS 0 PUBLISH for outgoing messages.
- SUBSCRIBE at QoS 0, with re-subscription after reconnect.
- Incoming PUBLISH parsing, including QoS 1 PUBACK handling.
- Retained messages.
- PINGREQ every 30 s and 1 s receive timeout.
- Automatic reconnect every 3 s.
- Client ID suffix derived from the Wi-Fi MAC address to avoid collisions.

The client is deliberately small. It does not implement TLS, QoS 2, persistent sessions, or wildcard management, none of which are required for this coursework.

#### 5.4.2 Topic design

The prefix `cocis/2026/opoka-eric` namespaces the project on the public broker. Each terminal has a state topic and a status/LWT topic; rooms also have a command topic. Retained state ensures that the entrance receives the latest room state even if it starts after the rooms.

`seq` is incremented on each state change. It is not a distributed consensus mechanism; it is a lightweight ordering hint for the dashboard.

#### 5.4.3 Online/offline monitoring

Each terminal registers a retained last-will message (`offline`) on its status topic and publishes `online` after a successful connection. The entrance subscribes to both room status topics. If a room simulation is stopped, the broker publishes the will after the connection is lost; the entrance logs and displays the room as offline. This demonstrates fault visibility without a separate heartbeat protocol.

### 5.5 Custom MFRC522 driver

`rc522.c` implements the SPI protocol directly:

- Register address encoding: `(reg << 1)` with the read bit `0x80`.
- SPI mode 0 at 1 MHz.
- Soft reset, timer setup, 100% ASK, and antenna control.
- Transceive helper with FIFO flush, IRQ polling, and error-register checks.
- CRC_A calculation using the MFRC522 CRC coprocessor.
- REQA, anticollision, BCC verification, SELECT with CRC, and cascade levels 1-3.
- Support for 4-byte, 7-byte, and 10-byte UIDs.
- Field reset after each read.

Supporting the 7-byte NFC tag required the full cascade sequence, not just the first anticollision response. This is why the driver includes CRC generation and SELECT frames.

### 5.6 Concurrency and reliability

| Shared resource | Protection |
| --- | --- |
| Occupant list, room state, online flags | `s_state_lock` mutex |
| SD file access | `s_sd_lock` mutex |
| MQTT socket writes | `s_tx_lock` mutex |
| MQTT connection state and subscriptions | `s_state_lock` in `mqtt_bus.c` |

Tasks:

| Task | Stack | Priority | Purpose |
| --- | --- | --- | --- |
| `rfid_entry` | 4096 | 5 | Entry reader |
| `rfid_exit` | 4096 | 5 | Exit reader |
| `mqtt_bus` | 8192 | 5 | MQTT connection/read loop |
| `control` (rooms) | 4096 | 5 | PIR/light state machine |
| `menu` | 8192 | 3 | Serial console |
| `heartbeat` | 3072 | 2 | Periodic state publish |

The MQTT task holds the transmit mutex while sending CONNECT, subscriptions, publishes, pings, and PUBACKs, preventing interleaved bytes on the socket. The read loop uses a 1 s socket timeout so it can check the connection flag and send pings without blocking forever.

## 6. Implementation results

All three projects compile for `esp32-s3-devkitc-1` with ESP-IDF 6.0.1:

| Project | RAM | Flash |
| --- | --- | --- |
| Entrance | 42,480 / 327,680 B (13.0%) | 897,217 / 1,048,576 B (85.6%) |
| Room 1 | 38,640 / 327,680 B (11.8%) | 772,625 / 1,048,576 B (73.7%) |
| Room 2 | 38,640 / 327,680 B (11.8%) | 772,625 / 1,048,576 B (73.7%) |

The entrance image is close to the 1 MB application partition limit. This is acceptable for the coursework and leaves enough headroom for the current feature set. Future additions should either reduce log verbosity or use a larger application partition.

The Wokwi diagrams were checked against the official part references:

- `board-mfrc522` with pins `3.3V`, `RST`, `GND`, `IRQ`, `MISO`, `MOSI`, `SCK`, `SDA`.
- `wokwi-microsd-card` with `VCC`, `GND`, `CS`, `DI`, `DO`, `SCK`.
- `wokwi-buzzer` with pin 1 negative and pin 2 positive.
- `wokwi-pir-motion-sensor` with `VCC`, `GND`, `OUT`.

## 7. Testing and validation

### 7.1 Build verification

Each project was built with PlatformIO. All three returned `SUCCESS` with no compiler warnings or errors. The generated `firmware.bin` and `firmware.elf` are used by `wokwi.toml`.

### 7.2 Functional test matrix

The following scenarios are implemented and are exercised by `DEMO_SCRIPT.md`. The observed result should be recorded in the demo video.

| ID | Scenario | Expected result |
| --- | --- | --- |
| T1 | Boot entrance | Wi-Fi + MQTT connect, SD mounted, hub state published |
| T2 | Tap Blue Card at entry | `GRANTED`, inside count 1, event + CSV line |
| T3 | Tap Blue Card again after removing | `DENIED_DUPLICATE` |
| T4 | Tap Yellow Card | `GRANTED`, inside count 2 |
| T5 | Tap Key Fob | `DENIED_UNKNOWN`, three beeps |
| T6 | Tap Blue Card at exit | `GRANTED`, inside count 1 |
| T7 | Tap Red Card outside schedule | `DENIED_SCHEDULE` |
| T8 | Tap NFC 7-byte tag | UID read correctly; `GRANTED` |
| T9 | Room 1 motion | occupied=true, light=true; after 15 s idle, both false |
| T10 | Send `OFF` to Room 1 | light=false, mode=OFF |
| T11 | Send `AUTO` to Room 1 | mode=AUTO, light follows occupancy |
| T12 | `logs 10` on entrance | Last CSV lines printed |
| T13 | Stop Room 2 simulation | Entrance logs Room 2 offline |
| T14 | Restart Room 2 | Entrance logs Room 2 online, retained state recovered |
| T15 | `login 1234`, change schedule | New schedule saved; admin session works |
| T16 | Wrong PIN | `DENIED_PIN` logged; admin remains locked |

### 7.3 Static checks

- The MQTT packet layout was reviewed against MQTT 3.1.1: CONNECT flags, remaining-length encoding, topic length prefixes, and SUBSCRIBE packet identifiers.
- The RFID sequence was reviewed against the ISO 14443A cascade procedure: REQA, anticollision with BCC, SELECT with CRC_A, and level 2/3 handling.
- Concurrency was reviewed for lock ordering: state, SD, and MQTT transmit locks are never acquired in conflicting nested orders.
- The Wokwi diagrams were reviewed against the official part pin names.

## 8. Discussion

### 8.1 Design decisions

**Entrance as hub.** The entrance already has the SD card, RFID readers, and the physical access point. Making it the authority keeps the access decision, log, schedule, and PIN in one place. Rooms remain simple and locally autonomous.

**Retained MQTT state.** Retained state means a late-starting entrance or observer sees the current room state immediately. The `seq` field helps detect stale data.

**Presence latch.** Real access systems must not grant access repeatedly while a card rests on the reader. The latch processes a card once per presentation and requires a short absence before the same card is processed again. A different card is processed immediately.

**Fail-open schedule when unsynchronised.** A clock failure should not lock everyone out of the building. The hub reports `synced=false` so the operator knows the schedule is not being enforced. The `settime` command provides a deterministic fallback for the demo.

**Custom MQTT and RFID code.** The ESP-IDF package available in this environment lacks the MQTT component and JSON library, and no external RFID library is guaranteed. Implementing the small required subsets keeps the build reproducible and demonstrates the underlying protocols.

### 8.2 Deviations from the original roadmap

| Roadmap item | Final implementation | Reason |
| --- | --- | --- |
| ESP-IDF 5.2.1 | ESP-IDF 6.0.1 | Version provided by the installed PlatformIO package |
| ESP-NOW synchronization | MQTT over Wi-Fi | Wokwi does not emulate ESP-NOW |
| Room 2 as monitoring terminal | Entrance hub as monitoring authority | SD, schedule, PIN, and RFID are physically at the entrance |
| Unknown/planned SD layout | `/sdcard/access.log` CSV | Simple, inspectable, and robust |

The functional requirements are still met; the monitoring role is centralised at the entrance rather than at Room 2.

### 8.3 Limitations

- The public HiveMQ broker is shared and unencrypted. The topic prefix reduces collisions but does not provide security.
- MQTT QoS 0 means an event can be lost if the broker or network fails at the wrong moment. Retained state and periodic heartbeats reduce the impact.
- The admin PIN is stored in plaintext in NVS. A production system should hash it.
- The schedule uses local time and assumes the timezone is correct.
- Wokwi's SD card and NVS may reset between simulation runs.
- The entrance firmware uses a large fraction of the 1 MB application partition.
- The RFID driver is tailored to the MFRC522 and the Wokwi card presets; it does not implement card authentication or sector reads.

### 8.4 Future work

- Add TLS and authenticated MQTT.
- Store a salted PIN hash and add login rate limiting.
- Add a web or display dashboard for room state and access events.
- Add a real-time clock with battery backup.
- Add door-open sensing and anti-tailgating logic.
- Add per-user access schedules and roles.
- Add OTA firmware updates.

## 9. Conclusion

The project delivers a working three-terminal smart control and monitoring system on the ESP32-S3. The entrance hub performs RFID access control, enforces a configurable schedule, tracks occupancy, logs decisions to SD, and exposes a serial admin menu. The two room terminals provide occupancy-aware lighting and remote control. MQTT provides the synchronization layer required by the Wokwi one-MCU-per-project constraint. All three firmware images build successfully, and the repository includes the Wokwi diagrams, a complete demo script, and setup instructions.

## References

1. Espressif Systems. *ESP-IDF Programming Guide* (ESP-IDF 6.0). https://docs.espressif.com/projects/esp-idf/
2. Wokwi. *board-mfrc522 RFID Reader Reference*. https://docs.wokwi.com/parts/board-mfrc522
3. Wokwi. *microSD Card Reference*. https://docs.wokwi.com/parts/wokwi-microsd-card
4. Wokwi. *PIR Motion Sensor Reference*. https://docs.wokwi.com/parts/wokwi-pir-motion-sensor
5. Wokwi. *Buzzer Reference*. https://docs.wokwi.com/parts/wokwi-buzzer
6. OASIS. *MQTT Version 3.1.1 Specification*. http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/
7. NXP Semiconductors. *MFRC522 Standard Performance MIFARE and NTAG Frontend Datasheet*.
8. ISO/IEC 14443-3. *Identification cards - Contactless integrated circuit cards - Proximity cards - Part 3: Initialization and anticollision*.

## Appendix A - MQTT payload schemas

### Entrance event

```json
{
  "term": "entrance",
  "type": "ENTRY|EXIT",
  "uid": "01:02:03:04",
  "name": "Blue Card (Alice)",
  "result": "GRANTED|DENIED_UNKNOWN|DENIED_SCHEDULE|DENIED_DUPLICATE|DENIED_CAPACITY|DENIED_NOT_INSIDE",
  "inside": 1,
  "seq": 7,
  "ts": "2026-10-26 18:42:10"
}
```

### Entrance state

```json
{
  "term": "entrance",
  "inside": 1,
  "capacity": 50,
  "schedule": "07:00-21:00",
  "time": "2026-10-26 18:42:10",
  "synced": true,
  "sd": true
}
```

### Room state

```json
{
  "term": "room1",
  "occupied": true,
  "light": true,
  "mode": "AUTO|ON|OFF",
  "seq": 12,
  "uptime_ms": 345678
}
```

### Status/LWT

```text
online
offline
```

## Appendix B - Serial commands

| Command | Authentication | Description |
| --- | --- | --- |
| `help` | No | Show command list |
| `status` | No | Show hub, rooms, MQTT, SD, admin state |
| `list` | No | List occupants |
| `users` | No | Show allow-list |
| `logs [n]` | No | Show last `n` SD log lines |
| `login <pin>` | No | Start a 60 s admin session |
| `logout` | No | End the admin session |
| `schedule` | No | Show the schedule |
| `schedule HH:MM HH:MM` | Yes | Change the schedule |
| `settime YYYY-MM-DD HH:MM:SS` | Yes | Set the clock |
| `setpin <old> <new>` | Old PIN | Change the PIN |
| `room1 on\|off\|auto\|status` | No | Control Room 1 |
| `room2 on\|off\|auto\|status` | No | Control Room 2 |
| `mqtt` | No | Show MQTT state |
| `reboot` | Yes | Restart the hub |

## Appendix C - Test card policy

| Preset | UID | Policy |
| --- | --- | --- |
| Blue Card | `01:02:03:04` | Allowed (Alice) |
| Green Card | `11:22:33:44` | Denied (unknown) |
| Yellow Card | `55:66:77:88` | Allowed (Bob) |
| Red Card | `AA:BB:CC:DD` | Allowed (Carol) |
| NFC Tag | `04:11:22:33:44:55:66` | Allowed (Dave) |
| Key Fob | `C0:FF:EE:99` | Denied |

## Appendix D - Contribution statement

Each member should complete this table and sign it before submission.

| Name | Work completed | Evidence (commit/video/section) | Signature |
| --- | --- | --- | --- |
| OPOKA ERIC |  |  |  |
| OJOK ERICK |  |  |  |
| OPETO ISAAC |  |  |  |
| KIZITO RYAN |  |  |  |
| AUMA DILIS |  |  |  |
