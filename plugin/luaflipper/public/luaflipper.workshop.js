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
  function hookSubscribe(btn, id) {
    if (!btn || btn.getAttribute("data-lf-hooked")) return;
    if (!id) return;

    var label = btn.querySelector("span") || btn;
    var original = label.textContent;
    var hadText = !!(original && original.trim());
    var originalTitle = btn.getAttribute("title") || "";
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
      function show(words) {
        if (hadText) label.textContent = words; else btn.title = words;
      }
      var tick = setInterval(function () {
        show("Downloading " + Math.round((new Date().getTime() - started) / 1000) + "s");
      }, 1000);
      show("Downloading 0s");

      function done(s, why) {
        clearInterval(tick);
        show(s);
        if (hadText) btn.title = why || "";
        else if (why) btn.title = s + " \u2014 " + why;
      }
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
        show("Subscribing");
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
    // The item page's button says what it does; the listing's quick-add is an
    // icon with no words, so it gets a tooltip instead of having text forced
    // into it.
    if (hadText) label.textContent = "Download with LUAFlipper";
    else btn.title = "Download with LUAFlipper";
    undo.push(function () {
      btn.removeEventListener("click", onClick, true);
      btn.removeAttribute("data-lf-hooked");
      btn.classList.remove(MARK);
      if (hadText) label.textContent = original;
      btn.title = originalTitle;
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
  /**
   * Whether a class actually paints anything on this page.
   *
   * The workshop spans two different UIs. An item's own page is the older web
   * one and carries newmodal and the btn_*_steamui family; the hub, browse and
   * collections are the newer design system and carry none of them, so a
   * dialog built from those class names there is an unstyled <dialog> -- which
   * the browser renders white on white.
   */
  function paints(cls, tag) {
    var a = document.createElement(tag || "div"), b = document.createElement(tag || "div");
    b.className = cls;
    document.body.appendChild(a);
    document.body.appendChild(b);
    var ca = getComputedStyle(a), cb = getComputedStyle(b);
    var differs = ca.backgroundColor !== cb.backgroundColor ||
                  ca.backgroundImage !== cb.backgroundImage ||
                  ca.color !== cb.color;
    a.remove();
    b.remove();
    return differs;
  }

  /**
   * The page's own look, for when Steam's dialog classes are not on it.
   *
   * Read off the live page rather than hardcoded: the surface colour comes
   * from the body, and the buttons are copied from a real quick-add button
   * sitting in the listing. So the dialog matches whatever theme is applied,
   * including one this was never written against, and it does it without
   * knowing a single class name.
   */
  function pageLook() {
    var bs = getComputedStyle(document.body);
    var look = {
      surface: bs.backgroundColor && bs.backgroundColor !== "rgba(0, 0, 0, 0)"
        ? bs.backgroundColor : "#262b34",
      ink: bs.color || "#c6d4df",
      font: bs.fontFamily,
      green: null
    };
    var sample = document.querySelector('button[data-accent-color="green"]');
    if (sample) {
      var cs = getComputedStyle(sample);
      look.green = {
        backgroundColor: cs.backgroundColor, backgroundImage: cs.backgroundImage,
        color: cs.color, borderRadius: cs.borderRadius, border: cs.border,
        fontSize: cs.fontSize, fontWeight: cs.fontWeight, fontFamily: cs.fontFamily
      };
    }
    return look;
  }

  function ask(then) {
    // Which of the two UIs is under us decides whether the dialog can lean on
    // Steam's classes or has to take its colours from the page.
    var native = paints("newmodal");

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

    var look = native ? null : pageLook();
    if (look) {
      // Steam's classes are absent here, so the dialog is dressed from the page
      // it is sitting on.
      modal.style.background = look.surface;
      modal.style.color = look.ink;
      modal.style.fontFamily = look.font;
      modal.style.border = "1px solid rgba(255,255,255,.10)";
      modal.style.borderRadius = "3px";
      modal.style.boxShadow = "0 12px 40px rgba(0,0,0,.6)";
      modal.style.padding = "0";
    }

    var topBar = div("modal_top_bar", modal);
    if (look) {
      topBar.style.cssText = "height:3px;background:linear-gradient(to right," +
                             "#417a9b,#67c1f5)";
    }

    var headBorder = div("newmodal_header_border", modal);
    var head = div("newmodal_header", headBorder);
    var shut = div("newmodal_close", head);
    var title = div("title_text", head);
    title.textContent = "Download with LUAFlipper";
    if (look) {
      head.style.cssText = "display:flex;align-items:center;justify-content:space-between;" +
                           "flex-direction:row-reverse;padding:14px 18px;" +
                           "border-bottom:1px solid rgba(255,255,255,.08)";
      title.style.cssText = "font-size:17px;font-weight:700;color:#ffffff";
      shut.style.cssText = "width:16px;height:16px;opacity:.6;background:none;" +
                           "font:16px/16px sans-serif;text-align:center";
      shut.textContent = "\u2715";
    }

    var contentBorder = div("newmodal_content_border", modal);
    var content = div("newmodal_content", contentBorder);

    if (look) content.style.cssText = "padding:18px;line-height:1.5";

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

    function button(cls, words, fn, accent) {
      var b = div(cls + " btn_medium", row);
      var s = document.createElement("span");
      s.textContent = words;
      b.appendChild(s);
      b.style.cursor = "pointer";
      if (look) {
        // Copy the page's own green button, then tint the secondaries from it,
        // so all three belong to the same set as the buttons around them.
        if (look.green) {
          for (var k in look.green) if (look.green.hasOwnProperty(k)) b.style[k] = look.green[k];
        }
        b.style.padding = "7px 14px";
        b.style.display = "inline-block";
        if (accent !== "green") {
          b.style.backgroundImage = "none";
          b.style.backgroundColor = accent === "blue"
            ? "rgba(103,193,245,.20)" : "rgba(255,255,255,.10)";
          b.style.color = look.ink;
        }
      }
      b.addEventListener("click", function () { close(); fn(); });
      return b;
    }

    // Green is the primary action in Steam's dialogs, and the primary action
    // here is the one that changes nothing outside this machine.
    button("btn_green_steamui", "Download only", function () { then(false); }, "green");
    button("btn_blue_steamui", "Subscribe and download", function () { then(true); }, "blue");
    button("btn_grey_steamui", "Cancel", function () {}, "grey");

    shut.style.cursor = "pointer";
    shut.addEventListener("click", close);
    back.addEventListener("click", close);
    document.addEventListener("keydown", onKey, true);

    document.body.appendChild(back);
    document.body.appendChild(modal);
  }

  /**
   * The item a listing control belongs to.
   *
   * Walks out from the button to the smallest ancestor holding exactly one
   * link to an item page, and reads the id off that. Structural on purpose:
   * the workshop hub and browse pages are Steam's newer UI, where every class
   * name is a build hash -- YAnzAmuUUnI- today, something else after the next
   * client update -- so anything keyed to one would break silently. The
   * relationship between a row and its own link does not change.
   */
  function rowId(btn) {
    var n = btn;
    for (var up = 0; up < 8 && n && n.parentNode; up++) {
      n = n.parentNode;
      if (!n.querySelectorAll) continue;
      var links = n.querySelectorAll('a[href*="filedetails/?id="]');
      if (links.length === 1) {
        var m = /[?&]id=(\d+)/.exec(links[0].getAttribute("href") || "");
        if (m) return m[1];
      }
      if (links.length > 1) return "";     // walked past the row into the grid
    }
    return "";
  }

  function apply() {
    if (!active || !document.body) return;

    var here = fileId();
    if (here) {
      // An item's own page. Steam has used several of these over the years and
      // ships more than one on a page (the sticky header carries its own copy).
      var seen = document.querySelectorAll(
        "#SubscribeItemBtn, .subscribeOption, #SubscribeItemOptionAdd, " +
        "[onclick*='SubscribeItem'], [id^='SubscribeItem']");
      for (var i = 0; i < seen.length; i++) hookSubscribe(seen[i], here);
      return;
    }

    /*
     * A listing: the workshop hub, browse, a collection. Each row carries the
     * green quick-add button, which subscribes in one click without ever
     * opening the item.
     *
     * Matched on data-accent-color, which is what makes it the green one, and
     * is a semantic attribute rather than one of the hashed class names beside
     * it. Rows without a resolvable item are left alone, so a green button that
     * is not a quick-add -- a filter, a paginator -- keeps working as Steam
     * intended.
     */
    var adds = document.querySelectorAll('button[data-accent-color="green"]');
    for (var j = 0; j < adds.length; j++) {
      var id = rowId(adds[j]);
      if (id) hookSubscribe(adds[j], id);
    }
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
