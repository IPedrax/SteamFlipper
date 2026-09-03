/*
 * LUAFlipper, the SteamFlipper client UI.
 *
 * Adds a nav tab next to the account name. Clicking it opens a dropdown, and
 * each entry opens a page that takes over the content area the same way Store
 * and Library do, rather than floating a modal over them.
 *
 * Steam ships this UI as a React app with hashed CSS class names that change
 * between client builds, so nothing here matches on a hashed class. What it
 * does rely on is the unhashed half of those class names, which has been stable
 * across builds:
 *
 *   SuperNavBar / MenuButton     the nav and its buttons
 *   ContentFrame                 the region below the nav
 *   LocalContentContainer        the routed page inside it
 *
 * Theme compatibility: custom themes restyle Steam by overriding its own
 * classes, so this deliberately avoids a private palette. Backgrounds are
 * inherited from the Steam elements we sit inside, accents are alpha over
 * whatever that resolves to, and the floating dropdown copies its background
 * from the live nav bar. A theme that restyles Steam restyles this too.
 * Everything is under .LUAFlipperPage / .luaflipper-* so themes can target it.
 *
 * React re-renders the nav (navigating, going online/offline, resizing), which
 * drops anything we appended. A MutationObserver puts it back.
 */
(function () {
  "use strict";

  var TAB_ID = "luaflipper-tab";
  var MENU_ID = "luaflipper-menu";
  var PAGE_ID = "luaflipper-page";

  // Backend served by SteamFlipper on localhost. Nothing else is contacted.
  var API = "http://127.0.0.1:1987/api/";

  // Text of the stock nav buttons, used to recognise the nav container.
  var NAV_WORDS = ["store", "library", "community"];

  // Dropdown entries, in order. `page` is both the panel id and the API path.
  // Unlocker is where manifests are found and added; Manage is what is already
  // installed. Modelled on LuaTools, which splits the same way (Download vs
  // Manage) because the two answer different questions.
  var PAGES = [
    { page: "unlocker", label: "Unlocker" },
    { page: "manage",   label: "Manage" },
    { page: "fixes",    label: "Fixes" },
    { page: "config",   label: "Config" }
  ];

  function log() {
    var a = ["[LUAFlipper]"].concat([].slice.call(arguments));
    try { console.log.apply(console, a); } catch (e) {}
  }
  log("script loaded", location.href);

  // Evict a previous instance. The injector re-runs this script whenever it
  // reconnects or the page is replaced, and an old copy's MutationObserver
  // would otherwise survive and keep re-creating its own tab, bound to its own
  // (stale) handlers. Because that tab carries the id this script checks for,
  // the new copy would then skip injection and the old code would stay live.
  if (typeof window.__luaflipperCleanup === "function") {
    try { window.__luaflipperCleanup(); } catch (e) {}
  }

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  /* ---------------------------------------------------------------- nav --- */

  /**
   * Locate the nav. Returns {container, wrapper, sample}: an existing button to
   * copy, the per-button wrapper around it, and the row both sit in.
   */
  function findNav() {
    // Steam builds the nav from plain <div>s carrying two classes: a hashed one
    // that changes per build, and a stable "MenuButton". Neither is an <a> nor
    // carries role="button", so those selectors find nothing here; MenuButton is
    // the part worth matching on. The rest is kept as a fallback in case a
    // future build goes back to real buttons.
    var anchors = document.querySelectorAll(
      "div.MenuButton, a, div[role='button'], button");
    var sample = null;
    for (var i = 0; i < anchors.length && !sample; i++) {
      var t = (anchors[i].textContent || "").trim().toLowerCase();
      if (NAV_WORDS.indexOf(t) !== -1) sample = anchors[i];
    }
    if (!sample) return null;

    // Each button sits in its own MenuWrapper, and that wrapper is what carries
    // the nav's font size, weight and uppercase transform - the button inherits
    // them. Appending a bare button to the shared row therefore inherits none of
    // it and renders visibly smaller and lighter than the stock tabs, even with
    // an identical className. So clone the wrapper, not the button.
    var wrapper = sample.closest ? sample.closest(".MenuWrapper") : null;
    if (!wrapper) wrapper = sample.parentElement;
    if (!wrapper || !wrapper.parentElement) return null;

    return { container: wrapper.parentElement, wrapper: wrapper, sample: sample };
  }

  function injectTab() {
    if (document.getElementById(TAB_ID)) return true;
    var nav = findNav();
    if (!nav) return false;

    // A structural copy, so the tab picks up whatever markup and classes this
    // build wraps its nav buttons in without us having to know any of them.
    var tab = nav.wrapper.cloneNode(true);
    tab.id = TAB_ID;                        // on the wrapper: removing it removes all of it
    tab.removeAttribute("aria-label");
    // Never inherit the highlight from whichever wrapper happened to be cloned,
    // then record what is left as the pristine base for takeHighlight().
    selectedClasses().forEach(function (c) { tab.classList.remove(c); });
    tab.setAttribute("data-lf-base", tab.className);

    var label = tab.querySelector(".MenuButton") || tab.firstElementChild || tab;
    label.textContent = "LUAFlipper";
    label.removeAttribute("id");
    tab.style.cursor = "pointer";

    // Steam's nav menus open on hover, so this one does too. Click still works
    // as a toggle, for anyone who clicks before the hover registers.
    tab.addEventListener("mouseenter", function () {
      cancelClose();
      openMenu(tab);
    });
    tab.addEventListener("mouseleave", scheduleClose);
    // Clicking the tab opens the default page, the way clicking STORE or
    // LIBRARY goes straight to that section; the dropdown is for jumping
    // directly to one of the others.
    tab.addEventListener("click", function (ev) {
      ev.preventDefault();
      ev.stopPropagation();
      closeMenu();
      openPage(PAGES[0].page);
    });

    nav.container.appendChild(tab);

    // Navigating away via a stock nav button must restore Steam's own content,
    // exactly as switching between Store and Library does. Except when the
    // click is ours: opening the Unlocker means clicking Store on the user's
    // behalf, and that lands here looking exactly like leaving.
    nav.container.addEventListener("click", function (ev) {
      if (selfNav) return;
      if (ev.target !== tab && !tab.contains(ev.target)) closePage();
    }, true);

    // React re-renders the nav and this runs again to restore the tab; if a
    // page is open, its highlight has to come back with it.
    if (currentPage) takeHighlight();

    log("nav tab injected");
    return true;
  }

  /* ----------------------------------------------------------- dropdown --- */

  /**
   * Get the web view out from under the menu, and put it back.
   *
   * Store, Community and the profile are not HTML in this window: Steam renders
   * them in a browser view, a native surface composited over the page. It wins
   * against any DOM regardless of z-index, and the dropdown is already at
   * 100000 and position:fixed, so the menu was drawing correctly and being
   * painted over. Steam has the same problem with its own nav menus and solves
   * it by making each one a separate popup window; that machinery lives in
   * SharedJSContext, a different CEF target, and is not reachable from a script
   * injected here.
   *
   * So the view stands aside while the menu is open. The page under it blanks
   * to Steam's own background for as long as the pointer is on the menu, which
   * is the moment someone is leaving that page anyway.
   *
   * The previous inline value is restored rather than cleared, for the reason
   * hideStock has to: Steam drives these views by writing display straight onto
   * them, and blanking that un-hides whichever one it had hidden.
   */
  var hiddenView = null;

  function standAside(px) {
    if (hiddenView) return;
    // Moved down, not hidden. Hiding it blanked the page, which is a worse
    // trade than the overlap it was fixing: the point of those pages is that
    // they are showing something. The native surface follows its layout box,
    // so offsetting the box slides the page down and leaves the strip the menu
    // needs to whatever paints behind it.
    //
    // .Browser, not the .BrowserWrapper around it: the wrapper holds the
    // address bar, which should not move, and it is the node openPage hides.
    // Two pieces of code writing to one node is how the view got stuck hidden
    // for a whole session.
    var views = document.querySelectorAll(".BrowserWrapper .Browser");
    for (var i = 0; i < views.length; i++) {
      if (views[i].style.display === "none") continue;      // Steam already hid it
      if (!views[i].getBoundingClientRect().height) continue;
      // The strip the page vacates is painted by an ancestor that is plain
      // black, so an offset alone reads as a black bar across the window. The
      // address bar directly above it is the right neighbour to borrow from:
      // it is the chrome this strip is visually part of, and reading its
      // computed colour rather than naming one keeps a theme authoritative.
      var wrap = views[i].parentElement;
      var bar = wrap ? wrap.querySelector(".URLBar") : null;
      var paint = bar ? window.getComputedStyle(bar).backgroundColor : "";
      if (!paint || paint === "rgba(0, 0, 0, 0)" || paint === "transparent") {
        paint = "";                       // leave it alone rather than guess
      }

      hiddenView = {
        el: views[i], margin: views[i].style.marginTop,
        wrap: paint ? wrap : null,
        wrapBg: wrap ? wrap.style.background : ""
      };
      views[i].style.marginTop = px + "px";
      if (paint) wrap.style.background = paint;
      return;
    }
  }

  function comeBack() {
    if (!hiddenView) return;
    hiddenView.el.style.marginTop = hiddenView.margin;
    if (hiddenView.wrap) hiddenView.wrap.style.background = hiddenView.wrapBg;
    hiddenView = null;
  }

  function closeMenu() {
    cancelClose();
    var m = document.getElementById(MENU_ID);
    if (m) m.remove();
    // Always, with no exception for one of our pages being open. Leaving it
    // hidden was the other half of the stuck view: openPage hides the wrapper,
    // so a .Browser left hidden underneath survives every restore and the page
    // comes back blank.
    comeBack();
    document.removeEventListener("mousedown", onOutside, true);
    document.removeEventListener("keydown", onEsc, true);
  }

  function onOutside(ev) {
    var m = document.getElementById(MENU_ID);
    if (m && !m.contains(ev.target) && ev.target.id !== TAB_ID) closeMenu();
  }

  function onEsc(ev) { if (ev.key === "Escape") closeMenu(); }

  /**
   * Class names to build the dropdown from.
   *
   * Steam's menu styling lives on per-build hashed class names, not on the
   * readable aliases beside them, so wearing the aliases alone gets no styling
   * at all. The hashed names cannot be hardcoded either, since they change with
   * every client build. The injector therefore reads them off a live Steam menu
   * and hands them over in window.__luaflipperMenuClasses; this document's
   * stylesheets already contain the matching rules.
   *
   * Falls back to the readable aliases plus our own stylesheet when the
   * injector could not supply them.
   */
  function steamMenuClasses() {
    var k = window.__luaflipperMenuClasses;
    if (k && k.menu && k.item) {
      return { menu: k.menu, contents: k.contents || "", item: k.item, styled: true };
    }
    return {
      menu: "contextMenu visible ContextMenuPosition",
      contents: "contextMenuContents MenuPopup",
      item: "MenuItem Item contextMenuItem",
      styled: false
    };
  }

  /**
   * Build the dropdown using Steam's own menu class names.
   *
   * Steam renders its real menus in separate CEF windows, so their stylesheets
   * are not in this document and the hashed classes that carry the styling
   * cannot be reused here. What can be reused are the readable class names
   * Steam puts alongside them (contextMenu / MenuPopup / contextMenuItem),
   * because those are exactly what custom themes target. The stylesheet then
   * supplies defaults measured from the live Account Menu, so it matches the
   * stock skin and still gets restyled by a theme.
   */
  function openMenu(tab) {
    // A close scheduled by a previous mouseleave must not fire onto the menu
    // we are about to open.
    cancelClose();
    if (document.getElementById(MENU_ID)) return;

    // Wear Steam's own classes when the injector could supply them (see
    // steamMenuClasses). The readable aliases carry no styling of their own -
    // .contextMenu, .MenuItem and .MenuPopup match zero rules - so it is the
    // hashed names that make this a real Steam menu, and a theme editing
    // Steam's menus edits this one in the same stroke. `luaflipper-*` stays on
    // as the fallback styling and as a stable handle.
    var K = steamMenuClasses();
    var menu = el("div", K.menu + " luaflipper-menu" + (K.styled ? " is-steam-styled" : ""));
    menu.id = MENU_ID;

    var contents = el("div", K.contents + " luaflipper-menu-contents");

    PAGES.forEach(function (p) {
      // The dropdown is the only page switcher, so it has to show which page is
      // currently open; there is no in-page tab strip left to carry that.
      var item = el("div",
        K.item + " luaflipper-menu-item" +
        (p.page === currentPage ? " active" : ""), p.label);
      item.setAttribute("role", "menuitem");
      item.addEventListener("click", function () {
        closeMenu();
        openPage(p.page);
      });
      contents.appendChild(item);
    });
    menu.appendChild(contents);

    // Anchored under the tab. Fixed positioning keeps it correct regardless of
    // where the nav sits in the scroll/transform tree.
    var r = tab.getBoundingClientRect();
    menu.style.top = (r.bottom + 2) + "px";
    menu.style.left = Math.max(4, Math.min(r.left, window.innerWidth - 240)) + "px";

    // Moving the pointer from the tab into the menu passes over a gap; keep the
    // menu open while the pointer is inside it.
    menu.addEventListener("mouseenter", cancelClose);
    menu.addEventListener("mouseleave", scheduleClose);

    document.body.appendChild(menu);
    // After mounting, because the offset has to be the menu's real height and
    // that is not known until it has been laid out.
    standAside(Math.ceil(menu.getBoundingClientRect().height) + 4);
    document.addEventListener("mousedown", onOutside, true);
    document.addEventListener("keydown", onEsc, true);
  }

  // Hover open/close. The close is delayed so the pointer can cross the gap
  // between the tab and the menu without the menu vanishing underneath it.
  var closeTimer = null;

  function cancelClose() {
    if (closeTimer) { clearTimeout(closeTimer); closeTimer = null; }
  }

  function scheduleClose() {
    cancelClose();
    closeTimer = setTimeout(closeMenu, 220);
  }

  /* --------------------------------------------------------------- page --- */

  // Which page is open, or null. Drives the dropdown's active marker.
  var currentPage = null;
  // The stock nav wrapper we took the selected highlight away from, so it can
  // be handed back when our page closes.
  var stolenFrom = null;

  /**
   * The class names Steam puts on the selected nav wrapper.
   *
   * The highlight lives on the MenuWrapper, not the button, and is carried by a
   * per-build hashed class next to the readable "Selected". Derive both by
   * diffing a currently selected wrapper against a plain one, so this keeps
   * working when the hash changes.
   */
  var selectedCache = null;

  function selectedClasses() {
    // Cache it: once our page is open nothing carries .Selected any more, and
    // recomputing then would lose the hashed half and leave it applied forever.
    if (selectedCache) return selectedCache;

    var sel = document.querySelector(".MenuWrapper.Selected");
    if (!sel || !sel.parentElement) return ["Selected"];

    // Compare against a SIBLING wrapper. Matching any .MenuWrapper in the
    // document picks up the title bar's menus (View / Friends / Games / Help),
    // which are a different component with a different base class list; the
    // diff then includes styling classes such as SuperNavMenu, and stripping
    // those off the previously selected tab visibly breaks it.
    var plain = null, sibs = sel.parentElement.children;
    for (var i = 0; i < sibs.length && !plain; i++) {
      if (sibs[i] !== sel && sibs[i].classList &&
          sibs[i].classList.contains("MenuWrapper") &&
          !sibs[i].classList.contains("Selected")) plain = sibs[i];
    }
    if (!plain) return ["Selected"];

    var base = (plain.className || "").split(/\s+/);
    var out = (sel.className || "").split(/\s+/).filter(function (c) {
      return c && base.indexOf(c) === -1;
    });
    if (!out.length) return ["Selected"];
    selectedCache = out;
    return out;
  }

  /**
   * The tab's class list with no selected state, recorded when it was cloned.
   *
   * Kept on the element rather than recomputed: add/remove bookkeeping across
   * React re-renders lost the wrapper's styling classes (SuperNavMenu among
   * them), which silently dropped the tab to 16px grey. Composing the whole
   * className from a known-good base each time cannot drift that way.
   */
  function tabBase(tab) {
    return tab.getAttribute("data-lf-base") || tab.className;
  }

  /** Move the nav highlight onto our tab, remembering where it came from. */
  function takeHighlight() {
    var tab = document.getElementById(TAB_ID);
    if (!tab) return;
    var classes = selectedClasses();
    // Steam's router still considers Library (or whichever) current, so its
    // wrapper keeps the highlight. Two blue tabs at once reads as a bug.
    var sel = document.querySelector(".MenuWrapper.Selected");
    if (sel && sel !== tab) {
      stolenFrom = { el: sel, className: sel.className };
      classes.forEach(function (c) { sel.classList.remove(c); });
    }
    tab.className = tabBase(tab) + " " + classes.join(" ");
  }

  /**
   * Whether our tab is the one currently lit.
   *
   * This is what "the store is showing as the Unlocker tab" actually means, so
   * it is what the mode lease is renewed against. Checked on the live element,
   * not on a remembered flag: React re-renders the nav and the observer rebuilds
   * the tab, and if either ever left us un-highlighted the user is looking at
   * the real store and the lease has to lapse.
   */
  function hasHighlight() {
    var tab = document.getElementById(TAB_ID);
    if (!tab) return false;
    var classes = selectedClasses();
    for (var i = 0; i < classes.length; i++) {
      if (!tab.classList.contains(classes[i])) return false;
    }
    return classes.length > 0;
  }

  /** Put the highlight back where Steam had it. */
  function releaseHighlight() {
    var tab = document.getElementById(TAB_ID);
    if (tab) tab.className = tabBase(tab);
    if (stolenFrom) {
      // Restore verbatim, rather than re-adding a computed class list.
      stolenFrom.el.className = stolenFrom.className;
      stolenFrom = null;
    }
  }

  /**
   * Take over the content area, and give it back exactly as it was.
   *
   * Steam switches its own views by writing display straight onto the element:
   * LocalContentContainer holds the routed React pages, BrowserWrapper holds the
   * web views, and whichever is not current carries an inline display:none. Both
   * stay mounted either way.
   *
   * So restoring means putting back the value that was there, not blanking it.
   * Blanking un-hides whatever Steam had hidden, which is what left a strip of a
   * stale page sitting above the open tab: the store route keeps the last
   * library render mounted underneath, and clearing the inline none showed it.
   */
  var STOCK_VIEWS = ".LocalContentContainer, .BrowserWrapper";
  var hiddenStock = [];

  function hideStock() {
    restoreStock();
    var els = document.querySelectorAll(STOCK_VIEWS);
    for (var i = 0; i < els.length; i++) {
      hiddenStock.push({ el: els[i], display: els[i].style.display });
      els[i].style.display = "none";
    }
  }

  function restoreStock() {
    for (var i = 0; i < hiddenStock.length; i++) {
      hiddenStock[i].el.style.display = hiddenStock[i].display;
    }
    hiddenStock = [];
  }

  /**
   * Tell the module whether the store is being shown as our tab.
   *
   * Fire and forget with an optional continuation: the click that follows must
   * not wait on the network, or the store would navigate before the flag lands
   * and the first paint would show real prices.
   */
  function setUnlockerMode(on, then) {
    var done = false;
    function go() { if (!done) { done = true; if (then) then(); } }
    try {
      fetch(API + "unlocker/mode?set=" + (on ? "1" : "0"))
        .then(go).catch(go);
    } catch (e) { go(); }
    // Never let a stalled request strand the navigation.
    setTimeout(go, 400);
  }

  /**
   * Hold the mode open for as long as the store is genuinely our tab.
   *
   * The module treats the flag as a lease that expires, so the claim has to be
   * renewed. That is the point: it means the only thing keeping the store
   * rewritten is this tab still being the open one. Anything that ends the tab
   * without a clean close, a nav we did not see, a re-render that loses us, this
   * script going away, stops the renewals and the real store comes back by
   * itself instead of staying hooked.
   */
  var holdTimer = null;

  // Set while we drive a stock nav button ourselves, so the listener that
  // watches for the user leaving does not mistake our own click for theirs.
  var selfNav = false;

  function holdUnlockerMode(on) {
    if (holdTimer) { clearInterval(holdTimer); holdTimer = null; }
    if (!on) { setUnlockerMode(false, null); return; }
    holdTimer = setInterval(function () {
      // Renew only while we are still the open tab and still highlighted; if
      // Steam moved the highlight elsewhere, the user is on the real store.
      if (currentPage === "unlocker" && hasHighlight()) setUnlockerMode(true, null);
      else holdUnlockerMode(false);
    }, 2000);
  }

  function closePage() {
    var p = document.getElementById(PAGE_ID);
    if (p) p.remove();
    restoreStock();
    // Leaving our tab must put the store back, whether or not it was ours.
    if (currentPage === "unlocker") holdUnlockerMode(false);
    currentPage = null;
    releaseHighlight();
  }

  /**
   * Open a page in the content area, the way a stock tab does: Steam's routed
   * content is hidden rather than destroyed, and ours becomes its sibling
   * inside ContentFrame, so it inherits that frame's themed background.
   */
  function openPage(page) {
    var frame = document.querySelector(".ContentFrame");
    if (!frame) { log("ContentFrame not found; cannot open page"); return; }

    closePage();

    // Unlocker is Steam's own store, not a page of ours. Flip the shared mode
    // flag, then click the stock Store button so the client does its normal
    // navigation: reimplementing that routing would mean owning a browser view
    // and a history stack we have no reason to own. The store target's script
    // reads the flag and presents prices as free with Add to Cart rerouted.
    if (page === "unlocker") {
      currentPage = page;
      takeHighlight();          // keep our tab lit, not Store's
      setUnlockerMode(true, function () {
        var b = document.querySelectorAll(".SuperNavBar .MenuButton");
        selfNav = true;
        try {
          for (var i = 0; i < b.length; i++) {
            if (b[i].textContent.trim().toLowerCase() === "store") { b[i].click(); break; }
          }
        } finally {
          selfNav = false;
        }
        // Steam moves the highlight to Store as it routes, so take it back once
        // that has settled.
        setTimeout(function () {
          if (currentPage === "unlocker") takeHighlight();
        }, 150);
      });
      holdUnlockerMode(true);
      return;
    }

    // Both of Steam's views, not just the routed one: on the store route the
    // browser view is what is showing, and hiding only the React container
    // would leave the store visible under our page.
    hideStock();
    currentPage = page;
    takeHighlight();

    var root = el("div", "LUAFlipperPage luaflipper-page");
    root.id = PAGE_ID;

    // No page title bar. Page switching lives in the nav dropdown, and the tab
    // is already highlighted, so a heading repeating the page name only ate
    // vertical space above the content.
    var body = el("div", "luaflipper-body");
    body.appendChild(el("div", "luaflipper-loading", "Loading…"));

    root.appendChild(body);
    frame.appendChild(root);

    loadPage(body, page);
  }

  /**
   * Call the SteamFlipper backend. Guarded so the page still renders an error
   * rather than staying blank when the backend is not running.
   */
  function callBackend(method) {
    if (typeof fetch !== "function") {
      return Promise.reject(new Error("fetch unavailable"));
    }
    return fetch(API + method, { method: "GET" }).then(function (r) {
      if (!r.ok) throw new Error("backend returned HTTP " + r.status);
      return r.json();
    });
  }

  function loadPage(body, page) {
    // Config asks for its own reply here and fetches /api/cloud and /api/status
    // itself, when the section that needs them is opened.
    var method = { unlocker: "unlocker", manage: "manifests",
                   // The catalog, not /api/fixes: the page draws the games a
                   // published fix exists for, and asks for one game's fix list
                   // only when that game is opened.
                   fixes: "fixes/catalog", config: "config" }[page];

    callBackend(method).then(function (data) {
      body.innerHTML = "";
      body.appendChild(renderPage(page, data));
    }).catch(function (err) {
      body.innerHTML = "";
      var box = el("div", "luaflipper-error");
      box.appendChild(el("div", null,
        "Backend unavailable: " + (err && err.message ? err.message : String(err))));
      box.appendChild(el("div", "luaflipper-sub",
        "SteamFlipper serves this on 127.0.0.1:1987. The UI is loaded; the " +
        "backend for this page is not wired up yet."));
      body.appendChild(box);
    });
  }

  function renderPage(page, data) {
    // luaflipper.pages.js owns the per-page rendering. It is loaded alongside
    // this file; the inline fallback below only runs if it is missing, so a
    // partial install still shows something rather than a blank panel.
    var pages = window.LUAFlipperPages;
    if (pages && typeof pages[page] === "function") {
      try { return pages[page](data, el); }
      catch (e) { return el("div", "luaflipper-error", "Page failed: " + e.message); }
    }

    var wrap = el("div");
    if (data && data.error) {
      wrap.appendChild(el("div", "luaflipper-error", data.error));
      return wrap;
    }

    if (page === "unlocker") {
      var list = (data && data.manifests) || [];
      wrap.appendChild(el("div", "luaflipper-sub",
        list.length + " manifest" + (list.length === 1 ? "" : "s") +
        " in config/stplug-in"));
      if (!list.length) {
        wrap.appendChild(el("div", "luaflipper-empty",
          "No manifests yet. Drop .lua files into config/stplug-in."));
      }
      list.forEach(function (m) {
        var row = el("div", "luaflipper-row");
        row.appendChild(el("span", "luaflipper-appid", String(m.appid || "?")));
        row.appendChild(el("span", "luaflipper-name", m.name || m.file || ""));
        row.appendChild(el("span", "luaflipper-meta",
          (m.keys || 0) + " key" + (m.keys === 1 ? "" : "s")));
        wrap.appendChild(row);
      });
    } else if (page === "config") {
      // Same {rows:[{label,value}]} shape the status endpoint used to answer
      // with; this is the fallback for a partial install, so it draws the
      // reply the page was loaded with and nothing else.
      ((data && data.rows) || []).forEach(function (r) {
        var row = el("div", "luaflipper-row");
        row.appendChild(el("span", "luaflipper-name", r.label));
        row.appendChild(el("span", "luaflipper-meta", String(r.value)));
        wrap.appendChild(row);
      });
    } else {
      wrap.appendChild(el("div", "luaflipper-empty", "Not implemented yet."));
    }
    return wrap;
  }

  /* --------------------------------------------------------------- boot --- */

  function boot() {
    injectTab();
    // Keep observing after the first success: React re-renders the nav on
    // navigation and our tab goes with it.
    var obs = new MutationObserver(function () { injectTab(); });
    obs.observe(document.body, { childList: true, subtree: true });

    window.__luaflipperCleanup = function () {
      obs.disconnect();
      closeMenu();
      closePage();
      var tab = document.getElementById(TAB_ID);
      if (tab) tab.remove();
      window.__luaflipperCleanup = null;
    };
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
