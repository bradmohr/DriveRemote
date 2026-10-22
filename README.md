# ACS550 Remote Panel

Touch-screen remote control for an **ABB ACS550-U1-072A-4** (50 HP, firmware 3.13D) running on a
**Waveshare ESP32-S3-Touch-LCD-4** (480×480). The panel talks Modbus RTU to the drive over RS485 and
mirrors everything to MQTT at the same time.

- **Drive page:** speed gauge (0102), run state, frequency, current, power and torque. It also has the speed
  setpoint in rpm (slider, ± buttons, or tap the value for a keypad), FWD/REV, START / STOP, coast stop,
  quick stop and fault reset, with the latest log line along the bottom.
- **Values page:** all eight actual values, the last fault with its name, and the 13 status-word bits.
- **Param page:** read or write any parameter in engineering units (2202 in seconds, 1105 in rpm, …).
- **Log page:** every command, comms event and fault with a timestamp, plus Modbus counters.
- **Setup page:** Wi-Fi (with network scan), MQTT broker (optional username and password), Modbus
  settings, brightness, screen timeout and time zone. Everything is stored in flash and survives reboots.
- **Screen timeout:** the backlight goes off after 15 minutes without a touch (Setup: never, 1, 5, 15, 30 or
  60 min). The drive is still monitored and MQTT keeps working. Touching the dark screen only turns it back on;
  that touch never reaches the button underneath, so buttons work again from the next touch.
- **Watchdog:** if the UI, Modbus or network task hangs for 20 s, the panel reboots and picks up
  control of the drive again.

> Not a safety function. Keep the hard-wired E-stop on the machine.

---

## 1. Wiring

| ACS550 terminal (X1) | Signal | Waveshare RS485 header |
|---|---|---|
| 28 | Screen | Cable shield (earth at one end only) |
| 29 | B (+) | **A** |
| 30 | A (−) | **B** |
| 31 | AGND | GND |
| 32 | Screen | – |

ABB names A and B the opposite way round to most RS485 devices. If the panel only ever reports
**Timeout (no reply)**, swap the two data wires. Doing so causes no damage. Use twisted pair. Set the drive's termination
switch **J2 = ON** if it is the end of the bus, and isolate the drive before opening the cover.

The board runs from 7–36 V DC on its power terminal, or from USB-C.

## 2. Drive parameters (set in this order)

| Par. | Name | Set to | Why |
|---|---|---|---|
| 1602 | PARAMETER LOCK | 1 OPEN | Allow edits |
| 9802 | COMM PROT SEL | 1 STD MODBUS | Enables the built-in RS485 port |
| 5302 | EFB STATION ID | 1 | Must match Setup → Station ID |
| 5303 | EFB BAUD RATE | 19.2 kb/s | Must match Setup → Baud rate |
| 5304 | EFB PARITY | 0 = 8 NONE 1 | Must match Setup → Format |
| 5305 | EFB CTRL PROFILE | 0 ABB DRV LIM | Control word format used by the panel |
| 1001 | EXT1 COMMANDS | 10 COMM | Start/stop from Modbus |
| 1102 | EXT1/EXT2 SEL | 0 EXT1 | Always use EXT1 |
| 1103 | REF1 SELECT | 8 COMM | Speed reference from Modbus |
| 1104 / 1105 | REF1 MIN / MAX | 0 / max speed | Bus reference 20000 = 1105 |
| 1003 | DIRECTION | 1 FORWARD (or 3 REQUEST) | 3 is needed for REV |
| 1601 | RUN ENABLE | 0 NOT SEL (or 7 COMM) | Don't leave it waiting on a DI |
| 1604 | FAULT RESET SEL | 8 COMM | Reset faults from the panel |
| 3018 | COMM FAULT FUNC | 1 FAULT | Drive trips if the panel goes quiet |
| 3019 | COMM FAULT TIME | 3.0 s or more | Comm-loss delay |

**Power-cycle the drive** after changing group 53, then press **LOC/REM** so the keypad shows
**REMOTE**. In LOCAL the drive ignores Modbus commands, and the panel shows `LOCAL / NOT REMOTE`. Parameter
writes over the bus are not saved permanently by the drive until `1607 PARAM SAVE = 1`.

## 3. Build and flash

Open the folder in VS Code with the PlatformIO extension, then use **Build** and **Upload** (✓ and → in the status bar).
From a terminal:

```
pio run -t upload          # or: python -m platformio run -t upload
pio device monitor         # USB log output, 115200
```

Connect the board's USB-C port. If the upload can't find the chip, hold **BOOT**, tap **RESET**, release
**BOOT**, and upload again. Libraries are fetched automatically. The only external one is LVGL 8.4.0.

Board revisions V1–V4 are all supported. The firmware detects the IO expander, either a TCA9554 or a
CH32V003, and the Setup page shows which revision it found. If the picture is upside down for your mounting, set
`LCD_ROTATION` to `0` in [include/app_config.h](include/app_config.h).

## 4. First start

1. Open **Setup**, tap **Scan**, pick your network and enter the password.
2. The broker comes pre-filled as `l11f1aXXXX.ala.asia-southeast1.emqxsl.com` (port 8883, TLS and login on).
   Replace `XXXX` to complete your EMQX deployment address, and enter the panel's username and password. For a
   different broker, enter its host and port. Turn on *Broker needs
   username / password* if needed. For a broker on the internet such as EMQX Cloud, turn on **Use TLS
   encryption**, which sets port 8883 (see section 9).
3. Check that the Modbus settings match the drive (19200, 8N1, station 1 by default).
4. Tap **Save & apply**. Settings take effect immediately without a reboot.

The header dots show **DRIVE / WIFI / MQTT** status: green means connected, yellow means connecting, red or grey means down
or not set up.

### Bench testing without the drive

[tools/acs550_sim.py](tools/acs550_sim.py) makes a PC act like the ACS550. Connect a USB-RS485 adapter to the
panel's RS485 A/B terminals, then double-click [tools/run_sim.bat](tools/run_sim.bat) (it uses COM4; edit the file if
your adapter is on another port). Or run it with the Python that PlatformIO installed, which already has pyserial:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools\acs550_sim.py --port COM4
```

It answers the same registers as the drive: 19200 8N1, station 1, and scalar mode with a 37 kW 50 Hz 1475 rpm motor.
All of this is set in **ACS550-Simulator.ini**, which it creates next to itself on the first run (press **e** to open
it, then restart): COM port, station, baud and format, scalar or vector, motor nameplate, REF1 MAX, ramp times,
direction, comm-loss trip, load, supply voltage, and any other parameter by number. START ramps the motor to the setpoint, STOP ramps it down, and COAST, QUICK STOP and
RESET work as on the drive. Amps, kW and temperature follow a pump load. If the panel goes quiet for 3 s, the
simulator trips with fault 28, as the drive does with `3018 = 1`. Flashing the panel while it runs causes this
too.

Keys: **f** trip a fault, **r** reset from the keypad, **l** keypad LOC/REM, **p** power-cycle the drive, **c** cut
the cable, **+ / −** load, **v** show every Modbus frame, **e** open the settings, **q** quit. If it reports bytes
arriving but no valid frames, swap A and B.

**On any Windows PC:** double-click [tools/build_exe.bat](tools/build_exe.bat) to build
`tools\dist\ACS550-Simulator.exe` (about 7 MB). Copy that one file to any 64-bit Windows 10/11 PC and double-click
it: no Python or install needed. It finds the adapter by itself, or asks if there are several serial ports. The PC
needs the adapter's driver (for CH340 adapters Windows Update usually installs it). Because the exe isn't
code-signed, Windows SmartScreen or antivirus may warn the first time; choose *More info → Run anyway*.

## 5. Speed scaling

Setpoints are always in **rpm**, both on the panel and over MQTT. On connect the panel reads `9904`, `1105`,
`9907` and `9908`:

- **Vector mode** (`9904` = 1 or 2): `1105` is in rpm, so reference = rpm / 1105 × 20000.
- **Scalar mode** (`9904` = 3): `1105` is in Hz. rpm is first converted with the motor nameplate:
  Hz = rpm × 9907 / 9908.

The slider's maximum is 1105 expressed in rpm. Larger requests are limited and logged.

## 6. MQTT

The base topic defaults to `Drive`. `XXXX` is always the 4-digit parameter or register number.

### Published by the panel

| Topic | Payload | Notes |
|---|---|---|
| `Drive/0102` | `1487` | Speed, rpm |
| `Drive/0103` | `49.6` | Output frequency, Hz |
| `Drive/0104` `0105` `0106` | `61.2` `48.3` `33.0` | A, %, kW |
| `Drive/0107` `0109` `0110` | `648` `455` `41.5` | DC bus V, output V, temperature °C |
| `Drive/0401` | `0` | Last fault code |
| `Drive/0004` | `1847` | Status word (raw) |
| `Drive/0001` | `1151` | Control word the panel is sending |
| `Drive/0002` | `1500` | Speed setpoint, rpm (negative = reverse) |
| `Drive/9904` `1105` `9907` `9908` | … | Read on connect |
| `Drive/XXXX` | scaled value | Any parameter read or written from the Param page or over MQTT |
| `Drive/state` | `RUNNING - AT SPEED` | Readable run state |
| `Drive/status` | `online` / `offline` | Retained, `offline` is the last-will message |
| `Drive/log` | `14:02:11 T Start: CW 0x047F REF 16667` | Every log line |

Values are scaled with the ACS550 resolutions (for example, 0103 raw 496 is published as `49.6`). A changing value is
published at most once per *publish interval* (1000 ms by default), and unchanged values are refreshed every
60 s. Values are retained unless you turn that off in Setup.

### Commands to the panel

| Topic | Payload | Action |
|---|---|---|
| `Drive/0001` | `START` `STOP` `COAST` `QSTOP` `RESET` (`ON` / `OFF` also work) | Start/stop the drive |
| `Drive/0001` | number, e.g. `1151` | Raw control word |
| `Drive/0002` | rpm, e.g. `3000` or `-1200` | Speed setpoint, scaled to the bus reference. Negative means reverse |
| `Drive/XXXX` | `?` | Read parameter XXXX and publish it to `Drive/XXXX` |
| `Drive/XXXX` | number, e.g. `Drive/2202` = `12.5` | Write parameter XXXX in engineering units |

```
mosquitto_pub -h broker -q 1 -t Drive/0002 -m 3000
mosquitto_pub -h broker -q 1 -t Drive/0001 -m START
mosquitto_pub -h broker -q 1 -t Drive/1010 -m "?"
```

Rules that keep this safe:

- **Retained command messages are ignored.** A stale retained `START` must never restart the motor after a
  reboot, so publish commands with retain off.
- Commands share the topics the panel publishes to, so the panel recognises and ignores its own messages
  when they come back. It publishes with QoS 0, so a command sent with **QoS 1** (as the phone app does) is
  always treated as a command. With QoS 0, a command that exactly matches a value the panel has just published
  on the same topic may be taken for its echo; send commands with `-q 1` to be sure. To keep commands
  separate completely, set **Command topic suffix** to `/set`. Commands then go to `Drive/XXXX/set`, and the
  panel still publishes values to `Drive/XXXX`.
- Actual values and fault history (groups 01, 03 and 04) are read-only.
- *Allow start / stop / speed over MQTT* is on by default. *Allow parameter writes over MQTT* is **off** by
  default, so turn it on in Setup if you want it.
- Anyone who can publish to your broker can run the motor. Use a broker username and password, and ACLs. Never
  use an open public broker such as test.mosquitto.org.

## 7. Watchdog and recovery

- The LVGL/UI task, the Modbus task and the network task are all watched by the ESP32 task watchdog. If any of them
  stops responding for **20 s** (`WDT_TIMEOUT_S`), the panel reboots.
- The reason for the last reset (for example *task watchdog (hang)*) is shown on Setup → Status, in the log, and
  in `Drive/log`. A count of watchdog reboots since power-on is shown with it.
- The Modbus task starts before the display, so polling resumes about a second after a reboot.
- After a reboot the panel reads the drive's current control word and reference and carries on from there.
  After a comms drop it only resumes writing the control word if the drive still holds the panel's
  last one. If the drive was power-cycled in the meantime, the panel monitors only until START is pressed.
- The drive's own `3018/3019` comm-loss trip is the safety net if the panel stops talking altogether.

## 8. Troubleshooting

| Symptom | Check |
|---|---|
| `Timeout (no reply)` | Nothing came back: wiring, 9802 = 1, station ID, drive power-cycled after changing group 53 |
| `Timeout: N bytes, no valid reply (…)` | Something came back but it's garbage: A/B swapped, or baud/parity don't match 5303/5304 |
| `Modbus exception 2` | Parameter or register does not exist on this firmware |
| `LOCAL / NOT REMOTE` | Press LOC/REM on the keypad |
| START does nothing | 1001 = 10, 1601 not waiting on a digital input, no active fault |
| REV is ignored | 1003 = 3 REQUEST |
| MQTT shows `bad username/password` | Broker credentials in Setup |
| Screen upside down | `LCD_ROTATION` in `include/app_config.h` |

## 9. Phone app and EMQX

[phone-app/](phone-app/) is a web app you add to your phone's home screen, on iPhone or Android. It connects to the same
EMQX broker as the panel over secure WebSockets and never talks to the panel directly. It has the same pages
as the panel: Drive, Values, Param, Log and Settings.

### EMQX setup

1. In the EMQX console, open the deployment overview and note the **address** and two ports: **8883** (MQTT over
   TLS, used by the panel) and **8084** (WebSocket over TLS, used by the phone).
2. Under **Access Control → Authentication**, add one user for the panel and one for the phone.
3. Recommended: under **Access Control → Authorization**, allow those users only `Drive/#`.
4. On the panel, open **Setup** and set host = the address, port 8883, **Use TLS** on, login on with the panel user,
   then **Save & apply**.
5. In the phone app, open **Settings** and enter `wss://<address>:8084/mqtt` with the phone user and base topic `Drive`.

The panel checks the broker's certificate against the public root certificates built into the ESP32. That
works for EMQX Cloud. A self-hosted EMQX needs a certificate from a public authority such as Let's Encrypt,
because a self-signed certificate will be rejected.

### Putting the app on the phone

The app must be served over HTTPS. Two free options:

- **Netlify Drop:** drag the `phone-app` folder onto <https://app.netlify.com/drop>.
- **GitHub Pages:** put the folder's files in a repository and turn on Pages.

Open the URL on the phone, then use **Share → Add to Home Screen** (iPhone, Safari) or **⋮ → Install app**
(Android, Chrome). The files contain no passwords. You type the broker login on the phone, and it is
stored only in that phone's browser. Untick *Remember password* if you don't want it stored.

### Behaviour

- **START** and changing direction while the motor runs both ask for confirmation. **STOP**, coast stop and quick
  stop act immediately.
- **Set speed** sends the setpoint in rpm and is limited to 1105 REF1 MAX.
- A banner warns when the phone isn't connected to the broker, when the panel is offline (from its last-will
  message), or when the panel can't reach the drive. Values are greyed out while the data is stale.
- Commands are published with QoS 1 and retain off, which is what the panel requires.
- The phone relies on the internet and the broker. It does not replace the local STOP or E-stop.

## 10. Source layout

| File | Purpose |
|---|---|
| [src/main.cpp](src/main.cpp) | Start-up order, watchdog, reset reason |
| [src/board.cpp](src/board.cpp) | IO expander detection, ST7701 + RGB panel, GT911 touch, LVGL drivers |
| [src/modbus_rtu.cpp](src/modbus_rtu.cpp) | Modbus RTU master (FC03/06/16) on UART1 |
| [src/acs550.cpp](src/acs550.cpp) | Control words, status bits, fault names, parameter scaling table |
| [src/drive.cpp](src/drive.cpp) | Drive task: polling, commands, rpm scaling, MQTT publishing |
| [src/net.cpp](src/net.cpp) | Wi-Fi, MQTT client, incoming commands |
| [src/ui.cpp](src/ui.cpp), [src/ui_setup.cpp](src/ui_setup.cpp) | Touch UI |
| [src/settings.cpp](src/settings.cpp) | Saved settings (NVS) |
| [include/app_config.h](include/app_config.h) | Pins, rotation, timeouts |
| [phone-app/](phone-app/) | Phone web app (`index.html`, `app.js`, `app.css`, service worker, icons, MQTT.js 5.10.1) |
| [phone-app/brand/](phone-app/brand/), [phone-app/fonts/](phone-app/fonts/) | Powermate logo and mark, Libre Franklin font (SIL Open Font License, see `OFL.txt`) |
| [phone-app-tests/](phone-app-tests/) | Phone app tests: local broker + simulated panel (`npm install`, `npm test`, `npm run shots`) |
| [tools/acs550_sim.py](tools/acs550_sim.py) | ACS550 simulator for bench testing the panel over a USB-RS485 adapter |
| [tools/run_sim.bat](tools/run_sim.bat), [tools/build_exe.bat](tools/build_exe.bat) | Start the simulator; build it into a stand-alone `ACS550-Simulator.exe` |
| [CLAUDE.md](CLAUDE.md) | Project notes that Claude Code loads automatically in new sessions |

To publish more parameters with proper scaling, add them to the table in `src/acs550.cpp`. Parameters not in the
table are published as raw integers.
