// QuietCool ESPHome Web-BLE installer
// Talks the OEM hub's "V2" numeric BLE protocol (the dialect used by firmware
// IT-BLT-ATTICFAN V3.9 and later, including V4.x).
//
// V2 wire format:
//
//   {"A":<int>, ...other fields with SINGLE-CHARACTER KEYS}
//
// Specifically:
//   Login    (A=13)  {"A":13,"P":"<pair_id>"}
//   Pair     (A=14)  {"A":14,"P":"<pair_id>"}
//   Upgrade  (A=10)  {"A":10,"U":"<url>"}
//   SetRouter(A=11)  {"A":11,"S":"<ssid>","P":"<password>"}
//
// V1 (used by the analyzed Android build of the OEM app) is verbose:
// {"Api":"Login","PhoneID":"..."}
// V2 (the new dialect) abbreviates every key. The wizard targets V2 because that
// gets us numeric dispatch and tighter response framing.
//
// Critical: the V2 dispatcher gates ALL post-gate commands behind
// `pair_state == 1`. Login (A=13) sits pre-gate and always runs;
// Pair (A=14) sits pre-gate but only fires when pair_state == 2; every
// other command (Upgrade, SetRouter, PairMode, GetUpgradeState…) requires
// pair_state == 1.
//
// The wizard exposes two auth modes (visible on page load — the user picks
// one BEFORE clicking Connect, because once Web BLE grabs the link the OEM
// QuietCool Smart Control app can't talk to the hub):
//   - "login":  user supplies an existing valid pair-id (factory test for
//               never-paired hubs, or a saved one from a previous run of
//               this wizard). One write: Login → Upgrade → SetRouter.
//   - "pair":   user has just hit "Pair Mode" in the QuietCool app
//               (which sets pair_state == 2 from an already-authenticated
//               session) and force-closed the app so we can grab BLE.
//               The wizard adds a fresh random pair-id, reconnects,
//               logs in with it, then Upgrade → SetRouter.

const SERVICE_UUID = "000000ff-0000-1000-8000-00805f9b34fb";
const CHAR_UUID    = "0000ff01-0000-1000-8000-00805f9b34fb";
const IMPROV_SERVICE_UUID = "00467768-6228-2272-4663-277478268000";
const IMPROV_STATUS_UUID  = "00467768-6228-2272-4663-277478268001";
const IMPROV_ERROR_UUID   = "00467768-6228-2272-4663-277478268002";
const IMPROV_RPC_UUID     = "00467768-6228-2272-4663-277478268003";
const NAME_PREFIX  = "ATTICFAN";              // OEM BLE device name prefix (advertised as [<model>]ATTICFAN_<mac>)
const MODEL_DIGITS = ["0", "1", "2", "3", "4", "5", "6", "7"];  // OEM fan-model codes that can lead the name
const FACTORY_PAIR_ID = "1234567dsad8wqw9asasd"; // works only on never-paired devices
const STORAGE_KEYS = Object.freeze({
  pairId: "qc_last_pair_id",
  pairIds: "qc_pair_ids",
  ssid: "qc_wifi_ssid",
  password: "qc_wifi_password",
});
const PAIR_ID_REGISTERED_MESSAGE =
  "This new Pair ID was registered with the hub and saved in this browser. " +
  "Keep a copy so you can authenticate after restoring stock without entering Pair Mode again.";
const PAIR_ID_AUTHENTICATED_MESSAGE =
  "This Pair ID authenticated successfully and was saved in this browser for the next visit.";
const PAIR_ID_READY_MESSAGE =
  "This new Pair ID was registered and authenticated successfully. It is saved in this browser; " +
  "keep a copy so you can authenticate after restoring stock without entering Pair Mode again.";

// Latest production OEM firmware, served from QuietCool's own CDN. Flashing this
// rolls a hub back to stock. 88 chars — within the OEM Upgrade buffer's 100-char limit.
const OEM_RESTORE_URL = "http://myquietcool.com/profile/upload/2025/11/18/IT-BLT-ATTICFAN_V4.1_20251118010357A008.bin";
// Per-write chunk size: bleak uses max_write_without_response_size which is
// commonly 20 on Android. Web BLE has no MTU getter; 20 is the lowest-common-
// denominator that always works. Slow but correct.
const CHUNK_SIZE = 20;

const els = {};
const log = {
  el: null,
  line(text, cls = "") {
    const div = document.createElement("div");
    if (cls) div.className = cls;
    div.textContent = text;
    this.el.appendChild(div);
    this.el.scrollTop = this.el.scrollHeight;
  },
  step(text) { this.line(text, "step"); },
  out(text)  { this.line(text, "out"); },
  ok(text)   { this.line(text, "ok"); },
  err(text)  { this.line(text, "err"); },
};

let bleDevice = null;
let bleChar = null;
let rxBuf = "";
let pendingResolve = null;   // resolver for the next JSON response
let pendingTimeout = null;

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function buildImprovWifiFrame(ssid, password) {
  const enc = new TextEncoder();
  const s = enc.encode(ssid);
  const p = enc.encode(password);
  if (!s.length || s.length > 32) throw new Error("SSID must be 1-32 UTF-8 bytes");
  if (p.length > 63) throw new Error("Password must be at most 63 UTF-8 bytes");
  const data = new Uint8Array(2 + s.length + p.length);
  data[0] = s.length;
  data.set(s, 1);
  data[1 + s.length] = p.length;
  data.set(p, 2 + s.length);
  const frame = new Uint8Array(3 + data.length);
  frame[0] = 1; // WIFI_SETTINGS
  frame[1] = data.length;
  frame.set(data, 2);
  frame[frame.length - 1] = frame.slice(0, -1).reduce((sum, b) => sum + b, 0) & 0xff;
  return frame;
}

async function runImprovWifi() {
  const ssid = els.ssid.value.trim();
  const pwd = els.pwd.value;
  let frame;
  try { frame = buildImprovWifiFrame(ssid, pwd); }
  catch (e) { alert(e.message); return; }

  const btn = els.btnImprov;
  btn.disabled = true;
  els.cardProgress.style.display = "block";
  log.el.innerHTML = "";
  rememberWifiCredentials(ssid, pwd);
  setProgress(5, "Opening the Improv-BLE device picker…");
  let device;
  try {
    device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [IMPROV_SERVICE_UUID] }],
      optionalServices: [IMPROV_SERVICE_UUID],
    });
    log.ok("Picked Improv device: " + (device.name || device.id));
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(IMPROV_SERVICE_UUID);
    const statusChar = await service.getCharacteristic(IMPROV_STATUS_UUID);
    const errorChar = await service.getCharacteristic(IMPROV_ERROR_UUID);
    const rpcChar = await service.getCharacteristic(IMPROV_RPC_UUID);
    await statusChar.startNotifications();
    await errorChar.startNotifications();

    setProgress(30, "Connected to Improv; sending Wi-Fi credentials…");
    const result = new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("Timed out waiting for Wi-Fi connection")), 45000);
      const finish = fn => value => { clearTimeout(timer); fn(value); };
      statusChar.addEventListener("characteristicvaluechanged", event => {
        const state = event.target.value.getUint8(0);
        log.out("Improv state: 0x" + state.toString(16).padStart(2, "0"));
        if (state === 3) setProgress(65, "Device is connecting to Wi-Fi…");
        if (state === 4) finish(resolve)(true);
      });
      errorChar.addEventListener("characteristicvaluechanged", event => {
        const code = event.target.value.getUint8(0);
        if (code) finish(reject)(new Error("Improv error 0x" + code.toString(16).padStart(2, "0")));
      });
    });
    for (let i = 0; i < frame.length; i += CHUNK_SIZE) {
      const chunk = frame.slice(i, i + CHUNK_SIZE);
      if (rpcChar.writeValueWithResponse) await rpcChar.writeValueWithResponse(chunk);
      else await rpcChar.writeValue(chunk);
    }
    await result;
    setProgress(100, "Wi-Fi connected and saved.");
    log.ok("Improv provisioning succeeded. OEM BLE will resume after Improv shuts down.");
  } catch (e) {
    setProgress(0, "Improv provisioning failed — see log.");
    log.err("ERROR: " + e.message);
  } finally {
    try { if (device?.gatt?.connected) device.gatt.disconnect(); } catch (e) {}
    btn.disabled = false;
  }
}


function checkCompat() {
  const hasBluetooth = "bluetooth" in navigator && typeof navigator.bluetooth.requestDevice === "function";
  const isSecure = window.isSecureContext;
  const pills = [];
  if (hasBluetooth) pills.push(`<span class="pill ok">Web BLE OK</span>`);
  else pills.push(`<span class="pill err">No Web BLE</span>`);
  if (isSecure) pills.push(`<span class="pill ok">HTTPS / secure context</span>`);
  else pills.push(`<span class="pill err">Insecure context (Web BLE blocked)</span>`);
  els.compatStatus.innerHTML = pills.join(" ");

  if (!hasBluetooth) {
    els.btnFlash.disabled = true;
    els.btnFlash.textContent = "Browser not supported";
    els.btnImprov.disabled = true;
    els.btnImprov.textContent = "Browser not supported";
    return false;
  }
  return true;
}

function onRx(event) {
  const value = event.target.value;
  // value is a DataView in Web BLE. Convert to string and append to buffer.
  const decoder = new TextDecoder("utf-8");
  const bytes = new Uint8Array(value.buffer);
  const chunk = decoder.decode(bytes);
  rxBuf += chunk;
  log.out("← " + chunk);

  // Strip optional QQ prefix (V2 responses).
  const idx = rxBuf.indexOf("{");
  if (idx < 0) return;
  const candidate = rxBuf.slice(idx);

  try {
    const parsed = JSON.parse(candidate);
    rxBuf = "";   // reset only on successful parse
    if (pendingResolve) {
      const r = pendingResolve;
      pendingResolve = null;
      clearTimeout(pendingTimeout);
      r(parsed);
    } else {
      log.out("(unsolicited) " + JSON.stringify(parsed));
    }
  } catch (e) {
    // partial / not yet complete — wait for more notify packets
  }
}

async function sendCommand(obj, opts = {}) {
  const timeoutMs = opts.timeoutMs ?? 5000;
  const expectsResponse = opts.expectsResponse ?? true;
  const json = JSON.stringify(obj);
  const enc = new TextEncoder().encode(json);

  log.step("→ " + json);

  // Drop any stale bytes from a previous timeout / unsolicited push so the
  // next parse starts clean.
  rxBuf = "";

  // Build a promise BEFORE sending so we don't miss a fast response.
  let respPromise = null;
  if (expectsResponse) {
    respPromise = new Promise((resolve, reject) => {
      pendingResolve = resolve;
      pendingTimeout = setTimeout(() => {
        pendingResolve = null;
        reject(new Error(`response timeout (${timeoutMs}ms) for ${json}`));
      }, timeoutMs);
    });
  }

  for (let i = 0; i < enc.length; i += CHUNK_SIZE) {
    const slice = enc.slice(i, i + CHUNK_SIZE);
    await bleChar.writeValueWithResponse(slice);
  }

  if (!expectsResponse) return null;
  return respPromise;
}

async function connectToHub() {
  log.step("Requesting BLE device with name prefix " + NAME_PREFIX + "…");
  bleDevice = await navigator.bluetooth.requestDevice({
    // namePrefix matches from the first character, so a hub advertising
    // "<model digit>ATTICFAN_<mac>" does not match a bare "ATTICFAN" prefix.
    // The OEM app reads that leading digit out of advertising byte 5 to pick
    // the fan photo, so both shapes are in the field: stock and current
    // firmware lead with the digit, older builds of this firmware do not.
    // List both rather than drop the name filter, which would show every
    // nearby BLE device in the chooser.
    filters: [
      { namePrefix: NAME_PREFIX },
      ...MODEL_DIGITS.map((d) => ({ namePrefix: d + NAME_PREFIX })),
      { services: [SERVICE_UUID] },
    ],
    optionalServices: [SERVICE_UUID],
  });
  log.ok("Picked: " + (bleDevice.name || "(no name)"));

  bleDevice.addEventListener("gattserverdisconnected", () => {
    log.err("GATT disconnected.");
    onDisconnectUiUpdate();
  });

  log.step("Connecting GATT…");
  const server = await bleDevice.gatt.connect();
  const service = await server.getPrimaryService(SERVICE_UUID);
  bleChar = await service.getCharacteristic(CHAR_UUID);

  log.step("Subscribing to notifications…");
  await bleChar.startNotifications();
  bleChar.addEventListener("characteristicvaluechanged", onRx);
  log.ok("Connected & subscribed.");
}

async function disconnectFromHub() {
  try { bleChar?.removeEventListener("characteristicvaluechanged", onRx); } catch (e) {}
  try { await bleDevice?.gatt?.disconnect(); } catch (e) {}
  bleDevice = null;
  bleChar = null;
  rxBuf = "";
  pendingResolve = null;
  if (pendingTimeout) { clearTimeout(pendingTimeout); pendingTimeout = null; }
  onDisconnectUiUpdate();
  log.ok("Disconnected. You can now use the QuietCool Smart Control app again if needed.");
}

function onDisconnectUiUpdate() {
  els.btnFlash.disabled = false;
  els.btnDisconnect.style.display = "none";
  els.connectStatus.innerHTML = '<span class="pill warn">Disconnected</span>';
  // Restore the flash button label to match the current target.
  updateTargetUi();
}

// V2 response decoder. R-field values:
//   "Success" → handler accepted (Pair saved, Login matched, Upgrade buffered, SetRouter ACKed)
//   "Fail"    → input envelope problem (field missing, length too long)
//   "Beyond"  → Pair counter overflow specifically (pair_num > 49 in NVS)
//   "TRUE"    → seen on some firmware revisions; accepted for compatibility
// V1 responses use "Result":"Success" / "Result":"FALSE" with the same semantics.
function resultOk(resp) {
  if (!resp) return false;
  const r = resp.R ?? resp.Result;
  const f = resp.F ?? resp.Flag;
  return r === "Success" || r === "TRUE" || f === "TRUE";
}

function resultErrorReason(resp, command) {
  if (!resp) return "no response";
  const r = resp.R ?? resp.Result;
  if (command === "Pair" && r === "Beyond") {
    return "Pair counter overflow — the OEM's pair_num in NVS namespace 'hx_list' has exceeded 49. " +
           "There's no BLE-exposed reset; only KEY1 5-second long-hold (factory reset) clears the counter. " +
           "Recommended: use Login mode with an existing pair-id if you have one.";
  }
  if (r === "Fail") {
    return "Handler returned generic Fail — typically means the input JSON was missing a required field " +
           "or a string field exceeded length. The wizard's V2 wire format should be correct " +
           "(P/U/S single-char keys); if you're seeing this, check the log for the exact JSON sent.";
  }
  return "unexpected response: " + JSON.stringify(resp);
}

function randomPairId() {
  // 16 hex chars = 8 random bytes, matching the analyzed Android build's
  // pair-id convention and accepted by the same hub protocol used from iOS.
  // The OEM Pair handler's length check allows up to 100 chars, but
  // sticking to the OEM's 16-char convention is the safe default. 2^64 entropy
  // is plenty for collision-free assignment within the 50-slot pair list.
  const b = new Uint8Array(8);
  crypto.getRandomValues(b);
  return Array.from(b, x => x.toString(16).padStart(2, "0")).join("");
}

function storageError(action, error) {
  const message = `Browser-local storage ${action} failed: ${error.message}`;
  if (log.el) log.err(message);
  else console.warn(message);
}

function loadRememberedDetails() {
  const details = { pairId: null, ssid: null, password: null };
  try {
    details.pairId = localStorage.getItem(STORAGE_KEYS.pairId);
    details.ssid = localStorage.getItem(STORAGE_KEYS.ssid);
    details.password = localStorage.getItem(STORAGE_KEYS.password);
  } catch (e) {
    storageError("read", e);
  }
  return details;
}

function rememberWifiCredentials(ssid, password) {
  try {
    localStorage.setItem(STORAGE_KEYS.ssid, ssid);
    localStorage.setItem(STORAGE_KEYS.password, password);
  } catch (e) {
    storageError("save", e);
  }
}

function rememberPairId(id) {
  try {
    // Set the last-known-good ID first so malformed legacy history cannot
    // prevent the value used for next-visit autofill from being saved.
    localStorage.setItem(STORAGE_KEYS.pairId, id);
    let previous = [];
    try {
      const parsed = JSON.parse(localStorage.getItem(STORAGE_KEYS.pairIds) || "[]");
      if (Array.isArray(parsed)) previous = parsed;
    } catch (e) {
      // Replace malformed history below; qc_last_pair_id is still authoritative.
    }
    previous = previous.filter(value => value !== id);
    previous.unshift(id);
    localStorage.setItem(STORAGE_KEYS.pairIds, JSON.stringify(previous.slice(0, 5)));
  } catch (e) {
    storageError("save", e);
  }
}

function forgetRememberedDetails() {
  try {
    Object.values(STORAGE_KEYS).forEach(key => localStorage.removeItem(key));
    return true;
  } catch (e) {
    storageError("clear", e);
    return false;
  }
}

function showRememberedPairId(id, message) {
  const card = document.getElementById("card-pairid-saved");
  const input = document.getElementById("generated-pairid");
  const wasHidden = card.style.display === "none";
  input.value = id;
  document.getElementById("pairid-saved-message").textContent = message;
  card.style.display = "block";
  rememberPairId(id);
  // Scroll the user to the new card so they actually see it.
  if (wasHidden) card.scrollIntoView({ behavior: "smooth", block: "start" });
}

// Diagnostic probes. Runs when Pair (A=14) returns Fail in pair-mode flow.
// All four probes are side-effect-light:
//   - Pair attempts only mutate device state on SUCCESS (counter += 1); a Fail
//     leaves NVS alone. We're already in a known-Fail state so re-trying is safe.
//   - Login probes mutate ble_session.pair_state from 2 → 1 ONLY on success;
//     on failure pair_state stays 2 and the device stays in pair mode.
// Returns one of:
//   { ok: "pair",  pairId, needsReconnect: true  } — a Pair-via-new-id worked
//   { ok: "login", pairId, needsReconnect: false } — a Login worked directly
//   null — nothing worked
async function diagnosticProbes(failedPairId, userPairId) {
  log.step("════ DIAGNOSTICS ════");
  log.out("Pair failed with " + failedPairId.slice(0,8) + "… — running 4 probes:");
  log.out("  1. Re-send same Pair payload — tests if the failure is deterministic");
  log.out("  2. Pair with a fresh random pair-id — tests if the failure is tied to that specific id");
  log.out("  3. Login with the factory test pair-id — works only on never-paired hubs");
  log.out("  4. Login with the pair-id from the form above — works if you typed your real id");

  // Probe 1: re-send identical Pair.
  log.step("Probe 1/4: re-send same Pair");
  try {
    const r1 = await sendCommand({ A: 14, P: failedPairId }, { timeoutMs: 6000 });
    log.out("  → " + JSON.stringify(r1));
    if (resultOk(r1)) {
      log.ok("  Pair acked on retry — counter must have been transient. Using this id.");
      return { ok: "pair", pairId: failedPairId, needsReconnect: true };
    }
  } catch (e) { log.err("  Probe 1 error: " + e.message); }

  // Probe 2: different random id.
  const probeId = randomPairId();
  log.step("Probe 2/4: Pair with fresh random id " + probeId.slice(0,8) + "…");
  try {
    const r2 = await sendCommand({ A: 14, P: probeId }, { timeoutMs: 6000 });
    log.out("  → " + JSON.stringify(r2));
    if (resultOk(r2)) {
      log.ok("  Pair acked with a different id. Using this id.");
      return { ok: "pair", pairId: probeId, needsReconnect: true };
    }
  } catch (e) { log.err("  Probe 2 error: " + e.message); }

  // Probe 3: Login with factory id.
  log.step("Probe 3/4: Login with factory test pair-id");
  try {
    const r3 = await sendCommand({ A: 13, P: FACTORY_PAIR_ID }, { timeoutMs: 6000 });
    log.out("  → " + JSON.stringify(r3));
    if (resultOk(r3)) {
      log.ok("  Factory test id Login worked — your hub never paired, or the slot still has it.");
      return { ok: "login", pairId: FACTORY_PAIR_ID, needsReconnect: false };
    }
  } catch (e) { log.err("  Probe 3 error: " + e.message); }

  // Probe 4: Login with whatever's in the form (might be the user's real id).
  if (userPairId && userPairId !== FACTORY_PAIR_ID && userPairId !== failedPairId) {
    log.step("Probe 4/4: Login with the pair-id from the form (" + userPairId.slice(0,8) + "…)");
    try {
      const r4 = await sendCommand({ A: 13, P: userPairId }, { timeoutMs: 6000 });
      log.out("  → " + JSON.stringify(r4));
      if (resultOk(r4)) {
        log.ok("  That pair-id authenticates — using it.");
        return { ok: "login", pairId: userPairId, needsReconnect: false };
      }
    } catch (e) { log.err("  Probe 4 error: " + e.message); }
  } else {
    log.out("Probe 4 skipped — form pair-id is the factory test id or matches the failed id. Paste your real 16-char pair-id in the Pair ID field above (in 'Brand-new hub' mode) and retry to use it.");
  }

  log.step("════ DIAGNOSTICS COMPLETE — no auth path worked ════");
  return null;
}

async function reconnectGatt() {
  log.step("Reconnecting (V2 commits pair only on disconnect)…");
  try { bleChar.removeEventListener("characteristicvaluechanged", onRx); } catch (e) {}
  try { await bleDevice.gatt.disconnect(); } catch (e) {}
  rxBuf = "";
  pendingResolve = null;
  if (pendingTimeout) { clearTimeout(pendingTimeout); pendingTimeout = null; }
  await sleep(500);
  const server = await bleDevice.gatt.connect();
  const service = await server.getPrimaryService(SERVICE_UUID);
  bleChar = await service.getCharacteristic(CHAR_UUID);
  await bleChar.startNotifications();
  bleChar.addEventListener("characteristicvaluechanged", onRx);
  log.ok("Reconnected.");
}

async function runFlashFlow() {
  // Validate EVERYTHING before requesting the BLE device. The Web BLE API
  // requires a user-gesture for requestDevice(), so we can't do anything
  // async first — but synchronous validation of the form fields is fine and
  // avoids wasting the (short) pair-mode window on a typo.
  const mode   = document.querySelector('input[name="auth-mode"]:checked')?.value || "pair";
  const ssid   = els.ssid.value.trim();
  const pwd    = els.pwd.value;
  const url    = els.url.value.trim();
  let   pairId = els.pairid.value.trim();

  if (mode === "login") {
    if (!pairId) { alert("Pair ID required for Login mode"); return; }
    if (pairId.length > 100) { alert("Pair ID too long (max 100)"); return; }
  }
  if (!ssid) { alert("Wi-Fi SSID required — the hub uses it to download the firmware"); return; }
  if (ssid.length > 32) { alert("SSID too long (max 32 — the hub's SSID buffer)"); return; }
  if (pwd.length > 63) { alert("Password too long (max 63)"); return; }
  if (!url || !/^https?:\/\//.test(url)) { alert("URL must start with http:// or https://"); return; }
  if (url.length > 100) { alert("URL too long (max 100 chars per OEM buffer)"); return; }

  els.btnFlash.disabled = true;
  els.btnFlash.textContent = "Connecting…";
  els.cardProgress.style.display = "block";
  els.cardDone.style.display = "none";
  log.el.innerHTML = "";
  rememberWifiCredentials(ssid, pwd);
  setProgress(2, "Opening Bluetooth picker…");

  // Connect first (requestDevice + GATT). If the user cancels the picker or
  // the connection fails, surface that here and let them retry without
  // touching the form.
  try {
    await connectToHub();
    els.connectStatus.innerHTML = '<span class="pill ok">Connected</span> ' + (bleDevice.name || "(no name)");
    els.btnDisconnect.style.display = "block";
    els.btnFlash.textContent = "Flashing…";
  } catch (e) {
    log.err("Connect failed: " + e.message);
    setProgress(0, "Failed to connect — see log. Retry.");
    els.btnFlash.disabled = false;
    updateTargetUi();
    els.connectStatus.innerHTML = '<span class="pill err">Failed</span> ' + e.message;
    return;
  }

  let alreadyLoggedIn = false;

  try {
    if (mode === "pair") {
      // Pair-mode path: assumes the user just put the hub into pair_state == 2
      // via the KEY2 long-hold or the OEM app's Pair Mode button. Pair (A=14)
      // has a pre-gate slot that fires only when pair_state == 2.
      pairId = randomPairId();
      setProgress(15, "Adding new pair (assumes Pair Mode is active)…");
      log.step("Generated random pair-id: " + pairId);
      const pairResp = await sendCommand({ A: 14, P: pairId });
      if (resultOk(pairResp)) {
        log.ok("Pair acked. Reconnecting to commit…");
        showRememberedPairId(pairId, PAIR_ID_REGISTERED_MESSAGE);
        await reconnectGatt();
      } else {
        // Pair Fail → run diagnostics WITHOUT disconnecting. The hub is still
        // in pair mode (the Fail doesn't change pair_state), so we have one
        // BLE connection to send several probes and figure out what works.
        log.err("Pair returned: " + JSON.stringify(pairResp));
        log.err(resultErrorReason(pairResp, "Pair"));
        const userPairId = els.pairid.value.trim();   // whatever's in the form
        const diag = await diagnosticProbes(pairId, userPairId);
        if (!diag) {
          throw new Error("Pair rejected and no diagnostic path worked. " +
            "Either the hub left pair mode (re-trigger KEY2 long-hold or use the OEM app), " +
            "or the NVS pair-list state is wedged — only KEY1 5-second long-hold (factory " +
            "reset) recovers from that. See the log above for what each probe returned.");
        }
        pairId = diag.pairId;
        showRememberedPairId(
          pairId,
          diag.ok === "pair" ? PAIR_ID_REGISTERED_MESSAGE : PAIR_ID_AUTHENTICATED_MESSAGE
        );
        if (diag.ok === "pair" && diag.needsReconnect) {
          await reconnectGatt();
        } else if (diag.ok === "login") {
          alreadyLoggedIn = true;   // Login already succeeded as part of the probe
        }
      }
    }

    // Login (skipped if a diagnostic Login probe already authenticated us).
    // Always sent as V1 — universal across every firmware version ever
    // shipped (V1-only V2.5/V3.0/V3.8 hubs AND V4.1+). Reading the response
    // shape tells us which dialect to use for SetRouter + Upgrade.
    let useV2Dialect = false;
    if (!alreadyLoggedIn) {
      setProgress(35, "Logging in…");
      const loginResp = await sendCommand({ Api: "Login", PhoneID: pairId });
      if (!resultOk(loginResp)) {
        throw new Error("Login failed (" + JSON.stringify(loginResp) + "). " +
          (mode === "login"
            ? "On a hub that's been paired with the OEM app, the factory test ID stops working — disconnect, switch to the 'put it into pair mode' option above, and follow either the button or OEM-app method."
            : "Pair may not have committed. Disconnect, re-trigger pair mode (long-press the Pair button or use the OEM app), and retry."));
      }
      // V4.1+ wraps every response with numeric "A"; V1-only firmware
      // (pre-V3.9) responds in long-form Api/Result/Flag shape.
      useV2Dialect = "A" in loginResp;
      log.ok("Logged in (pair_state := 1, pair-id " + pairId.slice(0, 8) + "…). "
             + "Firmware dialect: " + (useV2Dialect ? "V2 (V4.1+)" : "V1 (pre-V3.9)"));
      showRememberedPairId(
        pairId,
        mode === "pair" ? PAIR_ID_READY_MESSAGE : PAIR_ID_AUTHENTICATED_MESSAGE
      );
    } else {
      // Diagnostic probe authenticated us using V2 (it sends {A:13}); only
      // V4.1+ would have answered that, so we know it's V2.
      useV2Dialect = true;
      log.ok("Already logged in via diagnostic probe — skipping Login.");
    }

    // Step 2: Upgrade — buffer the OTA URL. MUST precede SetRouter.
    //   V2 (V4.1+):  {A:10, U:<url>}
    //   V1 (pre-3.9): {Api:"Upgrade", URL:<url>}
    // Upgrade must precede SetRouter on stock V4.1: SetRouter (A=11) *spawns* the
    // OTA download task, and that task reads the URL global that Upgrade (A=10)
    // buffers. Send SetRouter first and the task launches with an EMPTY URL →
    // "Failed to initialise HTTP connection" → abort + reboot, and the later
    // Upgrade is then rejected with {}. Sent first, Upgrade stores the URL and
    // returns {"A":10,"F":"TRUE"}.
    // (V1 Upgrade is a stub on V4.1 that returns {}; on true V1-only firmware it works.)
    setProgress(50, "Sending OTA URL…");
    const upPayload = useV2Dialect
        ? { A: 10, U: url }
        : { Api: "Upgrade", URL: url };
    const upResp = await sendCommand(upPayload);
    if (!resultOk(upResp)) throw new Error("Upgrade rejected: " + JSON.stringify(upResp)
        + " — on stock V4.1 an empty {} here means SetRouter was sent before Upgrade.");
    log.ok("OTA URL buffered.");

    // Step 3: SetRouter — set Wi-Fi creds; this SPAWNS the OTA download task,
    // which now finds the URL buffered in Step 2.
    //   V2 (V4.1+):  {A:11, S:<ssid>, P:<password>}
    //   V1 (pre-3.9): {Api:"SetRouter", Ssid:<ssid>, Password:<password>}
    setProgress(75, "Sending Wi-Fi creds (triggers download)…");
    const srPayload = useV2Dialect
        ? { A: 11, S: ssid, P: pwd }
        : { Api: "SetRouter", Ssid: ssid, Password: pwd };
    const srResp = await sendCommand(srPayload, { timeoutMs: 8000 });
    if (!resultOk(srResp)) throw new Error("SetRouter rejected: " + JSON.stringify(srResp));
    log.ok("Wi-Fi creds set — hub is now connecting to Wi-Fi and downloading.");

    setProgress(95, "Hub flashing. Will disconnect shortly.");
    // Optional: try a couple of GetUpgradeState polls. May or may not succeed depending
    // on when the OEM tears down BLE during OTA.
    for (let i = 0; i < 3; i++) {
      await sleep(2000);
      try {
        const st = await sendCommand({ A: 5 }, { timeoutMs: 3000 });
        log.out("GetUpgradeState: " + JSON.stringify(st));
      } catch (e) {
        log.out("(GetUpgradeState unreachable — likely OTA in progress)");
        break;
      }
    }

    setProgress(100, "Done — hub flashing in background.");
    els.cardDone.style.display = "block";
  } catch (e) {
    log.err("ERROR: " + e.message);
    setProgress(0, "Failed — see log above. You can retry.");
  } finally {
    els.btnFlash.disabled = false;
    updateTargetUi();   // restore correct button label
  }
}

function setProgress(pct, status) {
  els.progbar.style.width = pct + "%";
  els.progressStatus.textContent = status;
}

// The public installer is HTTPS, while the hub's intentionally simple web
// server is HTTP. Navigate to the device page instead of attempting a
// mixed-content cross-origin POST that browsers can block.
function openDeviceFirmwarePage() {
  const input = document.getElementById("esphome-host");
  const raw = input.value.trim();
  if (!raw) {
    input.setCustomValidity("Enter the device hostname or LAN IP");
    input.reportValidity();
    return;
  }

  try {
    const url = new URL(/^https?:\/\//i.test(raw) ? raw : `http://${raw}`);
    if (!url.hostname) throw new Error("missing hostname");
    url.protocol = "http:";
    url.pathname = "/restore-stock";
    url.search = "";
    url.hash = "";
    input.setCustomValidity("");
    try { localStorage.setItem("qc_esphome_host", url.host); } catch (e) {}
    window.location.assign(url.href);
  } catch (e) {
    input.setCustomValidity("Enter a hostname or IP, optionally followed by a port");
    input.reportValidity();
  }
}

function updateAuthModeUi() {
  const mode = document.querySelector('input[name="auth-mode"]:checked')?.value || "pair";
  const loginBlock = document.getElementById("mode-login-block");
  const pairBlock  = document.getElementById("mode-pair-block");
  loginBlock.classList.toggle("selected",  mode === "login");
  loginBlock.classList.toggle("collapsed", mode !== "login");
  pairBlock .classList.toggle("selected",  mode === "pair");
  pairBlock .classList.toggle("collapsed", mode !== "pair");
}

function selectedTarget() {
  return document.querySelector('input[name="target"]:checked')?.value || "esphome";
}

function updateTargetUi() {
  const target = selectedTarget();
  const esphomeBlock = document.getElementById("target-esphome-block");
  const oemBlock     = document.getElementById("target-oem-block");
  esphomeBlock.classList.toggle("selected",  target === "esphome");
  esphomeBlock.classList.toggle("collapsed", target !== "esphome");
  oemBlock    .classList.toggle("selected",  target === "oem");
  oemBlock    .classList.toggle("collapsed", target !== "oem");

  // Swap the firmware URL field + flash button label to match the target.
  const url = target === "oem" ? OEM_RESTORE_URL
                               : new URL("./firmware.ota.bin", location.href).href;
  els.url.value = url;
  const info = document.getElementById("url-target-info");
  if (target === "oem") {
    info.innerHTML = "Set to the OEM <code>V4.1</code> bin at <code>myquietcool.com</code>. Override only if you want a different OEM version. Hub must have HTTP egress on the chosen Wi-Fi.";
  } else {
    info.innerHTML = "Set to the ESPHome image hosted alongside this page (<code>" + location.host + "</code>). Override only for testing custom builds.";
  }
  if (els.btnFlash && !els.btnFlash.disabled) {
    els.btnFlash.textContent = target === "oem" ? "Connect & restore OEM" : "Connect & flash ESPHome";
  }
  const doneNote = document.getElementById("done-target-note");
  if (doneNote) {
    doneNote.textContent = target === "oem"
      ? "After ~3-5 min the hub reboots into the OEM firmware. The QuietCool Smart Control app on Android or iOS should see it normally."
      : "After ~3-5 min look for an mDNS device named quietcool-atticfan-XXXXXX.local on your network, or wait for Home Assistant to discover a new ESPHome device.";
  }
}

window.addEventListener("DOMContentLoaded", () => {
  Object.assign(els, {
    compatStatus:   document.getElementById("compat-status"),
    btnDisconnect:  document.getElementById("btn-disconnect"),
    connectStatus:  document.getElementById("connect-status"),
    ssid:           document.getElementById("ssid"),
    pwd:            document.getElementById("pwd"),
    url:            document.getElementById("url"),
    urlInfo:        document.getElementById("url-info"),
    pairid:         document.getElementById("pairid"),
    btnFlash:       document.getElementById("btn-flash"),
    btnImprov:      document.getElementById("btn-improv-wifi"),
    cardProgress:   document.getElementById("card-progress"),
    cardDone:       document.getElementById("card-done"),
    progbar:        document.getElementById("progbar"),
    progressStatus: document.getElementById("progress-status"),
  });
  log.el = document.getElementById("log");

  // Default firmware URL is filled in by updateTargetUi() below based on the
  // selected install target (ESPHome custom by default).
  els.urlInfo.innerHTML = "URL has a 100-char OEM-side limit. The hub fetches this URL from the Wi-Fi above, not from your phone's connection.";

  // Restore the last-used credentials. A remembered Pair ID selects Login mode
  // so a return visit does not accidentally create another pair-list entry.
  const remembered = loadRememberedDetails();
  els.pairid.value = remembered.pairId || FACTORY_PAIR_ID;
  if (remembered.ssid !== null) els.ssid.value = remembered.ssid;
  if (remembered.password !== null) els.pwd.value = remembered.password;
  if (remembered.pairId) {
    document.querySelector('input[name="auth-mode"][value="login"]').checked = true;
  }

  // Wire up the auth-mode AND target radios. Both groups need expand/collapse
  // animations so the visible mode-block highlights itself.
  document.querySelectorAll('input[name="auth-mode"]').forEach(r => {
    r.addEventListener("change", updateAuthModeUi);
  });
  document.querySelectorAll('input[name="target"]').forEach(r => {
    r.addEventListener("change", updateTargetUi);
  });
  // Allow clicking anywhere in a collapsed mode-block to expand it (no need
  // to hit the small radio dot on a phone). Dispatch to the right updater
  // based on which radio group the block belongs to.
  document.querySelectorAll(".mode-block").forEach(block => {
    block.addEventListener("click", e => {
      if (e.target.tagName === "INPUT") return;
      const radio = block.querySelector('input[type="radio"]');
      if (radio && !radio.checked) {
        radio.checked = true;
        if (radio.name === "auth-mode") updateAuthModeUi();
        else if (radio.name === "target") updateTargetUi();
      }
    });
  });
  updateAuthModeUi();
  updateTargetUi();

  // Copy-to-clipboard for the generated pair-id card.
  document.getElementById("btn-copy-pairid").addEventListener("click", async () => {
    const input = document.getElementById("generated-pairid");
    const status = document.getElementById("copy-status");
    try {
      await navigator.clipboard.writeText(input.value);
      status.textContent = "Copied to clipboard.";
      status.className = "ok";
    } catch (e) {
      // Fallback: select the text so the user can long-press / Cmd-C / Ctrl-C.
      input.focus();
      input.select();
      status.textContent = "Clipboard blocked — text selected, press your copy shortcut.";
      status.className = "warn";
    }
    setTimeout(() => { status.textContent = ""; status.className = "muted"; }, 4000);
  });

  document.getElementById("btn-forget-details").addEventListener("click", () => {
    const forgotten = forgetRememberedDetails();
    els.pairid.value = FACTORY_PAIR_ID;
    els.ssid.value = "";
    els.pwd.value = "";
    document.querySelector('input[name="auth-mode"][value="pair"]').checked = true;
    updateAuthModeUi();
    document.getElementById("card-pairid-saved").style.display = "none";
    const status = document.getElementById("forget-details-status");
    status.textContent = forgotten
      ? "Saved Pair ID and Wi-Fi credentials removed from this browser."
      : "This browser did not allow the saved details to be cleared.";
    status.className = forgotten ? "ok" : "err";
    setTimeout(() => { status.textContent = ""; status.className = "muted"; }, 4000);
  });

  // Link into the same-origin HTTP update page served by an ESPHome-running hub.
  try {
    const savedHost = localStorage.getItem("qc_esphome_host");
    if (savedHost) document.getElementById("esphome-host").value = savedHost;
  } catch (e) {}
  document.getElementById("esphome-host").addEventListener("input", event => {
    event.currentTarget.setCustomValidity("");
  });
  document.getElementById("btn-open-device-firmware").addEventListener("click", openDeviceFirmwarePage);

  if (!checkCompat()) return;

  els.btnDisconnect.addEventListener("click", disconnectFromHub);
  els.btnFlash.addEventListener("click", runFlashFlow);
  els.btnImprov.addEventListener("click", runImprovWifi);
});
