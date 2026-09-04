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
var at = src.indexOf("function changelog(md, mine, el, pane)");
var end = src.indexOf("\n  function updatesSection", at);
if (at < 0 || end < 0) { console.log("FAIL could not find changelog()"); process.exit(1); }

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

var changelog = eval("(" + src.substring(at, end) + ")");

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

console.log("The published changelog, read as 1.0.1");
var pane = el("div");
var ok = changelog(md, "1.0.1", el, pane);
check(ok === true, "parsed");
check(pane.kids.length === 4, "four releases, one box each (got " +
      pane.kids.length + ")");

var heads = pane.kids.map(function (b) { return walk(b.kids[0], []); });
check(JSON.stringify(heads.map(function (h) { return h[0]; })) ===
      '["1.0.3","1.0.2","1.0.1","1.0.0"]', "newest first, versions read off");
check(heads[0][1] === "2026-09-03", "the date is split from the version");
check(heads[0][2] === "newer" && heads[1][2] === "newer",
      "releases above this build are marked newer");
check(heads[2][2] === "this build", "the running version is marked");
check(heads[3].length === 2, "older releases carry no badge");
check(pane.kids[3].style.opacity === "0.7", "history is dimmed");
check(pane.kids[2].style.opacity === "1", "this build is not");

var body = walk(pane.kids[0], []);
check(body.length > 4, "1.0.3 kept its paragraphs (" + (body.length - 3) + ")");
check(body.join(" ").indexOf("## ") < 0, "no heading markup leaked into text");
check(body.join(" ").indexOf("Every released version") < 0,
      "the file's own preamble is not a release");
check(body[3].indexOf("Downloading a fix works.") === 0,
      "a paragraph starts where the file does");
check(body[3].indexOf("\n") < 0, "wrapped lines are rejoined into one line");

console.log("On the newest build");
pane = el("div");
changelog(md, "1.0.3", el, pane);
heads = pane.kids.map(function (b) { return walk(b.kids[0], []); });
check(heads[0][2] === "this build", "the top entry is marked, not 'newer'");
check(heads.filter(function (h) { return h[2] === "newer"; }).length === 0,
      "nothing is offered as an update");

console.log("On a version the file does not mention");
pane = el("div");
changelog(md, "0.9.9", el, pane);
heads = pane.kids.map(function (b) { return walk(b.kids[0], []); });
check(heads.every(function (h) { return h[2] === "newer"; }),
      "every release reads as newer");

console.log("Input the file does not promise");
check(changelog("", "1.0.3", el, el("div")) === false, "empty is refused");
check(changelog("# Changelog\n\nno releases yet\n", "1.0.3", el, el("div")) === false,
      "a file with no entries is refused");
pane = el("div");
check(changelog("## 1.2.3\n\nA release with no date.\n", "1.0.3", el, pane) === true,
      "a heading without a date still parses");
check(walk(pane.kids[0].kids[0], [])[0] === "1.2.3", "and keeps its version");
pane = el("div");
changelog("## 1.2.3 - 2026-01-01\n\nHyphen, not an em dash.\n", "x", el, pane);
check(walk(pane.kids[0].kids[0], [])[1] === "2026-01-01",
      "a hyphen separator parses too");

console.log(fails ? "\n" + fails + " FAILED" : "\nall passed");
process.exit(fails ? 1 : 0);
