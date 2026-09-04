/*
 * Runnable check for the changelog reader on the update page.
 *
 * The function is private to the pages IIFE, so it is lifted out by name and
 * run against stubs of the four closure values it touches. That keeps the test
 * on the real source: editing the parser in luaflipper.pages.js changes what
 * runs here, and editing CHANGELOG.md is checked against the reader that has
 * to display it.
 *
 *   node tools/changelog_test.js
 */
var fs = require("fs");
var path = require("path");
var ROOT = path.join(__dirname, "..");
var SRC = path.join(ROOT, "plugin/luaflipper/public/luaflipper.pages.js");

var src = fs.readFileSync(SRC, "utf8");
function lift(name, stop) {
  var a = src.indexOf("function " + name + "(");
  var b = src.indexOf("\n  " + stop, a);
  if (a < 0 || b < 0) { console.log("FAIL could not find " + name); process.exit(1); }
  return src.substring(a, b);
}

// The stubs. Nodes remember their text and children so the result can be read
// back as a tree rather than as a DOM.
function el(tag, cls, txt) {
  return { tag: tag, cls: cls || "", text: txt === undefined ? "" : txt,
           kids: [], style: {},
           appendChild: function (k) { this.kids.push(k); return k; } };
}
function style(n, o) { for (var k in o) n.style[k] = o[k]; return n; }
function text(v) { return (v === null || v === undefined) ? "" : String(v); }
var SET = { label: "#l", desc: "#d", field: "#f" };
var LINK_TEXT = "#link";

var findRelease = eval("(" + lift("findRelease", "/**") + ")");
var drawRelease = eval("(" + lift("drawRelease", "function updatesSection") + ")");

var fails = 0;
function check(ok, what) {
  console.log((ok ? "  ok   " : "  FAIL ") + what);
  if (!ok) fails++;
}
function walk(n, out) {
  if (n.text) out.push(n.text);
  n.kids.forEach(function (k) { walk(k, out); });
  return out;
}

// The repo's own file, which is the one that gets published and therefore the
// one the page will be handed.
var md = fs.readFileSync(path.join(ROOT, "CHANGELOG.md"), "utf8");
var versions = md.match(/^## \S+/gm).map(function (h) { return h.slice(3); });

console.log("Every release in the published file");
versions.forEach(function (v) {
  var e = findRelease(md, v);
  check(!!e, v + " is found");
  if (!e) return;
  check(e.version === v, v + " reports its own version");
  check(/^\d{4}-\d{2}-\d{2}$/.test(e.date), v + " has a date, split off the heading");
  check(e.items.length > 0 && e.items.every(function (i) { return i.bullet; }),
        v + " is all bullets (" + e.items.length + ")");
  check(e.items.every(function (i) { return i.text && i.text.indexOf("- ") !== 0; }),
        v + " bullets keep no markup");
  check(e.items.every(function (i) { return i.text.length < 160; }),
        v + " bullets stay short enough for a settings panel");
});

console.log("Only that release");
var e = findRelease(md, versions[1]);
check(e.items.join(" ").indexOf(versions[0]) < 0, "the entry above does not bleed in");
check(findRelease(md, versions[0]).items.length !==
      findRelease(md, versions[1]).items.length ||
      versions.length < 2, "entries are distinct");
check(findRelease(md, "9.9.9") === null, "a version not in the file is null");
check(findRelease("", "1.0.0") === null, "an empty file is null");
check(findRelease(md, versions[0]).items.join(" ").indexOf("load-bearing") < 0,
      "the file's own preamble is never a release");

console.log("Drawing it");
var pane = el("div");
drawRelease(findRelease(md, versions[0]), "available", el, pane);
var out = walk(pane, []);
check(out[0] === versions[0], "the version leads");
check(out[2] === "available", "the state is badged");
check(out.filter(function (t) { return t === "\u2022"; }).length ===
      findRelease(md, versions[0]).items.length, "one bullet glyph per item");
check(pane.kids.length === 1, "one box, not a list of releases");

console.log("Input the file does not promise");
check(findRelease("## 1.2.3\n\n- No date.\n", "1.2.3") !== null,
      "a heading without a date still parses");
check(findRelease("## 1.2.3 - 2026-01-01\n\n- Hyphen.\n", "1.2.3").date === "2026-01-01",
      "a hyphen separator parses too");
var prose = findRelease("## 1.2.3 \u2014 2026-01-01\n\nA paragraph, not a bullet.\n", "1.2.3");
check(prose && prose.items.length === 1 && !prose.items[0].bullet,
      "prose renders as prose rather than being dropped");
check(findRelease("## 1.2.3 \u2014 2026-01-01\n\n", "1.2.3") === null,
      "an entry with no content is null");

console.log(fails ? "\n" + fails + " FAILED" : "\nall passed");
process.exit(fails ? 1 : 0);
