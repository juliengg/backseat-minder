/* Run with node tests/setup_positioning_test.cjs. No browser/npm dependencies. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const script = fs.readFileSync(path.join(__dirname, "../main/setup_positioning.js"), "utf8");

async function settle() { for (let i = 0; i < 12; ++i) await Promise.resolve(); }

function harness(fetchFrame) {
  const element = () => ({
    hidden: true, checked: false, textContent: "", listeners: {},
    addEventListener(name, callback) { this.listeners[name] = callback; },
    fire(name) { this.listeners[name](); },
    removeAttribute(name) { delete this[name]; }
  });
  const toggle = element(), preview = element(), image = element(), status = element(), form = element();
  const document = element(), window = element();
  document.hidden = false;
  document.getElementById = id => ({"view-positioning": toggle, "positioning-preview": preview,
    "positioning-image": image, "positioning-status": status})[id];
  document.querySelector = () => form;
  let now = 0, timerId = 0, urlId = 0;
  const timers = new Map(), requests = [], revoked = [];
  const ok = () => ({ok: true, blob: async () => ({type: "image/jpeg"})});
  vm.runInNewContext(script, {
    document, window, AbortController, Date: {now: () => now},
    URL: {createObjectURL: () => "blob:" + (++urlId), revokeObjectURL: url => revoked.push(url)},
    fetch: (url, options) => {
      requests.push({url, options, at: now});
      return fetchFrame ? fetchFrame(requests.length, options, ok) : Promise.resolve(ok());
    },
    setTimeout: (callback, delay) => {
      const id = ++timerId;
      timers.set(id, {callback, at: now + delay});
      return id;
    },
    clearTimeout: id => timers.delete(id)
  });
  return {toggle, preview, image, status, document, window, form, requests, revoked, timers,
    async enable(value = true) { toggle.checked = value; toggle.fire("change"); await settle(); },
    async advance(duration) {
      const target = now + duration;
      let count = 0;
      while (true) {
        const next = [...timers].filter(([, value]) => value.at <= target)
          .sort((a, b) => a[1].at - b[1].at)[0];
        if (!next) break;
        assert.ok(++count < 100, "unexpected timer loop");
        now = next[1].at;
        timers.delete(next[0]);
        next[1].callback();
        await settle();
      }
      now = target;
      await settle();
    }
  };
}

async function run() {
  const normal = harness();
  assert.equal(normal.requests.length, 0);
  assert.equal(normal.preview.hidden, true);
  await normal.enable();
  assert.equal(normal.preview.hidden, false);
  assert.equal(normal.image.hidden, false);
  assert.equal(normal.requests[0].options.cache, "no-store");
  await normal.advance(999);
  assert.equal(normal.requests.length, 1);
  await normal.advance(1);
  assert.equal(normal.requests.length, 2);
  assert.deepEqual(normal.revoked, ["blob:1"]);
  await normal.enable(false);
  assert.equal(normal.preview.hidden, true);
  assert.equal(normal.image.src, undefined);
  assert.deepEqual(normal.revoked, ["blob:1", "blob:2"]);
  await normal.advance(5000);
  assert.equal(normal.requests.length, 2);

  let resolveLate;
  const pending = harness(() => new Promise(resolve => { resolveLate = resolve; }));
  await pending.enable();
  await pending.advance(1000);
  assert.equal(pending.requests.length, 1); // One outstanding request, never an interval backlog.
  await pending.enable(false);
  assert.equal(pending.requests[0].options.signal.aborted, true);
  resolveLate({ok: true, blob: async () => ({})}); // Simulate a late response ignoring abort.
  await settle();
  assert.equal(pending.image.src, undefined);
  assert.equal(pending.timers.size, 0);

  const failure = harness((count, options, ok) => Promise.resolve(count === 1 ? {ok: false} : ok()));
  await failure.enable();
  assert.equal(failure.image.hidden, true);
  assert.match(failure.status.textContent, /unavailable/);
  await failure.advance(1000);
  assert.equal(failure.image.hidden, false);
  assert.match(failure.status.textContent, /1 FPS/);

  const timeout = harness((count, options, ok) => count > 1 ? Promise.resolve(ok()) : new Promise((resolve, reject) => {
    options.signal.addEventListener("abort", () => reject(new Error("aborted")));
  }));
  await timeout.enable();
  await timeout.advance(2500);
  assert.equal(timeout.requests.length, 2);
  assert.equal(timeout.requests[0].options.signal.aborted, true);
  assert.equal(timeout.image.hidden, false);

  const lifecycle = harness();
  await lifecycle.enable();
  lifecycle.document.hidden = true;
  lifecycle.document.fire("visibilitychange");
  await lifecycle.advance(5000);
  assert.equal(lifecycle.requests.length, 1);
  lifecycle.document.hidden = false;
  lifecycle.document.fire("visibilitychange");
  await settle();
  assert.equal(lifecycle.requests.length, 2);
  lifecycle.form.fire("submit");
  await lifecycle.advance(5000);
  assert.equal(lifecycle.requests.length, 2);
  assert.equal(lifecycle.image.src, undefined);

  const leaving = harness();
  await leaving.enable();
  leaving.window.fire("pagehide");
  await leaving.advance(5000);
  assert.equal(leaving.requests.length, 1);
  assert.equal(leaving.timers.size, 0);
  leaving.window.fire("pageshow"); // Returning from the browser's back/forward cache.
  await settle();
  assert.equal(leaving.requests.length, 2);
  assert.equal(leaving.image.hidden, false);
  return "PASS: off by default, 1 FPS, cancellation, late replies, errors/timeouts, hidden page and form submission";
}

module.exports = run;
if (require.main === module) run().then(console.log);
