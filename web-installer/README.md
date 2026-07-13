# QuietCool Web Installer

## ▶ Already hosted — flash without setting anything up

<a href="https://crazycoder.github.io/quietcool-esphome-native/"><img src="../docs/images/web-installer-qr.png" align="right" width="150" alt="QR code — scan with a phone to open the Web Installer" /></a>

**[Open the Web Installer → crazycoder.github.io/quietcool-esphome-native](https://crazycoder.github.io/quietcool-esphome-native/)** in a Web Bluetooth browser (Chrome or Edge on Windows, macOS, Linux, or Android, or Bluefy on iOS) on any computer or phone within Bluetooth range of the hub. It flashes the credential-free firmware over BLE, so there's nothing to build or host.

Installing from a phone? **Scan the QR code** instead of typing the URL. The rest of this document covers self-hosting and the internals.

A single-page wizard that flashes custom ESPHome firmware onto a QuietCool
IT-AF-SMT hub — no UART or APK sideload. A previously paired hub still needs
either its existing pair ID or physical KEY2 access to authorize a new client.
The installer has two independent paths, picked by where the hub is in its
lifecycle:

1. **Web BLE flow (sections 1–6 on the page).** Flashes the custom ESPHome
   firmware onto a hub still running the stock OEM firmware. Open the page in a Web
   Bluetooth browser — Chrome or Edge on a Windows/macOS/Linux/Android machine, or
   Bluefy on iOS — within Bluetooth range of the hub and walk the wizard. The same flow can
   also re-install OEM `V4.1` on a stock-firmware hub. Once the ESPHome firmware is
   running, this flow no longer serves to change its firmware: the hub still speaks
   the OEM BLE protocol, but its `Upgrade` command refuses QuietCool's firmware
   domains, so it can't be rolled back to stock over BLE — use path 2 instead.
2. **HTTP flash flow (section at the bottom).** Pushes any firmware URL onto a hub
   already running the ESPHome firmware. It calls the device's built-in
   `POST /api/flash_url` endpoint, provided by the `http_flash_handler` component
   in [`../components/http_flash_handler/`](../components/http_flash_handler/)
   (which ships host-side validation tests). Use it to roll back to the verified
   OEM V4.1 image, push a custom build, or test alternative firmware without
   touching BLE. The known OEM URL+MD5 pair automatically gets the stock-specific
   slot confirmation and NVS cleanup needed for the restore to stick.

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
  which is byte-for-byte what the QuietCool Smart Control app would flash. Useful
  for rolling
  back from a buggy OEM update or starting fresh before pairing. **Only works while
  the hub is still running OEM firmware** — once ESPHome is installed, its `Upgrade`
  command refuses QuietCool's firmware domains (the hub still speaks OEM BLE, it just
  won't accept a stock bin over it), so restoring stock goes through the HTTP path below.

To roll a hub that is **already running ESPHome** back to stock, use the **HTTP
flash** section at the bottom of the page (the `http_flash_handler` endpoint), the
firmware's own *Restore Stock Firmware* button in Home Assistant, or a UART
reflash. The OEM `Upgrade` command over BLE deliberately refuses OEM firmware
domains, so it can't be used to flash stock over an ESPHome hub — that's why
rollback goes through the HTTP path, not BLE.

Factory restore via this page rewrites only the app partition and erases only the
private `esphome` NVS namespace during final shutdown. The OEM `hx_list` namespace
is preserved, including BLE pair IDs, Smart Mode thresholds, presets, timer defaults,
and fan metadata. Repeated live Web Installer round trips from ESPHome to OEM V4.1
and back confirmed that pair IDs still authenticate and presets plus distinctive OEM
settings persist. The KEY1 5-second Factory Reset clears Wi-Fi credentials and
ESPHome preferences for re-onboarding but also deliberately preserves OEM `hx_list`;
use **Clear BLE Pairings** separately when the stored phone list must be removed.

### Path 2: HTTP flash (any URL onto an ESPHome-running hub)

The bottom card on the page (`#card-esphome-flash`) is independent of the BLE flow
and works after the firmware is installed. Inputs:

- **Device hostname or IP** — e.g. `quietcool-atticfan-abcdef.local` (mDNS, where
  `abcdef` is the last 3 bytes of the device's Wi-Fi MAC) or a static LAN IP.
  Auto-filled from the last successful flash in this browser.
- **Firmware URL** — any `http://` or `https://` URL the hub's Wi-Fi can reach. The
  hub fetches the bin itself, not your phone.
- **MD5 (optional)** — 32 hex chars. If supplied, the device validates the
  downloaded bin and aborts before writing on mismatch; if omitted, it fetches
  `<firmware-url>.md5`. A **Pre-fill OEM V4.1** button drops the exact OEM CDN
  URL + verified MD5 in one click. Both values are required for the endpoint to
  recognize and safely finalize a stock restore.

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

The page POSTs `http://<host>/api/flash_url?url=…&md5=…` (an empty body, no custom
headers — so no CORS preflight is required for the simple case). The device's
`http_flash_handler` responds `200 OK` ("Accepted") immediately, then ~500 ms later starts
the OTA — pulls the bin, writes to the inactive OTA slot, reboots into it. The
running slot can never be overwritten (ESP-IDF guarantees this), so the previous
firmware remains in the other slot. Ordinary custom images retain ESP-IDF's normal
app-rollback protection. For the exact verified OEM V4.1 URL+MD5 pair, the handler
marks the stock slot VALID before reboot because stock cannot self-confirm under this
build's rollback-enabled bootloader; automatic rollback is deliberately disabled for
that explicit restore.

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

**Redeploy after changing the firmware or the page.** Every push to `main`
automatically runs the workflow. To rebuild and republish manually:

```sh
gh workflow run "Deploy Web installer"
```

or use the **Actions → Deploy Web installer → Run workflow** button.

After a successful Pages deployment, the workflow compares the credential-free build
with the latest firmware release. If the firmware inputs changed and the image differs,
it publishes a release and tag named
`qc-esphome-<esphome-version>-<short-commit-hash>` with the matching `.bin`, `.md5`,
`.sha256`, and build-fingerprint assets. GitHub-generated release notes compare the new
commit with the previous firmware-release tag. Page-only changes, cache evictions, and
other rebuilds from the same firmware inputs do not create a release. The live installer
continues to use its short `./firmware.ota.bin` URL rather than a long GitHub Release
asset URL.

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
   paired (e.g. via the QuietCool Smart Control app on Android or iOS), the
   wizard re-pairs with a fresh random
   UUID first, then logs in with that. A disconnect/reconnect is required between
   Pair and Login because the V2 firmware commits pair state only on disconnect.
5. **Ordinary custom images keep ESP-IDF app rollback.** If one fails validation
   after boot, the bootloader can fall back to the previous OTA slot. The exact
   verified OEM restore is deliberately marked VALID so stock stays installed;
   if that known image unexpectedly fails to boot, UART is the recovery path.

## Protocol reference

> The full BLE wire protocol — framing, the V1/V2 dialects, the auth/pair-state machine,
> every command, the quirks, and the OTA flow — is documented in
> **[`../docs/OEM-BLE-PROTOCOL.md`](../docs/OEM-BLE-PROTOCOL.md)**. This section covers only
> what the installer needs.

The installer speaks V2 (single-character JSON field names) once a hub's Login response
shows it is V4.1+, and falls back to V1 for older firmware. Every command except `Login`
and `Pair` is gated behind an authenticated session (`pair_state == 1`), so it
authenticates first, then flashes. **The flash step is `Upgrade` then `SetRouter`, in that
order** — on stock firmware `SetRouter` spawns the download task, which reads the URL that
`Upgrade` buffered, so `Upgrade` must come first.

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
- **QuietCool Smart Control app (Android or iOS)**: tap *Pair Mode* in the app
  from its already-authenticated
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

The [protocol doc](../docs/OEM-BLE-PROTOCOL.md) covers these in full; the ones that bite
during a flash:

- **V2 field names are single characters** — Login/Pair `"P"` (pair-id), Upgrade `"U"`
  (URL), SetRouter `"S"`/`"P"` (SSID/password). Sending V1-style names in a V2 envelope
  silently fails.
- **Generate 16-char hex pair-ids** (the OEM convention), not 32.
- **The factory test pair-id `1234567dsad8wqw9asasd` only works on never-paired hubs** —
  once a hub has been paired it's overwritten; pair anew or use the real id.
- **The pair counter is capped at 50** and only a KEY1 5-second long-hold clears it.

BLE service `000000ff-0000-1000-8000-00805f9b34fb`, characteristic
`0000ff01-0000-1000-8000-00805f9b34fb` (write + notify); responses carry a leading `QQ`
before the JSON (strip to the first `{`); writes are chunked at 20 bytes.

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

2. **Android app data** (Android-specific extraction option) —
   `/data/data/com.quietcool.smartcontrol/...` via
   `adb backup` or root. SharedPreferences or SQLite-backed. The id is the same one
   the app sent in `{"Api":"Login","PhoneID":"<id>"}` during pairing.

3. **Re-pair from scratch** — factory-reset the hub (KEY1 5-second long-hold), pair
   once with the QuietCool Smart Control app, then read the new id back via methods 1 or 2. Note:
   factory reset also wipes Wi-Fi creds and Smart Mode thresholds.
