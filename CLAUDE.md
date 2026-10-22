# Drive Remote - project notes for Claude

Remote control for an **ABB ACS550-U1-072A-4** variable speed drive (37 kW / 50 HP, 72 A, firmware 3.13D).
The owner is **Brad, Powermate** (powermate.com.au, electrical / off-grid solar, Gatton QLD, Australia).
The brief is in `Project.md` / `Project.txt`. `Project.html` is an older Web Serial panel that served as the
example for the register map and the look.

Two parts, which only talk to each other through an MQTT broker (the owner uses **EMQX Cloud**):

1. **Panel firmware** (`src/`, `include/`) for a **Waveshare ESP32-S3-Touch-LCD-4** (480×480) wired to the drive's
   RS485 (EFB) port. It acts as Modbus RTU master and bridges to MQTT.
2. **Phone app** (`phone-app/`): an installable web app (PWA) connecting to the same broker over `wss://`.

User-facing documentation is in `README.md`. Keep it up to date when behaviour changes.

## Repo and hosting
- GitHub: https://github.com/bradmohr/DriveRemote (public, branch `main`)
- Phone app on GitHub Pages: https://bradmohr.github.io/DriveRemote/phone-app/
- Commit only when the owner asks, and ask before pushing. Commits end with the `Co-Authored-By` trailer.

## Building (Windows)
- `pio.exe` is **blocked by Windows Application Control** on this PC. Run PlatformIO through its Python instead:
  `& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m platformio run -d "C:\Code\Drive Remote"`
  (use `-t upload` to flash). The VS Code PlatformIO buttons work for the owner.
- Platform: pioarduino `55.03.12` = Arduino-ESP32 **3.3.12** / ESP-IDF **5.5**. Board `esp32-s3-devkitc1-n16r8`
  (16 MB QIO flash, 8 MB OPI PSRAM). The PlatformIO core dir resolves to `C:/p/...`.
- Libraries: only `lvgl/lvgl@8.4.0` (configured by `include/lv_conf.h`, widget memory in PSRAM via
  `include/lv_mem_psram.h`). **Arduino_GFX was dropped**: 1.5.3 doesn't compile on core 3.3.12, so the display uses
  ESP-IDF `esp_lcd` directly. MQTT uses ESP-IDF **esp-mqtt** (`mqtt_client.h`), not PubSubClient.
- No host C/C++ compiler or system Node.js is installed. For JS checks, a portable Node 20 was downloaded into
  the session scratchpad.
- The firmware is flashed and running on the owner's board, a **V4 (CH32V003)**. Display and backlight confirmed working.
  Flash over USB on COM7; close any serial monitor first or the port is busy.

## Hardware facts (from Waveshare's code and wiki)
- RS485: UART1, **TX GPIO44, RX GPIO43**, transceiver switches direction itself (no DE pin).
  GPIO43/44 are UART0's default TX/RX, and this build's ESP-IDF console is UART0 (USB-JTAG is secondary).
  `ModbusRtu::begin` calls `gpio_reset_pin` on both first. Without it, UART0 TX kept driving GPIO43 and **no reply was
  ever received** (found on the bench 2026-10-22: TX worked, every poll timed out).
- I2C SDA 15 / SCL 7: IO expander, GT911 touch (0x5D or 0x14, polled), PCF85063 RTC.
  The GT911 once didn't answer for three ESP32 resets in a row (the expander and touch chip aren't reset with the
  ESP32). `touchInit` now retries and pulses TP_RST. If it's still missing, it logs the I2C devices it can see,
  and `touchRetry()` keeps probing every 3 s from the touch callback, pulsing reset every 15 s.
- IO expander is **auto-detected**: V4 boards have a **CH32V003 at 0x24** (reg 0x02 direction, 0x03 output, 0x05
  backlight PWM 0-255, **inverted: 0 = full, 255 = off** (confirmed on hardware: writing 255 gave a black screen); bits: TP_RST 1, LCD_RST 3, SYS_EN 5, buzzer 6). V1-V3 boards have a
  **TCA9554 at 0x20** (EXIO0 TP_RST, 1 backlight on/off, 2 LCD_RST, 3 SD_CS, 5 buzzer).
- ST7701 init over bit-banged 3-wire SPI (CS 42, SCK 2, MOSI 1). The init sequence is Waveshare's "type1"
  (in `board.cpp`). RGB565 bus: DE 40, VSYNC 39, HSYNC 38, PCLK 41, data B0-4 = 5,45,48,47,21,
  G0-5 = 14,13,12,11,10,9, R0-4 = 46,3,8,18,17. 12 MHz pclk, hsync 10/8/50, vsync 10/8/20, bounce buffer 20 lines.
- `LCD_ROTATION 2` (180°, Waveshare's default) is applied with LVGL software rotation, and LVGL also flips touch.
- **Screen saver** (owner's request): `ui.cpp` `screenSaver()` calls `boardScreenSleep()` after `screenOffMin`
  (setting, default 15, 0 = never) of `lv_disp_get_inactive_time`. In `board.cpp`'s touch callback, a touch while
  asleep restores the backlight and calls `lv_disp_trig_activity`. LVGL is told "released" until that finger lifts
  (`s_wakeTouch`), so the waking touch can't press a button. `boardBacklight()` stores the brightness and only
  applies it while awake.

## Firmware structure
| File | Role |
|---|---|
| `main.cpp` | Start order: settings → **drive task first** (the drive's comm-loss timer is running after a reboot) → display/UI → network. Task watchdog setup. Reset reason kept in RTC memory. |
| `board.cpp` | Expander detection, ST7701 + `esp_lcd` RGB panel, GT911, LVGL drivers |
| `modbus_rtu.cpp` | FC03/06/16 master using 4xxxx register numbers. **No flush after TX** (it raced the reply while Wi-Fi/TLS held the task up and cut off its first bytes): an echo is recognised as an exact copy of the request (for FC06 only after an echo has been seen on another request). Re-syncs on noise; a timeout reports how many bytes came back (garbage = A/B swapped) |
| `acs550.cpp` | Control words (ABB DRV LIM), status bits, fault names, **parameter scaling table** |
| `drive.cpp` | Drive task (core 0, **priority 10**, above MQTT/TLS at 5: at equal priority a TLS handshake paused it between TX and the echo flush, and the first reply byte was lost): polls, runs commands from a queue, rpm↔reference scaling, MQTT publish cache |
| `net.cpp` | Net task: Wi-Fi, esp-mqtt client (optional TLS using `esp_crt_bundle_attach`), incoming command parsing, echo suppression, log forwarding |
| `ui.cpp`, `ui_setup.cpp` | LVGL 8 UI: tabs Drive / Values / Param / Log / Setup, on-screen keyboard |
| `settings.cpp` | NVS settings (Wi-Fi, MQTT incl. TLS, Modbus, brightness, screen timeout, timezone) |

- The **watchdog** (20 s, `WDT_TIMEOUT_S`) covers the loop/LVGL task, the drive task, the net task and both idle tasks, and panics into a reboot.
- Only the UI task touches LVGL and I2C. The drive task never blocks on the network: publishes go through a queue
  to the net task.

## Drive control rules (safety-relevant, keep these)
- ACS550 settings: 5305 = 0 (ABB DRV LIM). CW STOP 0x0476, RUN 0x047F, COAST 0x047D, QUICK 0x047B, RESET 0x04F6.
  Bit 10 (0x0400) = fieldbus control.
- Speed setpoints are always **rpm**. Bus reference ±20000 = 1105 REF1 MAX. In scalar mode (9904 = 3), 1105 is in Hz
  and rpm→Hz uses 9907 / 9908. Scaling is read on connect from 9904, 1105, 9907 and 9908.
- On boot or reconnect, the panel reads back 40001/40002. It adopts the drive's command word on boot. After a comms
  drop it only resumes writing CW if the drive **still holds the panel's last CW**; otherwise it goes monitor-only
  until START.
- When the panel isn't writing CW, setpoint changes write REF1 (40002) only, which never starts the motor.
- STOP is always allowed. START needs comms and scaling, and refuses while the drive is faulted.

## MQTT contract (panel ↔ phone app ↔ other clients)
- Base topic `Drive` (configurable). The panel publishes **scaled** values to `Drive/XXXX`, where XXXX is the 4-digit
  parameter/register number: 0102 rpm, 0103 Hz, 0104 A, 0105 %, 0106 kW, 0107 V, 0109 V, 0110 °C, 0401 fault,
  0004 status word, 0001 CW, **0002 = setpoint rpm (negative = reverse)**, plus 9904/1105/9907/9908 on connect and
  any parameter read or written. Also `Drive/state` (text), `Drive/status` (online/offline, retained last will),
  `Drive/log`. Values are retained by default, changes are limited to one per `mqttMinIntervalMs`, and a 60 s heartbeat resends.
- Commands go to `Drive/XXXX<suffix>`, with an empty suffix by default as the owner asked (`/set` is optional):
  0001 = START/STOP/COAST/QSTOP/RESET/ON/OFF or a raw CW; 0002 = rpm; any other XXXX = `?` to read, or a value to write
  (engineering units; groups 01/03/04 are read-only).
- **Retained commands are always ignored.** With an empty suffix the panel ignores its own messages coming back.
  It publishes QoS 0, so **QoS ≥ 1 messages are always commands** (the phone uses QoS 1). QoS 0 messages are
  matched against what it published (`echoConsume`, 60 s), with a separate list for 0001/0002 so value traffic
  can't push them out. History: with `esp_mqtt_client_enqueue` the MQTT task sent only one queued message per
  loop (poll timeout 1 s, `MQTT_EVENT_QUEUE_SIZE` 1 in this build), so while running the echoes came back more than
  5 s late. The owner saw "read-only" warnings, and a late 0001/0002 echo could have been acted on as a command.
  Now `publishNow()` uses `esp_mqtt_client_publish` directly.
- Setup switches: "Allow start/stop/speed over MQTT" (default on), "Allow parameter writes over MQTT" (default off).
- EMQX Cloud: the panel uses host + port **8883** + TLS + login. For now the firmware default is the placeholder
  `l11f1aXXXX.ala.asia-southeast1.emqxsl.com` (8883, TLS, login on). The owner asked to keep the real deployment ID
  out of the public repo, so don't write it into code or docs. The owner's panel has the real address saved in NVS.
  An empty saved host falls back to the default. The phone uses `wss://<address>:8084/mqtt` (the app
  builds this from a bare address). Use separate users for panel and phone, with an ACL limited to `Drive/#`.

## Phone app (`phone-app/`)
- Plain HTML/CSS/JS with no build step. `vendor/mqtt.min.js` is MQTT.js **5.10.1** (MIT). It uses `timerVariant: 'native'`
  (the worker timers fail in jsdom, and the app reconnects on `visibilitychange` anyway).
- `brokerUrl()` in `app.js` normalises whatever is pasted (bare host, host:port, mqtts://, https://) to `wss://…:8084/mqtt`.
  It keeps a typed WebSocket port and replaces 1883/8883.
- START and reversing while running need confirmation; STOP/coast/quick are immediate. Commands are published with QoS 1 and retain off.
- **Powermate branding**: navy `#000C2F`, orange `#F05423`, blue `#3A7DC0`, grey `#DCDDDE`, font **Libre Franklin**
  (bundled in `fonts/`, OFL). White logo and orange battery mark are in `brand/` (taken from powermate.com.au).
  Installs as "Drive Remote". START/STOP sit directly under the gauge so they show without scrolling.
- `sw.js` serves network-first and falls back to its cache. Bump `CACHE` and the `SHELL` list when assets change.
- It works opened straight from disk (`file://`) as well as on GitHub Pages.

## Tests (`phone-app-tests/`)
- `test.js`: 40 end-to-end checks. Local aedes broker with a password, a simulated panel publishing retained values,
  and the real app in jsdom driven by clicks.
- `urltest.js`: 17 broker-address formats.
- `shots.js`: phone-size (390×844) screenshots via headless Edge and puppeteer-core.
- Run with Node 20: `npm install`, then `npm test` / `npm run shots` in `phone-app-tests/`. All passed at the last change.

## Drive simulator (`tools/acs550_sim.py`)
- Python + pyserial (run with PlatformIO's penv Python). Modbus RTU slave acting like the ACS550 for bench tests:
  ABB Drives state machine driven by the CW, ramps from 2202/2203/2208, pump load model, fault/reset, LOC/REM,
  power cycle, cable cut, and a 3018/3019 comm-loss trip (fault 28). Unknown registers answer exception 02.
- Owner's bench adapter: CH340 USB-RS485 on **COM4**. First hookup (2026-10-22) had **A/B swapped**: COM4 received
  `7f 7e f9 ff fd 97 eb fe` per request, which is the panel's 40004 read decoded with inverted polarity.
- `Drive`/`RtuServer` take an explicit `now`, so they can be tested in-process with a fake clock (a 57-check
  harness passed; it lived in the session scratchpad and isn't in the repo).
- Settings: `ACS550-Simulator.ini` next to the script/exe (`program_dir()`), written from `DEFAULT_CONFIG` on the
  first run. Engineering units are converted to raw parameters in `settings_from_config`; `[Parameters]` takes raw
  values by number. CLI options override it via `cfg.set`. The ini is gitignored.
- `tools/build_exe.bat` builds `tools/dist/ACS550-Simulator.exe` (PyInstaller 6.11.1 one-file, console, ~7 MB) in a
  private venv `tools/.buildenv`. Build output is gitignored. The unsigned exe ran here despite Application Control,
  and the owner confirmed it working on another PC (2026-10-22).
- Windows quirk: `isatty()` is True for stdin = NUL, so `interactive()` also checks `GetConsoleMode`. Without that,
  prompts block forever when run from scripts or tools.

## Status and open items
- The phone app is confirmed connecting to the owner's EMQX.
- Not yet verified: Modbus comms with the drive (first run showed `Poll: Timeout`); RS485 A/B polarity (ABB
  terminal 29 = B+, 30 = A−, swap if timeouts); whether ACS550 fw 3.13D allows reading 40001/40002 (falls back to
  monitor-only); touch alignment with the 180° rotation.
- Offered but not done: apply the Powermate palette and logo to the panel's LVGL UI as well (colours are in
  `src/ui_internal.h`).
- Offered for after drive testing: cut idle MQTT traffic by raising `MQTT_HEARTBEAT_MS` from 60 s to ~10 min (the
  phone app's stale banner uses `Drive/status`, not heartbeats) and adding a ~5 V deadband on 0107 DC bus voltage.
  Estimate: ~6 M msgs/month at 8 h/day running, about 0.3-0.8 GB, inside the EMQX Serverless free 1 GB.

## Future idea: multi-user, multi-drive (IDEA ONLY, do not implement unless the owner asks)
The owner is testing the single-drive setup first. Later he may extend it to several users and several drives.
Ideas discussed, roughly in priority order:
1. **One base topic per panel** (e.g. `Gatton/Pump1`). The firmware already supports this. Each panel publishes a
   retained `<base>/info` (name, site, model, kW, firmware) so the app can discover drives and show a drive list.
2. **Per-user EMQX accounts and ACLs**: viewer (read only), operator (start/stop/speed on chosen drives), installer
   (parameter writes). At scale, an app login that hands out EMQX JWTs instead of raw broker logins.
3. **Command identity**: per-user command topics (e.g. `<base>/cmd/<username>/0001`) enforced by EMQX ACL
   `${username}` placeholders, so the panel can log who issued each command (audit trail).
4. **Command acknowledgement and expiry**: commands carry an id and timestamp; the panel replies on an ack topic
   (accepted / rejected + reason) and ignores stale commands.
5. **Local lockout**: a physical "remote enabled" switch or the drive's run-enable input, so people working on the
   machine can block remote starts. Treat as a prerequisite for wider remote access.
6. **OTA firmware updates** once panels are on several sites (`default_16MB.csv` already has OTA slots).
7. **Back end later**: EMQX data integration to a database for history, trends, fault push notifications, reports.
8. **Other drive models**: keep drive-specific code (`acs550.cpp`) separate as a "drive profile". Consider
   generic topics (`<base>/speed`, …) alongside the parameter-number topics so the app is brand-agnostic.
