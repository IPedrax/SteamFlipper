/*
 * LUAFlipper, workshop page integration.
 *
 * The community sibling of luaflipper.store.js, and the same shape: Steam's
 * real workshop pages, in the client's own browser view, with one control
 * rebound. The store hooks Add to Cart; this hooks Subscribe.
 *
 * Two modes, pushed in by the module and never asked for:
 *
 *   off  the workshop is the workshop. Subscribe subscribes. This is the state
 *        on the Community tab, and the state everywhere unless told otherwise.
 *   on   the page was opened as the LUAFlipper Workshop tab. Subscribe opens a
 *        dialog first, offering the download on its own or a subscription with
 *        it, and does neither until asked.
 *
 * Why rebind it rather than add a button: subscribing is what a person came to
 * that page to do, and the thing they actually want from it -- the files -- is
 * available without the account change. Most of the time only the files are
 * wanted, so the dialog offers that as a real option instead of making a
 * subscription the price of a download.
 *
 * Talking to the module: over the debugger binding, not fetch. Community pages
 * carry a CSP whose connect-src does not include loopback, exactly like the
 * store, and the binding is not a network request.
 *
 * ES5 only, no build step.
 */
(function () {
  "use strict";

  var MARK = "luaflipper-hooked";

  if (typeof window.__luaflipperWorkshopCleanup === "function") {
    try { window.__luaflipperWorkshopCleanup(); } catch (e) {}
  }

  var active = false;
  var undo = [];

  function text(v) { return (v === null || v === undefined) ? "" : String(v); }

  /* --------------------------------------------------------------- bridge --- */

  var pending = {}, seq = 0;

  window.__luaflipperBridgeReply = function (id, body) {
    var f = pending[id];
    delete pending[id];
    if (f) { try { f(body); } catch (e) {} }
  };

  function call(path, cb) {
    if (typeof window.__luaflipperBridge !== "function") { cb(null); return; }
    var id = ++seq;
    pending[id] = cb;
    try {
      window.__luaflipperBridge(JSON.stringify({ id: id, path: path }));
    } catch (e) { delete pending[id]; cb(null); return; }
    setTimeout(function () {
      if (pending[id]) { delete pending[id]; cb(null); }
    }, 180000);
  }

  // The module pushes replies in as a JSON literal, so an object arrives
  // already parsed; running JSON.parse over one stringifies it first and then
  // throws. The store script learned this the hard way.
  function parse(body) {
    if (!body) return null;
    if (typeof body === "object") return body;
    try { return JSON.parse(body); } catch (e) { return null; }
  }

  /* ------------------------------------------------------------- workshop --- */

  /**
   * The published file this page is about.
   *
   * Read from the URL rather than the DOM: every workshop item page is
   * /sharedfiles/filedetails/?id=N, and the markup around the button has
   * changed more than once.
   */
  function fileId() {
    var m = /[?&]id=(\d+)/.exec(location.search);
    return m ? m[1] : "";
  }

  /**
   * The app the item belongs to.
   *
   * Steam puts it in a breadcrumb link to the game's workshop hub. Worth taking
   * from the page when it is there, because it saves a round trip; the module
   * can resolve it from the item id when it is not.
   */
  function appId() {
    var a = document.querySelector('.breadcrumbs a[href*="/app/"]') ||
            document.querySelector('a[href*="/workshop/browse/?appid="]');
    if (a) {
      var m = /\/app\/(\d+)|appid=(\d+)/.exec(a.getAttribute("href") || "");
      if (m) return m[1] || m[2];
    }
    return "";
  }

  /**
   * Rebind the Subscribe button on an item page.
   *
   * Capture phase with propagation stopped, so Steam's own handler never runs
   * and nothing reaches the account. The element is left in place with its own
   * classes: relabelling Valve's button is the point, and replacing it would
   * lose the page's styling.
   */
  function hookSubscribe(btn) {
    if (!btn || btn.getAttribute("data-lf-hooked")) return;
    var id = fileId();
    if (!id) return;

    var label = btn.querySelector("span") || btn;
    var original = label.textContent;
    btn.setAttribute("data-lf-hooked", "1");
    btn.classList.add(MARK);

    var busy = false;
    function onClick(ev) {
      ev.preventDefault();
      ev.stopPropagation();
      ev.stopImmediatePropagation();
      if (busy) return;
      // The choice is the user's, and it is not one to infer: downloading
      // writes a folder on this machine, subscribing writes to the account and
      // shows on a public profile. So the click opens the question rather than
      // answering it.
      ask(function (alsoSubscribe) {
        busy = true;
        run(alsoSubscribe);
      });
    }

    function run(alsoSubscribe) {
      var started = new Date().getTime();
      var tick = setInterval(function () {
        label.textContent = "Downloading " +
          Math.round((new Date().getTime() - started) / 1000) + "s";
      }, 1000);
      label.textContent = "Downloading 0s";

      function done(s, why) { clearInterval(tick); label.textContent = s; btn.title = why || ""; }
      function retry(s, why) { done(s, why); busy = false; }

      function start(app) {
        if (!app) { retry("No app id", "Could not work out which game this is for"); return; }
        call("/api/workshop/download?appid=" + encodeURIComponent(app) +
             "&id=" + encodeURIComponent(id), function (body) {
          var res = parse(body);
          if (!res || res.error) {
            retry("Unavailable", (res && res.error) || "LUAFlipper unreachable");
            return;
          }
          watch(app, 0);
        });
      }

      // Steam reports an item as downloaded only when it has finished, so this
      // waits for the client rather than claiming success on the request.
      function watch(app, n) {
        call("/api/workshop/status?appid=" + encodeURIComponent(app) +
             "&id=" + encodeURIComponent(id), function (body) {
          var res = parse(body);
          if (res && res.downloaded) {
            done("Downloaded", text(res.path));
            return;
          }
          if (n > 90) {                       // three minutes
            retry("Still going", "Steam has not reported it yet; check Downloads");
            return;
          }
          setTimeout(function () { watch(app, n + 1); }, 2000);
        });
      }

      // Subscribing first when it was asked for, because a subscription is
      // what makes Steam keep the item updated; the download is the same call
      // either way.
      function go(app) {
        if (!alsoSubscribe) { start(app); return; }
        label.textContent = "Subscribing";
        call("/api/workshop/subscribe?appid=" + encodeURIComponent(app) +
             "&id=" + encodeURIComponent(id) + "&set=1", function (body) {
          var res = parse(body);
          if (!res || res.error) {
            retry("Unavailable", (res && res.error) || "The client refused to subscribe");
            return;
          }
          start(app);
        });
      }

      var app = appId();
      if (app) { go(app); return; }
      // Not in the page, so ask the module, which reads it off Steam's API.
      call("/api/workshop/info?q=" + encodeURIComponent(id), function (body) {
        var res = parse(body);
        if (res && res.appid) go(res.appid);
        else retry("No app id", "Could not work out which game this is for");
      });
    }

    btn.addEventListener("click", onClick, true);
    label.textContent = "Download with LUAFlipper";
    undo.push(function () {
      btn.removeEventListener("click", onClick, true);
      btn.removeAttribute("data-lf-hooked");
      btn.classList.remove(MARK);
      label.textContent = original;
      btn.title = "";
    });
  }

  /* --------------------------------------------------------------- ask --- */

  /**
   * The question, in Steam's own dialog.
   *
   * The markup is Steam's, copied from what ShowConfirmDialog builds on this
   * page rather than approximated: a <dialog class="newmodal"> holding a top
   * bar, a header border wrapping the close box and title_text, and a content
   * border wrapping the content and its newmodal_buttons row. Getting that
   * nesting right is what makes it a Steam dialog instead of a box that looks
   * like one -- several of those classes are styled only in context, and a
   * theme restyling Steam's dialogs targets exactly this shape.
   *
   * Buttons are the btn_*_steamui family the client's own dialogs use, not the
   * older btn_*_white_innerfade web buttons.
   *
   * `then` is called with true to subscribe as well, false to only download,
   * and not at all if the user backs out.
   */
  function ask(then) {
    function div(cls, parent) {
      var d = document.createElement("div");
      if (cls) d.className = cls;
      if (parent) parent.appendChild(d);
      return d;
    }

    // Steam's own JS positions its dialogs; ours only has to be centred, and
    // the backdrop is separate so a click outside can dismiss it.
    var back = document.createElement("div");
    back.style.cssText = "position:fixed;inset:0;z-index:999;background:rgba(0,0,0,.6)";

    var modal = document.createElement("dialog");
    modal.className = "newmodal";
    modal.setAttribute("open", "");
    modal.style.cssText = "position:fixed;z-index:1000;left:50%;top:50%;" +
                          "transform:translate(-50%,-50%);outline:none;margin:0;" +
                          "max-width:min(520px, calc(100% - 40px))";

    div("modal_top_bar", modal);

    var headBorder = div("newmodal_header_border", modal);
    var head = div("newmodal_header", headBorder);
    var shut = div("newmodal_close", head);
    var title = div("title_text", head);
    title.textContent = "Download with LUAFlipper";

    var contentBorder = div("newmodal_content_border", modal);
    var content = div("newmodal_content", contentBorder);

    var says = div("", content);
    says.textContent =
      "Steam downloads the files either way. Subscribing also adds this item " +
      "to your account, keeps it updated, and shows it on your public profile.";

    // newmodal_buttons carries no styling of its own outside Steam's own panel
    // layout, so the row is laid out here; the class stays for themes that do
    // target it.
    var row = div("newmodal_buttons", content);
    row.style.cssText = "display:flex;gap:8px;flex-wrap:wrap;margin-top:20px;" +
                        "justify-content:flex-end";

    function close() {
      if (back.parentNode) back.parentNode.removeChild(back);
      if (modal.parentNode) modal.parentNode.removeChild(modal);
      document.removeEventListener("keydown", onKey, true);
    }
    function onKey(e) { if (e.key === "Escape") { e.preventDefault(); close(); } }

    function button(cls, words, fn) {
      var b = div(cls + " btn_medium", row);
      var s = document.createElement("span");
      s.textContent = words;
      b.appendChild(s);
      b.style.cursor = "pointer";
      b.addEventListener("click", function () { close(); fn(); });
      return b;
    }

    // Green is the primary action in Steam's dialogs, and the primary action
    // here is the one that changes nothing outside this machine.
    button("btn_green_steamui", "Download only", function () { then(false); });
    button("btn_blue_steamui", "Subscribe and download", function () { then(true); });
    button("btn_grey_steamui", "Cancel", function () {});

    shut.style.cursor = "pointer";
    shut.addEventListener("click", close);
    back.addEventListener("click", close);
    document.addEventListener("keydown", onKey, true);

    document.body.appendChild(back);
    document.body.appendChild(modal);
  }

  function apply() {
    if (!active || !document.body) return;
    if (!fileId()) return;              // a browse page has nothing to rebind
    // Steam has used several of these over the years and ships more than one
    // on a page (the sticky header carries its own copy).
    var seen = document.querySelectorAll(
      "#SubscribeItemBtn, .subscribeOption, #SubscribeItemOptionAdd, " +
      "[onclick*='SubscribeItem'], [id^='SubscribeItem']");
    for (var i = 0; i < seen.length; i++) hookSubscribe(seen[i]);
  }

  function revert() {
    while (undo.length) {
      try { undo.pop()(); } catch (e) {}
    }
  }

  window.__luaflipperSetMode = function () {
    var on = !!window.__luaflipperMode;
    if (on === active) { if (on) apply(); return; }
    active = on;
    if (on) apply(); else revert();
  };

  var obs = null;

  function start() {
    if (!document.body || obs) return;
    obs = new MutationObserver(apply);
    obs.observe(document.body, { childList: true, subtree: true });
    window.__luaflipperSetMode();
  }

  if (document.body) start();
  else document.addEventListener("DOMContentLoaded", start);

  window.__luaflipperWorkshopCleanup = function () {
    if (obs) { obs.disconnect(); obs = null; }
    document.removeEventListener("DOMContentLoaded", start);
    revert();
    window.__luaflipperWorkshopCleanup = null;
  };
})();
