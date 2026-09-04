/*
 * LUAFlipper, store page integration.
 *
 * Injected into Steam's real store view, which is a separate CEF target from
 * the client window: the client renders the store by loading
 * store.steampowered.com in its own browser, so this is ordinary store HTML and
 * not React. That is why this exists rather than the store being patched
 * through React internals the way Decky and Millennium reach the client's own
 * UI. There is no fiber here to patch.
 *
 * Two modes:
 *
 *   off  the store is the store. Nothing is touched, so the Store tab, where
 *        the user may be about to spend money, behaves exactly as Valve
 *        shipped it. This is the state unless told otherwise.
 *   on   the store was opened as the LUAFlipper tab. Purchase blocks are
 *        presented as a 100% discount and Add to Cart installs a manifest
 *        instead of adding to the cart.
 *
 * The mode is pushed in by the module, never asked for, and the module only
 * says on while the Unlocker tab is holding its lease open. Nothing here can
 * turn itself on.
 *
 * Talking to the module: not over fetch. Store pages carry a CSP whose
 * connect-src lists Valve's hosts and Steam's own helper port and nothing else,
 * so a request to 127.0.0.1:1987 is refused before it leaves the page. The
 * module bridges instead over the debugger channel it already holds, which is
 * not a network request and so is not gated by CSP. window.__luaflipperBridge
 * is that channel; the module installs it.
 *
 * ES5 only, no build step. Steam's own store classes are reused throughout, so
 * the result inherits the store's look rather than approximating it.
 */
(function () {
  "use strict";

  var MARK = "luaflipper-hooked";

  if (typeof window.__luaflipperStoreCleanup === "function") {
    try { window.__luaflipperStoreCleanup(); } catch (e) {}
  }

  var active = false;      // last mode the module pushed
  var undo = [];           // restores the page when the mode goes off

  function text(v) { return (v === null || v === undefined) ? "" : String(v); }

  /* --------------------------------------------------------------- bridge --- */

  var pending = {}, seq = 0;

  // The module calls this with the reply to a request. Installed on window
  // because the module reaches it by name from outside the page.
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
    // An install downloads and unpacks, so the ceiling is generous; it exists
    // only so a lost reply cannot leave the button stuck on "Adding".
    setTimeout(function () {
      if (pending[id]) { delete pending[id]; cb(null); }
    }, 180000);
  }

  /* ---------------------------------------------------------------- store --- */

  /**
   * The app id a purchase block is for.
   *
   * Read from the block's own data-* first: a store page carries one block per
   * purchasable item, so the page-level id would tag every DLC row with the base
   * game and install the wrong thing.
   */
  function appIdFor(block) {
    var n = block;
    while (n && n !== document.body) {
      var v = n.getAttribute && (n.getAttribute("data-appid") ||
                                 n.getAttribute("data-ds-appid"));
      if (v) return text(v).split(",")[0];
      n = n.parentNode;
    }
    var page = document.querySelector("[data-appid]");
    return page ? text(page.getAttribute("data-appid")).split(",")[0] : "";
  }

  /**
   * Present the price as a 100% discount, using Steam's own discount markup.
   *
   * Built from the store's classes rather than styled by hand so it matches
   * whatever the store currently looks like, including a theme. The original
   * price is kept struck through rather than erased: the point is that this is
   * the paid product, obtained another way, and hiding the real price would just
   * make the row look like a free game.
   */
  function freePrice(block) {
    var price = block.querySelector(".game_purchase_price, .discount_block");
    if (!price || price.getAttribute("data-lf-price")) return;

    var original = text(price.textContent).replace(/\s+/g, " ").trim();
    if (!original) return;

    var wrap = document.createElement("div");
    wrap.className = "discount_block game_purchase_discount";
    wrap.setAttribute("data-lf-price", "1");

    var pct = document.createElement("div");
    pct.className = "discount_pct";
    pct.textContent = "-100%";

    var prices = document.createElement("div");
    prices.className = "discount_prices";

    var was = document.createElement("div");
    was.className = "discount_original_price";
    was.textContent = original;

    var now = document.createElement("div");
    now.className = "discount_final_price";
    now.textContent = "Free";

    prices.appendChild(was);
    prices.appendChild(now);
    wrap.appendChild(pct);
    wrap.appendChild(prices);

    var parent = price.parentNode;
    parent.insertBefore(wrap, price);
    price.style.display = "none";

    /*
     * Widen the price column until the struck-through price clears the badge.
     *
     * Steam positions that price absolutely, inset from the right of a column
     * whose width is decided by the final price below it. For a real discount
     * the final price is the longer string and everything fits; the moment the
     * final price is the word "Free" the original has nowhere to go and spills
     * left across the -100% badge.
     *
     * Measured, not computed. Widening by the difference between the two
     * strings is the obvious arithmetic and it lands a pixel and a half short,
     * because it knows nothing about the column's own left padding or the inset
     * the price sits at. Asking the layout where the two boxes actually ended up
     * needs neither number, so it holds for any currency, font and theme.
     *
     * Looped because a widening can land fractionally short of its own target,
     * and bounded because a layout that will not converge must not hang the page.
     */
    var GUTTER = 6;   // Steam's own inset, reused so the gap reads as deliberate
    var padLeft = parseFloat(window.getComputedStyle(now).paddingLeft) || 0;
    for (var pass = 0; pass < 3; pass++) {
      var over = pct.getBoundingClientRect().right -
                 was.getBoundingClientRect().left + GUTTER;
      if (over <= 0.5) break;          // already clear, or close enough to it
      padLeft += over;
      now.style.paddingLeft = padLeft + "px";
    }
    undo.push(function () {
      if (wrap.parentNode) wrap.parentNode.removeChild(wrap);
      price.style.display = "";
    });
  }

  /**
   * Send this block's Add to Cart to LUAFlipper.
   *
   * The listener is added in the capture phase and stops propagation, so the
   * store's own handler never runs and nothing reaches the real cart. The anchor
   * is left in place with its own classes: relabelling Steam's button is the
   * point, and replacing it would lose the store's styling.
   */
  function hookCart(block) {
    var a = block.querySelector("a.btn_green_steamui, a.btn_addtocart");
    if (!a || a.getAttribute("data-lf-hooked")) return;
    var appId = appIdFor(block);
    if (!appId) return;

    var label = a.querySelector("span") || a;
    var original = label.textContent;
    a.setAttribute("data-lf-hooked", "1");
    a.classList.add(MARK);

    var busy = false;
    function onClick(ev) {
      ev.preventDefault();
      ev.stopPropagation();
      ev.stopImmediatePropagation();
      if (busy) return;
      busy = true;

      var started = new Date().getTime();
      var tick = setInterval(function () {
        label.textContent = "Adding " +
          Math.round((new Date().getTime() - started) / 1000) + "s";
      }, 1000);
      label.textContent = "Adding 0s";

      function done(s) { clearInterval(tick); label.textContent = s; }
      function retry(s) { done(s); busy = false; }

      // The bridge hands back the reply as a value, not as text: the module
      // pushes it in as a JSON literal, so it arrives already parsed. Running
      // JSON.parse over an object stringifies it to "[object Object]" first and
      // then throws, which read back here as every source being unavailable.
      function parse(body) {
        if (!body) return null;
        if (typeof body === "object") return body;
        try { return JSON.parse(body); } catch (e) { return null; }
      }

      // Ask which sources carry it, then take the first that installs. Same
      // fallthrough the Unlocker page uses; sequential because the sources serve
      // the same pack and write the same files.
      call("/api/sources?appid=" + encodeURIComponent(appId), function (body) {
        var res = parse(body);
        if (!res) { retry("Unavailable"); a.title = "LUAFlipper unreachable"; return; }

        // "needs key" and "bad key" are Hubcap saying this machine cannot use
        // it, not that the app is missing. Neither is worth a request.
        var live = (res.sources || []).filter(function (s) {
          return s && s.name && s.status !== "unavailable" &&
                 s.status !== "needs key" && s.status !== "bad key" &&
                 s.status !== "needs sign-in";
        });
        if (!live.length) { retry("No source"); return; }

        function attempt(i) {
          if (i >= live.length) { retry("Unavailable"); return; }
          call("/api/install?appid=" + encodeURIComponent(appId) +
               "&source=" + encodeURIComponent(live[i].name), function (b) {
            var out = parse(b);
            if (out && out.ok) {
              done("In library");
              a.title = "Installed " + (out.installed || 0) +
                        " files. Restart Steam to see it in your library.";
              return;
            }
            attempt(i + 1);
          });
        }
        attempt(0);
      });
    }

    a.addEventListener("click", onClick, true);
    label.textContent = "Add to LUAFlipper";
    undo.push(function () {
      a.removeEventListener("click", onClick, true);
      a.removeAttribute("data-lf-hooked");
      a.classList.remove(MARK);
      label.textContent = original;
      a.title = "";
    });
  }

  function apply() {
    if (!active || !document.body) return;
    var blocks = document.querySelectorAll(".game_purchase_action");
    for (var i = 0; i < blocks.length; i++) {
      freePrice(blocks[i]);
      hookCart(blocks[i]);
    }
  }

  function revert() {
    // Run backwards: later entries were layered on earlier ones.
    while (undo.length) {
      try { undo.pop()(); } catch (e) {}
    }
  }

  // Called by the module after it sets window.__luaflipperMode.
  window.__luaflipperSetMode = function () {
    var on = !!window.__luaflipperMode;
    if (on === active) { if (on) apply(); return; }
    active = on;
    if (on) apply(); else revert();
  };

  var obs = null;

  function start() {
    if (!document.body || obs) return;
    // The store swaps purchase blocks in on navigation and lazy DLC lists.
    obs = new MutationObserver(apply);
    obs.observe(document.body, { childList: true, subtree: true });
    window.__luaflipperSetMode();
  }

  // This runs at document-start on every navigation, where there is no body
  // yet, as well as on an already-loaded page.
  if (document.body) start();
  else document.addEventListener("DOMContentLoaded", start);

  window.__luaflipperStoreCleanup = function () {
    if (obs) { obs.disconnect(); obs = null; }
    document.removeEventListener("DOMContentLoaded", start);
    revert();
    window.__luaflipperStoreCleanup = null;
  };
})();
