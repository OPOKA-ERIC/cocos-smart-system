# Demo Script

Target length: **5-7 minutes**.  
Recording layout: entrance simulation and its serial monitor on the left; Room 1 and Room 2 simulations on the right. Keep all three serial monitors visible when possible.

## 1. Pre-demo checklist (do this before recording)

- [ ] All three projects build: `pio run` in `entrance`, `room1`, `room2`.
- [ ] Wokwi Simulator extension is installed and licensed.
- [ ] Internet access is available (the broker is `broker.hivemq.com`).
- [ ] Entrance serial monitor is at 115200 baud.
- [ ] Room 1 and Room 2 serial monitors are open.
- [ ] Entrance simulation starts first, then Room 1, then Room 2.
- [ ] Wait for the entrance to print `MQTT connected` and the SD mount result.
- [ ] Run `status` once to confirm all three terminals are online.
- [ ] Close other programs using the same public MQTT topic prefix.
- [ ] Record at 1080p or higher; zoom the serial text so it is readable.

## 2. Opening narration (15 s)

> "This is our CoCIS Smart Control and Monitoring System. It has three ESP32-S3 terminals: an entrance hub with dual RFID readers and SD logging, and two room controllers with PIR-based automatic lighting. The terminals synchronize over MQTT because Wokwi does not support ESP-NOW or multi-MCU projects."

## 3. Boot and connectivity (45 s)

1. Start the **entrance** simulation.
2. Point at the serial output:
   - NVS/config load.
   - SD card mount at `/sdcard`.
   - Entry and exit MFRC522 initialisation.
   - Wi-Fi IP address.
   - `MQTT connected`.
3. Type:

```text
status
mqtt
schedule
```

Expected:

- `status` shows inside count 0, capacity 50, `SD: mounted`, MQTT connected, schedule `07:00-21:00`, and rooms online once they are started.
- `mqtt` prints the broker, client ID, and topic prefix.

4. Start **Room 1**, then **Room 2**. Wait for their `MQTT connected` lines.
5. Type `status` again on the entrance and show both rooms online.

## 4. Room automation (60 s)

1. In the Room 1 simulation, click the PIR sensor to trigger motion.
2. Show the Room 1 serial output: `occupied=1 light=1 mode=AUTO`.
3. Show the entrance `status` output: Room 1 `occupied=1 light=1`.
4. Stop interacting with the PIR. After about 15 seconds, show Room 1 turn the light off and publish `occupied=0 light=0`.
5. Repeat briefly for Room 2 (trigger motion, show state, then let it time out).

Narration:

> "Each room samples the PIR every 200 milliseconds. In AUTO mode the light follows occupancy, and it switches off 15 seconds after the last motion. The state is retained on MQTT, so the entrance and any late subscriber see the current state."

## 5. Remote room control (45 s)

On the entrance serial monitor:

```text
room1 off
room1 status
room1 on
room1 auto
room2 off
room2 auto
```

Expected:

- Entrance prints `Sent OFF to Room 1`, etc.
- Room 1 serial output shows `mode=OFF`, then `mode=ON`, then `mode=AUTO`.
- When Room 1 is back in AUTO and the room is empty, the light turns off.

Narration:

> "The entrance menu publishes plain-text commands to each room's command topic. The rooms accept ON, OFF, AUTO, and STATUS, and they acknowledge by publishing their new retained state."

## 6. RFID access control (90 s)

### 6.1 Granted entry

1. In the entrance simulation, click the **entry** reader (the one on the left; CS GPIO 5).
2. Select the **Blue Card** preset and click **Tap** (or **Hold** briefly).
3. Show:
   - One long beep and the door LED on.
   - Serial: `ENTRY ... 01:02:03:04 ... GRANTED`, inside count 1.
   - A JSON event on MQTT is published (show the serial log line or an MQTT client).
4. Type `status` and show inside count 1. Type `list` to show Alice inside.

### 6.2 Duplicate entry

1. Remove the card (or wait for the reader to report absence).
2. Tap the **Blue Card** on the entry reader again.
3. Show `DENIED_DUPLICATE`, three short beeps, and inside count still 1.

Narration:

> "The presence latch processes a card once per presentation. A card that stays on the reader does not generate a new event every few seconds, but a different card is processed immediately."

### 6.3 Unknown card

1. Tap the **Key Fob** (`C0:FF:EE:99`) on the entry reader.
2. Show `DENIED_UNKNOWN` and three short beeps.

### 6.4 Seven-byte UID

1. Tap the **NFC Tag** preset (`04:11:22:33:44:55:66`) on the entry reader.
2. Show the full 7-byte UID in the log and `GRANTED`.
3. This demonstrates the cascade-level 2 path in the custom MFRC522 driver.

### 6.5 Exit

1. Tap the **Blue Card** on the **exit** reader (CS GPIO 16).
2. Show `EXIT ... GRANTED` and inside count back to 1 (the NFC tag is still inside).
3. Tap the NFC tag on the exit reader to leave the building; inside count returns to 0.
4. Optional: tap the Blue Card on the exit reader again and show `DENIED_NOT_INSIDE`.

## 7. Schedule and admin authentication (75 s)

1. Try an admin command without logging in:

```text
schedule 08:00 20:00
```

Show `Login first: login <pin>`.

2. Log in:

```text
login 1234
```

3. Set the clock outside the access window (NTP is disabled after this command until reboot):

```text
settime 2026-10-26 22:00:00
schedule
```

Expected: schedule is `07:00-21:00 (CLOSED)`.

4. Tap the **Yellow Card** on the entry reader. Show `DENIED_SCHEDULE`.
5. Set the clock back inside the window:

```text
settime 2026-10-26 12:00:00
schedule
```

6. Tap the **Yellow Card** again. Show `GRANTED` and inside count 1.
7. Tap the Yellow Card on the exit reader to leave.
8. Optional: change the schedule to `06:00 22:00`, show `Schedule saved`, then change it back to `07:00 21:00`.
9. Type `logout`.

Narration:

> "Entry is schedule-gated, but exit is always allowed for someone inside. The schedule and PIN live in NVS. If the clock has never synchronized, the schedule fails open and the state reports `synced=false` so the operator knows enforcement is not active."

## 8. SD-card audit log (30 s)

```text
logs 12
```

Expected: the last CSV lines, including granted, duplicate, unknown, schedule-denied, entry, and exit events.

Narration:

> "Every decision is appended to `/sdcard/access.log` with a timestamp, event type, UID, name, inside count, and result. The file is CSV so it can be opened in a spreadsheet for the report."

## 9. Fault visibility and recovery (45 s)

1. Show both rooms online with `status`.
2. Stop the **Room 2** simulation (or disconnect it).
3. Wait for the entrance to print the Room 2 offline status. Run `status` and show `online=0`.
4. Restart Room 2. Wait for `MQTT connected`.
5. Run `status` on the entrance and show Room 2 online again, with its retained state restored.

Narration:

> "Each terminal registers a retained last-will message. If a room disappears, the broker publishes offline and the entrance marks it as offline. When the room returns, it reconnects, republishes online, and the entrance recovers the retained state."

## 10. Closing (30 s)

Show a final `status` and summarize:

- Entrance: dual RFID, allow-list, occupancy, schedule, PIN, SD log, MQTT hub.
- Rooms: PIR occupancy, automatic lighting, remote modes, retained state.
- Synchronization: MQTT with retained state, LWT, and automatic reconnect.
- All three ESP-IDF projects build successfully for the ESP32-S3.

> "The system meets the coursework requirements and is ready to demonstrate. The repository contains the firmware, Wokwi diagrams, report, and this demo script."

## 11. Useful commands during the demo

| Command | Purpose |
| --- | --- |
| `status` | Show hub + room + MQTT + SD state |
| `list` | Show people inside |
| `users` | Show the allow-list |
| `logs 12` | Show the last 12 SD log lines |
| `login 1234` | Unlock admin for 60 s |
| `logout` | Lock admin |
| `schedule` | Show the schedule and OPEN/CLOSED |
| `schedule 07:00 21:00` | Restore the default schedule |
| `settime 2026-10-26 12:00:00` | Set a deterministic demo time |
| `room1 status` / `room2 status` | Ask a room to publish its state |
| `mqtt` | Show MQTT connection details |
| `reboot` | Restart the entrance (admin) |

## 12. Troubleshooting while recording

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| No IP address | Wokwi gateway or Wi-Fi name | Confirm the Wi-Fi SSID is `Wokwi-GUEST` and the gateway is enabled |
| MQTT keeps retrying | Broker/DNS unavailable | Check internet access; wait for reconnect |
| Room not online on entrance | Room not started or topic mismatch | Start the room; run `status`; check the topic prefix |
| RFID reads nothing | Wrong part or pin | Use `board-mfrc522`, `3.3V`, shared SPI pins, separate CS |
| Card reads repeatedly | Card held on reader | Remove the card after the first event; the latch requires ~2 s absence |
| SD mount fails | Card not connected or FAT issue | Check `VCC/GND/CS/DI/DO/SCK`; firmware formats on mount failure |
| Schedule test fails | NTP overwrote manual time | `settime` now stops SNTP until reboot; use it after login |
| Buzzer silent | Wrong polarity | Buzzer pin 1 to GND, pin 2 to GPIO 18 |
| Room light stays on | Mode is ON | Send `room1 auto` |
