// Runnable check for the Deck UI fallback in luaflipper.js.
//
//   node tools/deck_ui_test.js
//
// Runs the real script against a DOM stub small enough to be obvious, twice:
// once as the desktop client and once as the Deck UI.
//
// The case worth protecting is the desktop one. The launcher is new code on a
// path every existing user already walks, and the only thing keeping it away
// from them is a gate on ContentFrame being absent. If that gate is ever wrong,
// every desktop user grows a floating button nobody asked for. So the first
// assertion below is that nothing appears where a nav already works.
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const SRC = fs.readFileSync(
  path.join(__dirname, "..", "plugin", "luaflipper", "public", "luaflipper.js"),
  "utf8");

let failures = 0;
function check(ok, what) {
  console.log(`  ${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failures++;
}

// --- the DOM, in as few lines as will hold the script up --------------------
function makeNode(tag) {
  const node = {
    tagName: tag, id: "", className: "", textContent: "",
    children: [], parentElement: null,
    style: new Proxy({ cssText: "" }, { set: (t, k, v) => (t[k] = v, true) }),
    classList: { add() {}, remove() {}, contains: () => false },
    setAttribute() {}, getAttribute: () => null, removeAttribute() {},
    // Real enough to click: the hide-and-restore path only runs from handlers,
    // and it is the one that can leave the client blank.
    _on: {},
    addEventListener(type, fn) { (node._on[type] ||= []).push(fn); },
    removeEventListener() {},
    click() { (node._on.click || []).forEach((fn) => fn({ preventDefault() {}, stopPropagation() {} })); },
    querySelector: () => null, querySelectorAll: () => [],
    closest: () => null, contains: () => false,
    appendChild(c) { c.parentElement = node; node.children.push(c); return c; },
    remove() {
      const p = node.parentElement;
      if (p) p.children = p.children.filter((c) => c !== node);
    },
    cloneNode: () => makeNode(tag),
    get firstElementChild() { return node.children[0] || null; },
  };
  return node;
}

// byId walks the tree, so a node the script appends is findable exactly as it
// would be in a browser, rather than through a registry the test controls.
function byId(root, id) {
  if (root.id === id) return root;
  for (const c of root.children) {
    const hit = byId(c, id);
    if (hit) return hit;
  }
  return null;
}

function run(kind) {
  const body = makeNode("body");
  // Stands in for popup_target, the Deck UI's single root. Hiding it is how
  // the panel gets Steam's browser views out from under itself, and putting
  // it back is the half that matters.
  const steamRoot = makeNode("div");
  steamRoot.id = "popup_target";
  body.appendChild(steamRoot);
  const contentFrame = kind === "desktop" ? makeNode("div") : null;

  const document = {
    readyState: "complete",
    body,
    documentElement: makeNode("html"),
    head: makeNode("head"),
    createElement: makeNode,
    addEventListener() {}, removeEventListener() {},
    getElementById: (id) => byId(body, id),
    // The two selectors the fallback turns on. Everything else is absent in
    // both clients, which is what makes the Deck run reach the launcher.
    querySelector(sel) {
      if (sel === ".ContentFrame") return contentFrame;
      return null;
    },
    querySelectorAll: () => [],
  };

  const timers = [];
  const sandbox = {
    console: { log() {}, warn() {}, error() {} },
    document,
    location: { href: "about:blank" },
    fetch: () => Promise.resolve({ ok: true, json: () => Promise.resolve({}) }),
    setTimeout: (fn, ms) => (timers.push({ fn, ms }), timers.length),
    clearTimeout() {}, setInterval: () => 0, clearInterval() {},
    MutationObserver: class { observe() {} disconnect() {} },
    Promise, JSON, Math, Date, encodeURIComponent, String, Number, Array, Object,
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;

  vm.createContext(sandbox);
  vm.runInContext(SRC, sandbox, { filename: "luaflipper.js" });

  // Fire the twenty-second decision, which is where the fallback is chosen.
  timers.filter((t) => t.ms === 20000).forEach((t) => t.fn());

  return { body, sandbox, steamRoot };
}

console.log("The desktop client");
{
  const { body } = run("desktop");
  check(byId(body, "luaflipper-deck-btn") === null,
        "no launcher is added where a ContentFrame exists");
  check(byId(body, "luaflipper-deck-host") === null,
        "and no panel either");
}

console.log("The Deck UI");
{
  const { body } = run("deck");
  const btn = byId(body, "luaflipper-deck-btn");
  check(btn !== null, "a launcher is added when there is no ContentFrame");
  check(btn && btn.textContent === "LUAFlipper", "and it says what it is");
  check(byId(body, "luaflipper-deck-host") === null,
        "the panel waits to be asked for, rather than opening over the client");
}

console.log("Opening and closing the panel");
{
  // A z-index cannot win against a browser view, so the panel hides Steam's
  // root instead. That makes putting it back the safety-critical half: leave
  // it hidden and the client is blank with no way out but restarting Steam.
  const { body, steamRoot } = run("deck");
  const before = steamRoot.style.display;
  byId(body, "luaflipper-deck-btn").click();

  check(byId(body, "luaflipper-deck-host") !== null, "the panel opens");
  check(steamRoot.style.display === "none",
        "and Steam's root is hidden, so nothing of theirs can paint over it");

  const bar = byId(body, "luaflipper-deck-host").children[0];
  bar.children.find((c) => c.textContent === "Close").click();
  check(byId(body, "luaflipper-deck-host") === null, "Close removes the panel");
  check(steamRoot.style.display === before,
        "and puts Steam back exactly as it was, not merely visible");
  check(byId(body, "luaflipper-deck-btn") !== null,
        "the launcher survives, so it can be opened again");
}

console.log("Cleanup while the panel is open");
{
  // The injector re-evaluates this script on reconnect, which runs cleanup
  // whenever it happens to run. If that leaves Steam hidden, the client is
  // dead until it restarts.
  const { body, sandbox, steamRoot } = run("deck");
  const before = steamRoot.style.display;
  byId(body, "luaflipper-deck-btn").click();
  check(steamRoot.style.display === "none", "Steam is hidden with the panel up");
  sandbox.window.__luaflipperCleanup();
  check(steamRoot.style.display === before,
        "and cleanup puts it back, rather than leaving a blank client");
}

console.log("Running it twice, as the injector does");
{
  // The injector re-evaluates this script whenever it reconnects. A second
  // launcher stacked on the first would be the visible symptom.
  const { body, sandbox } = run("deck");
  check(byId(body, "luaflipper-deck-btn") !== null, "the first run adds one");
  sandbox.window.__luaflipperCleanup();
  check(byId(body, "luaflipper-deck-btn") === null,
        "and cleanup takes it away again, so a re-run cannot stack them");
}

console.log(failures ? "\nFAILED" : "\nall passed");
process.exit(failures ? 1 : 0);
