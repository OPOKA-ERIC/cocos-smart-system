# Wokwi Setup and Run Guide

This repository contains three separate ESP-IDF projects. Wokwi simulates one MCU per project, so the entrance, Room 1, and Room 2 must run as three separate simulations connected through MQTT.

## 1. Prerequisites

Install:

1. **Visual Studio Code**
2. **PlatformIO IDE** extension
3. **Wokwi Simulator** extension

The projects use:

- Board: `esp32-s3-devkitc-1`
- Framework: ESP-IDF 6.0.1
- PlatformIO Core 6.2.0

You need a Wokwi account/license for the simulator extension. Internet access is required for the Wokwi gateway and the MQTT broker.

## 2. Build a project

Open a terminal in each project folder and run PlatformIO. If `pio` is not on `PATH`, use the full path:

```powershell
cd D:\cocis-smart-system\entrance
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run

cd D:\cocis-smart-system\room1
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run

cd D:\cocis-smart-system\room2
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
```

Alternatively, open each folder in VS Code and use the PlatformIO **Build** button (checkmark icon).

The first build downloads the ESP-IDF toolchain and can take several minutes. Later builds are much faster.

## 3. Start the simulations

Wokwi uses the `diagram.json` and `wokwi.toml` in the project folder.

1. Open the `entrance` folder in VS Code.
2. Build the project.
3. Open the Command Palette (`Ctrl+Shift+P`) and run **Wokwi: Start Simulator**.
4. Wait for the simulation window and the serial output.
5. Open a **new VS Code window** (`File > New Window`), open `room1`, build, and start its simulator.
6. Repeat for `room2`.
7. Arrange the three simulation windows side by side.

Starting the entrance first is recommended so it is already subscribed when the rooms publish their retained state.

### Recommended window layout

```text
+-----------------------------+  +----------------------+
| Entrance simulation         |  | Room 1 simulation    |
| + serial monitor            |  | + serial monitor     |
+-----------------------------+  +----------------------+
+-----------------------------+  +----------------------+
| Entrance serial (large)     |  | Room 2 simulation    |
|                             |  | + serial monitor     |
+-----------------------------+  +----------------------+
```

For recording, keep the entrance serial monitor large; it shows the access decisions and room state.

## 4. Controls in Wokwi

### RFID readers

- Click the `board-mfrc522` component.
- Choose a card preset: Blue, Green, Yellow, Red, NFC Tag, or Key Fob.
- Click **Tap** for a short presentation or **Hold** to keep the card on the reader.

The entrance has two readers:

- **Entry reader:** CS GPIO 5, RST GPIO 4.
- **Exit reader:** CS GPIO 16, RST GPIO 17.

### PIR sensor

- Click the PIR component to toggle motion.
- The room light turns on in AUTO mode and turns off 15 seconds after the last motion.

### Reset and power

- Use the simulation's **Restart** button to reboot a terminal.
- Stop/start the simulation to simulate a device going offline (this triggers the MQTT last-will message).

## 5. Serial monitor

The Wokwi simulation opens a serial terminal at 115200 baud. If it does not, open the Wokwi serial monitor from the simulation panel.

Useful entrance commands:

```text
help
status
users
list
logs 12
login 1234
schedule
mqtt
```

The full command list is in `README.md` and `REPORT.md`.

## 6. Network behaviour

- Wi-Fi SSID: `Wokwi-GUEST` (open network).
- MQTT broker: `broker.hivemq.com:1883`.
- Topic prefix: `cocis/2026/opoka-eric`.
- Client IDs include a MAC-address suffix, so simultaneous runs should not collide.

If the broker does not connect:

1. Confirm the Wokwi gateway is enabled for the simulation.
2. Confirm the machine has internet access.
3. Wait for the retry loop (every 3 seconds).
4. Run `mqtt` on the entrance to see the current state.

If another team is using the same topic prefix, change `MQTT_TOPIC_PREFIX` in each project's `include/config.h` and rebuild all three projects. The prefix must match across the three terminals.

## 7. Expected startup messages

Entrance (abridged):

```text
CoCIS entrance controller starting
...
SD card mounted at /sdcard
RC522 entry reader ready
RC522 exit reader ready
Wi-Fi connected, IP 10.x.x.x
MQTT connected
```

Room (abridged):

```text
CoCIS room controller starting: room1
PIR GPIO 4, light GPIO 2
Wi-Fi connected, IP 10.x.x.x
MQTT connected
```

If MQTT connects, the room publishes `online` on its status topic. The entrance `status` command should show `online=1` for that room.

## 8. Resetting the demo state

### Occupancy

If people are left inside from a previous run, either:

- Restart the entrance simulation (in-memory occupant list is cleared), or
- Tap the matching cards on the exit reader until `status` shows inside count 0.

### NVS (schedule and PIN)

Wokwi may persist NVS between runs. To restore the defaults:

- Change the values back with `schedule 07:00 21:00` and `setpin <current> 1234`, or
- Recreate/reset the simulation's storage if your Wokwi version provides that option.

### SD card

The simulated SD card may be reset when the project is recreated. The firmware attempts to mount it and formats it if mounting fails. The log is written to `/sdcard/access.log` during the current session. Wokwi may not preserve SD writes across full simulation restarts.

## 9. Recording the demo

1. Follow `DEMO_SCRIPT.md`.
2. Use OBS Studio, Windows Game Bar (`Win+G`), or another screen recorder.
3. Record the full screen or a region containing all three simulations.
4. Keep the serial text readable (increase the font size or zoom the terminal).
5. Save the video as `demo.mp4` and upload it to Google Drive/YouTube (unlisted) for submission.
6. Add the link to `README.md` and the report cover sheet.

A typical recording is 5-7 minutes. Do a short dry run first to confirm the broker and all three terminals connect.

## 10. Sharing the simulation

Wokwi for VS Code runs the project locally and uses the local firmware build. Sharing options depend on your Wokwi plan and the simulator version:

- Record the demo video (always accepted for coursework).
- If your Wokwi environment supports publishing a project, create a public copy for each terminal and add the links to `README.md`.
- Otherwise, submit the repository plus the video; the grader can run the three projects using this guide.

Do not upload the `.pio` build directory or any local credentials. The repository already ignores build output.

## 11. Troubleshooting

| Symptom | Check |
| --- | --- |
| `Wokwi: Start Simulator` is missing | Install/enable the Wokwi Simulator extension and open a folder containing `wokwi.toml` |
| Firmware not found | Run `pio run` first; confirm the `firmware` path in `wokwi.toml` |
| `board-mfrc522` not recognised | Update the Wokwi extension; use `board-mfrc522`, not `wokwi-rfid-rc522` |
| RFID does not read | Check `3.3V`, `GND`, shared `MOSI=6`, `MISO=7`, `SCK=15`, and separate CS/RST pins |
| Buzzer does not sound | Pin 1 to GND, pin 2 to GPIO 18; the firmware drives a square wave |
| SD mount fails | Check `VCC/GND/CS=10/DI=11/DO=13/SCK=12`; the firmware formats on failure |
| Room never turns on | Check the PIR `OUT` is on GPIO 4 and the mode is `AUTO` or `ON` |
| Room offline on entrance | Start the room, check Wi-Fi/MQTT, and confirm the topic prefix matches |
| MQTT reconnect loop | Check internet/DNS; the broker is public and occasionally rate-limited |
| Time shows `unsynced` | Wait for NTP; or `login 1234` then `settime YYYY-MM-DD HH:MM:SS` |
| Schedule test keeps passing | Run `settime` after login; it stops NTP until reboot so the manual time persists |
| Entrance build exceeds flash | Remove debug strings or increase the application partition; current usage is about 85.6% |
| Client ID collision | Restart one terminal; IDs include the MAC suffix, but identical cloned simulations can still collide |

## 12. Quick verification sequence

After all three simulations are running, do this 60-second check:

```text
status                 # all three online, SD mounted
room1 off              # Room 1 light off
room1 auto             # Room 1 back to automatic
logs 5                 # SD log is writable
```

Then tap the Blue Card on the entry reader and confirm `GRANTED` and inside count 1. Tap it on the exit reader and confirm the count returns to 0. If those steps work, the demo is ready to record.
