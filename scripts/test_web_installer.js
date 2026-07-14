"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { webcrypto } = require("node:crypto");

const APP_PATH = path.join(__dirname, "..", "web-installer", "app.js");
const APP_SOURCE = fs.readFileSync(APP_PATH, "utf8");

function makeStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); },
    values,
  };
}

function loadApp(storage) {
  const context = vm.createContext({
    TextDecoder,
    TextEncoder,
    URL,
    Uint8Array,
    clearTimeout,
    console: { warn() {} },
    crypto: webcrypto,
    localStorage: storage,
    navigator: {},
    setTimeout,
    window: { addEventListener() {} },
  });
  vm.runInContext(APP_SOURCE, context, { filename: APP_PATH });
  return expression => vm.runInContext(expression, context);
}

{
  const storage = makeStorage();
  const run = loadApp(storage);

  run('rememberWifiCredentials("QuietCool Wi-Fi", "secret password")');
  assert.equal(storage.values.get("qc_wifi_ssid"), "QuietCool Wi-Fi");
  assert.equal(storage.values.get("qc_wifi_password"), "secret password");

  run('rememberPairId("pair-one")');
  run('rememberPairId("pair-two")');
  run('rememberPairId("pair-one")');
  assert.equal(storage.values.get("qc_last_pair_id"), "pair-one");
  assert.deepEqual(JSON.parse(storage.values.get("qc_pair_ids")), ["pair-one", "pair-two"]);

  const remembered = JSON.parse(run("JSON.stringify(loadRememberedDetails())"));
  assert.deepEqual(remembered, {
    pairId: "pair-one",
    ssid: "QuietCool Wi-Fi",
    password: "secret password",
  });

  assert.equal(run("forgetRememberedDetails()"), true);
  for (const key of ["qc_last_pair_id", "qc_pair_ids", "qc_wifi_ssid", "qc_wifi_password"]) {
    assert.equal(storage.values.has(key), false, `${key} should be cleared`);
  }
}

{
  const storage = makeStorage({ qc_pair_ids: "not-json" });
  const run = loadApp(storage);
  run('rememberPairId("replacement")');
  assert.equal(storage.values.get("qc_last_pair_id"), "replacement");
  assert.deepEqual(JSON.parse(storage.values.get("qc_pair_ids")), ["replacement"]);
}

{
  const storage = makeStorage();
  storage.removeItem = () => { throw new Error("blocked"); };
  const run = loadApp(storage);
  assert.equal(run("forgetRememberedDetails()"), false);
}

console.log("Web installer storage tests passed.");
