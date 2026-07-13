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
2. **Device firmware page (section at the bottom).** For a hub already running the
   ESPHome firmware, the public installer asks only for its hostname or IP and
   navigates to `http://<host>/restore-stock`. The hub serves both the URL-flash and
   local OEM-file forms itself. Their requests are therefore same-origin instead of
   HTTPS-to-HTTP mixed content, which browsers may block. The local-file path is
   version-independent and keeps working if the vendor removes an old firmware URL.

## Layout

```
index.html        # UI
app.js            # Web BLE logic (PairMode / Pair / Login / Upgrade / SetRouter)
manifest.json     # version metadata
README.md         # this file
```

`manifest.json` is generated from `firmware-version.yaml` and the exact staged
binary by `scripts/generate_update_manifest.py`. Besides deployment metadata, it
is the ESPHome managed-update manifest used by factory-build hubs.

When deployed, drop `firmware.ota.bin` (built by `esphome compile` from the
firmware in this repo) into the same directory as `index.html`. The page defaults
its OTA URL to `./firmware.ota.bin` relative to wherever it's served.

> **Host the credential-free build.** Build
> `quietcool-atticfan.factory.yaml` before hosting it. The separate
> `quietcool-atticfan.dev.yaml` build bakes Wi-Fi credentials and the API
> encryption key into the image; the factory build has neither, onboards Wi-Fi
> through OEM-NVS import or Improv-BLE, and includes managed release updates.
> See [Caveats](#caveats--risks) below.

## Install targets

The wizard exposes two install targets at the top of the page; both use the same
OEM OTA mechanism — only the firmware URL changes.

- **Install ESPHome (custom firmware)** — defaults to `./firmware.ota.bin`
  relative to the page. Requires the custom firmware to be present in the same
  directory as `index.html`. After installation, shared factory builds check the
  sibling `manifest.json` for new releases and expose them in Home Assistant and
  on the hub's root web page.
- **Restore OEM factory firmware** — defaults to the OEM `IT-BLT-ATTICFAN_V4.1`
  bin on QuietCool's CDN (`http://myquietcool.com/.../IT-BLT-ATTICFAN_V4.1_*.bin`),
  which is byte-for-byte what the QuietCool Smart Control app would flash. Useful
  for rolling
  back from a buggy OEM update or starting fresh before pairing. **Only works while
  the hub is still running OEM firmware** — once ESPHome is installed, its `Upgrade`
  command refuses QuietCool's firmware domains (the hub still speaks OEM BLE, it just
  won't accept a stock bin over it), so restoring stock goes through the HTTP path below.

To roll a hub that is **already running ESPHome** back to stock, use the bottom
card to open the hub's own page at `http://<host>/restore-stock`. Choose either
the verified V4.1 URL preset or a locally saved OEM `.bin` there. Because the
forms are built into the device, a restore does not depend on this installer
remaining hosted. The firmware's *Restore Stock Firmware* button in
Home Assistant and UART are additional paths. The OEM `Upgrade` command over BLE
deliberately refuses OEM firmware domains, so rollback goes through HTTP, not BLE.

Factory restore via this page rewrites only the app partition and erases only the
private `esphome` NVS namespace during final shutdown. The OEM `hx_list` namespace
is preserved, including BLE pair IDs, Smart Mode thresholds, presets, timer defaults,
and fan metadata. Repeated live Web Installer round trips from ESPHome to OEM V4.1
and back confirmed that pair IDs still authenticate and presets plus distinctive OEM
settings persist. The KEY1 5-second Factory Reset clears Wi-Fi credentials and
ESPHome preferences for re-onboarding but also deliberately preserves OEM `hx_list`;
use **Clear BLE Pairings** separately when the stored phone list must be removed.

### Path 2: device firmware page (ESPHome-running hub)

The bottom card (`#card-esphome-flash`) is independent of the BLE flow. Enter the
device hostname or IP—such as `quietcool-atticfan-abcdef.local` or a LAN IP—and
click **Open device firmware page**. The browser navigates to
`http://<host>/restore-stock`; the hostname is remembered for the next visit.

Restoring the known OEM V4.1 still requires no firmware download on the user's
phone or computer: click **Use known OEM V4.1**, then **Download and flash URL**.
The preset fills the verified URL and MD5, and the hub downloads the image itself.
The local-file form is a separate, version-independent fallback for when the
vendor URL is unavailable or the user already has another OEM release saved.

That device-local page offers:

- **Flash firmware from a URL** — any `http://` or `https://` URL the hub's Wi-Fi
  can reach. The hub fetches the bin itself. An optional 32-character MD5 checks
  the download; if omitted, the hub fetches `<firmware-url>.md5`. **Use known OEM
  V4.1** fills the exact verified stock URL and checksum.
- **Restore OEM firmware from a local file** — streams a locally selected OEM
  IT-AF-SMT application `.bin` directly from the browser into the inactive slot.

Both actions disable the page controls as soon as the user submits. URL flashing
shows an accepted/downloading state. Local upload shows percentage, then an
image-verification state. On success the browser remains on the page with reboot
guidance rather than navigating to the endpoint's plain-text response. Failures
re-enable the controls so the user can correct the input and retry.

The standard ESPHome upload remains available separately at `http://<host>/`:
use its **OTA Update** file picker for this project's `firmware.ota.bin` or another
compatible normal OTA application image. That root-page upload posts to `/update`
and retains ordinary app rollback. The `/restore-stock` local-file form is only
for an explicit permanent OEM restore and rejects this project's own image.

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

The local page POSTs `/api/flash_url?url=…&md5=…` with an empty body. The device's
`http_flash_handler` responds `200 OK` ("Accepted") immediately, then ~500 ms later starts
the OTA — pulls the bin, writes to the inactive OTA slot, reboots into it. The
running slot can never be overwritten (ESP-IDF guarantees this), so the previous
firmware remains in the other slot. Ordinary custom images retain ESP-IDF's normal
app-rollback protection. For the exact verified OEM V4.1 URL+MD5 pair, the handler
marks the stock slot VALID before reboot because stock cannot self-confirm under this
build's rollback-enabled bootloader; automatic rollback is deliberately disabled for
that explicit restore.

#### Local OEM file restore

The device page uploads the selected image as multipart data to:

```text
POST /api/restore_stock_file?confirm=RESTORE_OEM_FIRMWARE
```

The device independently repeats the structural and size checks while streaming
directly into the inactive slot. ESP-IDF verifies the complete image checksum,
embedded SHA-256, segments, and chip compatibility before the handler marks the new
slot valid. The known marker is deliberately informational rather than a whitelist:
a future legitimate OEM release remains usable even if its version, digest, project
name, or embedded strings change. This explicit path rejects this project's own
`quietcool-atticfan` image because replacement-firmware updates must retain ordinary
rollback protection.

The fixed image prefix is checked before the OTA backend erases the inactive slot.
Slot confirmation and reboot are deferred until the complete multipart request has
finished, multi-file requests are rejected, and an abandoned upload is aborted after
60 seconds. The endpoint follows the firmware's intentional LAN-trust default; Home
Assistant adoption secures API/OTA but does not add web-server authentication. Add
ESPHome `web_server.auth` in a custom or adopted build when HTTP authentication is
desired.

The public installer deliberately navigates to the local page instead of sending
these POSTs itself. This avoids the mixed-content/private-network restrictions that
can block an HTTPS page from calling a plain-HTTP LAN device.

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
`index.html`, generates a version/MD5-matched `manifest.json`, and deploys this
folder. GitHub Pages' certificate is trusted by ESP-IDF's CA bundle, so the hub's
own HTTPS update checks and firmware downloads are certificate-validated.

**Redeploy after changing the firmware or the page.** Every push to `main`
automatically runs the workflow. To rebuild and republish manually:

```sh
gh workflow run "Deploy Web installer"
```

or use the **Actions → Deploy Web installer → Run workflow** button.

After a successful Pages deployment, the workflow compares the credential-free build
with the latest firmware release. If the firmware inputs changed and the image differs,
it publishes a release and tag named
`qc-esphome-<firmware-version>` with the matching `.bin`, `.md5`,
`.sha256`, and build-fingerprint assets. GitHub-generated release notes compare the new
commit with the previous firmware-release tag. Page-only changes, cache evictions, and
other rebuilds from the same firmware inputs do not create a release. The live installer
continues to use its short `./firmware.ota.bin` URL rather than a long GitHub Release
asset URL.

`firmware-version.yaml` is the single installed/manifest/release version. The workflow
rejects changed firmware bytes when the live manifest already uses that version, forcing
an intentional version bump before another public firmware release.

**Self-hosting elsewhere.** The installer is a static site — serve the `web-installer/`
folder from any host with a public-CA HTTPS cert (Cloudflare Pages, Netlify, your own
server). Build and stage the firmware next to `index.html` yourself:

```sh
esphome compile quietcool-atticfan.factory.yaml
cp .esphome/build/quietcool-atticfan/.pioenvs/quietcool-atticfan/firmware.ota.bin \
   web-installer/firmware.ota.bin
md5sum web-installer/firmware.ota.bin | awk '{print $1}' \
   > web-installer/firmware.ota.bin.md5
python scripts/generate_update_manifest.py
```

The page auto-detects its firmware URL as `./firmware.ota.bin` relative to wherever
it's served, so no code change is needed. Keep the final URL short — the OEM
`Upgrade` buffer truncates past 100 characters, so a path like
`https://<user>.github.io/<repo>/firmware.ota.bin` is fine, but deeply nested paths
are not.

## Caveats / risks

1. **The OEM firmware does not verify signatures on OTA images.** Anything
   reachable at the URL gets flashed. Trust only firmware you built or verified.
2. **The developer `firmware.ota.bin` bakes Wi-Fi credentials and the API
   encryption key into the image.** Anyone who downloads such a `.bin` gets those
   credentials. Always host the `quietcool-atticfan.factory.yaml` build, which
   carries no secrets and onboards Wi-Fi through OEM-NVS import or Improv-BLE.
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
