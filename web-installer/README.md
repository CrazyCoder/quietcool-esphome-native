# QuietCool Web Installer

> **▶ Already hosted — flash without setting anything up:**
> **<https://crazycoder.github.io/quietcool-esphome-native/>**
>
> Open that in a Web Bluetooth browser (Chrome or Edge on Windows/macOS/Linux/Android,
> or Bluefy on iOS) on any computer or phone within Bluetooth range of the hub to flash
> it over BLE — the credential-free firmware bin is served alongside it, so there's
> nothing to build or host. The rest of this document covers self-hosting and the internals.

A single-page wizard that flashes custom ESPHome firmware onto a QuietCool
IT-AF-SMT hub — no UART, no attic crawl, no APK sideload. It has two independent
paths, picked by where the hub is in its lifecycle:

1. **Web BLE flow (sections 1–6 on the page).** Flashes the custom ESPHome
   firmware onto a hub still running the stock OEM firmware. Open the page in a Web
   Bluetooth browser — Chrome or Edge on a Windows/macOS/Linux/Android machine, or
   Bluefy on iOS — within Bluetooth range of the hub and walk the wizard. The same flow can
   also re-install OEM `V4.1` on a stock-firmware hub. Once the ESPHome firmware is
   running this path no longer applies — the hub no longer speaks the OEM BLE
   protocol.
2. **HTTP flash flow (section at the bottom).** Pushes any firmware URL onto a hub
   already running the ESPHome firmware. It calls the device's built-in
   `POST /api/flash_url` endpoint, provided by the `http_flash_handler` component
   in [`../components/http_flash_handler/`](../components/http_flash_handler/)
   (which ships host-side validation tests). Use it to roll back to OEM V4.1, push
   a custom build, or test alternative firmware without touching BLE.

## Layout

```
index.html        # UI
app.js            # Web BLE logic (PairMode / Pair / Login / Upgrade / SetRouter)
manifest.json     # version metadata
README.md         # this file
```

When deployed, drop `firmware.ota.bin` (built by `esphome compile` from the
firmware in this repo) into the same directory as `index.html`. The page defaults
its OTA URL to `./firmware.ota.bin` relative to wherever it's served.

> **Host the credential-free build.** Build the firmware with
> `esphome -s build_mode dist compile quietcool-atticfan.yaml` before hosting it.
> The default `dev` build bakes Wi-Fi credentials and the API encryption key into
> the image; the `dist` build has neither and onboards Wi-Fi via Improv-BLE, so the
> hosted `.bin` carries no secrets. See [Caveats](#caveats--risks) below.

## Install targets

The wizard exposes two install targets at the top of the page; both use the same
OEM OTA mechanism — only the firmware URL changes.

- **Install ESPHome (custom firmware)** — defaults to `./firmware.ota.bin`
  relative to the page. Requires the custom firmware to be present in the same
  directory as `index.html`.
- **Restore OEM factory firmware** — defaults to the OEM `IT-BLT-ATTICFAN_V4.1`
  bin on QuietCool's CDN (`http://myquietcool.com/.../IT-BLT-ATTICFAN_V4.1_*.bin`),
  which is byte-for-byte what the OEM Android app would flash. Useful for rolling
  back from a buggy OEM update or starting fresh before pairing. **Only works while
  the hub is still running OEM firmware** — once ESPHome is installed, the hub no
  longer speaks the OEM BLE V2 protocol and this Web-BLE path can't reach it.

To roll a hub that is **already running ESPHome** back to stock, use the **HTTP
flash** section at the bottom of the page (the `http_flash_handler` endpoint), the
firmware's own *Restore Stock Firmware* button in Home Assistant, or a UART
reflash. The OEM `Upgrade` command over BLE deliberately refuses OEM firmware
domains, so it can't be used to flash stock over an ESPHome hub — that's why
rollback goes through the HTTP path, not BLE.

Factory restore via this page does NOT wipe NVS (Wi-Fi creds, pair-ids, and Smart
Mode thresholds all persist — only the app partition gets rewritten). A real
reset-to-defaults still requires the KEY1 5-second long-hold on the hub.

### Path 2: HTTP flash (any URL onto an ESPHome-running hub)

The bottom card on the page (`#card-esphome-flash`) is independent of the BLE flow
and works after the firmware is installed. Inputs:

- **Device hostname or IP** — e.g. `quietcool-atticfan-abcdef.local` (mDNS, where
  `abcdef` is the last 3 bytes of the device's Wi-Fi MAC) or a static LAN IP.
  Auto-filled from the last successful flash in this browser.
- **Firmware URL** — any `http://` or `https://` URL the hub's Wi-Fi can reach. The
  hub fetches the bin itself, not your phone.
- **MD5 (optional)** — 32 hex chars. If supplied, the device validates the
  downloaded bin and aborts before writing on mismatch. A **Pre-fill OEM V4.1**
  button drops the OEM CDN URL + verified MD5 in one click.

**Partition layout requirement.** The hub's flash uses the OEM IT-AF-SMT layout:
`nvs` @ `0x9000`, `otadata` @ `0xd000`, `phy_init` @ `0xf000`, `coredump` @
`0x10000` (64 KB), `ota_0` @ `0x20000` (1.875 MB), `ota_1` @ `0x200000`
(1.875 MB). Any firmware you flash here has to be built for the same layout — same
OTA slot addresses, same slot sizes, same nvs/otadata addresses, same overall 4 MB
image. Examples:

- ✅ **OEM bins from `myquietcool.com`** (V2.x, V3.x, V4.x — all built for this layout).
- ✅ **This project's own builds** — its `partitions.csv` mirrors the OEM layout exactly.
- ✅ **Forks / derivative builds** that ship the same `partitions.csv`.
- ❌ **Stock ESPHome images built with default partitioning** (different OTA
  addresses, smaller slots, different nvs location) — will either refuse to write
  (too large for the slot) or write successfully but never boot.
- ❌ **Anything not built for ESP32 / not a `0xE9`-magic ESP-IDF image** — the OTA
  aborts when the image header doesn't validate, so this fails fast.

When in doubt, build your custom firmware from this project (which already wires
`partitions: ./partitions.csv`) — that guarantees compatibility.

The page POSTs `http://<host>/api/flash_url?url=…&md5=…` (no body, no custom
headers — so no CORS preflight is required for the simple case). The device's
`http_flash_handler` responds `200 OK` ("Accepted") immediately, then ~500 ms later starts
the OTA — pulls the bin, writes to the inactive OTA slot, reboots into it. The
running slot can never be overwritten (ESP-IDF guarantees this) so the previous
firmware survives in the other slot for one-OTA rollback.

**Mixed-content gotcha:** if you're loading this page over HTTPS (e.g.
`https://<user>.github.io/<repo>/`) and the device serves `http://`, modern
browsers may block the cross-protocol fetch. Workarounds:

1. **Use Chrome's Private Network Access** — the device sends
   `Access-Control-Allow-Private-Network: true` in the OPTIONS preflight, so
   Chrome 117+ allows the request. Other browsers may not.
2. **Load this page over plain HTTP from your LAN** — drop the same files on a
   local server, or use the device's own web UI at `http://<host>/`.
3. **Reverse-proxy the device through HTTPS** — overkill for one-off flashes.

## Hosting requirements

- **HTTPS context.** Web BLE only works in a secure context (HTTPS or `localhost`).
  Plain HTTP is rejected by the browser.
- **Firmware URL accessible from the hub's Wi-Fi.** The hub fetches the binary
  itself after it joins the user's network — not the phone's network. If you host
  on a LAN-only server, the hub must be able to resolve and reach it.
- **HTTPS cert trusted by ESP-IDF.** ESP-IDF's HTTPS client uses an embedded CA
  bundle. Public CAs (Let's Encrypt, GitHub Pages, Cloudflare, etc.) work.
  Self-signed certs do not — fall back to plain `http://` in that case.
- **URL ≤ 100 characters.** The OEM `Upgrade` handler buffers up to 100 chars and
  silently truncates beyond that. Use short paths.

## Deploying to GitHub Pages

**This repository already publishes the installer to
<https://crazycoder.github.io/quietcool-esphome-native/>.** Pages is enabled with
source *GitHub Actions*, and the
[`deploy-pages.yml`](../.github/workflows/deploy-pages.yml) workflow builds the
credential-free (`dist`) firmware, stages `firmware.ota.bin` + an `.md5` next to
`index.html`, and deploys this folder. GitHub Pages' certificate is trusted by
ESP-IDF's CA bundle, so the hub's own HTTPS download of the firmware bin works too.

**Redeploy after changing the firmware or the page.** The workflow runs on manual
dispatch — rebuild and republish with:

```sh
gh workflow run "Deploy Web installer"
```

or the **Actions → Deploy Web installer → Run workflow** button. To make every push to
`main` redeploy automatically, uncomment the `push:` trigger in the workflow.

**Self-hosting elsewhere.** The installer is a static site — serve the `web-installer/`
folder from any host with a public-CA HTTPS cert (Cloudflare Pages, Netlify, your own
server). Build and stage the firmware next to `index.html` yourself:

```sh
esphome -s build_mode dist compile quietcool-atticfan.yaml
cp .esphome/build/quietcool-atticfan/.pioenvs/quietcool-atticfan/firmware.ota.bin \
   web-installer/firmware.ota.bin
```

The page auto-detects its firmware URL as `./firmware.ota.bin` relative to wherever
it's served, so no code change is needed. Keep the final URL short — the OEM
`Upgrade` buffer truncates past 100 characters, so a path like
`https://<user>.github.io/<repo>/firmware.ota.bin` is fine, but deeply nested paths
are not.

## Caveats / risks

1. **The OEM firmware does not verify signatures on OTA images.** Anything
   reachable at the URL gets flashed. Trust only firmware you built or verified.
2. **The `dev`-build `firmware.ota.bin` bakes Wi-Fi creds and the API encryption
   key into the image** (its YAML uses `!secret wifi_password` etc.). Anyone who
   downloads such a `.bin` gets those credentials. **Always host the `dist` build**
   (`build_mode dist`), which carries no secrets and onboards Wi-Fi via Improv-BLE.
3. **Wi-Fi password leaks in cleartext over BLE during SetRouter.** This is an OEM
   trait, not this project's. Acceptable for a one-shot bootstrap (BLE range
   ~10 m), not for repeated use.
4. **Factory test pair ID only works on never-paired devices.** If the hub has been
   paired (e.g. via the OEM Android app), the wizard re-pairs with a fresh random
   UUID first, then logs in with that. A disconnect/reconnect is required between
   Pair and Login because the V2 firmware commits pair state only on disconnect.
5. **If the firmware fails to boot, the OEM bootloader falls back to the other OTA
   slot** (whatever was active before this flash). A UART reflash of a known-good
   image is the last resort.

## Protocol reference

**V2 uses single-character JSON input field names** — not V1's verbose
`"PhoneID"`/`"URL"`/`"Ssid"`/`"Password"`. The V4.1 V2 parser silently fails
(returns the default Fail string) if you send V1-style names inside a V2 numeric
envelope.

The V2 dispatcher reads `pair_state` from the session state and **rejects every
command except Login and Pair when `pair_state != 1`**. Login (A=13) is pre-gate
and always allowed. Pair (A=14) is also pre-gate but only fires when
`pair_state == 2`. PairMode (A=15) and everything else are behind the
`pair_state == 1` gate, so they can't be used to bootstrap a never-logged-in
session.

Two auth modes:

**Mode A — Login with existing pair-id** (works for never-paired hubs via the
factory test ID, or for paired hubs if you know the original 16-char id):

| Step | V2 command | JSON | Purpose |
|------|------------|------|---------|
| 1 | Login     | `{"A":13,"P":"<your existing pair id>"}` | Authenticates and unlocks the rest of the dispatcher. |
| 2 | Upgrade   | `{"A":10,"U":"..."}` | Buffers the OTA URL (≤100 chars). |
| 3 | SetRouter | `{"A":11,"S":"<ssid>","P":"<password>"}` | Triggers OTA. SSID lives in a 32-byte buffer, password in a 64-byte buffer. |
| 4 | (optional) GetUpgradeState | `{"A":5}` | Poll for progress; BLE usually dies during OTA so this is best-effort. |

**Mode B — Pair as new device** (no pair-id required, but the hub has to be in pair
mode beforehand). Two equivalent ways to get the hub into `pair_state == 2`:

- **Physical button**: long-press the Pair button (KEY2 / GPIO 26) on the hub for
  ≥3 seconds. Same effect as the BLE PairMode command.
- **OEM Android app**: tap *Pair Mode* in the app from its already-authenticated
  session (this issues a V1 `{"Api":"PairMode"}` over BLE). Then force-close the OEM
  app to release the BLE link.

Either way, the subsequent flow is:

| Step | V2 command | JSON | Purpose |
|------|------------|------|---------|
| 1 | Pair      | `{"A":14,"P":"<random 16-char hex id>"}` | Adds a fresh pair-id to the OEM's NVS list under namespace `hx_list`, key `Phone<N>`. Pre-gate, requires `pair_state == 2`. Hard cap of 50 (monotonic counter `pair_num`). |
| 2 | (disconnect + reconnect) |  | V2 firmware commits the pair only on disconnect. |
| 3 | Login     | `{"A":13,"P":"<same id>"}` | Authenticates with the just-added id. Sets `pair_state := 1`. |
| 4-5 | Upgrade + SetRouter | | Same as Mode A from here. |

### Protocol gotchas

1. **V2 input fields are single-character.** Guessing V2 field names by analogy
   with V1's verbose names does not work. The actual mapping: Login/Pair use `"P"`
   for pair-id; Upgrade uses `"U"` for URL; SetRouter uses `"S"` for SSID and `"P"`
   for password (yes, `"P"` is reused for different meanings per command — that's
   fine because `A=` disambiguates).
2. **The factory test pair-id `"1234567dsad8wqw9asasd"` is NVS-only, not
   hardcoded.** It's a factory-shipped NVS entry that gets overwritten the first
   time a real phone calls Pair. On a previously-paired hub, it doesn't work for
   Login regardless of protocol version.
3. **The `pair_num` NVS counter is monotonic and capped at 50** — and there's no
   BLE-exposed way to reset it. Only KEY1 5-second long-hold (`Clear_All_Pair` in
   firmware) clears the whole `hx_list` namespace.
4. **Generate 16-char pair-ids, not 32-char.** The OEM convention is 16 hex chars.
   The length check in the handler is `<101`, so 32 chars should work — but matching
   the OEM convention sidesteps any tighter buffer-size constraint.
5. **Power-cycle resets `pair_state` (RAM) but not `pair_num` (flash NVS).** If
   you're debugging by power-cycling, re-trigger pair mode each time, and don't
   expect any persistent NVS state to change.

BLE specifics: service UUID `000000ff-0000-1000-8000-00805f9b34fb`, characteristic
UUID `0000ff01-0000-1000-8000-00805f9b34fb` (write + notify). Responses are prefixed
with literal ASCII `QQ` before the JSON — the page strips this by finding the first
`{`. Writes are chunked at 20 bytes per packet to match the BLE 4.2 default MTU.

## Finding the existing pair-id on a paired hub

If Login with `1234567dsad8wqw9asasd` returns `{"R":"Fail"}`, the hub has been
paired and that NVS entry was overwritten — you need the real 16-char id. Three
places to find it:

1. **NVS partition dump.** With the hub in bootloader mode over UART:

   ```bash
   python -m esptool --port <COM> --baud 460800 read_flash 0x9000 0x4000 nvs-live.bin
   ```

   Parse the dump for entries in NVS namespace `hx_list`. Look specifically for
   `Phone1` (or `Phone2`, etc. up to `pair_num`). The string value is the pair-id
   (16 hex chars).

2. **OEM Android app data** — `/data/data/com.quietcool.smartcontrol/...` via
   `adb backup` or root. SharedPreferences or SQLite-backed. The id is the same one
   the app sent in `{"Api":"Login","PhoneID":"<id>"}` during pairing.

3. **Re-pair from scratch** — factory-reset the hub (KEY1 5-second long-hold), pair
   once with the OEM Android app, then read the new id back via methods 1 or 2. Note:
   factory reset also wipes Wi-Fi creds and Smart Mode thresholds.
