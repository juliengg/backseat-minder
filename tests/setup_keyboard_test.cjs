/* Run with node tests/setup_keyboard_test.cjs. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

function target() {
  return {
    listeners: {},
    addEventListener(type, callback) { this.listeners[type] = callback; },
    fire(type, props = {}) {
      const event = { target: this, prevented: false,
        preventDefault() { this.prevented = true; }, ...props };
      this.listeners[type]?.(event);
      return event;
    }
  };
}
const document = target(), window = target(), timers = [];
const field = value => ({
  value, matches: () => true, closest: () => null,
  blur() {
    if (document.activeElement === this) document.activeElement = null;
    document.fire("focusout", {target: this});
  }
});
const name = field("My Name"), phone = field("+15555550123");
const background = {matches: () => false, closest: () => null};
const checkbox = {matches: () => false, closest: () => ({control: checkbox})};
document.activeElement = name; // The portal restored focus on arrival.
vm.runInNewContext(fs.readFileSync(path.join(__dirname, "../main/setup_keyboard.js"), "utf8"), {
  document, window, setTimeout: callback => timers.push(callback)
});
assert.equal(document.activeElement, null);
assert.equal(name.value, "My Name");
function focus(element) {
  const previous = document.activeElement;
  if (previous) previous.blur();
  document.activeElement = element;
  document.fire("focusin", {target: element});
}
function tap(element) {
  document.fire("pointerdown", {target: element});
  focus(element);
  document.fire("click", {target: element});
}

tap(name);
assert.equal(document.activeElement, name);
tap(phone); // Authorize the next field before the old one blurs.
assert.equal(document.activeElement, phone);
document.fire("pointerdown", {target: background});
assert.equal(document.activeElement, null);
assert.equal(phone.value, "+15555550123");

// Regression: enable positioning, then the portal repeatedly restores Name.
tap(name);
document.fire("pointerdown", {target: checkbox});
assert.equal(document.activeElement, null);
for (let frame = 0; frame < 10; frame++) {
  focus(name);
  assert.equal(document.activeElement, null);
}
// Editing must still work with the preview enabled.
tap(name);
assert.equal(document.activeElement, name);
assert.equal(document.fire("keydown", {target: name, key: "Enter", isComposing: true}).prevented, false);
assert.equal(document.activeElement, name);
assert.equal(document.fire("keydown", {target: name, key: "Enter"}).prevented, true);
assert.equal(document.activeElement, null);
focus(name); // Immediate refocus after pressing the keyboard checkmark.
assert.equal(document.activeElement, null);

tap(phone);
assert.equal(document.fire("keydown", {target: phone, key: "Escape"}).prevented, true);
assert.equal(document.activeElement, null);
tap(name);
name.blur(); // iOS may dismiss without sending an Enter key event.
focus(name);
assert.equal(document.activeElement, null);

// Label taps, physical keyboard navigation and required-field validation work.
const label = {matches: () => false, closest: () => ({control: phone})};
document.fire("click", {target: label});
focus(phone);
assert.equal(document.activeElement, phone);
document.fire("keydown", {target: phone, key: "Tab"});
focus(name);
assert.equal(document.activeElement, name);
timers.splice(0).forEach(callback => callback());
name.blur();
focus(phone);
assert.equal(document.activeElement, null);
document.fire("invalid", {target: name});
focus(name);
assert.equal(document.activeElement, name);
window.fire("pageshow");
assert.equal(document.activeElement, null);
focus(name);
assert.equal(document.activeElement, null);
assert.equal(name.value, "My Name");
console.log("PASS: repeated portal refocus blocked; deliberate editing, labels, Tab, validation and keyboard dismissal preserved");
