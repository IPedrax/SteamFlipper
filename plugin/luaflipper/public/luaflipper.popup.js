/*
 * LUAFlipper, the nav dropdown as a real Steam window.
 *
 * This runs in SharedJSContext, which is where Steam keeps g_PopupManager and
 * therefore the only place a popup can be made. The tab that opens the menu is
 * in a different CEF target, so the module brokers: the client window asks it
 * to show a menu, it calls in here, and a click in here goes back out the same
 * way. Neither side can reach the other directly.
 *
 * Why a window at all. Store, Community and the profile are browser views, a
 * native surface composited over the client window's DOM, and it wins against
 * anything in that DOM whatever the stacking order says. Steam has the same
 * problem with its own nav menus and answers it by making each one a top-level
 * window; this is that answer. Nothing has to be hidden, moved or repainted.
 *
 * Steam will not let page script open a window: window.open returns null here
 * and in the client window, under a real click, with or without
 * NotifyUserActivation. Its own popup class goes through the client's native
 * path instead, so the class is borrowed from a popup Steam already made. That
 * is an internal, and the module falls back to its in-page menu when any of it
 * is missing rather than leaving the user with no menu at all.
 *
 * ES5, no build step, same as the rest of the injected UI.
 */
(function () {
  "use strict";

  var NAME = "luaflipper_menu";
  var API = "http://127.0.0.1:1987/api/";

  if (window.__luaflipperMenu && window.__luaflipperMenu.destroy) {
    try { window.__luaflipperMenu.destroy(); } catch (e) {}
  }

  var popup = null;      // the CPopup we own, kept between opens
  var items = [];        // [{page, label}], whatever the client window sent

  function manager() { return window.g_PopupManager || null; }

  /**
   * A popup to copy creation parameters from.
   *
   * Those params carry the window flags that make a menu a menu: no title bar,
   * hide rather than close, which browser owns it. They are not documented
   * anywhere, so they are taken from one Steam made rather than invented, and
   * only the dimensions are overridden.
   */
  function template() {
    var pm = manager();
    if (!pm || !pm.m_mapPopups) return null;
    var found = null;
    pm.m_mapPopups.forEach(function (p, name) {
      if (!found && String(name).indexOf("contextmenu_") === 0) found = p;
    });
    return found;
  }

  function available() {
    var t = template();
    return !!(t && t.constructor && t.m_rgParams);
  }

  /**
   * Screen position of the client window, for turning its coords into ours.
   *
   * Read synchronously off the window. SteamClient.Window.GetWindowDimensions
   * would answer the same thing and be the tidier source, but it hands back a
   * promise that did not settle here, and everything downstream of it -- the
   * whole popup -- silently never happened. Nothing on this path is allowed to
   * be asynchronous for that reason.
   */
  function clientOrigin() {
    var pm = manager();
    var main = pm && pm.GetExistingPopup ? pm.GetExistingPopup("SP Desktop_uid0") : null;
    try {
      if (main && main.m_popup) {
        return { x: main.m_popup.screenX || 0, y: main.m_popup.screenY || 0 };
      }
    } catch (e) {}
    return { x: 0, y: 0 };
  }

  /* ---------------------------------------------------------------- draw --- */

  var ROW = 32;          // one item, matching Steam's own menu rows
  var PAD = 8;           // the strip above and below them

  function height() { return items.length * ROW + PAD * 2; }

  function paint(win) {
    var d = win.document;
    d.body.style.margin = "0";
    d.body.style.overflow = "hidden";
    d.body.style.background = "#3d4450";      // Steam's menu surface
    d.body.style.userSelect = "none";

    var html = '<div style="font-family:\'Motiva Sans\',Arial,sans-serif;' +
               'font-size:14px;color:#dcdedf;padding:' + PAD + 'px 0">';
    for (var i = 0; i < items.length; i++) {
      html += '<div data-page="' + items[i].page + '" ' +
              'style="padding:0 18px;line-height:' + ROW + 'px;cursor:pointer">' +
              items[i].label + '</div>';
    }
    d.body.innerHTML = html + "</div>";

    /*
     * The pointer crossing into this window has to reach the tab that opened
     * it, and cannot get there on its own.
     *
     * The tab closes the menu shortly after the pointer leaves it, which is
     * right for an in-page menu because the menu is under the same pointer.
     * A separate window is not: leaving the tab means leaving it towards this
     * window, and the tab sees only the leaving. So this says when the pointer
     * arrives, in time to cancel that close, and when it leaves for real.
     */
    d.addEventListener("mouseenter", function () {
      try { fetch(API + "navmenu/keep"); } catch (e) {}
    }, true);
    d.addEventListener("mouseleave", function () {
      try { fetch(API + "navmenu/close"); } catch (e) {}
    }, true);

    // Hover and click are wired here rather than with inline handlers, which
    // this document's CSP would refuse.
    var rows = d.body.querySelectorAll("[data-page]");
    for (var j = 0; j < rows.length; j++) {
      (function (row) {
        row.addEventListener("mouseenter", function () {
          row.style.background = "rgba(255,255,255,0.10)";
        });
        row.addEventListener("mouseleave", function () {
          row.style.background = "";
        });
        row.addEventListener("click", function () {
          hide();
          // Out through the module, which is the only way back to the window
          // that owns the tab.
          try {
            fetch(API + "navmenu/pick?page=" +
                  encodeURIComponent(row.getAttribute("data-page")));
          } catch (e) {}
        });
      })(rows[j]);
    }
  }

  /* -------------------------------------------------------------- window --- */

  function build() {
    var t = template();
    if (!t) return null;
    var params;
    try { params = JSON.parse(JSON.stringify(t.m_rgParams || {})); }
    catch (e) { params = {}; }
    params.dimensions = { width: 220, height: height(), left: 0, top: 0 };
    try {
      return new t.constructor(NAME, params, function () { return true; });
    } catch (e) {
      return null;
    }
  }

  function hide() {
    if (!popup) return;
    try { if (popup.BIsValid()) popup.m_popup.SteamClient.Window.HideWindow(); }
    catch (e) {}
  }

  function show(x, y, list) {
    if (!available()) return false;
    items = list && list.length ? list : items;
    if (!items.length) return false;

    // Rebuilt when the previous one has gone: a popup that was closed rather
    // than hidden cannot be shown again, and Steam closes them on shutdown of
    // the browser that owned them.
    var alive = false;
    try { alive = !!(popup && popup.BIsValid() && !popup.BIsClosed()); } catch (e) {}
    if (!alive) popup = build();
    if (!popup) return false;

    // Show first: the window and its document do not exist until it is shown,
    // so painting or moving before this has nothing to act on.
    try { popup.Show(); } catch (e) { return false; }
    if (!popup.m_popup) return false;

    var origin = clientOrigin();
    try {
      var sc = popup.m_popup.SteamClient;
      sc.Window.ResizeTo(220, height(), true);
      sc.Window.MoveTo(origin.x + x, origin.y + y, true);
    } catch (e) {}
    // Focus is what keeps it up: these windows hide themselves on blur, which
    // is also how the menu closes when the user looks somewhere else.
    try { popup.Focus(); } catch (e) {}
    try { paint(popup.m_popup); } catch (e) { return false; }
    return true;
  }

  window.__luaflipperMenu = {
    available: available,
    show: show,
    hide: hide,
    destroy: function () {
      try { if (popup) popup.Close(); } catch (e) {}
      popup = null;
      window.__luaflipperMenu = null;
    }
  };
})();
