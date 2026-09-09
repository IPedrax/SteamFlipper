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
    addEventListener() {}, removeEventListener() {},
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

  return { body, sandbox };
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
