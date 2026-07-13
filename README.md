# QuietCool Attic Fan — ESPHome firmware

Open-source ESPHome firmware for the **QuietCool IT-AF-SMT Smart Attic Fan Control** hub (ESP32-WROOM-32D). It turns the controller already mounted on your fan into a local Wi-Fi device with native Home Assistant integration, a built-in web UI, and a REST API — no separate ESP32, Bluetooth proxy, or cloud service required.

It does everything the stock firmware does — including fan speeds, the countdown timer, autonomous temperature/humidity Smart Mode, buttons, and LEDs — while adding the Wi-Fi, Home Assistant, web, and API features above. The QuietCool Smart Control app continues to work over BLE just as it does with the stock firmware, and you can use it alongside Home Assistant at the same time. Install this firmware when you want native Home Assistant control and automation — locally or remotely through your Home Assistant setup — without keeping a phone or Bluetooth bridge within range of the attic.

**The hardware:** QuietCool sells the controller standalone as a retrofit for existing attic fans (~$69 — [manufacturer store](https://store.quietcoolsystems.com/products/quietcool-smart-attic-fan-control-replacement), [Amazon](https://www.amazon.com/QuietCool-Smart-Attic-Fan-Control/dp/B0B2ZKD5PG), [Home Depot](https://www.homedepot.com/p/QuietCool-Wireless-Smart-Control-For-Attic-Fans-IT-AF-SMT/321550100)), and it ships bundled with the AFG SMT series smart attic fans. Official docs: [IT-AF-SMT owner's guide (PDF)](https://quietcoolsystems.com/wp-content/uploads/2024/08/IT-AF-SMT-Owners-Guide-6-30-23-Web.pdf).

**Status:** fan control, Smart Mode (autonomous temp/humidity auto-switch), countdown timer, reboot-resume, LED indicators, button gestures, and full OEM BLE compatibility (stock QuietCool Smart Control app works with this firmware) are all live and tested end-to-end.

## ▶ Install it now — nothing to build

<a href="https://crazycoder.github.io/quietcool-esphome-native/"><img src="docs/images/web-installer-qr.png" align="right" width="150" alt="QR code — scan with a phone to open the Web Installer" /></a>

**[Open the Web Installer → crazycoder.github.io/quietcool-esphome-native](https://crazycoder.github.io/quietcool-esphome-native/)** from any Bluetooth-equipped computer or phone within radio range of the powered hub — Chrome or Edge on Windows, macOS, Linux, or Android, or Bluefy on iOS. It flashes the ready-to-run, credential-free firmware over the hub's own BLE OTA in about two minutes — no compiling, no downloads, no UART, nothing to host yourself. Step-by-step below: [Installing the firmware](#installing-the-firmware).

Installing from a phone? **Scan the QR code** instead of typing the URL.

> **A distribute-anywhere firmware bundle.** The credential-free factory build (`esphome compile quietcool-atticfan.factory.yaml`) ships with no Wi-Fi credentials, API encryption key, or OTA password baked in. At boot, `oem_nvs_reader` imports Wi-Fi credentials from the OEM-stored NVS partition, so a hub previously connected through the QuietCool app normally joins its network within about five seconds. The shared build then offers managed project updates from GitHub Pages in Home Assistant and on its local web page.

---

## Why replace the stock firmware?

The stock hub is controllable **only over BLE** — whatever controls it (the QuietCool app, or one of the [third-party BLE clients below](#alternatives--prior-art)) has to be within radio range of a box mounted in the attic. This firmware keeps everything the stock one does and adds a Wi-Fi brain:

| | Stock `IT-BLT-ATTICFAN_V4.1` | This firmware |
|---|---|---|
| **Control channel** | BLE only (Wi-Fi is used just for OTA downloads) — phone must be near the attic hub | **Wi-Fi + BLE at the same time** — control from anywhere on your network *while* the stock app keeps working over BLE |
| **Home Assistant** | — | **Native ESPHome integration**: fan card, temp/humidity sensors, timers, Smart Mode presets — automate against forecasts, presence, electricity prices, anything HA knows |
| **No Home Assistant?** | BLE app only | **Built-in web UI + REST API** — control from any browser, `curl`, or shell script on the LAN |
| **Timers** | One in-app countdown | Configurable default run time, delta services (`+30 min` / `-15 min` buttons), absolute "run High for 4 h" service |
| **Smart Mode** | Fixed decision tree | Same OEM tree and presets, plus per-rule enable/disable switches and temp/humidity calibration offsets |
| **History** | 31-day rolling log, viewable in the app | Unlimited via HA recorder / long-term statistics |
| **Power blip at 2 AM** | Fan stays off | Opt-in **Resume on Boot**: speed and remaining timer survive a reboot |
| **Louvers** | — | Opt-in HIGH pulse on start-up to pop sluggish louvers open |
| **Safety** | Over-temp + 24 h watchdog | Same cutoffs, plus fail-closed on invalid DIP wiring (stock leaves HIGH unguarded when the DIP is misconfigured) |
| **Recovery** | Re-flash via app | Layered: auto safe mode, Improv-BLE, captive portal AP, UART — plus one-button stock restore |
| **Updates & source** | Closed firmware, app-driven updates from the vendor CDN | GPL-3.0 source, unit-tested control logic, OTA from your own machine — fully local, no cloud in the loop |

Switching costs nothing: Wi-Fi credentials, Smart Mode thresholds, presets, and BLE pairings are **auto-imported from the OEM firmware on first boot**, the stock app continues to pair and control as before, and [multiple recovery paths](#going-back-to-stock)—including a local OEM-file upload that does not depend on a vendor URL—lead back to stock firmware if you change your mind.

---

## Alternatives & prior art

If replacing the firmware isn't for you, several projects control the fan by speaking the stock firmware's BLE protocol from the outside (that protocol is fully documented in [docs/OEM-BLE-PROTOCOL.md](docs/OEM-BLE-PROTOCOL.md)). Credit where due — the protocol reverse-engineering lineage starts with `emerose/quietcool`, which most of the others build on.

| Project | Approach | What you need | Feature surface & constraints |
|---|---|---|---|
| [rwarner/ha-quietcool-ble](https://github.com/rwarner/ha-quietcool-ble) | HA custom integration (HACS) speaking BLE to the stock hub | Bluetooth adapter on the HA host, or an ESPHome Bluetooth Proxy in range | Fan speeds, Idle/Timer/TH mode select, temp/humidity, TH thresholds. Polls every 10 s over a link the hub drops after ~25 s idle; its README notes pairing with the phone app can evict HA's stored credential; can read but not set the timer duration. Actively maintained. |
| [awkaplan/quietcool-esphome](https://github.com/awkaplan/quietcool-esphome) | Separate ESP32 running ESPHome as a BLE→HA bridge | A second, dedicated ESP32 placed in BLE range of the hub | On/off + 2 speeds, timer hour/minute entities, temp/humidity. No Smart (TH) mode control; ~15 s poll with a ~25 s full refresh cycle; the bridge holds the hub's single BLE connection, so the phone app has to be "forgotten". |
| [snyamathi/quietcool](https://github.com/snyamathi/quietcool) | Python BLE CLI + Homebridge shim (HomeKit, not HA) — fork of `emerose/quietcool` adding V2 (V4.x firmware) protocol support | BLE-capable host (e.g. a Raspberry Pi) near the fan | On/off, low/high; reads temp/humidity/presets. No Smart or Timer mode control; each BLE connection costs ~3–4 s, so HomeKit is served from a cache refreshed every 10 min. |
| [emerose/quietcool](https://github.com/emerose/quietcool) | The original Python BLE library/CLI (PyPI) — the reverse-engineering root | BLE-capable host near the fan | CLI reads status and pairs; most setters are unimplemented or untested. Dormant since early 2025. |

All of these keep the stock firmware — that's their strength (nothing to reflash) and their ceiling: every command rides a BLE link into the attic, inheriting the radio's range limit, connect-per-command latency, and polling instead of push. And since a BLE hub serves **one connected client at a time** (true of any GATT server, this firmware's BLE included), the bridge and the phone app contend for that connection — multiple phones work only by funneling everyone through HA.

Running *on* the hub removes that entire class of problems:

- **No BLE in the control path.** HA talks to the hub over Wi-Fi with ESPHome's encrypted, push-based native API — state changes appear instantly, from anywhere on the network, with no radio-range constraint and no polling loop. (The OEM BLE protocol authenticates clients with a pair code but has no transport encryption — equally true of stock, the alternatives, and this firmware's own BLE channel; the difference is that here HA control doesn't ride on it.)
- **No extra hardware.** No second ESP32, no Bluetooth proxy, no Pi near the attic — the hub itself is the smart device.
- **No contention with the app.** The stock app keeps the BLE connection to itself while HA uses Wi-Fi — simultaneously, not either/or.
- **The whole feature surface, not a client subset.** Delta and absolute timers, preset CRUD synced with the app, Smart Mode with per-rule toggles and sensor calibration, resume-on-boot, louver assist, web UI + REST — the logic runs where the relays are, so none of it depends on keeping a BLE session alive.

The honest trade-off: this project reflashes the hub. That's a bigger first step than installing a HACS integration — mitigated by the browser-based installer (no disassembly, ~2 minutes over the OEM's own BLE OTA) and [several paths back to stock](#going-back-to-stock), including local-file recovery.

---

## What you get in Home Assistant

After flashing and pairing, the device exposes the entities below. The fan, sensors, and timer service are the daily-use stuff; the rest are configuration / diagnostic.

### Daily use

| Entity | Purpose |
|---|---|
| `fan.<device>_attic_fan` | The fan. Preset modes are the available speed names per DIP wiring (Low, Med, High). Operating mode (Timer/Run/Smart) is controlled by the separate Fan Mode select — see below. |
| `select.<device>_fan_mode` | Operating mode: **Timer** / **Run** / **Smart** (default Timer). Run drops the countdown timer; Smart hands control to the autonomous temp/humidity logic. Lives in the device Configuration panel. See [Modes & timers](#modes--timers). |
| `sensor.<device>_runtime_remaining` | Live countdown of any active timer, in minutes. 0 when idle. |
| `sensor.<device>_temperature` | Attic temperature in °F (SHT30 sensor on the hub). |
| `sensor.<device>_humidity` | Attic humidity (%). |
| `text_sensor.<device>_fan_speed` | Last set fan speed: `Low` / `Medium` / `High`. Shows the speed that will be applied when the fan is turned on. |
| `text_sensor.<device>_smart_mode` | Smart Mode status: `Off` / `Monitoring` / `Running` / `Stabilizing` / `Sensor stale` / `Over-temp tripped` / `Watchdog tripped`. |
| `sensor.<device>_speeds_available` | Number of speed taps wired via DIP switch: `1` / `2` / `3` / `0` (invalid). |

### Custom HA actions (timer interface)

Two complementary services. Pick the one that matches how you're thinking about the action:

```yaml
# Delta-based — "add/subtract minutes to whatever's running"
service: esphome.<device>_extend_runtime
data:
  delta_minutes: 30      # positive = extend / start, negative = reduce / cancel, 0 = no-op

# Absolute — "run at this speed for exactly this long"
service: esphome.<device>_set_runtime
data:
  speed: high            # off / low / med / high (case-insensitive)
  minutes: 30            # 0 = run indefinitely; negative = ignored
```

Build buttons in HA for `+30 min`, `-15 min`, `Run High 4 hours`, `Stop now` — all from these two services. See [Modes & timers](#modes--timers) below.

### Custom HA actions (Smart Mode presets)

Up to 4 named presets (matching the OEM firmware limit). Each stores threshold temperatures, humidity values, enable/disable flags, and humidity response speed. There is always an active preset — selecting one applies its stored thresholds to the live entities. Editing any threshold in HA auto-saves back to the active preset. Use these actions to create, delete, or rename presets.

```yaml
# Save current live thresholds as a named preset (creates new or overwrites existing)
service: esphome.<device>_save_preset
data:
  name: "Summer"

# Delete a preset by name (refuses if it's the last one — at least 1 must remain)
service: esphome.<device>_delete_preset
data:
  name: "Winter"

# Rename a preset
service: esphome.<device>_rename_preset
data:
  old_name: "Custom 1"
  new_name: "Night Cooling"
```

On first boot after flashing over OEM firmware, existing presets are imported from the OEM NVS partition automatically. The stock Smart Control app's preset management (edit/save/delete) also works and syncs with the ESPHome select entity.

**Note:** After creating, deleting, or renaming a preset, the Smart Preset **dropdown options** in HA and the web UI are stale until the device is restarted (ESPHome limitation — select options are cached at startup). Press the **Restart** button in HA to refresh. The actual preset data is updated immediately — the OEM app's GetPresets, the web server's REST API, and all CRUD actions work in real-time.

### Configuration (in HA's device settings)

| Entity | Default | What it does |
|---|---|---|
| `number.<device>_default_run_time` | **180 min (3h)** | Auto-armed countdown when you turn the fan on from HA (Timer mode). Set to **0** to make default turn-on behave like Run mode (no auto-timer). Or set **Fan Mode** to **Run** to run indefinitely regardless of this setting. Persists across reboots. |
| `number.<device>_smart_t_high` | **37.8 °C (100 °F)** | Smart Mode: above this temperature → HIGH speed. |
| `number.<device>_smart_t_med` | **32.2 °C (90 °F)** | Smart Mode: above this → MED speed (3-speed wiring only). |
| `number.<device>_smart_t_low` | **26.7 °C (80 °F)** | Smart Mode: above this → LOW speed. |
| `number.<device>_smart_h_high` | **90%** | Smart Mode: above this humidity → STOP (condensation protection). |
| `number.<device>_smart_h_low` | **70%** | Smart Mode: above this humidity → run at Humidity Response speed. |
| `select.<device>_smart_h_response` | **Low** | Smart Mode: fan speed when humidity exceeds the low threshold (Off / Low / Medium / High). |
| `select.<device>_smart_preset` | **Summer** | Active Smart Mode preset. Selecting a preset applies its stored thresholds + enable/disable flags to the live entities. Editing thresholds auto-saves back to the active preset. Options populated from stored preset names (imported from OEM NVS on first boot). At least 1 preset always exists. |
| `switch.<device>_smart_control_ble` | **ON** | When ON, the firmware advertises the OEM BLE protocol (`ATTICFAN_<mac>`) so the stock QuietCool Smart Control app can discover, pair, and control the fan. **Control only — the app's in-app firmware update does nothing on this firmware (see [Going back to stock](#going-back-to-stock)).** Toggle OFF if you don't use the app — saves BLE radio time. |
| `text.<device>_fan_name` | **Generic** | Fan name shown in the Smart Control app. Auto-updates when Fan Model is changed. |
| `select.<device>_fan_model` | **Generic** | Fan model from the OEM app's model list (Generic, AFG SMT PRO-2.0, etc.). |
| `text.<device>_fan_serial_number` | *(empty)* | Fan serial number (max 25 chars). |
| `switch.<device>_louver_pop_open` | OFF | When ON, prepends a brief HIGH pulse on Off→Low/Med transitions to assist mechanical louvers opening via airflow. OEM stock controller doesn't have this; opt-in for fans with sluggish shutters. |
| `switch.<device>_dry_run_mode` | OFF | When ON, suppresses relay GPIO writes while everything else (logic, timer, HA state) runs normally. **Use this during install / debugging** so you can iterate firmware without actually cycling the fan. A deliberate ON persists across reboots, so OTA-flash iteration doesn't re-enable the relays behind your back. |
| `switch.<device>_resume_on_boot` | OFF | When ON, the firmware persists last speed + Smart Mode state and resumes after a controller reboot. Useful so a brief power blip doesn't kill night-cooling. OFF matches OEM cold-boot (safer). |
| `switch.<device>_smart_t_high_enabled` (×5) | **ON** | Per-rule enable switches for the five Smart Mode thresholds (T High / T Med / T Low / H High / H Low). Turning one OFF removes that rule from the decision tree. Stored per preset, like the threshold values. |
| `number.<device>_cal_t` | **0.0 °C** | Calibration offset added to the SHT30 temperature reading (applied before the °C→°F conversion). |
| `number.<device>_cal_h` | **0.0 %** | Calibration offset added to the SHT30 humidity reading. |

### Diagnostic

| Entity | Purpose |
|---|---|
| `button.<device>_restart` | Reboot the controller (everyday "I changed the DIP, refresh HA's view" button). |
| `button.<device>_safe_mode` | Reboot into ESPHome safe mode (Wi-Fi + OTA only, no custom components). Recovery path if a firmware push starts crashing. |
| `button.<device>_factory_reset` | **Factory reset from HA** — clears Wi-Fi credentials and ESPHome preferences, preserves OEM pairings/presets/settings, and reboots into Improv-BLE for re-onboarding. Same effect as the KEY1 ≥ 5 s hold. |
| `button.<device>_restore_stock_firmware` | Pull the verified OEM V4.1 image, preserve OEM NVS state, and reboot into stock firmware. |
| `button.<device>_reset_watchdog` | Clears the 24h runtime watchdog or over-temp trip. Required to re-enable Smart Mode after a safety event. |
| `binary_sensor.<device>_watchdog_tripped` | True when the 24h continuous runtime watchdog or over-temp safety has tripped. Clears via the Reset Watchdog button. |
| `binary_sensor.<device>_improv_ble_advertising` | True while the device is advertising Improv-BLE for Wi-Fi re-onboarding. |
| `sensor.<device>_ble_paired_devices` | Number of BLE pair-ids currently stored in NVS (max 50). Updates on pair/clear. |
| `switch.<device>_ble_pair_mode` | Toggle BLE pairing mode. Auto-turns OFF after 2 min timeout or when a device pairs successfully. Same effect as KEY2 short-press (OEM parity). |
| `button.<device>_clear_ble_pairings` | Wipe all stored BLE pair-ids from NVS. Existing Smart Control app clients must re-pair afterward. |
| `update.<device>_firmware` | New project releases for shared factory builds. Install from HA or from the local web UI; adopted Device Builder builds intentionally omit this generic updater. |
| `text_sensor.<device>_mac_wi_fi` | Wi-Fi MAC address. |
| `text_sensor.<device>_mac_ble` | BLE MAC address (differs from Wi-Fi MAC by +2 on ESP32). |
| `text_sensor.<device>_ota_active_partition` | OTA slot the running firmware booted from (label + flash offset). |
| `text_sensor.<device>_ota_target_partition` | OTA slot the next firmware push will be written to. |

---

## Modes & timers

The fan has three operating modes, selectable from the **Fan Mode** select (in HA's device Configuration panel) or via the stock QuietCool Smart Control app. The fan card's preset dropdown holds the *speeds* (Low/Med/High per DIP wiring), not the modes. Additionally, HA services provide fine-grained timer control.

### Mode comparison: ESPHome ↔ stock app ↔ OEM firmware

| ESPHome Fan Mode | Smart Control app | OEM firmware internal | Behavior |
|---|---|---|---|
| **Timer** *(default)* | Timer | mode 1 ("Run") | Countdown timer — fan runs at chosen speed for a set duration, auto-stops when timer expires |
| **Run** | Run | mode 4 ("Timer") | Indefinite — fan runs until manually stopped. 24h safety watchdog still applies |
| **Smart** | Smart (TH) | mode 2 ("Smart") | Autonomous — firmware reads SHT30 and drives speed per temp/humidity thresholds |
| *(fan off)* | Idle | mode 0 ("Off") | All relays off |

> **Why the confusing OEM names?** The OEM firmware's internal mode numbers have counter-intuitive labels: mode 1 is called "Run" internally but it's the *countdown timer*, and mode 4 is called "Timer" but it's actually *indefinite button-driven operation*. The BLE wire protocol swaps them to match user expectations ("Timer" on the wire = countdown, "Run" on the wire = indefinite). This firmware and the stock app both use the wire-protocol naming.

### 1. Timer mode (default — HA "Turn On" / fan card / percentage)

- Drives the relays at the requested speed.
- **Auto-arms a countdown** of `default_run_time` minutes (initial 180 = 3h). Fan auto-stops when timer expires.
- Matches the stock app's "Timer" mode.
- Set `default_run_time` to **0** in HA to disable the auto-arm (fan then runs indefinitely on turn-on, same as selecting Run in Fan Mode).

### 2. Run mode (select: `Fan Mode → Run`)

- Set **Fan Mode** to **"Run"** in HA's device Configuration panel (or call `select.select_option` with `entity_id: select.<device>_fan_mode, option: Run`). Also accessible via REST: `POST /select/fan_mode/set?option=Run`.
- Fan runs at the current/last speed with **no countdown timer**. Runs until you manually stop it, change modes, or the 24h safety watchdog trips.
- Matches the stock app's "Run" mode: *"Run mode will operate your attic fan at the set speed indefinitely."*
- Any `extend_runtime` or `set_runtime` call with a duration > 0 switches back to Timer behavior.

### 3. `extend_runtime` custom service (delta-based)

- Always armed at the duration you specify. Bypasses `default_run_time`.
- Positive `delta_minutes`: extend an existing timer OR start fresh from off (resumes last speed).
- Negative: reduce; saturates at 0 (turns the fan off).
- 0: defensive no-op.
- The resulting total remaining time (existing timer + delta) is capped at `max_run_minutes` (1440 = 24h).

Example HA buttons:

```yaml
# +30 min
service: esphome.<device>_extend_runtime
data: {delta_minutes: 30}

# -15 min
service: esphome.<device>_extend_runtime
data: {delta_minutes: -15}

# Stop immediately
service: esphome.<device>_extend_runtime
data: {delta_minutes: -9999}
# (or just fan.turn_off — same effect)
```

### 4. `set_runtime` custom service (absolute, deterministic)

- One call sets both speed AND duration. Use when you want a known end-state regardless of what the fan was doing.
- `speed`: `off` / `low` / `med` / `high` (case-insensitive; `medium` is also accepted).
- `minutes`: `0` runs indefinitely (no timer — same as Run mode); `>0` arms a timer for exactly N minutes (capped at `max_run_minutes`); `<0` is ignored.
- `speed: off` cancels any running timer and stops the fan regardless of `minutes`.
- Refuses speeds not allowed by your DIP wiring (e.g. `low` on a 1-speed fan is ignored).

Example HA buttons:

```yaml
# At 8 PM: run High for 30 minutes
service: esphome.<device>_set_runtime
data: {speed: high, minutes: 30}

# Pin to Low indefinitely (no auto-off — same as Run mode)
service: esphome.<device>_set_runtime
data: {speed: low, minutes: 0}

# Force-off (equivalent to fan.turn_off)
service: esphome.<device>_set_runtime
data: {speed: off, minutes: 0}
```

### 5. Smart Mode (OEM "TH" mode — autonomous temp/humidity control)

- Set **Fan Mode** to **"Smart"** in HA's device Configuration panel (or call `select.select_option` with `entity_id: select.<device>_fan_mode, option: Smart`). Also accessible via REST: `POST /select/fan_mode/set?option=Smart`.
- **No HA or Wi-Fi needed** — Smart Mode runs autonomously. If HA goes down, the fan keeps controlling itself.
- Any manual speed change, timer start, or turn-off cancels Smart Mode. To re-enter, select "Smart" again.
- Thresholds are editable from HA's device Configuration panel. Defaults match OEM factory values (100/90/80 °F, 90/70 %). Temperature thresholds are stored in °C; HA auto-converts the display to match your unit preference.
- **OEM migration**: on first boot after flashing over OEM firmware, existing Smart Mode thresholds are automatically imported from the OEM NVS partition. No manual re-entry needed.

**How the decision tree works:** every 10 seconds the firmware reads the SHT30 and evaluates the thresholds top-to-bottom. The first matching rule wins:

| Priority | Condition | Action |
|---|---|---|
| 1 | humidity **>** `H High` (default 90%) | **STOP** — condensation protection (all wirings) |
| 2 | temperature **≥** `T High` (default 100 °F) | **HIGH** speed |
| 3 | temperature **≥** `T Med` (default 90 °F) | **MED** speed (3-speed wiring only; skipped on 1/2-speed) |
| 4 | temperature **≥** `T Low` (default 80 °F) | **LOW** speed (skipped on 1-speed) |
| 5 | humidity **≥** `H Low` (default 70%) | Run at **H Response** speed (default Low; configurable Off/Low/Med/High) |
| 6 | none of the above | **STOP** |

**Latching behavior (matches OEM):** the decision tree only evaluates when the fan is currently off. Once it starts the fan at a given speed, the fan **latches** at that speed until an external action stops it (HA command, button press, sensor stale, or safety cutoff). The tree then re-evaluates on the next 10-second tick. This prevents relay chatter when temperature hovers near a threshold boundary.

On **1-speed wiring**, only rules 1, 2, and 5 apply (with H Response limited to High or Off — Low/Med aren't available). On **2-speed wiring**, rule 3 (T Med) is skipped. The condensation cutoff (rule 1) uses strict `>` — humidity exactly at `H High` does **not** trigger a stop.

**Safety features** (ported from OEM firmware):
- **Over-temperature cutoff**: if the SHT30 reads ≥ 83.3 °C (182 °F), all relays are forced off regardless of mode. Latching — requires pressing "Reset Watchdog" in HA to resume.
- **24-hour continuous runtime watchdog**: if any relay has been on for 24 consecutive hours (any mode, not just Smart), all relays are forced off. Prevents forgotten fans from running indefinitely. Latching — same reset button.
- **Sensor stale watchdog**: if the SHT30 stops delivering valid readings for 5 minutes, Smart Mode suspends (relays off). Resumes automatically when the sensor recovers — no manual reset needed.
- **60-second stabilization gate**: Smart Mode won't drive relays for the first 60 seconds after boot, giving the SHT30 time to stabilize after power-on.

### 6. KEY1 physical button (no timer — matches OEM stock button)

- Short press: cycle speeds per DIP wiring (Off → High → [Med] → Low → Off …).
- No auto-timer is armed. Fan runs until you cycle to Off, or HA stops it.

### Reboot behavior

| Event | Fan ON/OFF + speed | Timer |
|---|---|---|
| HA Core / OS restart | unchanged | keeps ticking |
| Wi-Fi outage | unchanged | keeps ticking |
| Controller reboot, **Resume on Boot = OFF** (default) | OFF (OEM cold-boot parity) | cleared |
| Controller reboot, **Resume on Boot = ON** | resumed at last speed | resumed with computed remaining time (wall-clock based) |
| Controller reboot during a timer that would have expired | OFF | stays off (didn't try to "catch up") |

---

## Physical buttons (gestures)

Each gesture has visual LED feedback during the hold so you know what's about to happen before you let go.

| Action | Press duration | Effect | LED feedback during hold |
|---|---|---|---|
| KEY1 short | < 3 s | Cycle speed per DIP (Off → High → Med? → Low → Off) | normal patterns |
| KEY1 hold to warning | 3 – 5 s | (warning) | **LED2 fast-blinks** (5 Hz) |
| KEY1 release at ≥5 s | ≥ 5 s | **FACTORY RESET** — clears Wi-Fi credentials and ESPHome preferences, preserves OEM pairings/presets/settings, and reboots into Improv-BLE for re-onboarding | All 3 LEDs panic-blink (10 Hz) |
| KEY2 short | < 3 s | Context-aware. **If `Smart Control (BLE)` is OFF: re-enable it + enter pair mode** — the only physical way back when HA is unreachable; LED4 SlowBlink confirms it blind. **If ON:** toggle BLE pair mode (OEM parity — KEY2 = Pair). Pair mode auto-exits after 2 min or on successful pair. | normal patterns |
| KEY2 release at 3–5 s | 3 – 5 s | **Start Improv-BLE** for Wi-Fi re-provisioning. Stops OEM BLE to free advertising space; auto-stops after 5 min and restarts OEM BLE. LED4 goes VeryFast (10 Hz) while Improv is active. | **LED4 fast-blinks** (5 Hz warning) |
| KEY2 release at ≥5 s | ≥ 5 s | **SAFE MODE** — reboot with only Wi-Fi + OTA, useful for recovery if HA-side controls are unreachable | All 3 LEDs panic-blink |
| **BOTH KEY1 + KEY2** held 5–10 s | 5 – 10 s | (warning — keep holding for stock restore, or release to abort) | All 3 LEDs **SlowBlink** (1 Hz — distinct from single-button warnings so you know which gesture you're in) |
| **BOTH KEY1 + KEY2** released at ≥10 s | ≥ 10 s | **STOCK FIRMWARE RESTORE** — pulls OEM `IT-BLT-ATTICFAN_V4.1` from QuietCool's CDN over HTTP, flashes the inactive OTA slot, reboots into OEM. Requires Wi-Fi to be reachable. | All 3 LEDs panic-blink (10 Hz) |
| **BOTH KEY1 + KEY2** released before 10 s | 5 – 10 s | Aborted — no stock restore, AND the single-button factory-reset / safe-mode gestures are suppressed too (releases that were part of a dual hold don't fire single-button actions) | (resets to normal) |

**Factory reset** is destructive — wipes Wi-Fi credentials. Recovery: after the device reboots, it'll advertise Improv-BLE (LED4 at 10 Hz). Re-onboard via the Web Installer or any Improv-compatible app.

**Stock restore** is reversible — re-flash via the Web Installer to come back. The dual-button gesture is deliberately 10 s (vs 5 s for single-button gestures) so a one-handed grab can't trigger it by accident; either button can be released to abort cleanly. Note: if the firmware is so broken that Wi-Fi doesn't work, the dual-button gesture won't help (the OTA needs the network) — fall back to UART reflash in that case.

---

## LED patterns

LED1 is hardwired to power — no firmware control. LED2/3/4 are driven by the firmware at 10 Hz based on fan state, connectivity, and button-hold time.

```
LED2 (Mode)              LED3 (Speed)            LED4 (BLE — OEM parity)
─────────────────────    ───────────────────     ─────────────────────────────
Off    fan off           Off    fan off          Off       idle (paired)
On     fan on (Run mode) On     fan on           SlowBlink BLE pair mode active
1 Hz   timer running                             VeryFast  Improv-BLE advertising
```

**DIP-invalid override** (higher priority than fan/connectivity, lower than button gestures):

```
Speeds Available = 0    ALL LEDs → solid On         OEM parity: "fix your DIP switches"
```

**Button-hold overrides** (higher priority than the above; dual takes priority over single):

```
KEY1 held 3–5 s         LED2 → FastBlink (5 Hz)       warning: factory reset incoming
KEY2 held 3–5 s         LED4 → FastBlink (5 Hz)       warning: Improv / safe-mode incoming
Either held ≥5 s        ALL LEDs → VeryFast (10 Hz)   commit imminent — let go to fire
BOTH held 5–10 s        ALL LEDs → SlowBlink (1 Hz)   stock-restore intermediate — keep holding
BOTH held ≥10 s         ALL LEDs → VeryFast (10 Hz)   stock-restore commit — let go to flash OEM
```

When both buttons are held, the dual pattern wins — so if you're past the 5 s single-commit panic-blink and start to release, seeing **SlowBlink instead of VeryFast** tells you you're still in the dual gesture (releasing now safely aborts without triggering factory-reset or safe-mode).

---

## DIP wiring

The physical 3-position DIP switch on the PCB tells the firmware how many speed taps your fan motor has wired. **Set it to match what the IT-AF-SMT owner's manual specifies for your fan model** — the firmware uses the exact same encoding as the QuietCool stock firmware, so any DIP setting that worked with the OEM controller works here.

The detected wiring is published as the `Speeds Available` HA sensor:

| Sensor value | Speeds available in HA |
|---|---|
| `1` | Off, High |
| `2` | Off, Low, High |
| `3` | Off, Low, Med, High |
| `0` | None — fan refuses all speed commands (safety fail-closed) |

Change the DIP while the device is unpowered. After re-powering (or pressing the **Restart** button in HA), the new wiring is reflected in HA's available preset modes.

> The firmware reads GPIO13, GPIO21, GPIO14 (in that order) and decodes per the OEM truth table:
>
> | GPIO13 | GPIO21 | GPIO14 | Wiring |
> |---|---|---|---|
> | 1 | 0 | 1 | 2-speed |
> | 1 | 1 | 0 | 3-speed |
> | 0 | 1 | 1 | 1-speed |
> | any other combination | | | `not configured` |
>
> This encoding is identical to the stock QuietCool firmware (decoded by reverse-engineering the OEM DIP-switch read routine). Any DIP setting that worked with the stock controller produces the same result here.

---

## Safety reminders

- **Test before installing.** Use the `Dry Run Mode` switch to verify behavior in HA without actually toggling relays. Once satisfied, turn Dry Run off.
- **Factory reset is destructive.** It wipes Wi-Fi credentials. Have the Web Installer or a phone Improv-BLE app ready before pressing KEY1 for 5+ seconds.
- **Relays drive mains AC.** Don't lift them off the PCB to probe with multimeters while the unit is powered.
- **Invalid DIP fails safely.** If the DIP is set to an unsupported combination (Speeds Available = 0), all three LEDs light up solid (matching OEM behavior) and the firmware refuses all fan commands. Fix the DIP and reboot.

---

## Recovery

The firmware layers multiple independent recovery paths so a single failure mode never bricks the device. Pick the one matching what's lost — no single path depends on any other (factory reset doesn't need Wi-Fi, Improv doesn't need HA, safe mode doesn't need the custom components to compile cleanly).

| What's broken | Recovery |
|---|---|
| **Wi-Fi unreachable** (moved house, AP password changed, wrong SSID) | KEY2 hold **3 – 5 s** → starts Improv-BLE for 5 min (LED4 VeryFast). Re-onboard via the Web Installer or any Improv-compatible phone app. **No gesture needed, though:** the WiFi component auto-starts *both* `esp32_improv` and the captive portal **~90 s after the last good connection drops** (`ap_timeout: 90s`; this fires on a runtime loss, not just at boot). Alternatives: the fallback Wi-Fi AP **`QuietCool Setup`** (captive portal) comes up at that same 90 s mark — connect a phone to it and open the captive page; or the companion `qc-ble` CLI (published separately) — `qc-ble provision-wifi` over BLE from a laptop/desktop with a USB HCI dongle (live-switches Wi-Fi at runtime — no reboot — and keeps BLE up so you can retry creds immediately). If Wi-Fi stays down 15 min the device reboots and the 90 s recovery timers simply re-arm. |
| **`Smart Control (BLE)` got turned OFF** and HA is unreachable, so you can't turn it back on | KEY2 **short press** (< 3 s) → re-enables `Smart Control (BLE)` and enters pair mode. **LED4 SlowBlink confirms it blind.** This press is inert when BLE is already on (it falls through to its normal pair-mode toggle). |
| **`Smart Control (BLE)` got turned OFF *and* Wi-Fi is broken** (the worst case — no HA, no physical access) | Automatic — ESPHome itself brings up two recovery paths ~90 s after STA fails, on different radios: the **`QuietCool Setup` captive portal AP** (join from a phone, the captive page re-enters Wi-Fi creds; iOS needs Settings → Wi-Fi) and **Improv-BLE** (the WiFi component auto-starts `esp32_improv` at the 90 s `ap_timeout` — re-onboard via the Web Installer or any Improv-compatible phone app). Either is sufficient. The OEM BLE protocol surface stays off in this case (Improv and OEM BLE can't coexist on one advert), but it's not needed for recovery — once Wi-Fi is restored, HA can flip `Smart Control (BLE)` back on. |
| **Firmware crash-loops on boot** | Automatic — after 10 consecutive failed boots, ESPHome enters safe mode (Wi-Fi + OTA only) on its own. No gesture needed. From there, push a fixed firmware image over OTA. |
| **Firmware boots but is otherwise misbehaving** (runaway control logic, HA unreachable, etc.) | KEY2 hold **≥ 5 s** → reboot into safe mode manually. Or press the **Safe Mode** button in HA. Same effect: Wi-Fi + OTA only, no custom components — flash a corrected build over OTA. |
| **Lost BLE pairing**, or stock app says *"device memory full"* | `button.<device>_clear_ble_pairings` in HA wipes just the pair-id list. KEY1 hold **≥ 5 s** performs a Wi-Fi/ESPHome factory reset and reboots into Improv, but deliberately preserves the OEM pair-id list and presets; use **Clear BLE Pairings** when those must also be removed. |
| **Need to roll back to stock OEM firmware** | See [Going back to stock](#going-back-to-stock) below. HA and physical-button restores use the verified CDN image; the hub's own `/restore-stock` page can use a firmware URL or upload any locally saved OEM application image directly over the LAN. The Web Installer links to that local page; UART remains the universal offline fallback. |
| **Need to push a fresh firmware image** | Any OTA path in [Updating a running hub](#updating-a-hub-already-running-this-firmware) — `esphome run`, the dashboard, HTTP flash, or BLE. From HA, the **Safe Mode** button is helpful if the current firmware is unstable. Worst case: UART reflash (BOOT pin held LOW, `esptool write-flash` to `0x20000` or `0x200000`) — always works. |

**Why the layering matters.** The 10× auto-safe-mode is a hard, no-touch backstop: even if every gesture is unreachable (e.g., the enclosure is sealed), a fixable crash-loop self-rescues into a flashable state. The captive portal AP works without a phone-paired prior history. Improv-BLE works without any prior Wi-Fi. The dual-button stock-restore is intentionally 10 s (vs 5 s for single-button gestures) so a one-handed grab can't trigger it. Local OEM upload removes the dependency on QuietCool retaining a particular firmware URL, but still needs the hub's LAN web server. **UART is the universal offline backstop:** when the network and running firmware are both unavailable, UART always works.

---

## Hardware overview

| Component | Detail |
|---|---|
| MCU | ESP32-WROOM-32D (ESP32-D0WDQ6, dual-core 240 MHz, no PSRAM) |
| Flash | 4 MB ISSI IS25LP032 |
| Sensor | Sensirion SHT30 (T+H, no barometer), I²C on a 4-wire daughter board |
| Buttons | 2 × tactile (active-LOW with internal pull-up) |
| LEDs | 4 × through-hole (LED1 power = direct from VCC; LED2/3/4 firmware-driven, active-LOW) |
| Relays | 3 × mechanical (one-hot speed selector) — confirmed not TRIAC, no zero-cross |
| UART debug | 4-pad header (`G T R V`) on the PCB |
| Download mode | Hold GPIO0 LOW at power-up (BOOT pin header) |

### GPIO pin map

| GPIO | Function | Direction | Notes |
|---|---|---|---|
| 5  | Relay LOW       | output | One-hot speed selector |
| 22 | Relay MED       | output | Only meaningful on 3-speed wiring |
| 23 | Relay HIGH      | output | Always available |
| 25 | LED2 (mode)     | output | Active-LOW |
| 33 | LED3 (speed)    | output | Active-LOW |
| 32 | LED4 (BLE/pair) | output | Active-LOW |
| 13 | DIP bit A       | input  | Pull-up |
| 21 | DIP bit B       | input  | Pull-up |
| 14 | DIP bit C       | input  | Pull-up |
| 27 | KEY1 Test/Speed | input  | Pull-up, active-LOW |
| 26 | KEY2 Pair       | input  | Pull-up, active-LOW |
| 18 | SHT30 SDA       | i²c    | 100 kHz, addr 0x44 |
| 19 | SHT30 SCL       | i²c    | Same bus |

All GPIO assignments were verified against the OEM firmware (which uses the exact same pinout — this firmware mirrors it for hardware compatibility).

---

## Installing the firmware

### Path A: existing hub running OEM firmware (Web BLE) — the easy path

**Just open the hosted installer:** **[crazycoder.github.io/quietcool-esphome-native](https://crazycoder.github.io/quietcool-esphome-native/)** in a Web Bluetooth browser — Chrome or Edge on Windows, macOS, Linux, or Android, or Bluefy on iOS — from any machine within Bluetooth range of the powered hub. Follow the prompts — it flashes via the OEM's own unauthenticated BLE OTA mechanism in under 2 minutes. No UART, no enclosure cracking, and nothing to host: the page already serves the credential-free firmware bin alongside it. (Want to self-host or read the internals? See [`web-installer/`](web-installer/README.md).)

The web installer hosts a **credential-free factory build** — no Wi-Fi password, API encryption key, or OTA password is baked in. Onboarding is straightforward:

1. **Wi-Fi**: on first boot, the firmware reads the Wi-Fi credentials your OEM controller was already using (stored in flash by the QuietCool app, survives the firmware swap) and joins your network within ~5 s. If the OEM controller was never connected to Wi-Fi, the device falls back to Improv-BLE and you complete onboarding via the web installer.
2. **Home Assistant**: HA discovers the device through the ESPHome integration as `quietcool-atticfan-<id>`. Add that discovered integration for fan controls and the **Firmware** update entity. You do not need to take control in ESPHome Device Builder for normal use.

The shared factory build checks the project's GitHub Pages manifest every six hours and shortly after boot. When a release is available, install it from Home Assistant's **Firmware** update entity or from the Firmware card at `http://<host>/`. This keeps working without ESPHome Device Builder.

Firmware installed before managed updates were introduced in **1.1.0** needs one
final manual update through the hub's existing **OTA Update** file form (or the
Web Installer). Starting with 1.1.0, future project releases appear through the
managed update paths above.

**Take Control is for users who want to customize or locally compile the firmware.** Device Builder creates a small per-device configuration which references this repository and can add a unique API encryption key. From then on, compile and install updates from Device Builder. The adopted configuration deliberately omits the generic GitHub binary updater because installing that credential-free image would discard the adopted build's personalized API key.

If you do not run Home Assistant, the device and its managed updater also work from the built-in web UI on a trusted LAN. You can instead build an image with your own credentials via Path B.

### Path B: build from source (developer, any OS)

Works the same on Windows, macOS, and Linux. Install the ESPHome CLI first — `uv tool install esphome` (or `pip install esphome`); ESPHome 2026.5+ is required and enforced by `min_version` in the YAML.

```sh
git clone https://github.com/CrazyCoder/quietcool-esphome-native.git
cd quietcool-esphome-native
cp secrets.yaml.example secrets.yaml         # `copy` on Windows cmd; edit with your Wi-Fi + API key + OTA password
esphome compile quietcool-atticfan.dev.yaml  # dev build — bakes all secrets in from secrets.yaml
```

For a **credential-free distribution build** (the same image hosted by the Web Installer):

```sh
esphome compile quietcool-atticfan.factory.yaml
# firmware.ota.bin has NO Wi-Fi creds, NO API encryption key, NO OTA password.
# At boot, oem_nvs_reader imports Wi-Fi creds from the OEM-stored NVS partition.
# The factory wrapper adds the GitHub-managed Firmware update entity.
```

**Getting a first build onto the hub** — UART is only needed if the board can't boot any firmware at all:

- **Hub still on OEM firmware:** flash via [Path A](#path-a-existing-hub-running-oem-firmware-web-ble--the-easy-path) first (or self-host the [web installer](web-installer/README.md) with your own bin), then push your build over Wi-Fi — see [Updating a running hub](#updating-a-hub-already-running-this-firmware).
- **Bare or bricked board:** UART with the BOOT pin held LOW at power-up:

```sh
esphome upload quietcool-atticfan.dev.yaml --device COM5              # Windows
esphome upload quietcool-atticfan.dev.yaml --device /dev/ttyUSB0      # Linux
esphome upload quietcool-atticfan.dev.yaml --device /dev/cu.usbserial-XXXX  # macOS
```

### Updating a hub already running this firmware

Once the hub runs this firmware, every update path is over the air — no enclosure, no UART. Pick whichever fits your setup (all cross-platform):

| How | Command / where | Notes |
|---|---|---|
| **Managed release update** | Home Assistant → **Firmware**, or `http://<host>/` → **Firmware** → **Install** | Recommended for the shared factory build. The hub checks the versioned GitHub Pages manifest itself, shows release availability, downloads the matching bin, verifies its MD5, and retains normal rollback protection. |
| **ESPHome CLI** | `esphome run quietcool-atticfan.dev.yaml --device quietcool-atticfan-<id>.local` | Developer path. Compiles and OTA-pushes in one step on any OS (`flash.bat` wraps the same dev configuration). The push authenticates with the `ota_password` in `secrets.yaml`; it must match the device. |
| **ESPHome Device Builder** | Device card → **Install** → **Wirelessly** | Canonical path after **Take Control**. The local per-device wrapper keeps the remote `@main` package and personalized API encryption settings; generic release binaries are intentionally not offered. |
| **Root web-page upload** | Open `http://<host>/` → **OTA Update** → choose file | The original ESPHome Web Server OTA path remains available. Upload this project's `firmware.ota.bin` (or another compatible normal OTA app image) from the phone/computer. It posts to `/update` and retains ordinary app-rollback protection; do not upload a full factory image. |
| **Device firmware page** | Open `http://<host>/restore-stock` (or enter the host in the bottom card of the [Web Installer](web-installer/README.md)) | Same-origin LAN page for URL flashing and local OEM file restore. For OEM V4.1, click **Use known OEM V4.1**, then **Download and flash URL**—the hub downloads the verified image directly, so no local download/upload is needed. The local-file form is the version-independent fallback for a saved OEM image. |
| **HTTP flash API** | `curl -X POST -d '' "http://<host>/api/flash_url?url=<bin-url>&md5=<md5>"` | Scriptable version of the URL form. Omit `md5` and the hub fetches `<bin-url>.md5` instead. |
| **BLE** | OEM `Upgrade` (A=10) from a paired client | Headless flashing with no HA and no browser — see [Path C](#path-c-re-flash-custom-firmware-over-ble) below. |

> **Adopted devices:** **Take Control** creates a per-device YAML and makes Device Builder the update owner. Its remote package follows this repository's `main` branch, so each Device Builder compile uses the refreshed project source while retaining local substitutions and the generated API key. Do not install the shared credential-free release bin over an adopted build unless you intentionally want to return to factory update management.

### Path C: re-flash custom firmware over BLE

A hub already running this firmware can be re-flashed **over BLE**, using the OEM `Upgrade` command (A=10) — the same mechanism stock uses, exposed here for *custom* firmware. A paired client (e.g. the companion `qc-ble` CLI, published separately) sends `Upgrade` with a firmware URL; the hub downloads it over its own Wi-Fi via `ota.http_request`, verifies, and reboots. Useful for scripted/headless flashing without HA or a browser.

- **Auth-gated.** Only a *paired* client can trigger it, and the first pairing needs the physical KEY2 long-hold — so this is not a remote-flash hole for in-range strangers.
- **OEM domains are blocked.** URLs on `myquietcool.com`/`quietcool.com` no-op (ack success, flash nothing) so the stock app can't revert you to stock this way. Any other `http(s)` URL flashes.
- **Progress.** `GetUpgradeState` (A=5) returns `Downloading_Progress` once accepted and `Download_Fail` if the download fails; on success the hub reboots (BLE drops, then the device reappears).
- **Host a `.md5` companion.** `ota.http_request` requires a checksum (there's no skip-check), and the BLE `Upgrade` command is kept OEM-identical (URL only) — so the hub fetches the checksum from **`<url>.md5`**. Serve `firmware.ota.bin` *and* `firmware.ota.bin.md5` (32 hex chars) side by side. Generate it with `md5sum firmware.ota.bin` (Linux), `md5 -q firmware.ota.bin` (macOS), or `(Get-FileHash -Algorithm MD5 firmware.ota.bin).Hash.ToLower()` (PowerShell).
- The bin must be built for this project's [`partitions.csv`](partitions.csv) layout — same constraint as the HTTP-flash paths.

This complements the HTTP endpoint (`POST /api/flash_url`) and ESPHome OTA: it's the BLE-triggered equivalent.

---

## Repository layout

```
  quietcool-atticfan.yaml       # credential-free config imported on adoption
  quietcool-atticfan.dev.yaml   # local secrets-based developer wrapper
  quietcool-atticfan.factory.yaml # shared release wrapper + managed updater
  firmware-version.yaml         # installed/manifest version (single source of truth)
  wifi-dev.yaml                 # dev — Wi-Fi creds from secrets.yaml
  wifi-dist.yaml                # dist — no Wi-Fi creds; OEM-NVS auto-import
  creds-dev.yaml                # dev — api_encryption_key + ota_password from secrets
  creds-dist.yaml               # shared/adopted base — no API or OTA secret
  partitions.csv                # OEM-compatible: nvs@0x9000, ota_0@0x20000, ota_1@0x200000
  secrets.yaml.example          # template (real secrets.yaml is gitignored)
  flash.bat                     # compile + OTA upload, one shot (Windows sugar — `esphome run` does the same anywhere)
  LICENSE                       # GPL-3.0
  web-installer/                # Web BLE installer + link to device HTTP page (see its README)
  scripts/generate_update_manifest.py # emits the version/MD5-matched Pages manifest
  components/
    fan_controller/             # speed control + DIP guards + countdown timer + reboot-resume
    led_state_machine/          # 10 Hz LED driver from world state
    oem_nvs_reader/             # auto-import of OEM Wi-Fi creds at boot (dist build)
    oem_ble_compat/             # full OEM BLE protocol for the stock Smart Control app (see docs/OEM-BLE-PROTOCOL.md)
    http_flash_handler/         # URL flash + version-neutral local OEM restore
```

Each custom component has a `test/` subdirectory with host-side unit tests (run via `run_tests.bat` on Windows or `g++ -std=c++17 -I..` directly elsewhere; the batch runner auto-detects MSYS2 mingw64 in `C:\msys64` or `C:\tools\msys64`, honors a pre-set `MINGW` env var, and otherwise falls back to `g++` on `PATH`). Tests cover the fan/timer/restore/Smart Mode decision tree, dual-button gesture tracker, LED state machine, URL-flash validation, streaming local-stock image inspection, OEM-NVS import decision tree, and OEM BLE compatibility protocol (gate checks, field mapping, frame assembly, fan model catalogue, input validation).

---

## Going back to stock

> **The Smart Control app's in-app "update firmware" does nothing here.** The OEM BLE `Upgrade` command (A=10) **blocks QuietCool firmware domains** (`myquietcool.com`/`quietcool.com`) — it acks the request so the app doesn't error, but flashes nothing. This is on purpose: honoring it would let the app flash *stock* QuietCool firmware over your ESPHome build. So if you tap "update" in the app it will appear to succeed but change nothing. To actually go back to stock, use one of the paths below. (Everything else in the app — fan control, pairing, presets, fan info — works normally. Note A=10 *does* flash **non-OEM** URLs — that's a deliberate path for pushing *custom* firmware over BLE, see [Path C](#path-c-re-flash-custom-firmware-over-ble) — it just won't flash stock.)

Choose the path that matches what is still reachable:

1. **HA button: `Restore Stock Firmware`.** In `Settings → Devices → QuietCool Attic Fan → Diagnostics`, press the button. The firmware pulls the byte-perfect OEM `IT-BLT-ATTICFAN_V4.1` bin from `http://myquietcool.com/profile/upload/2025/11/18/IT-BLT-ATTICFAN_V4.1_20251118010357A008.bin`, verifies its MD5, writes it to the inactive OTA slot via the ESP-IDF canonical OTA APIs, marks that foreign slot VALID, and reboots into it. The active OTA slot can never be overwritten (ESP-IDF guarantees this), so the previous ESPHome image remains physically present in the other slot, but automatic rollback is deliberately disabled for this explicit stock restore because stock cannot self-confirm under the rollback-enabled bootloader.
2. **Physical: hold BOTH KEY1 + KEY2 for 10 seconds.** Same flow as #1, fired from a button-gesture lambda. Works when HA is unreachable, the network is up, and the firmware boots far enough to read button GPIOs. LED feedback: all 3 LEDs SlowBlink from 5–10 s ("keep holding"), then all 3 LEDs VeryFast at 10 s ("commit imminent — release to fire").
3. **Device firmware page: `http://<host>/restore-stock`.** Open it directly, or enter the host in the Web Installer's "Already running the ESPHome firmware?" card and let that card navigate there. To restore the known OEM V4.1 without first downloading a file, click **Use known OEM V4.1**, review the pre-filled URL and MD5, then click **Download and flash URL**. The hub downloads the verified image directly. The page is served by the hub so its update requests are same-origin and are not blocked as HTTPS-to-HTTP mixed content. The handler recognizes that exact pair, marks the foreign stock slot VALID so rollback does not undo the restore, then erases only the private `esphome` NVS namespace during final powerdown while preserving OEM pairings and settings. Use **this**, not the Web Installer's Web-BLE flow: this firmware's OEM BLE `Upgrade` command (A=10) blocks OEM firmware domains.
4. **Local OEM file on the same device page.** Save any OEM IT-AF-SMT application `.bin`, browse to `http://<host>/restore-stock`, and use **Restore OEM firmware from a local file**. The browser streams the file over the LAN; no vendor or project firmware host is involved. The page immediately disables both update forms, shows upload/verification progress, and keeps the final result visible instead of navigating to a text response. The device requires a valid classic-ESP32 OTA application descriptor, enforces the 1.875 MB slot limit, rejects this project's replacement image, and relies on ESP-IDF to verify the complete image checksum and embedded SHA-256. `IT-BLT-ATTICFAN` is reported as a confidence marker, not used as a version whitelist. Because the user explicitly selected OEM restore, a structurally valid foreign image is marked VALID and will not roll back—use only firmware obtained for this exact controller.
5. **UART reflash.** Always works, fully offline. Use a saved OEM application bin, open the enclosure, jumper GPIO0 to GND on power-up, then `esptool write-flash` it to the **active** OTA slot — `0x20000` (ota_0) or `0x200000` (ota_1); the boot log line `Loaded app from partition at offset 0x...` tells you which. Worst-case escape hatch.

> **MD5 of the stock bin**: `36d2e90dcfdd553272fc4eebdc3c4444`. If the URL ever serves a different bin, the OTA will abort before writing — by design.

> **Partition-layout requirement.** OTA writes to whichever slot ESP-IDF picks as the next target (typically the inactive one). Both `ota_0` and `ota_1` are 1.875 MB at the OEM addresses (`0x20000` and `0x200000`). OEM application bins fit by definition. This project's own firmware fits because [`partitions.csv`](partitions.csv) mirrors the OEM layout. Other ESPHome or third-party builds must ship the equivalent partition layout and should use a normal rollback-protected OTA path, not the explicit OEM-file restore.

---

## NVS partition constraints

The IT-AF-SMT ships with a **16 KB NVS partition** (4 pages of 4 KB each). The OEM firmware pre-populates it with Wi-Fi credentials, BLE pair-ids, Smart Mode thresholds, presets, and — critically — five `Group_*` blobs that log hourly temperature, humidity, and relay state for 31 rolling days (~2.5 KB total). On a device that has been running OEM firmware, three of four NVS pages are typically FULL with this static data before this firmware even boots.

**Why this matters.** ESP-IDF's NVS is append-only within each page. When a key is updated, the old entry is marked erased and a new entry is appended. Page-level garbage collection ("compaction") reclaims erased entries, but it requires at least one completely free (UNINIT) page to use as scratch. If all pages contain live entries and no page is free, NVS writes fail with `ESP_ERR_NVS_NOT_ENOUGH_SPACE` — even if the partition has plenty of logically-erased space that could be reclaimed.

ESPHome's `preferences` system stores fan state, calibration offsets, and configuration switches in NVS. With only one page available and no compaction headroom, preference writes can fail on a device migrated from OEM firmware.

### Automatic mitigation

The `oem_nvs_reader` component performs a one-shot cleanup on first boot:

1. **Erases the 5 `Group_*` blobs** from the OEM's `hx_list` NVS namespace (`Group_Tem`, `Group_Hem`, `Group_Time`, `Group_Year`, `Group_month`). These are rolling sensor logs with no value to this firmware — reclaiming them frees ~2.5 KB across ~2 NVS pages.
2. **Erases up to 24 duplicated preset-name keys.** The OEM firmware writes each preset name three times (e.g. `testnamem1` / `testnamem11` / `testnamem111`) but only ever reads the primary copy — the duplicates waste ~16 NVS entry slots. The primary copies (and therefore the presets themselves) are untouched.
3. **Sets a marker preference** so the cleanup never re-runs on subsequent boots.
4. **Preserves everything else** in `hx_list` — BLE pair-ids, Smart Mode config, presets, and fan name all survive intact.

The cleanup is idempotent: `nvs_erase_key` on an already-missing key returns `ESP_ERR_NVS_NOT_FOUND` (treated as success). If the marker write itself fails (because NVS is critically full), the cleanup still executes — next boot retries both the erase and the marker.

### Additional write-pressure reduction

| Measure | Effect |
|---|---|
| `preferences.flash_write_interval: 600s` | Dirty preferences flush every 10 min instead of ESPHome's default 60 s — 10× fewer NVS append cycles |
| BLE Pair Mode switch uses `ALWAYS_OFF` restore mode | No NVS write at all for this switch (pair mode is momentary, not user state) |

### Stock reversion safety

The erased `Group_*` blobs are **not required** by the OEM firmware for normal operation. When the stock firmware boots and finds a missing `Group_*` key, `nvs_get_blob` returns `ESP_ERR_NVS_NOT_FOUND` and the hourly logger simply starts fresh — creating the blob on its next periodic write. No crash, no data loss, no reconfiguration needed.

BLE pair-ids, Smart Mode thresholds, presets, and Wi-Fi credentials are untouched by the cleanup itself.

**OEM settings are preserved when restoring stock.** The restore finalizer erases only the private `esphome` NVS namespace; it leaves the OEM `hx_list` namespace intact, including BLE pair IDs, Smart Mode thresholds, presets, timer defaults, and fan metadata. ESP-IDF application OTA itself writes only the inactive app slot and `otadata`; it does not erase NVS. This was live-verified on hardware through repeated Web Installer round trips from ESPHome to OEM V4.1 and back: existing pair IDs continued to authenticate, and presets plus other distinctive OEM settings survived each transition.

---

## OEM BLE protocol

The stock hub speaks a small JSON-over-BLE protocol, and this firmware reimplements it so the OEM QuietCool Smart Control app keeps working. The complete wire reference — transport and framing, the V1 (named) and V2 (numeric) dialects, the pairing/authentication state machine, all 22 commands plus the two binary ones, Smart Mode, presets, and the OTA flow — is documented in **[docs/OEM-BLE-PROTOCOL.md](docs/OEM-BLE-PROTOCOL.md)**, including where this firmware deliberately behaves differently from stock. It's an independent reverse-engineering write-up; useful whether you're reading `oem_ble_compat` or writing your own BLE client for a stock hub.

---

## License

[GPL-3.0](LICENSE) — matching the license of the ESPHome C++ core these components build against.

---

## Disclaimer

This project is **not affiliated with or endorsed by QuietCool / QC Manufacturing, Inc.** The OEM firmware and brand names referenced are property of their respective owners. The replacement firmware in this repository is an independent reverse-engineering effort by hobbyists, for personal use, with no warranty.
