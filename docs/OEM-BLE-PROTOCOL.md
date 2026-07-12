# QuietCool IT-AF-SMT — OEM BLE protocol

A complete, independent description of the Bluetooth Low Energy protocol spoken by
the stock **QuietCool IT-AF-SMT Smart Attic Fan Control** hub (the ESP32 firmware
branded `IT-BLT-ATTICFAN`), recovered by reverse-engineering the OEM firmware image,
the analyzed Android build of the OEM Smart Control app, and live BLE captures.

This document exists for two audiences:

- **Anyone building a BLE client** for the stock hub (an app, a Home Assistant
  integration, a CLI) — it is the wire reference the community projects lacked.
- **Anyone reading this firmware's [`oem_ble_compat`](../components/oem_ble_compat/)
  component**, which re-implements this protocol so the stock QuietCool Smart Control
  app keeps working after you flash the ESPHome firmware. Where this firmware
  deliberately behaves differently from stock, that is called out in
  [§12](#12-how-this-firmware-implements-the-protocol).

> **Not affiliated with QuietCool / QC Manufacturing, Inc.** This is an independent
> reverse-engineering effort. Command names, brand names, and the app are the
> property of their owners. There is **no transport encryption and no OTA signature
> check** anywhere in this protocol — that is the OEM design, documented here as-is;
> see [§9](#9-firmware-update-ota-flow) and [§11](#11-quirks--gotchas).

The protocol is small: **one GATT characteristic**, JSON (plus two binary commands),
**two interchangeable command dialects**, and 22 numbered commands. Everything below
is on that one characteristic.

---

## Table of contents

1. [Transport (GATT, naming, framing)](#1-transport)
2. [Two dialects: V1 (named) and V2 (numeric)](#2-two-dialects-v1-named-and-v2-numeric)
3. [Authentication & pairing state machine](#3-authentication--pairing-state-machine)
4. [Command catalog](#4-command-catalog)
5. [Command details (request / response schemas)](#5-command-details)
6. [Field-name map, result values, buffer limits](#6-field-name-map-result-values-buffer-limits)
7. [Modes & Smart Mode decision tree](#7-modes--smart-mode)
8. [Presets](#8-presets)
9. [Firmware update (OTA) flow](#9-firmware-update-ota-flow)
10. [Binary commands (GetRecordData, SynchronizeTime)](#10-binary-commands)
11. [Quirks & gotchas](#11-quirks--gotchas)
12. [How **this** firmware implements the protocol](#12-how-this-firmware-implements-the-protocol)
13. [Credits & prior art](#13-credits--prior-art)

---

## 1. Transport

### GATT layout

| | UUID |
|-|------|
| **Service** | `000000ff-0000-1000-8000-00805f9b34fb` |
| **Characteristic** | `0000ff01-0000-1000-8000-00805f9b34fb` (write + notify) |

A **single characteristic carries both directions**: the client writes JSON command
bytes; the hub pushes JSON responses as notifications. The only descriptor is the
standard Client Characteristic Configuration Descriptor (CCCD, `0x2902`) used to
enable notifications. The service is not advertised in the scan payload — clients
find the hub by device name (below) and discover the service after connecting.

### Device name / advertising

The hub advertises as:

```
ATTICFAN_<12-char-lowercase-hex-of-BT-MAC>
```

The name is built at boot from the hub's Bluetooth MAC formatted as 12 lowercase hex
characters with no separators. For example a hub with BT MAC `A4:CF:12:9B:7E:60`
advertises as `ATTICFAN_a4cf129b7e60` (21 characters).

The OEM app discovers hubs by **name prefix** — it scans for anything beginning
`ATTICFAN_`. There is no manufacturer-data match and no service-UUID scan filter, so a
client (or a compatible firmware) only needs to advertise with that name pattern to be
discoverable. Advertising interval is 320 ms.

### Write / notify chunking

Both directions are chunked at **`ATT_MTU − 3`** bytes. With the default ATT MTU of
23, that is **20-byte** packets. A single logical message therefore spans several BLE
packets:

- Longest request is `Upgrade` with a ~100-char URL (~6 writes).
- Longest response is `GetParameter` (~150 bytes, ~8 notifications).

### The `QQ` response prefix (V4.1+ only)

Firmware V4.1 and later **prepend a literal ASCII `QQ`** (2 bytes, `0x51 0x51`) to
every response before the JSON. Older firmware does not. A robust client ignores this
entirely by framing on the first `{`.

### Framing algorithm

Because responses arrive in 20-byte notifications and may carry the `QQ` prefix,
frame them like this:

1. Append each incoming notification to a buffer.
2. Find the first `{`.
3. Attempt to JSON-parse from that `{` to the end of the buffer.
4. If it parses, dispatch the object and clear the buffer. If not, wait for the next
   notification and retry.

This handles both the `QQ` prefix and multi-packet responses with no version
branching. (The two binary commands in [§10](#10-binary-commands) are the exception —
they are not JSON; distinguish them by the byte *after* the leading `{`, see §10.)

---

## 2. Two dialects: V1 (named) and V2 (numeric)

The firmware accepts **two equivalent command dialects** on the same characteristic:

| | V1 (named) | V2 (numeric) |
|-|------------|--------------|
| Request | `{"Api":"<verb>", …long field names…}` | `{"A":<int>, …single-char keys…}` |
| Dispatch | matches on the `"Api"` string | switches on the integer `"A"` |
| Field names | verbose (`"PhoneID"`, `"Ssid"`, `"URL"`) | single character (`"P"`, `"S"`, `"U"`) |

- **Pre-V3.9 firmware** speaks **V1 only**.
- **V3.9 and later (including V4.x)** added the V2 numeric dispatcher and speak both.

### V4.1+ always *responds* in V2 shape

Regardless of which dialect you send, V4.1+ firmware wraps **every response** in V2
short keys (e.g. `{"A":13,"R":"Success","P":"No"}`). A V1 request gets a V2 response.

**Version-detection rule for clients:** send a V1 `Login` (universally accepted), and
check whether the response contains an `"A"` key. If yes → V4.1+ (use V2 numeric for
everything else). If no → V1-only firmware (use V1).

### Only four verbs survive the V1 named path on V4.1+

On V4.1 firmware the V1 named dispatcher is a thin legacy shim — it recognizes **only**
`Login`, `Login2`, `Pair`, and `SetSpeed`. Any other verb sent in V1 form
(`{"Api":"GetWorkState"}`, `{"Api":"SetMode",…}`, etc.) falls through with **no handler
and an empty response**.

The reason V1 ever appeared to work fully is that **the analyzed Android build of
the OEM app converts V1 → V2 before writing to BLE**: its code constructs
`{"Api":"GetWorkState"}` but a
serialization layer rewrites it to `{"A":1}` on the wire. So in practice, on V4.1+:
**send V2 numeric for everything**, and treat V1 `Login`/`Pair`/`SetSpeed` only as
convenient aliases.

### V1 `Upgrade` is a stub on V4.1

V4.1's V1 dispatcher lists `Upgrade` but its handler returns a bare `{}` with no ack.
Use the **V2** `Upgrade` (`A=10`) to get a proper acknowledgement.

---

## 3. Authentication & pairing state machine

A per-session state — call it **`pair_state`** — gates the dispatcher:

| `pair_state` | Meaning | Commands allowed |
|---|---|---|
| **0** | Initial / not authenticated | **`Login` (A=13) only** |
| **1** | Authenticated ("logged in") | **All commands** |
| **2** | In pair mode | `Login` (A=13) **and** `Pair` (A=14) only |

Transitions:

- **Boot → 0.** The hub powers on unauthenticated.
- **0 → 1.** `Login` succeeds with a pair-id that is stored in the hub's pairing list.
- **0 → 2 — physical button only.** An unpaired hub can be put into pair mode **only by
  the physical KEY2 long-hold** (~3 s on stock). There is deliberately **no remote path
  to the first pairing**: `PairMode` (A=15) is gated behind `pair_state == 1`
  (authenticated), so it can only let an *already-paired* client invite another phone —
  it cannot bootstrap the first pairing. This is the trust boundary: physical access ==
  authority to pair.
- **1 → 2.** An authenticated client calls `PairMode` (A=15), or the user does the KEY2
  long-hold. Pair mode **auto-expires after 2 minutes.**
- **2 → 1.** `Pair` succeeds; the new pair-id is added to the hub's pairing list. On
  V4.1 the hub **drops the BLE connection right after a successful `Pair`** — the client
  must reconnect and `Login` with the new id to confirm it persisted.
- **1 → 0.** Reboot, or `Reset` (A=22, full factory reset).

### The pairing list

- Pair-ids are stored in the hub's NVS under keys `Phone1` … `Phone50` (namespace
  `hx_list`), with a monotonic counter `pair_num`.
- **Cap is 50.** When `pair_num` reaches 50, `Pair` returns `R:"Beyond"` and the only
  recovery is a factory reset (there is no BLE command to decrement the counter).
- **Factory test pair-id.** Never-paired units ship with a factory test id
  `1234567dsad8wqw9asasd` pre-loaded as `Phone1`. It authenticates with no button press
  — which is how a brand-new hub can be flashed via the web installer — but it is
  **overwritten the first time a real phone pairs**, so it does not work on a hub that
  the OEM app has already paired.

### Login response `PairState`

`Login`'s response carries a `PairState` field (`P` in V2): `"Yes"` means the hub is
*also* currently in pair mode (accept `Pair`), `"No"` means normal operation.

---

## 4. Command catalog

Client-callable JSON commands occupy V2 codes **1–11** and **13–22**. Code **12** is
**not** callable — the hub uses it only for an unsolicited *push* (see the table row).
Two additional **binary** commands (28, 29) share the characteristic but use a raw-byte
format — see [§10](#10-binary-commands).

Legend for **Gate**: **pre** = allowed before authentication; **pair** = requires pair
mode (`pair_state==2`); **auth** = requires `pair_state==1`.

| A= | V1 verb | Dir | Gate | Purpose |
|----|---------|-----|------|---------|
| 1  | GetWorkState | read | auth | Mode + temperature + humidity + sensor health + speed |
| 2  | GetParameter | read | auth | All Smart Mode + Timer thresholds ([§7](#7-modes--smart-mode)) |
| 3  | GetVersion | read | auth | Software/hardware version, build date, over-temp cutoff |
| 4  | GetRouter | read | auth | Currently-stored Wi-Fi SSID + password (returned over BLE) |
| 5  | GetUpgradeState | read | auth\* | OTA progress state ([§9](#9-firmware-update-ota-flow)) |
| 6  | SetTempHumidity | write | auth | Write Smart Mode thresholds ([§7.3](#73-settemphumidity)) |
| 7  | SetTime | write | auth | Timer-mode hours / minutes / speed |
| 8  | GetRemainTime | read | auth | Remaining countdown time (h/m/s) |
| 9  | SetMode | write | auth | Fan mode: Idle / Run / Timer / TH (Smart) |
| 10 | Upgrade | write | auth | Buffer a firmware URL (OTA) |
| 11 | SetRouter | write | auth | Set Wi-Fi SSID + password |
| 12 | ManualSendState | **push** | — | **Not a command you can send** — the dispatcher has no handler and replies `"Api Error"`. The hub *emits* it unsolicited: `{"A":12,"R":"<speed>"}` on a physical KEY1 speed change or an over-temp cutoff. The OEM app parses incoming A=12 as `ManualSendState`. |
| 13 | Login | auth | **pre** | Authenticate with a stored pair-id |
| 14 | Pair | auth | **pair** | Register a new pair-id (pair mode only) |
| 15 | PairMode | auth | auth | Enter pair mode to add another phone |
| 16 | SetFanInfo | write | auth | Custom fan name / model / serial |
| 17 | GetFanInfo | read | auth | Read custom fan name / model / serial |
| 18 | SetSpeed | write | auth | Set fan speed |
| 19 | GetPresets | read | auth | List saved Smart Mode presets ([§8](#8-presets)) |
| 20 | SetPresets | write | auth | Write the saved preset list |
| 21 | SetGuideSetup | write | auth | First-run wizard completion flag |
| 22 | Reset | write | auth | **Factory reset** — wipes NVS incl. pairing list |
| 28 | GetRecordData | read | auth | 31-day hourly sensor log (binary, [§10](#10-binary-commands)) |
| 29 | SynchronizeTime | write | auth | Set the hub's clock (binary, [§10](#10-binary-commands)) |

\* `GetUpgradeState` (A=5) is the one command still reachable while an OTA is in
progress; every other command is blocked once an update starts (until reboot).

---

## 5. Command details

Requests below are shown in **V2** form (what you send on the wire to V4.1+); the V1
named equivalent is `{"Api":"<verb>", …}` with the long field names from
[§6](#6-field-name-map-result-values-buffer-limits). Responses are V2 shape (V4.1+).

### 5.1 Login (A=13)

```jsonc
// request
{"A":13,"P":"<pair-id>"}
// response
{"A":13,"R":"Success","P":"No"}   // R = Result, P = PairState ("Yes"/"No")
```

`R:"Fail"` if the pair-id is not in the hub's list. On success, `pair_state` → 1.

### 5.2 Pair (A=14) — requires pair mode

```jsonc
// request
{"A":14,"P":"<new-pair-id>"}
// response
{"A":14,"R":"Success"}            // then the hub drops the connection (V4.1)
```

Requires `pair_state == 2`. Generate a 16-hex-char id (the OEM convention). After
success, **reconnect and `Login` with the new id** to confirm — V4.1 closes the link
on a successful pair. `R:"Beyond"` means the 50-pair cap was hit.

### 5.3 PairMode (A=15) — requires auth

```jsonc
{"A":15}                          // request
{"A":15,"R":"Success"}            // response
```

Sets `pair_state` → 2 for 2 minutes so an **already-authenticated** client can invite
another phone. It cannot bootstrap the first pairing (see [§3](#3-authentication--pairing-state-machine)).

### 5.4 GetWorkState (A=1)

```jsonc
{"A":1}
{"A":1,"M":"Smart","R":"HIGH","S":"OK","T":812,"H":47,"C":0}
```

| Key | Meaning |
|-----|---------|
| `M` | Mode string: `Off` / `Run` / `Timer` / `Smart` |
| `R` | Range = current/last speed (`CLOSE`/`LOW`/`MEDIUM`/`HIGH`) |
| `S` | Sensor health string |
| `T` | Temperature sample, **integer °F ×10** (e.g. `812` = 81.2 °F) |
| `H` | Humidity sample, integer % |
| `C` | Control type (integer) |

### 5.5 GetVersion (A=3)

```jsonc
{"A":3}
{"A":3,"V":"IT-BLT-ATTICFAN_V4.1","P":182,"D":"2025.11.18","M":"…","H":"A"}
```

`V` = software version, `P` = over-temp protection cutoff (°F), `D` = build date,
`M` = build/create mode, `H` = MCU/hardware version.

#### V4.1 versus V4.3

As of 2026-07-12, QuietCool's update service advertises V4.3 on its QC and
engineering channels while the production channel remains V4.1. We compared the
complete OEM V4.1 and V4.3 ESP32 application images before choosing the
compatibility version reported by this firmware.

V4.3 is 256 bytes larger (64 bytes of DROM and 192 bytes of IROM). A
relocation-aware segment diff found no new or removed string, constant, global
initializer, NVS key, BLE command, or protocol behavior. A separate Diaphora
function-level comparison matched 2,081 functions at ratio 1.0 and found zero
high-confidence functions with changed logic. Even the embedded application compile
timestamp is identical in both images (`Oct 15 2025 15:48:41`). The remaining byte
differences are consistent with rebuild hashes, address relocations, pointer-table
updates, and branch-displacement churn; no isolable semantic firmware change was
found. The Android app itself has one `>= 4.2` check that changes OTA progress-bar
rendering, but that is cosmetic app behavior rather than a hub capability.

The Smart Control app does not perform a newer-than comparison. Its update screen
increments the available-update count whenever
`BigDecimal(deviceVersion).compareTo(BigDecimal(channelVersion)) != 0`. Consequently,
a device reporting V4.3 is incorrectly offered the production channel's older V4.1
image because `4.3 != 4.1`. This was confirmed live against the app on 2026-07-12.

This compatibility implementation therefore answers `GetVersion` with the
production channel's exact `IT-BLT-ATTICFAN_V4.1` and date `2025.11.18`. This is
deliberately an OEM compatibility identity, separate from the ESPHome project's own
release version. It must track the channel selected by the app, not the highest OEM
version number. Users on QC or engineering app channels may still see V4.3 offered.

### 5.6 GetRouter (A=4)

```jsonc
{"A":4}
{"A":4,"S":"<ssid>","P":"<password>","M":"<wifi-mac>"}
```

> The stored Wi-Fi **password is returned in cleartext over BLE.** This is an OEM
> behavior. This firmware reimplements the command but see [§12](#12-how-this-firmware-implements-the-protocol).

### 5.7 GetRemainTime (A=8)

```jsonc
{"A":8}
{"A":8,"H":0,"M":42,"S":17}       // 0 h 42 m 17 s remaining on the active timer
```

### 5.8 GetFanInfo (A=17) / SetFanInfo (A=16)

```jsonc
{"A":17}
{"A":17,"N":"<name>","M":"<model>","S":"<serial>"}

{"A":16,"N":"Attic","M":"AFG SMT PRO-2.0","S":"…"}
{"A":16,"F":"TRUE"}
```

Fan name is capped at 32 bytes.

### 5.9 SetMode (A=9)

```jsonc
{"A":9,"M":"TH"}                  // M = mode string
{"A":9,"W":"Smart","F":"TRUE"}    // W = resulting WorkMode, F = Flag
```

Mode strings on the wire: `"Idle"` (off), `"Timer"` (countdown), `"Run"` (indefinite),
`"TH"` (Smart). See [§7](#7-modes--smart-mode) for the mode semantics (note the OEM's
internal numbering is counter-intuitive; the wire names are the sane ones).

### 5.10 SetSpeed (A=18)

```jsonc
{"A":18,"S":"HIGH"}               // S = speed: CLOSE / LOW / MEDIUM / HIGH
{"A":18,"S":"HIGH","F":"TRUE"}
```

### 5.11 SetTime (A=7)

```jsonc
{"A":7,"H":3,"M":0,"R":"LOW"}     // hours, minutes, speed for Timer mode
{"A":7,"F":"TRUE"}
```

### 5.12 SetTempHumidity (A=6) / GetParameter (A=2)

See [§7.2](#72-getparameter) and [§7.3](#73-settemphumidity) — these are the Smart Mode
threshold read/write pair.

### 5.13 GetPresets (A=19) / SetPresets (A=20)

See [§8](#8-presets).

### 5.14 Upgrade (A=10) / SetRouter (A=11) / GetUpgradeState (A=5)

See [§9](#9-firmware-update-ota-flow) — the OTA trio.

### 5.15 Reset (A=22)

```jsonc
{"A":22,"G":"…"}                  // request reuses the SetGuideSetup "G" field
{"A":22,"F":"TRUE"}               // ack is sent, THEN the hub wipes NVS and reboots
```

**Destructive.** Wipes the entire NVS, including the pairing list and Wi-Fi
credentials. The hub returns the ack first, then performs the reset a moment later.

---

## 6. Field-name map, result values, buffer limits

### V1 ↔ V2 field names

The V2 single-character keys are reused across commands; the `A` code disambiguates
(e.g. `P` is the pair-id on `Login` input but `PairState` on `Login` output; `M` is
`Mode` on some commands and `Model` on others).

| V1 field | V2 key | Used in |
|----------|--------|---------|
| `PhoneID` | `P` | Login, Pair (request) |
| `URL` | `U` | Upgrade (request) |
| `Ssid` | `S` | SetRouter (request) |
| `Password` | `P` | SetRouter (request) |
| `Mode` | `M` | SetMode (request), GetWorkState (response) |
| `Speed` | `S` | SetSpeed (request) |
| `Name` / `Model` / `SerialNum` | `N` / `M` / `S` | Get/SetFanInfo |
| `Result` | `R` | responses — `Success` / `Fail` / `Beyond` |
| `Flag` | `F` | responses — `TRUE` / `FALSE` |
| `PairState` | `P` | Login response — `Yes` / `No` |
| `Range` | `R` | GetWorkState response — speed |
| `SensorState` | `S` | GetWorkState response |
| `Temp_Sample` | `T` | GetWorkState response — °F ×10 |
| `Humidity_Sample` | `H` | GetWorkState response — % |
| `Version` / `Create_Date` | `V` / `D` | GetVersion response |

### Result (`R`) values

| `R` | Meaning |
|-----|---------|
| `Success` | Accepted and processed |
| `Fail` | Envelope problem — missing field, or a value longer than its buffer |
| `Beyond` | Pair-counter overflow only (`pair_num` ≥ 50); recovery needs a factory reset |

Setter commands generally use `F` (Flag) — `TRUE` = OK, `FALSE` = rejected — instead of
or in addition to `R`.

### Firmware-enforced buffer limits

| Field | Max length |
|-------|-----------|
| Pair-id (Login / Pair) | 100 |
| URL (Upgrade) | 100 |
| SSID (SetRouter) | 32 |
| Wi-Fi password (SetRouter) | 64 (the OEM app caps its own input at 32) |
| Custom fan name (SetFanInfo) | 32 |

Exceeding any of these returns `R:"Fail"` / `F:"FALSE"`.

---

## 7. Modes & Smart Mode

### Mode encoding

The wire mode strings (`Idle` / `Timer` / `Run` / `TH`) are the intuitive names.
Internally the firmware uses numeric mode values whose labels are counter-intuitive
(the internal "Run" is the countdown timer, and internal "Timer" is indefinite
operation); clients should use the **wire strings** and ignore the internal numbering.
Functionally:

| Wire string | Behavior |
|-------------|----------|
| `Idle` | All relays off; no auto-control |
| `Timer` | Countdown timer active — runs at a set speed for a set duration, then stops |
| `Run` | Indefinite — runs until stopped (a long-running safety watchdog still applies) |
| `TH` | Smart Mode — the hub auto-drives speed from temperature/humidity |

### 7.1 Smart Mode decision tree

In Smart Mode the hub reads its onboard SHT30 temperature/humidity sensor and picks a
speed. The tree depends on the DIP-configured wiring (1/2/3-speed):

| Wiring | Rules (first match wins) |
|--------|--------------------------|
| **3-speed** | humidity > `Hum_H` → **STOP** (condensation cutoff); else temp ≥ `Temp_H` → **HIGH**, ≥ `Temp_M` → **MED**, ≥ `Temp_L` → **LOW**, else STOP |
| **2-speed** | same, without the MED tier |
| **1-speed** | HIGH above `Temp_L`, else STOP |

There is also a **humidity-ventilation** path: above `Hum_L` the fan runs at a
configurable "humidity response" speed to air out a damp attic. And an **independent
over-temperature cutoff** (≈182 °F / 83.3 °C) forces everything off ahead of the tree,
as a hardware-protection backstop.

### 7.2 GetParameter

Reads all Smart Mode + Timer state. Response fields (shown with their V1 names for
clarity; on the wire V4.1 uses positional single-char keys):

```jsonc
{
  "Mode": "TH",             // current mode
  "FanType": "TWO",         // DIP wiring: ONE / TWO / THREE / NO
  "GetTemp_H": 100,         // high-temp threshold °F
  "GetTemp_M": 90,          // mid-temp °F   (only present on 3-speed)
  "GetTemp_L": 80,          // low-temp °F   (absent on 1-speed)
  "GetHum_H": 90,           // high humidity %  → STOP
  "GetHum_L": 70,           // low humidity %   → ventilate
  "GetHum_Range": "LOW",    // humidity-response speed: CLOSE/LOW/MEDIUM/HIGH
  "GetHour": "3",           // Timer hours
  "GetMinute": "0",         // Timer minutes
  "GetTime_Range": "LOW",   // Timer speed
  "GuideSetup": "Yes"       // first-run wizard completed
}
```

### 7.3 SetTempHumidity

Writes the live Smart Mode thresholds:

```jsonc
{
  "A": 6,
  "SetTemp_H": 100, "SetTemp_M": 90, "SetTemp_L": 80,
  "SetHum_H": 90, "SetHum_L": 70, "SetHum_Range": "LOW",
  "Index": 0
}
{"A":6,"F":"TRUE"}
```

Firmware rules:

- Each numeric value must be **< 256** or the command is rejected (`F:"FALSE"`).
- `SetTemp_M` is written **only on 3-speed wiring**; `SetTemp_L` is dropped on 1-speed.
  Values for a tier the wiring doesn't have are silently ignored (still ACKed).
- **255 is a sentinel meaning "this tier disabled."** The decision tree explicitly
  skips any threshold equal to 255. (The OEM app's picker uses "OFF" as index 0.)

### 7.4 Threshold defaults

Factory defaults, applied at first boot and by factory reset:

| Threshold | Default |
|-----------|---------|
| High-temp (`Temp_H`) | 100 °F → HIGH |
| Mid-temp (`Temp_M`) | 90 °F → MED (3-speed only) |
| Low-temp (`Temp_L`) | 80 °F → LOW |
| High-humidity (`Hum_H`) | 90 % → STOP |
| Low-humidity (`Hum_L`) | 70 % → ventilate |
| Humidity-response speed | LOW (HIGH on 1-speed wiring) |
| Timer | 3 h, 0 min |

---

## 8. Presets

Presets are **named saved threshold sets** (up to 4). They are pure persistence — there
is **no "apply preset" command on the wire**.

### GetPresets (A=19)

```jsonc
{"A":19,"FanType":"THREE"}        // the app always sends "THREE" regardless of wiring
{
  "A": 19,
  "P": [
    ["Summer", 100, 90, 80, 90, 70, "LOW"],
    ["Winter",  80, 70, 60, 85, 60, "HIGH"]
  ]
}
```

Each preset is a **7-element array**:
`[name, temp_h, temp_m, temp_l, hum_h, hum_l, speed]`. `name` is ≤ 50 bytes; the temp/hum
values follow the Smart Mode threshold semantics ([§7.3](#73-settemphumidity), including
the 255 = disabled sentinel); `speed` is `LOW`/`MEDIUM`/`HIGH`.

### SetPresets (A=20)

```jsonc
{"A":20,"FanType":"THREE","Presets":[ …same array shape… ]}
{"A":20,"F":"TRUE"}
```

### How presets activate

Selecting a preset is an **app-side action**, not a wire command. The app tracks which
preset is "current." When the user activates Smart Mode (or edits the *currently active*
preset), the app pushes that preset's values via `SetTempHumidity` (A=6). Editing a
*non-active* preset sends only `SetPresets` — the live thresholds do **not** change.

So for a server (or this firmware): receiving `SetPresets` **without** a following
`SetTempHumidity` is normal and means "the user saved a preset they didn't activate" —
do not auto-apply preset values on `SetPresets`.

---

## 9. Firmware update (OTA) flow

> **This section describes the STOCK firmware's OTA.** This firmware
> ([`oem_ble_compat`](../components/oem_ble_compat/)) implements the same commands but
> deliberately diverges — see [§12](#12-how-this-firmware-implements-the-protocol).

On the stock hub, an over-the-air update is a three-step sequence over BLE:

```
1. Login(pair-id)          → pair_state = 1
2. Upgrade(url)            → hub buffers the URL, returns {A:10,"F":"TRUE"}
3. SetRouter(ssid, pwd)    → hub buffers Wi-Fi creds AND starts the OTA
```

The download is done **by the hub, over its own Wi-Fi** — the phone only hands off the
URL and Wi-Fi credentials. **`SetRouter` is the actual trigger**: after it has both the
URL (from `Upgrade`) and the Wi-Fi creds, the hub joins Wi-Fi, downloads the image, and
writes it to the inactive OTA slot.

> **`Upgrade` MUST precede `SetRouter`.** `SetRouter` *spawns*
> the download task, which reads the URL global that `Upgrade` populates. Send `SetRouter`
> first and the task launches with an **empty URL**, fails to initialise its HTTP client,
> and resets. Once that task is running, a follow-up `Upgrade` is **rejected with a bare
> `{}`** — sent in the right order it stores the URL and returns `{A:10,"F":"TRUE"}`.

### Upgrade (A=10)

```jsonc
{"A":10,"U":"http://…/firmware.bin"}   // U = URL, ≤ 100 chars
{"A":10,"F":"TRUE"}
```

Buffers the URL. Does **not** start the download by itself on stock.

### SetRouter (A=11)

```jsonc
{"A":11,"S":"<ssid>","P":"<password>"}
{"A":11,"F":"TRUE"}
```

SSID → 32-byte buffer, password → 64-byte buffer. On stock, if both parsed and no OTA
is already running, this **starts the OTA task** (which also cuts power to the fan
relays for safety first).

### The OTA task (stock)

1. Join Wi-Fi (2 s timeout).
2. Open the HTTP connection (up to 30 retries).
3. Stream the image in 255-byte chunks into the inactive OTA slot.
4. Mark the new slot bootable and **reboot into it** (a failed transfer also ends in a
   reset). Standard ESP-IDF A/B semantics leave the previous firmware in the other slot
   for rollback.

**Security:** the stock OTA uses **plain HTTP with no TLS, no signature check, and no
content-length/status sanity check** — a 404 body would be written to flash. Anything
reachable at the URL is flashed. (This is precisely why the OEM OTA mechanism can be
repurposed to install this open firmware — and why you should only point it at images
you trust.)

### GetUpgradeState (A=5)

Poll during/after an OTA. Response `S` is a state string:

| `S` value | Meaning |
|-----------|---------|
| `Connect_NO` | Idle — no OTA in progress (also the post-reboot state) |
| `Connecting_Router` | Joining Wi-Fi |
| `Connect_Router_Fail` | Wi-Fi join failed (sticky until reboot) |
| `Connecting_Server` | Opening the HTTP connection |
| `Connect_Server_Fail` | HTTP open failed (sticky) |
| `Downloading_Progress` | Receiving the image |
| `Download_Fail` | Transfer interrupted (sticky) |
| `Download_Succeed` | Image written and marked bootable |

Failure states are **sticky** — once set they persist until reboot; treat any state
other than `Downloading_Progress` / `Download_Succeed` as terminal.

---

## 10. Binary commands

Two commands bypass the JSON dispatcher and use a **raw-byte** format on the same
characteristic. The hub (and a compatible server) distinguishes them by looking at the
byte **after** the leading `{` (`0x7B`): `0x1C` (=28) or `0x1D` (=29). Anything else is
parsed as JSON.

### GetRecordData (A=28) — 31-day hourly log

The hub keeps a rolling 31-day, hourly log of temperature, humidity, and fan-relay
state, and streams a day at a time.

- **Request** (4 bytes): `0x7B 0x1C <day-index> 0x7D` = `'{' 28 <idx> '}'`.
- **Response** (raw bytes): optional `QQ` prefix, a type marker `0x1C`, the day-index
  echo, then up to 24 one-byte hourly samples, terminated by `0x7D` (`'}'`). The OEM
  app renders these as a space-separated decimal string.

The samples come from the hub's hourly logger; the OEM app uses this to draw its trend
graphs.

### SynchronizeTime (A=29) — set the clock

- **Request** (13 bytes): `0x7B 0x1D <10 ASCII digits of UTC epoch seconds> 0x7D`.
- **Response** (raw bytes): `QQ`-prefixed marker `0x1D` followed by `0x01` (success) or
  `0x00` (failure). The OEM app wraps it as `{"Api":"SynchronizeTime","Flag":"TRUE"}`.

---

## 11. Quirks & gotchas

- **`QQ` prefix** on all V4.1+ responses — frame on the first `{` (or the type byte at
  offset after `{` for binary).
- **V4.1 always responds in V2 shape**, even to V1 requests.
- **Only 4 V1 named verbs work on V4.1** (`Login`, `Login2`, `Pair`, `SetSpeed`); send
  V2 numeric for everything else. The OEM app converts V1 → V2 before sending.
- **V1 `Upgrade` is a stub** on V4.1 (returns `{}`); use V2 `A=10`.
- **`Pair` disconnects on success** (V4.1) — reconnect and `Login` to confirm.
- **First pairing is physical-button-only.** `PairMode` (A=15) needs an authenticated
  session; it cannot bootstrap the first pair remotely.
- **Pair cap is 50** (`R:"Beyond"`); only a factory reset clears the counter.
- **Factory test pair-id** `1234567dsad8wqw9asasd` works only on never-paired hubs.
- **`GetRouter` returns the Wi-Fi password in cleartext** over BLE, and `SetRouter`
  transmits it in cleartext (an OEM trait — acceptable for a one-shot bootstrap over
  ~10 m BLE, not for repeated use).
- **Send `Upgrade` before `SetRouter` for an OTA** (see [§9](#9-firmware-update-ota-flow)).
  `SetRouter` spawns the download task with whatever URL `Upgrade` has already buffered;
  reversed, the task gets an empty URL, fails to init the HTTP client, and resets — and a
  late `Upgrade` returns a bare `{}`.
- **Once an OTA starts, only `GetUpgradeState` is answered**; all other commands are
  gated until reboot.
- **`Reset` acks before wiping** — the `{A:22,"F":"TRUE"}` response is sent, then NVS is
  wiped and the hub reboots.
- **No auth-mode field for Wi-Fi.** `SetRouter` carries only SSID + password; the hub
  accepts whatever security the AP advertises (open/WPA/WPA2/WPA3-mixed).
- **Turning the fan on takes two writes** on V2: `SetSpeed` to choose the speed, then
  `SetMode` with `"Run"` (or `"Timer"`). A single write won't start it.
- **The hub does not push state on BLE-initiated changes.** Only a physical KEY1 speed
  change or an over-temp cutoff produces an unsolicited notification — sent as an
  `{"A":12,"R":"<speed>"}` "ManualSendState" frame (code 12 is push-only; you cannot send
  it). Otherwise clients poll `GetWorkState` (the OEM app polls ~every 10 s).
- **Units ship with factory-baked Wi-Fi credentials** (a manufacturing test network) in
  NVS. They persist across an OTA. Not needed to operate the hub, but worth wiping via a
  factory reset if you care.

---

## 12. How **this** firmware implements the protocol

This firmware's [`oem_ble_compat`](../components/oem_ble_compat/) component reimplements
the protocol above so the **stock QuietCool Smart Control app keeps working** after you
flash it: pairing, `Login`, `GetWorkState`, speed/mode control, Smart Mode thresholds,
presets, and fan info all behave as the app expects (V2 numeric, `QQ` prefix, same field
names, same gating). It is exposed as the `Smart Control (BLE)` switch and advertises the
same `ATTICFAN_<mac>` name. **Pairing is preserved exactly as on stock** — an unpaired hub
enters pair mode only via a physical KEY2 press on the device, and `PairMode` (A=15) is
auth-gated, so an in-range stranger can't pair themselves remotely (the physical button is
the trust boundary — treating A=15 as pre-auth would be a remote-pairing hole).

Where it **deliberately diverges** from stock:

- **`Upgrade` (A=10) is the trigger, and it filters URLs.** Unlike stock — where
  `SetRouter` starts the OTA — this firmware acts on `Upgrade` itself. It classifies the
  URL:
  - malformed / too long / non-HTTP → `{"A":10,"F":"FALSE"}`;
  - a **QuietCool firmware domain** (`myquietcool.com` / `quietcool.com` and
    subdomains) → `{"A":10,"F":"TRUE"}` **no-op** — so the stock app's "update
    firmware" cannot silently flash stock *over* this firmware;
  - any other valid `http(s)` URL → a real OTA via ESP-IDF's HTTP OTA engine (the image
    checksum is fetched from a companion `<url>.md5` file), then a reboot into the new
    image. (This is the intentional path for pushing a *custom* build over BLE.)

  Because it is auth-gated, only a **paired** client can trigger it — and pairing needs
  the physical button, so this is not a remote-flash hole. To roll back to stock, use
  the HTTP-flash path in the [web installer](../web-installer/) instead (the OEM-domain
  block is why BLE can't do it).
- **`SetRouter` (A=11) is a live Wi-Fi switch, not an OTA trigger.** It switches the
  hub's Wi-Fi at runtime **without a reboot** (and only when the SSID actually changes —
  re-sending the current network is a no-op), keeping BLE up so you can retry
  credentials immediately.
- **Updates never wipe NVS.** A custom-firmware update preserves configuration; the only
  wipe is the deliberate dual-button stock-restore / factory reset.
- **`GetUpgradeState` (A=5)** reports a minimal state sequence
  (`Connect_NO` → `Downloading_Progress` → `Download_Fail`) rather than the full stock
  enumeration; note the HTTP download is blocking, so A=5 is meaningful before/after but
  not pollable mid-download.
- **`GetRouter` (A=4) does not leak the Wi-Fi password.** It returns the SSID and MAC
  but an empty password field — unlike stock, which returns the stored password in
  cleartext.
- **The two binary commands are stubs.** `GetRecordData` (A=28) returns an empty
  "no data" record and `SynchronizeTime` (A=29) acks without keeping a clock — ESPHome
  users get history from Home Assistant's recorder and the time from SNTP.
- **No `ManualSendState` push.** Stock emits an unsolicited `{"A":12,"R":"<speed>"}` on a
  physical KEY1 speed change so a connected OEM app updates instantly; this firmware
  doesn't. Since the app polls `GetWorkState` (~10 s) and that push is just the speed (a
  strict subset of `GetWorkState`), the app still reflects a local speed change within one
  poll — so it's a latency nicety, not a parity requirement.

The component ships with host-side unit tests covering gate checks, field mapping, frame
assembly, the fan-model catalogue, URL classification, and input validation — see
[`components/oem_ble_compat/test/`](../components/oem_ble_compat/test/).

---

## 13. Credits & prior art

The reverse-engineering lineage started with the community BLE clients, which recovered
much of V1 and (later) V2:

- [emerose/quietcool](https://github.com/emerose/quietcool) — the original Python BLE
  library/CLI; the reverse-engineering root.
- [snyamathi/quietcool](https://github.com/snyamathi/quietcool) — a fork that added V2
  (V4.x firmware) support.
- [rwarner/ha-quietcool-ble](https://github.com/rwarner/ha-quietcool-ble) — a Home
  Assistant integration speaking this protocol over BLE.

This document consolidates and corrects those findings against the V4.1 firmware image,
the analyzed Android build of the OEM Smart Control app, and live captures, and
documents the two binary commands and the full V2 field-name map that the earlier
clients did not have.
