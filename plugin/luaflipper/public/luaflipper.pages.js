/*
 * LUAFlipper page renderers.
 *
 * Split out of luaflipper.js so the backend response shapes live in one place:
 * luaflipper.js owns the nav, the dropdown and the fetch, and hands the parsed
 * response to the renderer named after the page.
 *
 * Each renderer takes (data, el) and returns exactly one element for the caller
 * to append. `el(tag, cls, text)` is luaflipper.js's element helper, passed in
 * rather than duplicated. Renderers build detached nodes only, so a response
 * they cannot make sense of cannot leave half a page in the document.
 *
 * ES5 only and no build step: this is injected into Steam's CEF frontend as
 * source. Styling is limited to the class names luaflipper.css already defines,
 * because a private stylesheet would ignore custom Steam themes and look wrong
 * on any non-stock skin.
 */
(function () {
  "use strict";

  /* ------------------------------------------------------------- shared --- */

  // Counts arrive as undefined from a partial or older backend. Treating those
  // as 0 keeps the row rendering instead of printing "undefined keys".
  function num(v) {
    var n = Number(v);
    return n > 0 ? n : 0;
  }

  // textContent of undefined would render the word "undefined" to the user.
  function text(v) {
    return (v === null || v === undefined) ? "" : String(v);
  }

  // A response of the wrong shape must not throw here. The caller catches into
  // its "backend unavailable" box, which would blame the backend for being
  // unreachable when it answered, just not with a list.
  function arr(v) {
    return Array.isArray(v) ? v : [];
  }

  // "fix" pluralises to "fixes", so the plural form is overridable.
  function plural(n, one, many) {
    return n + " " + (n === 1 ? one : (many || one + "s"));
  }

  /**
   * One list row of up to three columns. Blank columns are skipped rather than
   * appended empty, so a response missing a field still lines up with its
   * neighbours instead of leaving a gap where the value should be.
   */
  function row(el, tag, name, meta) {
    var r = el("div", "luaflipper-row");
    if (tag) r.appendChild(el("span", "luaflipper-appid", tag));
    if (name) r.appendChild(el("span", "luaflipper-name", name));
    if (meta) r.appendChild(el("span", "luaflipper-meta", meta));
    return r;
  }

  // Backends answer {"error"} for pages they cannot serve yet. Showing that
  // sentence beats an empty list, which would read as "nothing configured"
  // when the truth is "nothing asked".
  function errorEl(data, el) {
    return (data && data.error) ? el("div", "luaflipper-error", text(data.error))
                                : null;
  }

  /* -------------------------------------------------------------- pages --- */

  /**
   * Manifests found in the watched stplug-in directories.
   *
   * `keys` counts the addappid lines carrying a decryption key; `ids` counts
   * them all. A manifest with no keys still marks ownership, so the download
   * starts and then fails to decrypt. That is the most common cause of a failed
   * download, so it is called out per row and summarised at the top rather than
   * left for the user to infer from a key count of zero.
   */
  function unlocker(data, el) {
    var err = errorEl(data, el);
    if (err) return err;

    var list = arr(data && data.manifests);
    var wrap = el("div");

    if (!list.length) {
      wrap.appendChild(el("div", "luaflipper-empty",
        "No manifests found. Drop .lua files into config/stplug-in."));
      return wrap;
    }

    var keyless = list.filter(function (m) { return num(m.keys) === 0; }).length;
    wrap.appendChild(el("div", "luaflipper-sub",
      plural(list.length, "manifest") +
      (keyless ? ", " + keyless + " without keys" : "")));

    list.forEach(function (m) {
      var keys = num(m.keys);
      var r = row(el,
        text(m.appid) || "?",
        text(m.file),
        (keys ? plural(keys, "key") : "no keys") + ", " + plural(num(m.ids), "id"));
      // "no keys" names the symptom, not the consequence. Say the consequence.
      if (!keys) {
        r.title = "No decryption key in this manifest. Content downloads but " +
                  "cannot be decrypted.";
      }
      wrap.appendChild(r);
    });
    return wrap;
  }

  /**
   * The lua.tools sign-in, shared by every page that needs one.
   *
   * Their catalog is free and their downloads are not: fixes and the proxied
   * manifest sources sit behind the same session, so signing in on one page
   * unlocks the other. That is the whole reason this is one component rather
   * than two copies -- the account is not a property of the page it was
   * entered on.
   *
   * The route is their Discord bot: /login replies with a short code, the code
   * is the credential, and it is single-use with a five-minute life. Nothing
   * here handles a password or opens an OAuth window.
   */
  function luaToolsAccount(el, onChange) {
    var node = style(el("div"), {
      display: "flex", alignItems: "center", gap: "10px", marginBottom: "14px",
      padding: "10px 12px", borderRadius: "3px", background: "rgba(0,0,0,0.2)"
    });

    function draw(st) {
      node.innerHTML = "";
      var signedIn = !!(st && st.signedIn);

      node.appendChild(style(el("div", null,
        signedIn ? "Signed in to lua.tools" : "Not signed in to lua.tools"), {
        fontSize: "13px", fontWeight: "bold",
        color: signedIn ? "#a4d007" : SET.label, flex: "0 0 auto"
      }));

      node.appendChild(style(el("div", null, signedIn
        ? (st.supporter ? "Supporter: downloads are unlimited."
                        : "Downloads count against 25 a day, shared between "
                          + "fixes and manifests.")
        : "Unlocks fix downloads and the proxied manifest sources."), {
        fontSize: "12px", color: SET.desc, flex: "1 1 auto", minWidth: "0"
      }));

      if (signedIn) {
        var out = dialogBtn(el, "Sign out");
        node.appendChild(out);
        out.addEventListener("click", function () {
          out.busyLook(true);
          fetch(API + "fixes/logout").then(function (r) { return r.json(); })
            .then(function () { draw({ signedIn: false }); if (onChange) onChange(); })
            .catch(function () { out.busyLook(false); });
        });
        return;
      }

      var said = style(el("div"), {
        flex: "0 0 auto", fontSize: "12px", color: "#e7a94a", maxWidth: "40%"
      });

      /*
       * Discord in the real browser, not a code to hunt for.
       *
       * The bot route works but starts with knowing to run /login with a bot in
       * a Discord server, which is not something anyone finds unprompted. This
       * is the other route their own client offers, and the module is the
       * landing point: their Supabase accepts a loopback redirect, so the
       * browser comes back to 127.0.0.1 and the session is saved there.
       *
       * Polling afterwards because the answer arrives in another process
       * entirely; nothing in this window is told when the browser is done.
       */
      var go = dialogBtn(el, "Sign in with Discord");
      node.appendChild(go);
      node.appendChild(said);

      var poll = null;
      function stopPolling() { if (poll) { clearInterval(poll); poll = null; } }

      go.addEventListener("click", function () {
        go.busyLook(true);
        go.textContent = "Waiting for Discord";
        said.style.color = SET.desc;
        said.textContent = "Authorise in the browser, then come back.";

        fetch(API + "fixes/signin")
          .then(function (r) { return r.json(); })
          .then(function (res) {
            if (!res || !res.ok || !res.url) throw new Error("no sign-in url");
            openUrl(text(res.url));
            var tries = 0;
            stopPolling();
            poll = setInterval(function () {
              tries++;
              fetch(API + "fixes/account").then(function (r) { return r.json(); })
                .then(function (a) {
                  if (a && a.signedIn) {
                    stopPolling();
                    draw(a);
                    if (onChange) onChange();
                  } else if (tries > 60) {          // three minutes is plenty
                    stopPolling();
                    go.busyLook(false);
                    go.textContent = "Sign in with Discord";
                    said.style.color = "#e7a94a";
                    said.textContent = "Gave up waiting. Try again.";
                  }
                })
                .catch(skip);
            }, 3000);
          })
          .catch(function (e) {
            go.busyLook(false);
            go.textContent = "Sign in with Discord";
            said.style.color = "#e7a94a";
            said.textContent = "Could not start sign-in: " + why(e);
          });
      });
    }

    draw(null);
    fetch(API + "fixes/account").then(function (r) { return r.json(); })
      .then(draw).catch(skip);

    return { node: node, refresh: function () {
      fetch(API + "fixes/account").then(function (r) { return r.json(); })
        .then(draw).catch(skip);
    } };
  }

  /**
   * The Fixes page: the library, filtered to games a fix exists for.
   *
   * Two views in one element, toggled rather than rebuilt, so coming back from
   * a game lands on the grid that was already there.
   *
   * The grid is the same portrait grid Manage draws, because it is answering
   * the same kind of question about the same games. The game view is Steam's
   * own library page for a game: hero across the top, an action button at the
   * left of the bar under it, and stat blocks to its right. Steam puts play
   * time and last played in those blocks; this puts what is known about the
   * game's fixes there, since that is what the page is for. The Play button
   * becomes Fix, and opens the fix list rather than launching anything.
   *
   * Scope: only games this install has a manifest for. The catalog carries
   * about seventeen hundred, and a page listing fixes for games the user has no
   * manifest for would be a shop, not a fix list.
   */
  function fixes(data, el) {
    var err = errorEl(data, el);
    if (err) return err;

    var list = arr(data && data.games).filter(function (g) { return !!g; });

    var wrap = style(el("div"), { position: "relative" });
    wrap.appendChild(style(el("div"), {
      position: "absolute", top: "-18px", left: "-24px",
      width: "calc(100% + 48px)", height: "1200px", zIndex: "0",
      pointerEvents: "none", background: PAGE_WASH
    }));
    var page = style(el("div"), { position: "relative", zIndex: "1" });
    wrap.appendChild(page);

    if (!list.length) {
      page.appendChild(el("div", "luaflipper-empty",
        "No fixes for anything installed. " + num(data && data.scanned) +
        " games in the catalog were checked against " +
        plural(num(data && data.installed), "manifest") + " here."));
      return wrap;
    }

    /* ---------------------------------------------------------- views --- */

    var grid = style(el("div"), {
      display: "grid", gap: "16px",
      gridTemplateColumns: "repeat(auto-fill, minmax(152px, 1fr))"
    });
    var gridView = el("div");
    var gameView = el("div");
    gameView.style.display = "none";
    page.appendChild(gridView);
    page.appendChild(gameView);

    // Signing in here also unlocks the proxied manifest sources on the Sources
    // page, because it is one account behind both.
    gridView.appendChild(luaToolsAccount(el, null).node);

    gridView.appendChild(style(el("div", "luaflipper-sub",
      list.length + " of " + plural(num(data.installed), "installed manifest") +
      " have a published fix, out of " + num(data.scanned) +
      " games in the catalog."), { marginBottom: "14px" }));
    gridView.appendChild(grid);

    function showGrid() {
      gameView.style.display = "none";
      gridView.style.display = "";
    }

    /* ---------------------------------------------------------- modal --- */

    /**
     * One fix, over the page.
     *
     * The description is the release's own text, and it is instructions: which
     * files to extract where, what to disable first, what to undo afterwards.
     * It is shown whole and unstyled rather than summarised, because a fix
     * applied from a summary is a broken install.
     */
    function openFix(fix, appId, game) {
      var back = style(el("div"), {
        position: "fixed", top: "0", left: "0", right: "0", bottom: "0",
        background: "rgba(0,0,0,0.65)", zIndex: "9000",
        display: "flex", alignItems: "center", justifyContent: "center",
        padding: "40px"
      });

      var box = style(el("div"), {
        background: "#23262e", borderRadius: "3px", width: "660px",
        maxWidth: "100%", maxHeight: "100%", display: "flex",
        flexDirection: "column", boxShadow: "0 8px 32px rgba(0,0,0,0.6)"
      });
      back.appendChild(box);

      var head = style(el("div"), {
        display: "flex", alignItems: "center", gap: "12px",
        padding: "14px 16px", borderBottom: "1px solid " + SET.rule
      });
      head.appendChild(style(el("div", null, text(fix.title) || "Fix"), {
        fontSize: "18px", fontWeight: "700", color: "#ffffff",
        flex: "1 1 auto", minWidth: "0", overflow: "hidden",
        textOverflow: "ellipsis", whiteSpace: "nowrap"
      }));
      var x = style(el("div", null, "✕"), {
        cursor: "pointer", color: SET.desc, fontSize: "16px", padding: "0 4px"
      });
      head.appendChild(x);
      box.appendChild(head);

      var body = style(el("div"), {
        padding: "14px 16px", overflowY: "auto", flex: "1 1 auto"
      });
      box.appendChild(body);

      var tags = arr(fix.tags);
      if (tags.length) {
        var strip = style(el("div"), {
          display: "flex", flexWrap: "wrap", gap: "6px", marginBottom: "12px"
        });
        tags.forEach(function (t) {
          // The service ships a colour per tag; it is the one piece of the
          // catalog with an opinion about how it looks, so it is honoured.
          strip.appendChild(style(el("div", null, text(t && t.name)), {
            fontSize: "11px", fontWeight: "bold", padding: "3px 8px",
            borderRadius: "2px", color: "#111318",
            background: text(t && t.color) || "rgba(255,255,255,0.2)"
          }));
        });
        body.appendChild(strip);
      }

      var when = text(fix.createdAt).substring(0, 10);
      var facts = [];
      if (when) facts.push("published " + when);
      if (fix.fixFilename) facts.push(text(fix.fixFilename));
      if (fix.manifestFilename) facts.push(text(fix.manifestFilename));
      if (facts.length) {
        body.appendChild(style(el("div", null, facts.join("  ·  ")), {
          fontSize: "12px", color: SET.desc, marginBottom: "12px"
        }));
      }

      body.appendChild(style(el("div", null, text(fix.description) ||
        "This fix ships no instructions."), {
        whiteSpace: "pre-wrap", wordBreak: "break-word", fontSize: "13px",
        lineHeight: "19px", color: SET.label, userSelect: "text",
        webkitUserSelect: "text"
      }));

      var foot = style(el("div"), {
        display: "flex", alignItems: "center", gap: "12px",
        padding: "12px 16px", borderTop: "1px solid " + SET.rule
      });
      box.appendChild(foot);

      var said = style(el("div"), {
        flex: "1 1 auto", minWidth: "0", fontSize: "12px", lineHeight: "17px",
        color: SET.desc, wordBreak: "break-word",
        userSelect: "text", webkitUserSelect: "text"
      });
      foot.appendChild(said);

      /*
       * Straight to whoever made it.
       *
       * lua.tools mirrors rather than authors: 1630 of the 1769 games in its
       * catalog are tagged Online Fix, and its own file host is a private R2
       * bucket that answers "not authorized to view this bucket" to everything
       * outside its app. So the origin is worth reaching directly, and the
       * only honest way to point at it is a search for the name.
       *
       * Deliberately a search and not a resolved link. Their pages are keyed by
       * their own post id and slug, with no Steam appid anywhere, and matching
       * by title does not survive contact with sequels: tested against the open
       * Hydra catalogue, "Borderlands 4" matched Borderlands 3 and
       * "Subnautica 2" matched Subnautica, because the digit that makes a
       * sequel is the same digit that makes a version. A link that quietly
       * opens the wrong game is worse than one more click.
       */
      var origin = link(el, "Find on online-fix.me",
        "https://online-fix.me/index.php?do=search&subaction=search&story=" +
        encodeURIComponent(text(game && game.name) || ""));
      style(origin, { fontSize: "12px", marginRight: "4px" });
      foot.appendChild(origin);

      var get = dialogBtn(el, fix.hasFix ? "Download fix" : "No archive");
      if (!fix.hasFix) get.busyLook(true);
      foot.appendChild(get);

      get.addEventListener("click", function () {
        if (!fix.hasFix) return;
        get.busyLook(true);
        get.textContent = "Downloading";
        said.textContent = "";
        fetch(API + "fixes/download?fix=" + encodeURIComponent(text(fix.id)) +
              "&slot=fix&name=" + encodeURIComponent(text(fix.fixFilename)))
          .then(function (r) { return r.json(); })
          .then(function (res) {
            get.busyLook(false);
            if (!res || typeof res !== "object") throw new Error("unreadable reply");
            if (res.error) {
              get.textContent = (res.needsToken || res.needsLogin)
                ? "Download fix" : "Try again";
              said.textContent = text(res.error);
              said.style.color = "#e7a94a";
              return;
            }
            get.textContent = "Downloaded";
            get.busyLook(true);
            said.style.color = "#a4d007";
            // Where it landed, not "done". The archive still has to be applied
            // by hand, and the instructions above say where.
            said.textContent = "Saved " + bytes(num(res.bytes)) + " to " +
              text(res.path) + ". Apply it as the instructions above describe.";
          })
          .catch(function (e) {
            get.busyLook(false);
            get.textContent = "Try again";
            said.style.color = "#e7a94a";
            said.textContent = "Could not reach SteamFlipper: " + why(e);
          });
      });

      function close() {
        if (back.parentNode) back.parentNode.removeChild(back);
        document.removeEventListener("keydown", onKey, true);
      }
      function onKey(ev) { if (ev.key === "Escape") close(); }
      x.addEventListener("click", close);
      back.addEventListener("click", function (ev) {
        if (ev.target === back) close();
      });
      document.addEventListener("keydown", onKey, true);

      // On the page's own root, so it dies with the page rather than being left
      // over the client after the tab is closed.
      wrap.appendChild(back);
    }

    /* ----------------------------------------------------------- game --- */

    /**
     * Steam's library page for one game, with fix information where the play
     * stats go.
     *
     * Measured off the live one: the hero sits over a blurred copy of itself,
     * the bar under it carries the action button on the left, and each stat is
     * a 13px uppercase label with 1px tracking over a 12px value.
     */
    function openGame(game) {
      var appId = appid(text(game.appid));
      gridView.style.display = "none";
      gameView.style.display = "";
      gameView.innerHTML = "";

      var crumb = style(el("div", null, "‹ All games with fixes"), {
        cursor: "pointer", color: LINK_TEXT, fontSize: "13px",
        marginBottom: "12px", display: "inline-block"
      });
      crumb.addEventListener("click", showGrid);
      gameView.appendChild(crumb);

      // Hero. 23% is the proportion Steam draws library_hero.jpg at in its own
      // game page, measured off the live one; a padding-top box holds that
      // shape while the art is still loading or never arrives.
      var hero = style(el("div"), {
        position: "relative", paddingTop: "23%", borderRadius: "3px",
        overflow: "hidden", background: INSET, marginBottom: "0"
      });
      var heroImg = style(el("img"), {
        position: "absolute", top: "0", left: "0", width: "100%",
        height: "100%", objectFit: "cover", display: "block"
      });
      heroImg.alt = "";
      var heroName = style(el("div", null, text(game.name)), {
        position: "absolute", top: "0", left: "0", right: "0", bottom: "0",
        display: "none", alignItems: "center", justifyContent: "center",
        fontSize: "28px", fontWeight: "700", color: "#ffffff", padding: "16px",
        textAlign: "center"
      });
      hero.appendChild(heroImg);
      hero.appendChild(heroName);
      if (appId) {
        libraryArt(heroImg, appId, "library_hero.jpg", "library_hero",
          function () {
            heroImg.style.display = "none";
            heroName.style.display = "flex";
          });
      } else {
        heroImg.style.display = "none";
        heroName.style.display = "flex";
      }
      gameView.appendChild(hero);

      // The bar under the hero: action left, stats right.
      var bar = style(el("div"), {
        display: "flex", alignItems: "center", gap: "28px",
        padding: "14px 16px", background: "rgba(0,0,0,0.30)",
        borderRadius: "0 0 3px 3px", marginBottom: "18px"
      });
      gameView.appendChild(bar);

      var fixBtn = style(el("div", "luaflipper-button", "Fix"), {
        background: BLUE, border: "0", color: "#ffffff", fontWeight: "bold",
        fontSize: "15px", textTransform: "uppercase", letterSpacing: "0.5px",
        padding: "0 34px", lineHeight: "36px", borderRadius: "2px",
        flex: "0 0 auto", cursor: "pointer"
      });
      fixBtn.addEventListener("mouseenter", function () {
        fixBtn.style.background = BLUE_HOT;
      });
      fixBtn.addEventListener("mouseleave", function () {
        fixBtn.style.background = BLUE;
      });
      bar.appendChild(fixBtn);

      function stat(label, value) {
        var s = style(el("div"), { flex: "0 0 auto", minWidth: "0" });
        s.appendChild(style(el("div", null, label), {
          fontSize: "13px", textTransform: "uppercase", letterSpacing: "1px",
          color: "rgba(255,255,255,0.52)", lineHeight: "16px"
        }));
        var v = style(el("div", null, value), {
          fontSize: "12px", color: "rgba(255,255,255,0.32)", lineHeight: "17px",
          marginTop: "2px"
        });
        s.appendChild(v);
        bar.appendChild(s);
        return v;
      }

      var countVal = stat("Fixes available", plural(num(game.fixes), "fix", "fixes"));
      var latestVal = stat("Latest fix", "loading");
      // The other half of "is this game working": a manifest that registers
      // ownership it has no key for downloads and then fails to decrypt, which
      // no downloadable fix addresses. It belongs beside them, not hidden.
      stat("Manifest keys", num(game.keyless)
        ? num(game.keyless) + " appid(s) with no key, " + num(game.keyed) + " keyed"
        : num(game.keyed) + " keyed, all covered");

      var listBox = el("div");
      gameView.appendChild(listBox);
      listBox.appendChild(el("div", "luaflipper-loading", "Loading fixes…"));

      // Held so the Fix button can open the first one the moment it is pressed,
      // rather than the button doing nothing until the list has arrived.
      var loaded = null;

      fixBtn.addEventListener("click", function () {
        if (loaded && loaded.length) openFix(loaded[0], appId, game);
      });

      fetch(API + "fixes/list?appid=" + encodeURIComponent(text(game.appid)))
        .then(function (r) { return r.json(); })
        .then(function (res) {
          listBox.innerHTML = "";
          if (res && res.error) {
            listBox.appendChild(el("div", "luaflipper-error", text(res.error)));
            latestVal.textContent = "unknown";
            return;
          }
          loaded = arr(res && res.fixes);
          countVal.textContent = plural(loaded.length, "fix", "fixes");
          if (!loaded.length) {
            listBox.appendChild(el("div", "luaflipper-empty",
              "The catalog lists this game but ships no fix for it."));
            latestVal.textContent = "none";
            return;
          }

          // Newest first, and the newest is what the Fix button opens.
          loaded.sort(function (a, b) {
            return text(b.createdAt) < text(a.createdAt) ? -1 : 1;
          });
          latestVal.textContent = text(loaded[0].createdAt).substring(0, 10) ||
            "unknown";

          var g = fieldGroup(el);
          listBox.appendChild(g);
          loaded.forEach(function (f) {
            var names = arr(f.tags).map(function (t) { return text(t && t.name); });
            var right = field(el, g, text(f.title) || "Fix",
              names.join(", ") + (names.length ? "  ·  " : "") +
              text(f.createdAt).substring(0, 10),
              dialogBtn(el, "Open"));
            // The whole row, not just the button: a list of rows where only a
            // 60px target does anything is a list that feels broken.
            var host = right.parentNode;
            host.style.cursor = "pointer";
            host.addEventListener("click", function () { openFix(f, appId, game); });
          });
        })
        .catch(function (e) {
          listBox.innerHTML = "";
          listBox.appendChild(el("div", "luaflipper-error",
            "Could not reach SteamFlipper: " + why(e)));
          latestVal.textContent = "unknown";
        });
    }

    /* ----------------------------------------------------------- grid --- */

    list.forEach(function (game) {
      var id = appid(text(game.appid));
      var box = style(el("div"), { position: "relative", cursor: "pointer" });
      grid.appendChild(box);

      var cap = style(el("div"), {
        position: "relative", paddingTop: "150%", borderRadius: "3px",
        overflow: "hidden", background: INSET,
        boxShadow: "rgba(0,0,0,0.5) 0 4px 8px 0",
        transition: "transform 120ms ease, box-shadow 120ms ease"
      });
      box.appendChild(cap);

      var img = style(el("img"), {
        position: "absolute", top: "0", left: "0", width: "100%",
        height: "100%", objectFit: "cover", display: "block"
      });
      img.alt = "";
      cap.appendChild(img);

      var plate = style(el("div", null, text(game.name)), {
        position: "absolute", top: "0", left: "0", right: "0", bottom: "0",
        display: "none", alignItems: "center", justifyContent: "center",
        padding: "12px", textAlign: "center", fontSize: "13px",
        lineHeight: "1.35", wordBreak: "break-word"
      });
      cap.appendChild(plate);

      function blank() {
        img.style.display = "none";
        plate.style.display = "flex";
      }
      if (id) libraryArt(img, id, "library_600x900.jpg", "library_capsule", blank);
      else blank();

      // How many, on the tile. The grid exists to be scanned, and a game with
      // three fixes and a game with one are not the same thing to open.
      cap.appendChild(style(el("div", null,
        plural(num(game.fixes), "fix", "fixes")), {
        position: "absolute", top: "6px", right: "6px", padding: "2px 6px",
        borderRadius: "2px", fontSize: "11px", fontWeight: "bold",
        color: "#111318", background: "#67c1f5"
      }));

      // The same lift Manage uses, so the two grids behave alike.
      box.addEventListener("mouseenter", function () {
        style(cap, {
          transform: "scale(1.03)",
          boxShadow: "rgba(0,0,0,0.65) 0 6px 14px 0"
        });
      });
      box.addEventListener("mouseleave", function () {
        style(cap, {
          transform: "none", boxShadow: "rgba(0,0,0,0.5) 0 4px 8px 0"
        });
      });
      box.addEventListener("click", function () { openGame(game); });
    });

    return wrap;
  }

  /* ----------------------------------------------------------- unlocker --- */

  /**
   * The Unlocker page: a working copy of Steam's own store, in two views.
   *
   * View 1 is the store front. A search field over two carousels of
   * recommended capsules, which the search results replace while a term is
   * active. View 2 is an app page laid out the way store.steampowered.com lays
   * one out: breadcrumb, title, hero art beside a details block, and under both
   * a purchase area whose rows are the manifest sources that carry the app,
   * each with a green Install.
   *
   * Both views live in the returned element and are toggled rather than
   * rebuilt, so leaving an app page lands back on the results that were already
   * there instead of re-running the search.
   *
   * Almost everything below is styled inline. luaflipper.css defines only rows,
   * inputs and buttons, and a stylesheet of our own would ignore custom Steam
   * themes, so surfaces are alpha over whatever the theme resolves to.
   */
  var API = "http://127.0.0.1:1987/api/";
  var CDN = "https://cdn.cloudflare.steamstatic.com/steam/apps/";

  /*
   * Steam's store palette, sampled from the live page.
   *
   * These are the exception to the alpha rule, because they are the store's
   * identity rather than its chrome: a green Install button, blue metadata
   * values and dim uppercase labels are what make a page read as Steam at a
   * glance, and an alpha approximation of them just reads as broken. The
   * surfaces below them stay alpha so a custom theme still shows through.
   */
  var LINK = "#67c1f5";
  var LABEL = "#556772";
  var GREEN = "linear-gradient(to bottom, #75b022 5%, #588a1b 95%)";
  var GREEN_HOT = "linear-gradient(to bottom, #8ed629 5%, #6aa621 95%)";
  var BLUE = "linear-gradient(to bottom, #67c1f5 5%, #417a9b 95%)";
  var BLUE_HOT = "linear-gradient(to bottom, #8ed8ff 5%, #4f93ba 95%)";

  // Store surfaces. INSET is the store's details and purchase rows, DEEP the
  // well an action button sits in, PANEL the purchase area itself. PANEL is a
  // blue tint rather than the literal #1b2838 so it stays "slightly lighter
  // blue-grey than the page" on any theme; over the stock skin it lands on
  // Steam's own tone anyway.
  var INSET = "rgba(0,0,0,0.2)";
  var DEEP = "rgba(0,0,0,0.4)";
  var PANEL = "rgba(103,193,245,0.07)";
  var LIFT = "rgba(103,193,245,0.14)";

  // The store page's own tokens. Links (#66c0f4) and the button fill (#67c1f5,
  // BLUE above) are a shade apart on purpose: Steam uses the lighter blue only
  // as a fill, and keeping that split is what stops blue text glowing beside a
  // blue button. The design's Light Gray (#d9d9d9) is deliberately absent:
  // that one is body text, and a fixed near-white disappears on a light skin,
  // so text stays `inherit` - which resolves to it anyway on the stock client.
  var LINK_TEXT = "#66c0f4";
  var GRAY = "#8f98a0";

  /*
   * App page geometry, from the store page design (1920x1080, 706 content
   * column at x=602).
   *
   * That frame is the live store drawn at 75%: its 706 column is Steam's own
   * 940, its 450 gallery frame Steam's 600x338 screenshot, its 87x48
   * thumbnails Steam's 116x65, its 462 buy widget Steam's 616. So the ratios
   * below are the design's numbers as given, and the pixel sizes are those
   * numbers back at 1:1 (x 4/3): type, gaps and hairlines do not scale with
   * the window the way the columns do.
   *
   * The columns are grow factors rather than percentages so the 450:244 split
   * survives the gutter being subtracted from the row, and so the page fills
   * the window instead of being pinned to the design's 706.
   */
  var GAL_GROW = "450";        // Gallery, 450 wide
  var OVR_GROW = "244";        // Game Overview, 244 wide, over a 12px gutter
  var GUTTER = "16px";         // that gutter at 1:1
  var LARGE_RATIO = "56.4%";   // Large Image, 450x254
  var COVER_RATIO = "46.7%";   // Game Cover, 244x114
  var BUY_WIDTH = "65.4%";     // Buy Widget and About section, 462 of 706
  var ROW_H = "18px";          // a detail row, 14 tall
  var GROUP_GAP = "12px";      // the 8-10px spacer between detail groups

  // Art ladders, ordered by the width they are drawn at so nothing is upscaled
  // past its native size. capsule_616x353 is the store's hero but is missing
  // for plenty of apps, and header.jpg is the one asset Steam has for very
  // nearly everything, so it ends every list.
  // library_hero is 1920 wide, so the hero can span a maximised window without
  // upscaling. The narrower capsules stay as fallbacks; capsule() draws with
  // object-fit cover, so a fallback of a different shape still fills the box.
  var HERO = ["library_hero.jpg", "capsule_616x353.jpg", "header.jpg"];
  // library_hero ends both ladders as well. Newer apps 404 on the flat
  // header.jpg path because Steam moved that art to hashed store_item_assets
  // URLs, and without this those tiles render empty even though the app plainly
  // has art. It is oversized for a tile, but object-fit crops it to the box.
  var SHOT = ["header.jpg", "header_292x136.jpg", "library_hero.jpg"];
  var CAP = ["header_292x136.jpg", "header.jpg", "library_hero.jpg"];

  /*
   * Store front geometry and palette, from the SteamClone base whose .featured
   * block this card is a port of: a 950 wide row 350 tall, a wrapped 2x2 grid
   * of 160x70 shots at 3px gaps, and $black (#171a21) behind $textGrey
   * (#F5F5F5) and $funBlue (#67c1f5, LINK above).
   *
   * The base's height is a fixed 350px, which cannot survive a window that
   * resizes, so this derives it instead, and FEAT_MAX exists because that makes
   * the card the one block on this page whose height grows with the window.
   *
   * FEAT_RATIO is a 2:1 band rather than main_capsule's own 57.3%. At the cap
   * the art column is ~900 wide, half again past that asset's native 616, so
   * the ratio stopped reproducing it 1:1 some way back; what it still decides is
   * the height of the panel beside it, and 57.3% made that panel 513 tall
   * against 250 of content and left a void of blue under the thumbnails.
   */
  var FEAT_MAX = "1400px";
  var FEAT_RATIO = "50%";
  // 16:9, the native ratio of the screenshots that go in these boxes. The base's
  // 160x70 is 2.29:1, which cropped a quarter off every shot and cost the panel
  // the height that was making the void above worse.
  var THUMB_RATIO = "56.25%";
  // Half the grid less its own 3px margins, and 1px of slack so subpixel
  // rounding cannot wrap the second thumbnail of a pair onto its own line.
  var THUMB_W = "calc(50% - 7px)";
  // The slivers of the previous and next card showing past both edges, which
  // is what says "carousel" before an arrow is ever pressed. The arrows sit on
  // top of them, so the two are the same width by construction.
  var PEEK_W = "44px";
  var PEEK_GAP = "6px";
  // The store's own panel blue, not the base repo's $black: on Steam's blue-grey
  // page a black panel reads as a hole rather than as a card. Still fully
  // opaque, so near-white over #67c1f5 stays legible on any skin.
  var CARD_BG = "#1b2838";
  var CARD_TEXT = "#f5f5f5";
  // Steam's store body. Painted behind the top of the browse view and faded to
  // nothing rather than ending on a colour, so the theme resumes underneath
  // instead of this page claiming the whole tab.
  var PAGE_WASH = "linear-gradient(to bottom, #1b2838 0%, #1b2838 45%, " +
                  "rgba(27,40,56,0) 100%)";

  // Only digits reach a CDN URL or a query string. Steam app ids are numeric,
  // so stripping the rest doubles as the validity test for a search result.
  /**
   * Whether a source can actually be asked to install right now.
   *
   * "unavailable" means it does not carry the app. The rest are about this
   * machine rather than the app: "needs key" and "bad key" for Hubcap, and
   * "needs sign-in" for the sources lua.tools only serves through its proxy.
   * All are worth showing and none is worth trying, so a fallthrough that
   * treated them as live would spend a click to be told what the row says.
   */
  function installable(s) {
    var st = text(s && s.status);
    return !!(s && text(s.name)) && st !== "unavailable" &&
           st !== "needs key" && st !== "bad key" && st !== "needs sign-in";
  }

  /** A byte count as something a person reads. */
  function bytes(n) {
    if (!n) return "0 B";
    var u = ["B", "KB", "MB", "GB"], i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (i ? n.toFixed(1) : String(n)) + " " + u[i];
  }

  function appid(v) {
    return text(v).replace(/[^0-9]/g, "");
  }

  // A rejected fetch carries an Error; a throw from a .then handler might not.
  function why(e) {
    return (e && e.message) ? e.message : String(e);
  }

  // 5,248,129 is a review count; 5248129 is a phone number. toLocaleString is
  // the obvious answer and is not dependable in a CEF build with its ICU data
  // stripped, so the separators go in by hand.
  function comma(v) {
    return String(num(v)).replace(/\B(?=(\d{3})+(?!\d))/g, ",");
  }

  // innerHTML is never used on this page, not even with a constant, so no later
  // edit can quietly start handing it a backend string.
  function clear(node) {
    while (node.firstChild) node.removeChild(node.firstChild);
  }

  function only(node, child) {
    clear(node);
    node.appendChild(child);
    return child;
  }

  function style(node, css) {
    for (var k in css) {
      if (Object.prototype.hasOwnProperty.call(css, k)) node.style[k] = css[k];
    }
    return node;
  }

  /**
   * Walk `files` on the CDN until one of them loads, then give up and hide.
   *
   * Delisted apps and apps Steam never made art for are common enough that a
   * broken-image glyph would be the usual case, not the exception.
   *
   * Returns a setter that restarts the walk at a preferred URL. appdetails
   * answers long after the image is on screen, and its hashed
   * store_item_assets URL is the only art that resolves for newer apps, so it
   * has to be applied late without losing the CDN fallback underneath it.
   */
  function ladder(img, id, files) {
    var next = 0;
    img.onerror = function () {
      if (next < files.length) { img.src = CDN + id + "/" + files[next++]; return; }
      img.style.display = "none";
    };
    img.onerror();   // the first load goes down the same ladder as every retry
    return function (url) {
      if (!url) return;
      next = 0;
      img.style.display = "block";
      img.src = url;
    };
  }

  /**
   * A Steam capsule in a fixed-ratio box.
   *
   * The box keeps its shape whether or not any art arrives. `ratio` is a
   * padding-top percentage rather than aspect-ratio, which CEF may predate.
   * `setters` is optional and collects this image's preferred-URL setter for a
   * caller that is still waiting on appdetails.
   */
  function capsule(el, id, files, ratio, setters) {
    var box = style(el("div"), {
      position: "relative",
      paddingTop: ratio,
      borderRadius: "3px",
      overflow: "hidden",
      background: INSET
    });
    var img = el("img");
    style(img, {
      position: "absolute", top: "0", left: "0",
      width: "100%", height: "100%", objectFit: "cover", display: "block"
    });
    img.alt = "";
    var set = ladder(img, id, files);
    if (setters) setters.push(set);
    box.appendChild(img);
    return box;
  }

  /*
   * One request per URL for as long as the script is loaded.
   *
   * The featured card fetches art, reviews and sources for the game it is
   * showing and for no other, so paging back through twelve top sellers would
   * otherwise re-run four requests per card every time. It is module scope
   * rather than per page because the carousel tiles rescue their own art
   * through it too, and a tile and the card often want the same app.
   *
   * The promise is cached raw, rejection included: callers still validate and
   * catch for themselves, because a carousel wants to print the backend's own
   * error sentence and a shared wrapper would flatten that to "failed".
   */
  var asked = {};

  function ask(path) {
    if (!asked[path]) {
      asked[path] = fetch(API + path).then(function (r) { return r.json(); });
    }
    return asked[path];
  }

  // A reply worth drawing, or nothing. Everything ask() serves is decoration
  // over something already on screen, so a bad shape and an {"error"} are the
  // same answer here: leave it out.
  function usable(res) {
    return (res && typeof res === "object" && !res.error) ? res : null;
  }

  function skip() { /* every request behind ask() is optional, see above */ }

  /**
   * Last resort for a tile whose CDN ladder ran out.
   *
   * Steam moved store art behind hashed store_item_assets URLs, so the flat
   * .../steam/apps/<id>/header.jpg path 404s outright for anything published
   * since, and a shelf of newer releases comes up as a row of empty rectangles
   * with captions under them. /api/assets is the only address that resolves for
   * those, and it is one request per tile, so it is spent only on the tiles that
   * actually failed rather than on all twelve up front.
   *
   * ladder() already signals exhaustion by hiding the image, which is the exact
   * moment to spend it. Wrapping its handler rather than changing ladder() keeps
   * the app page's own images on the behaviour they were signed off with.
   */
  function rescueArt(box, id, set) {
    var img = box.firstChild;   // capsule() appends the image and nothing else
    var ladderStep = img.onerror;
    var tried = false;

    img.onerror = function () {
      ladderStep();
      if (tried || img.style.display !== "none") return;
      tried = true;
      ask("assets?appid=" + encodeURIComponent(id))
        .then(function (res) {
          var ok = usable(res);
          if (!ok) return;
          // Smallest first: these boxes are tiles, and main_capsule would be
          // downscaled from 616 into a 200px strip for no gain.
          set(text(ok.header) || text(ok.small_capsule) ||
              text(ok.main_capsule));
        })
        .catch(skip);
    };
  }

  /**
   * The 32px icon beside an app page's title.
   *
   * Steam's real app icon sits behind a filename hash the store API does not
   * hand out, so the small capsule is cropped square by object-fit instead. It
   * hides rather than showing a broken glyph, because it only repeats what the
   * title beside it already says.
   */
  function appIcon(el, id, setters) {
    var img = el("img");
    style(img, {
      width: "32px", height: "32px", flex: "0 0 auto", display: "block",
      objectFit: "cover", borderRadius: "2px", background: INSET
    });
    img.alt = "";
    var set = ladder(img, id, ["header_292x136.jpg", "header.jpg"]);
    if (setters) setters.push(set);
    return img;
  }

  /**
   * Fill an image with the art Steam's own library would draw, or give up.
   *
   * The client's own asset host comes first because that is where Steam keeps
   * what the library is currently showing, custom art the user set by hand
   * included, so a tile matches the library rather than merely resembling it.
   * The flat CDN path covers everything the client has not cached, and
   * /api/assets resolves the hashed URL for apps published since Steam moved
   * its art behind those, which is most of what gets added here.
   *
   * `flat` is the filename on both of those paths and `hashed` the /api/assets
   * field that answers for the same art: library_600x900.jpg / library_capsule
   * for a tile, library_hero.jpg / library_hero for a game page.
   */
  function libraryArt(img, id, flat, hashed, blank) {
    var urls = [
      "https://steamloopback.host/assets/" + id + "/" + flat,
      CDN + id + "/" + flat
    ];
    var next = 0;

    img.onerror = function () {
      if (next < urls.length) { img.src = urls[next++]; return; }
      // Flat paths exhausted. One request for the hashed one, then stop:
      // no art is a normal state for an app, not a failure to retry.
      img.onerror = blank;
      ask("assets?appid=" + encodeURIComponent(id))
        .then(function (res) {
          var ok = usable(res);
          var u = ok ? text(ok[hashed]) : "";
          if (u) img.src = u; else blank();
        })
        .catch(blank);
    };
    img.onerror();
  }

  /**
   * A width-bounded parent for a capsule.
   *
   * capsule() sizes itself from a padding-top percentage, and percentages
   * resolve against the containing block rather than the element's own width.
   * Capping or fixing the box directly therefore skews the ratio; the cap has
   * to go on a wrapper, which is what this is.
   */
  function slot(el, width, child) {
    var s = style(el("div"), { maxWidth: width, flex: "0 0 " + width });
    s.appendChild(child);
    return s;
  }

  /**
   * A store action button.
   *
   * Steam's green Install is the single most recognisable control on a store
   * page, so this is a real gradient rather than a flat accent. Inline styles
   * beat the stylesheet, which means .luaflipper-button's own hover rule stops
   * applying and the hover has to be wired here.
   */
  function storeBtn(el, label, base, hot) {
    var b = style(el("div", "luaflipper-button", label), {
      background: base, border: "0", color: "#ffffff",
      fontWeight: "bold", fontSize: "14px", padding: "0 15px",
      lineHeight: "30px", textShadow: "1px 1px rgba(0,0,0,0.2)"
    });
    b.addEventListener("mouseenter", function () { b.style.background = hot; });
    b.addEventListener("mouseleave", function () { b.style.background = base; });
    return b;
  }

  /** The store's carousel headings: small, uppercase, letter-spaced and dim. */
  function sectionHead(el, label) {
    return style(el("div", null, label), {
      fontSize: "14px", textTransform: "uppercase", letterSpacing: "0.04em",
      fontWeight: "400", opacity: "0.55", margin: "0 0 10px"
    });
  }

  /**
   * The store front's own headline, which Steam sets larger and plain rather
   * than as one of the small uppercase carousel labels above.
   *
   * A second function rather than an argument to sectionHead, because the app
   * page's "About This Game" header is built from that one and this would
   * change it: that page is settled and nothing here should reach it.
   *
   * No colour: this sits on the theme's own surface, and a hardcoded near-white
   * would disappear on a light skin. inherit already resolves to it on the
   * stock client, which is the same call the app page's title makes.
   */
  function bigHead(el, label) {
    return style(el("div", null, label), {
      fontSize: "20px", lineHeight: "26px", fontWeight: "400",
      margin: "0 0 12px"
    });
  }

  /** One carousel tile: capsule above, name below, the whole tile clickable. */
  function capsuleCard(el, game, open) {
    var id = appid(game.appid);
    var name = text(game.name);
    var c = style(el("div"), {
      cursor: "pointer", padding: "6px", borderRadius: "3px", background: INSET
    });
    // The strip is where blank tiles show up worst, a dozen at a time, so the
    // capsule keeps its ladder and falls through to /api/assets if that runs out.
    var setters = [];
    var cap = capsule(el, id, CAP, "46.58%", setters);
    rescueArt(cap, id, setters[0]);
    c.appendChild(cap);

    var label = style(el("div", null, name || ("App " + id)), {
      fontSize: "12.5px", padding: "8px 2px 2px",
      overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
    });
    // The tile clips long names, so the full one has to be reachable somehow.
    label.title = name;
    c.appendChild(label);

    // .luaflipper-row owns the only hover rule in the stylesheet and a tile is
    // not a row, so its hover is wired here.
    c.addEventListener("mouseenter", function () { c.style.background = LIFT; });
    c.addEventListener("mouseleave", function () { c.style.background = INSET; });
    c.addEventListener("click", function () { open(id, name); });
    return c;
  }

  /**
   * One search hit, laid out like the store's own search list: a small capsule,
   * the name, and the app id in Steam blue on the right.
   */
  function resultRow(el, game, open) {
    var id = appid(game.appid);
    var name = text(game.name);
    var r = el("div", "luaflipper-row");
    r.style.cursor = "pointer";

    // Same rescue as a carousel tile: a search for a new release is exactly
    // where the flat CDN path is most likely to have nothing.
    var setters = [];
    var cap = capsule(el, id, CAP, "46.58%", setters);
    rescueArt(cap, id, setters[0]);
    r.appendChild(slot(el, "120px", cap));

    r.appendChild(style(el("span", "luaflipper-name", name || ("App " + id)), {
      fontSize: "14px"
    }));
    r.appendChild(style(el("span", "luaflipper-meta", id), { color: LINK }));
    r.addEventListener("click", function () { open(id, name); });
    return r;
  }

  /**
   * One line of the store's details block: dim uppercase label on the left,
   * value in Steam blue on the right. Returns the value node, because the
   * source count and the install state only arrive once their requests answer.
   *
   * Rows are set tight at the design's 14px row (18 at 1:1), which is what
   * makes the block read as a table rather than a list of separate facts.
   */
  function metaRow(el, into, label, value) {
    var r = style(el("div"), {
      display: "flex", justifyContent: "space-between", alignItems: "baseline",
      gap: "12px", lineHeight: ROW_H
    });
    // The colon belongs to the label, not the layout: without it the two
    // columns read as unrelated at this letter-spacing.
    r.appendChild(style(el("span", null, label + ":"), {
      color: LABEL, textTransform: "uppercase", letterSpacing: "0.5px",
      fontSize: "11px", whiteSpace: "nowrap"
    }));
    var v = style(el("span", null, value), {
      color: LINK_TEXT, fontSize: "12px", textAlign: "right",
      overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
    });
    r.appendChild(v);
    into.appendChild(r);
    return v;
  }

  /**
   * One store tag: a small blue-tinted pill that wraps with its neighbours.
   * Sized to the design's 14px pill (18 at 1:1), so a wrapped second line of
   * them keeps the same rhythm as the detail rows above.
   */
  function tagPill(el, label) {
    return style(el("span", null, label), {
      background: "rgba(103,193,245,0.12)", color: LINK_TEXT, fontSize: "11px",
      padding: "0 8px", lineHeight: ROW_H, borderRadius: "2px",
      whiteSpace: "nowrap"
    });
  }

  /**
   * The store's page background: the app's own art bled to the window edges
   * behind the top of the page, fading out into whatever the theme paints.
   *
   * Design: a background image across the top under a dark overlay, plus a
   * gradient 454 tall from y=76 (so 101 to 707 at 1:1, under a 720 tall
   * image). That gradient is applied as a mask rather than painted as a fill,
   * because a fill would have to end on an opaque page colour and this page
   * has no colour of its own - it sits on the theme's surface. Masking fades
   * the art itself, so the theme shows through underneath on any skin.
   *
   * The negative offsets are .luaflipper-body's own 18px/24px padding, which
   * is exactly what this has to escape to reach the window edges. It is
   * absolute inside the page rather than the scroller, so it scrolls away with
   * the content the way the store's does.
   */
  function backdrop(el) {
    var fade = "linear-gradient(to bottom, rgba(0,0,0,1) 0px, " +
               "rgba(0,0,0,1) 101px, rgba(0,0,0,0) 707px)";
    var box = style(el("div"), {
      position: "absolute", top: "-18px", left: "-24px",
      width: "calc(100% + 48px)", height: "720px", zIndex: "0",
      overflow: "hidden", display: "none", pointerEvents: "none",
      WebkitMaskImage: fade, maskImage: fade
    });

    var img = el("img");
    style(img, {
      width: "100%", height: "100%", objectFit: "cover", display: "block"
    });
    img.alt = "";
    // page_background is generated per app and simply missing for plenty of
    // them. Dropping the whole layer leaves the themed surface, which is what
    // the page looked like before this existed.
    img.onerror = function () { box.style.display = "none"; };
    box.appendChild(img);

    // The design's dark overlay. The page's text sits straight on this art, so
    // the art has to lose the argument.
    box.appendChild(style(el("div"), {
      position: "absolute", top: "0", left: "0", width: "100%", height: "100%",
      background: "rgba(0,0,0,0.55)"
    }));

    return {
      node: box,
      set: function (url) {
        if (!url) return;
        img.src = url;
        box.style.display = "";
      }
    };
  }

  /**
   * The store's screenshot carousel: one shot at a time, chevrons over the
   * image and a row of dots under it.
   *
   * Built before any screenshot exists, because appdetails answers long after
   * the page is on screen. Until it does, the frame runs the same CDN art
   * ladder the single hero image used to, so the column is never empty; `show`
   * then swaps in the real screenshots and owns the frame from that point, so
   * a late header URL cannot overwrite the shot the user moved to.
   *
   * One frame rather than the design's large image over a thumbnail strip: a
   * strip under the image reads as a second stacked picture, which is the one
   * thing about this block the user asked twice to be rid of.
   *
   * Design: Large Image 450x254 (56.4%), and the carousel's own prev 29 |
   * next 29 hit areas, here overlaid on the image at 1:1 (x 4/3 = 40 wide)
   * the way the store draws them.
   */
  function gallery(el, id) {
    // Capped rather than filling the column. At a maximised window the 65%
    // gallery column runs past 1200px, and a 56.4% frame on that is ~680px
    // tall, which pushes the purchase panel off the bottom of the screen. This
    // keeps the frame near the design's own 600x338 without pinning it there.
    var box = style(el("div"), { maxWidth: "860px" });

    var setters = [];
    var frame = capsule(el, id, HERO, LARGE_RATIO, setters);
    var setLarge = setters[0];
    box.appendChild(frame);

    var shots = [];
    var dots = [];
    var at = 0;
    var live = false;

    // Centred under the image, and hidden until there is more than one shot:
    // a single dot is a control that cannot do anything.
    var rail = style(el("div"), {
      display: "none", justifyContent: "center", alignItems: "center",
      gap: "6px", marginTop: "8px"
    });
    box.appendChild(rail);

    function pick(i) {
      if (!shots.length) return;
      // Wraps both ways. An arrow that stops dead on the last shot reads as
      // broken, and the store's own carousel wraps.
      if (i < 0) i = shots.length - 1;
      if (i >= shots.length) i = 0;
      at = i;
      setLarge(text(shots[i].full) || text(shots[i].thumb));
      dots.forEach(function (d, j) {
        d.style.background = (j === i) ? LINK_TEXT : "rgba(255,255,255,0.28)";
      });
    }

    /*
     * One chevron hit area, full height of the image and dark enough to keep a
     * white glyph legible over any screenshot. It lives inside the frame, which
     * is position relative and overflow hidden, so it cannot escape the art;
     * the z-index puts it over the img, which capsule() draws with none.
     */
    function chevron(glyph, step, edge) {
      var c = style(el("div", null, glyph), {
        position: "absolute", top: "0", bottom: "0", width: "40px",
        display: "none", alignItems: "center", justifyContent: "center",
        fontSize: "24px", fontWeight: "bold", color: "#ffffff", zIndex: "2",
        background: "rgba(0,0,0,0.35)", cursor: "pointer",
        textShadow: "0 1px 2px rgba(0,0,0,0.8)"
      });
      c.style[edge] = "0";
      // .luaflipper-row owns the only hover rule in the stylesheet and this is
      // not a row, so its hover is wired here.
      c.addEventListener("mouseenter", function () {
        c.style.background = "rgba(0,0,0,0.62)";
      });
      c.addEventListener("mouseleave", function () {
        c.style.background = "rgba(0,0,0,0.35)";
      });
      c.addEventListener("click", function () { pick(at + step); });
      return c;
    }

    var prev = chevron("\u2039", -1, "left");
    var next = chevron("\u203a", 1, "right");
    frame.appendChild(prev);
    frame.appendChild(next);

    // Only until a screenshot lands. After that the frame belongs to wherever
    // the user moved to, and appdetails and assets both answer late.
    function hero(url) {
      if (!live) setLarge(url);
    }

    function show(list) {
      // An empty list would pick a shot that is not there. Callers already
      // check; this makes it safe to forget.
      if (!list.length) return;
      shots = list;
      live = true;
      clear(rail);
      dots = [];

      var many = shots.length > 1;
      prev.style.display = many ? "flex" : "none";
      next.style.display = many ? "flex" : "none";
      rail.style.display = many ? "flex" : "none";

      if (many) {
        shots.forEach(function (s, i) {
          var d = style(el("div"), {
            width: "10px", height: "10px", flex: "0 0 10px",
            borderRadius: "5px", background: "rgba(255,255,255,0.28)",
            cursor: "pointer"
          });
          // The dots are 10px of unlabelled circle, so what they select has to
          // be reachable some other way.
          d.title = "Screenshot " + (i + 1) + " of " + shots.length;
          d.addEventListener("click", function () { pick(i); });
          dots.push(d);
          rail.appendChild(d);
        });
      }
      pick(0);
    }

    return { node: box, hero: hero, show: show };
  }

  function addPage(data, el) {
    var err = errorEl(data, el);
    if (err) return err;

    var wrap = el("div");
    var browse = el("div");
    var game = el("div");
    game.style.display = "none";
    wrap.appendChild(browse);
    wrap.appendChild(game);

    // Every fetch path ends in one of these two, so no view can be left showing
    // "Loading" because a promise settled in a way nobody handled.
    function say(node, cls, msg) {
      return only(node, el("div", cls, msg));
    }

    // Quieter than .luaflipper-empty, which is a 32px tall centred block and
    // dwarfs the single carousel row it stands in for.
    function quiet(node, msg) {
      return only(node, style(el("div", null, msg), {
        fontSize: "12px", opacity: "0.55", padding: "16px 2px"
      }));
    }

    /* -------------------------------------- view 1: the store front --- */

    // The store's search: a dark inset field with the action button butted
    // against it inside the same rounded box.
    var bar = style(el("div"), {
      display: "flex", alignItems: "center", gap: "14px", marginBottom: "22px"
    });
    var field = style(el("div"), {
      display: "flex", alignItems: "stretch", flex: "0 1 420px",
      background: INSET, borderRadius: "3px", overflow: "hidden"
    });
    var input = el("input", "luaflipper-input");
    input.type = "text";
    input.placeholder = "search the store";
    // The field draws the rounding and the fill, so the input inside it must
    // draw neither or it shows through as a box inside a box.
    style(input, {
      border: "0", borderRadius: "0", background: "transparent",
      padding: "0 12px", height: "30px"
    });
    var go = storeBtn(el, "Search", BLUE, BLUE_HOT);
    style(go, { borderRadius: "0", fontSize: "13px" });
    field.appendChild(input);
    field.appendChild(go);
    bar.appendChild(field);

    /*
     * Steam's store body, bled to the window edges behind the top of the page.
     *
     * The card is a blue panel on a blue-grey page on the real store, and on the
     * theme's own near-black surface it read as a black box floating in space.
     * The negative offsets are .luaflipper-body's 18px/24px padding, which is
     * exactly what this has to escape to reach the edges; it is absolute inside
     * the view rather than the scroller, so it scrolls away with the content the
     * way the app page's backdrop does.
     */
    style(browse, { position: "relative" });
    browse.appendChild(style(el("div"), {
      position: "absolute", top: "-18px", left: "-24px",
      width: "calc(100% + 48px)", height: "900px", zIndex: "0",
      pointerEvents: "none", background: PAGE_WASH
    }));

    // The installed count lived here, but Manage already answers that question
    // and it competed with the search field for the same row.
    // Both stack above the wash; without this they paint under it.
    style(bar, { position: "relative", zIndex: "1" });
    browse.appendChild(bar);

    var out = style(el("div"), { position: "relative", zIndex: "1" });
    browse.appendChild(out);

    // Built once and re-shown rather than rebuilt, so emptying the search box
    // is instant and does not fire the store front's requests again.
    var recommended = el("div");

    function showRecommended() { only(out, recommended); }

    /**
     * The Featured & Recommended card, ported from the SteamClone base's
     * .featured block: a large capsule on the left, a dark detail panel on the
     * right holding the name, the review line, the base's 2x2 grid of small
     * shots, and a bottom row carrying the price where Steam carries it.
     *
     * It pages through the top sellers, which is the same list the carousel
     * under it draws, so the two share one reply.
     */
    function featuredCard() {
      var sec = style(el("div"), { marginBottom: "26px" });
      sec.appendChild(bigHead(el, "Featured & Recommended"));
      var body = el("div");
      sec.appendChild(body);
      recommended.appendChild(sec);
      quiet(body, "Loading...");

      ask("featured?list=top_sellers")
        .then(function (res) {
          if (!res || typeof res !== "object") throw new Error("unreadable reply");
          if (res.error) { quiet(body, text(res.error)); return; }

          var games = arr(res.results).filter(function (g) {
            return g && appid(g.appid);
          });
          if (!games.length) {
            quiet(body, "Steam returned nothing to feature.");
            return;
          }
          only(body, buildFeatured(games));
        })
        .catch(function (e) {
          // A dead featured list is not worth an error box, for the same reason
          // a dead carousel is not: the search field above it still works.
          quiet(body, "Featured unavailable: " + why(e));
        });
    }

    function buildFeatured(games) {
      var box = el("div");
      var at = 0;
      // Two page turns can leave two sets of requests in flight, and the loser
      // would write a review line and four screenshots over the card the user
      // is actually looking at.
      var turn = 0;
      // An arrow that cannot move is a control that lies, and one dot is a rail
      // that cannot select anything.
      var many = games.length > 1;

      /*
       * The carousel stage: the card between slivers of the card either side of
       * it, which is what says "there is more of this" before an arrow is ever
       * pressed. The arrows are absolute children of the stage rather than of
       * the card, so they land on the neighbours the way the store draws them;
       * out-of-flow children take no part in the flex row around them.
       */
      var stage = style(el("div"), {
        position: "relative", display: "flex", alignItems: "stretch",
        gap: PEEK_GAP, maxWidth: FEAT_MAX
      });
      box.appendChild(stage);

      function step(n) {
        // Wraps both ways: an arrow that stops dead on the last game reads as
        // broken, and Steam's own featured strip wraps.
        at = (at + n + games.length) % games.length;
        paint();
      }

      /**
       * One peeking neighbour.
       *
       * The image is full card height and its natural width at that height, so
       * the slot's overflow crops it to a sliver without anyone measuring the
       * card: anchored to the slot's far edge it shows the tail of the previous
       * card, to its near edge the head of the next one. Dimmed, because these
       * are context rather than content and the card has to win.
       */
      function peekArt(edge, n) {
        var p = style(el("div"), {
          position: "relative", flex: "0 0 " + PEEK_W, overflow: "hidden",
          background: INSET, cursor: "pointer",
          display: many ? "block" : "none"
        });
        var img = el("img");
        style(img, {
          position: "absolute", top: "0", height: "100%", width: "auto",
          maxWidth: "none", display: "block"
        });
        img.alt = "";
        img.style[edge] = "0";
        p.appendChild(img);
        p.appendChild(style(el("div"), {
          position: "absolute", top: "0", left: "0", width: "100%",
          height: "100%", background: "rgba(0,0,0,0.55)"
        }));
        // Clicking a neighbour pages to it, which is what it looks like it does.
        p.addEventListener("click", function () { step(n); });
        return { node: p, img: img };
      }

      var peekPrev = peekArt("right", -1);
      var peekNext = peekArt("left", 1);

      // The base's .featured: a flex row under one box shadow. Its columns are
      // grow factors over a zero basis so the 70/30 split is of the row rather
      // than of whatever each column happens to contain.
      var card = style(el("div"), {
        flex: "1 1 auto", minWidth: "0", display: "flex", alignItems: "stretch",
        overflow: "hidden", borderRadius: "3px",
        boxShadow: "2px 0 9px rgba(0,0,0,0.55)", cursor: "pointer"
      });

      var artCol = style(el("div"), {
        flexGrow: "70", flexBasis: "0", minWidth: "0"
      });
      // The one opaque surface on this page rather than alpha over the theme.
      // Near-white over #67c1f5 is what makes this read as Steam's featured
      // card, and both of those go illegible the moment a light skin shows
      // through, so the store's own panel blue stays solid behind them.
      var panel = style(el("div"), {
        flexGrow: "30", flexBasis: "0", minWidth: "0", display: "flex",
        flexDirection: "column", background: CARD_BG, color: CARD_TEXT,
        boxSizing: "border-box", padding: "10px", overflow: "hidden"
      });
      card.appendChild(artCol);
      card.appendChild(panel);

      stage.appendChild(peekPrev.node);
      stage.appendChild(card);
      stage.appendChild(peekNext.node);

      /*
       * One page arrow, sitting on the neighbour it pages to: same width as the
       * peek, full height of the stage, and dark enough to keep a white glyph
       * legible over any capsule.
       */
      function arrow(glyph, n, edge) {
        var a = style(el("div", null, glyph), {
          position: "absolute", top: "0", bottom: "0", width: PEEK_W,
          display: many ? "flex" : "none", alignItems: "center",
          justifyContent: "center", fontSize: "26px", fontWeight: "bold",
          color: "#ffffff", zIndex: "3", background: "rgba(0,0,0,0.2)",
          cursor: "pointer", textShadow: "0 1px 2px rgba(0,0,0,0.8)"
        });
        a.style[edge] = "0";
        // .luaflipper-row owns the only hover rule in the stylesheet and this is
        // not a row, so its hover is wired here.
        a.addEventListener("mouseenter", function () {
          a.style.background = "rgba(0,0,0,0.5)";
        });
        a.addEventListener("mouseleave", function () {
          a.style.background = "rgba(0,0,0,0.2)";
        });
        a.addEventListener("click", function () { step(n); });
        return a;
      }

      stage.appendChild(arrow("\u2039", -1, "left"));
      stage.appendChild(arrow("\u203a", 1, "right"));

      // Reads `at` rather than closing over an id, because the card outlives
      // every game shown in it. The arrows and the peeks are siblings of the
      // card rather than children, so neither can reach this by bubbling.
      card.addEventListener("click", function () {
        var g = games[at];
        openGame(appid(g.appid), text(g.name));
      });

      // Centred under the stage rather than under the window, so the rail sits
      // with the thing it pages instead of drifting right on a wide monitor.
      var rail = style(el("div"), {
        display: many ? "flex" : "none", justifyContent: "center",
        alignItems: "center", gap: "4px", marginTop: "8px", maxWidth: FEAT_MAX
      });
      var dots = [];
      games.forEach(function (g, i) {
        var d = style(el("div"), {
          width: "12px", height: "12px", flex: "0 0 12px", cursor: "pointer",
          background: LIFT
        });
        // 12px of unlabelled square, so what it selects has to be reachable
        // some other way.
        d.title = text(g.name) || ("App " + appid(g.appid));
        d.addEventListener("click", function () { at = i; paint(); });
        dots.push(d);
        rail.appendChild(d);
      });
      box.appendChild(rail);

      /**
       * One thumbnail of the base's .feature-images-small grid.
       *
       * capsule() with an empty ladder is the fixed-ratio frame plus the
       * hide-on-404 the rest of the page already uses, so a screenshot Steam
       * has moved cannot leave a broken glyph here. The cap goes on a slot
       * rather than the frame because a padding-top percentage resolves against
       * the parent, not against the element's own width.
       */
      function smallShot(url) {
        var setters = [];
        var s = slot(el, THUMB_W,
                     capsule(el, "", [], THUMB_RATIO, setters));
        s.style.margin = "3px";
        setters[0](url);
        return s;
      }

      // ladder() hides an image it could not fill, and only its setter puts the
      // display back, so a slot that failed for one game would stay invisible
      // for every game after it without the explicit unhide.
      //
      // The rescue goes on these too. A peek is the whole point of the stage and
      // half the top sellers are new enough to 404 on the flat path, so leaving
      // them out meant the carousel lost an edge as soon as a neighbour was a
      // recent release. ladder() reassigns onerror from scratch, so the wrapper
      // is rebuilt for this app rather than stacked on the last one's.
      function showPeek(p, g) {
        var id = appid(g.appid);
        p.img.style.display = "block";
        rescueArt(p.node, id, ladder(p.img, id, CAP));
      }

      /*
       * Rebuilt per game rather than updated in place: capsule() binds its CDN
       * fallback ladder to the app id it was built with, so a new game needs a
       * new image either way, and rebuilding is what guarantees a card can
       * never show one game's name over another's screenshots.
       */
      function paint() {
        var g = games[at];
        var id = appid(g.appid);
        var name = text(g.name) || ("App " + id);
        var mine = ++turn;

        clear(artCol);
        clear(panel);
        dots.forEach(function (d, i) {
          d.style.background = (i === at) ? LINK : LIFT;
        });

        // The neighbours, down the same CDN ladder as everything else. No
        // /api/assets rescue behind them: a 44px sliver that finds nothing
        // resolves to the dark inset it sits on, which under the dim and the
        // arrow is what a peeking neighbour looks like anyway.
        showPeek(peekPrev, games[(at - 1 + games.length) % games.length]);
        showPeek(peekNext, games[(at + 1) % games.length]);

        var setters = [];
        var big = capsule(el, id, HERO, FEAT_RATIO, setters);
        // The card owns the rounding, so the capsule inside it must not round
        // its own corners as well or a hairline of card shows at each one.
        style(big, { borderRadius: "0" });
        artCol.appendChild(big);

        // main_capsule is the 616x353 art Steam draws in this exact slot, so it
        // beats the header once it lands and nothing may paint over it after.
        var best = false;

        function setArt(url, strong) {
          if (!url || (best && !strong)) return;
          if (strong) best = true;
          setters[0](url);
        }

        var head = style(el("div"), { flex: "0 0 auto" });
        head.appendChild(style(el("div", null, name), {
          fontSize: "20px", lineHeight: "26px",
          overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
        }));
        // Empty until reviews answer, and left empty for good if they do not: a
        // score nobody can read is worse than no line at all.
        var review = style(el("div"), {
          color: LINK, fontSize: "12px", lineHeight: "17px", marginTop: "2px",
          overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
        });
        head.appendChild(review);
        panel.appendChild(head);

        // The base's .feature-images-small. Halves rather than its fixed 160px,
        // so the grid still reads as 2x2 when the panel is 30% of a narrow
        // window; the 3px margins are the base's own gaps. Sits straight under
        // the review line: the store groups name, status and shots together at
        // the top and the panel is taller than that group, so spreading them
        // apart just opened a band of dead panel in the middle.
        // Shrinkable, and the only shrinkable thing in the panel: on a window
        // narrow enough that the group no longer fits, a clipped second row of
        // thumbnails is a far better failure than a clipped price.
        var shots = style(el("div"), {
          display: "flex", flexWrap: "wrap", justifyContent: "center",
          margin: "8px 0 0", flex: "0 1 auto", minHeight: "0",
          overflow: "hidden"
        });
        panel.appendChild(shots);

        // The base's .tomb-bottom .bottom-text. marginTop auto is what pins it
        // to the floor while the group above it stays packed at the ceiling.
        var bottom = style(el("div"), {
          display: "flex", alignItems: "center", justifyContent: "space-between",
          gap: "10px", height: "50px", flex: "0 0 auto", marginTop: "auto"
        });
        // "Free" on its own is already true, so it says that and upgrades to a
        // count if one arrives. Nothing here ever says "checking": the probe
        // behind it reaches a remote host and can take seconds or never answer.
        var avail = style(el("div", null, "Free"), {
          fontSize: "12px", color: LINK, minWidth: "0", overflow: "hidden",
          textOverflow: "ellipsis", whiteSpace: "nowrap"
        });
        bottom.appendChild(avail);
        // Where the store prints the price. Nothing here costs money, and
        // Steam's own discount colours are the fastest way to say so.
        bottom.appendChild(style(el("div", null, "FREE"), {
          flex: "0 0 auto", background: DEEP, color: "#a4d007",
          fontWeight: "bold", fontSize: "13px", padding: "0 12px",
          lineHeight: "26px", borderRadius: "2px", whiteSpace: "nowrap"
        }));
        panel.appendChild(bottom);

        // Everything below is lazy and per game. `mine !== turn` means the user
        // paged on while this was in flight, and these nodes are already gone.

        ask("appdetails?appid=" + encodeURIComponent(id))
          .then(function (res) {
            if (mine !== turn) return;
            var ok = usable(res);
            if (!ok) return;
            setArt(text(ok.header), false);
            // Four, because the base's grid is 2x2. The backend caps the list
            // at 12 and the other eight have nowhere to go on this card.
            arr(ok.screenshots).slice(0, 4).forEach(function (s) {
              var url = s ? (text(s.thumb) || text(s.full)) : "";
              if (url) shots.appendChild(smallShot(url));
            });
          })
          .catch(skip);

        ask("assets?appid=" + encodeURIComponent(id))
          .then(function (res) {
            if (mine !== turn) return;
            var ok = usable(res);
            if (!ok) return;
            // The authoritative hashed URL. The flat CDN paths the ladder walks
            // 404 outright for anything Steam published after it moved store
            // art behind store_item_assets, which is what leaves a card blank.
            setArt(text(ok.main_capsule) || text(ok.header), true);
          })
          .catch(skip);

        ask("reviews?appid=" + encodeURIComponent(id))
          .then(function (res) {
            if (mine !== turn) return;
            var ok = usable(res);
            var summary = ok ? text(ok.summary) : "";
            if (!summary) return;
            var all = num(ok.total);
            review.textContent = all
              ? summary + " (" + comma(all) + " Reviews)"
              : summary;
          })
          .catch(skip);

        ask("sources?appid=" + encodeURIComponent(id))
          .then(function (res) {
            if (mine !== turn) return;
            var ok = usable(res);
            if (!ok) return;
            // The same filter the app page's Add button uses, so this line and
            // that button cannot disagree about how many sources there are.
            var live = arr(ok.sources).filter(function (s) {
              return installable(s);
            });
            avail.textContent = live.length
              ? "Free - " + plural(live.length, "source")
              : "No source carries this app";
          })
          .catch(skip);
      }

      paint();
      return box;
    }

    /**
     * One "New & Trending" style carousel.
     *
     * The section is appended before its request answers, so the two rows keep
     * the order they were asked for whichever one comes back first.
     */
    function shelf(title, list) {
      var sec = style(el("div"), { marginBottom: "26px" });
      sec.appendChild(sectionHead(el, title));
      var body = el("div");
      sec.appendChild(body);
      recommended.appendChild(sec);
      quiet(body, "Loading...");

      // Shared with the featured card above, which pages through this same
      // top_sellers list and should not ask for it a second time.
      ask("featured?list=" + encodeURIComponent(list))
        .then(function (res) {
          if (!res || typeof res !== "object") throw new Error("unreadable reply");
          if (res.error) { quiet(body, text(res.error)); return; }

          var games = arr(res.results).filter(function (g) {
            return g && appid(g.appid);
          });
          if (!games.length) {
            quiet(body, "Steam returned nothing for this list.");
            return;
          }
          // flex-start, not the default stretch: otherwise every tile grows to
          // the tallest one and the row gains a band of dead space.
          var strip = style(el("div"), {
            display: "flex", alignItems: "flex-start", gap: "12px",
            overflowX: "auto", paddingBottom: "8px"
          });
          games.forEach(function (g) {
            var tile = capsuleCard(el, g, openGame);
            // A flex item's automatic minimum is its content, and the name is
            // nowrap, so without min-width the longest title widens its tile
            // past the basis and the row stops being a row of equal capsules.
            style(tile, { flex: "0 0 200px", minWidth: "0" });
            strip.appendChild(tile);
          });
          only(body, strip);
        })
        .catch(function (e) {
          // A dead featured list is not worth an error box: the search field
          // above it still works, which is the point of the page.
          quiet(body, title + " unavailable: " + why(e));
        });
    }

    // Two searches in flight can answer out of order, and the loser would
    // overwrite the results the user is actually looking at.
    var seq = 0;
    var timer = null;

    function search() {
      if (timer) { clearTimeout(timer); timer = null; }
      var term = (input.value || "").trim();

      // The sequence bumps even for an empty box, so a search still in flight
      // cannot land on top of the recommended rows the user just came back to.
      var mine = ++seq;
      if (!term) { showRecommended(); return; }

      quiet(out, "Searching for " + term + "...");
      fetch(API + "search?term=" + encodeURIComponent(term))
        .then(function (r) { return r.json(); })
        .then(function (res) {
          if (mine !== seq) return;
          if (!res || typeof res !== "object") throw new Error("unreadable reply");
          if (res.error) { say(out, "luaflipper-error", text(res.error)); return; }

          var hits = arr(res.results).filter(function (g) {
            return g && appid(g.appid);
          });
          if (!hits.length) {
            noResults("Nothing found for " + term + ".", term);
            return;
          }
          showResults(hits, term);
        })
        .catch(function (e) {
          if (mine !== seq) return;
          noResults("Search failed: " + why(e), term);
        });
    }

    function showResults(list, term) {
      var box = el("div");
      box.appendChild(sectionHead(el, plural(list.length, "result") + " for " + term));
      list.forEach(function (g) { box.appendChild(resultRow(el, g, openGame)); });
      only(out, box);
    }

    // The page this replaces looked sources up by raw app id. Search will not
    // resolve every id, and can be down entirely, so that route stays open
    // whenever the term is an id rather than a name.
    function noResults(msg, term) {
      var box = el("div");
      box.appendChild(style(el("div", null, msg), {
        fontSize: "13px", opacity: "0.6", padding: "24px 2px 12px"
      }));
      if (/^[0-9]+$/.test(term)) {
        var open = storeBtn(el, "Open app " + term, BLUE, BLUE_HOT);
        style(open, { display: "inline-block" });
        open.addEventListener("click", function () { openGame(term, ""); });
        box.appendChild(open);
      }
      only(out, box);
    }

    /* ---------------------------------------------- view 2: app page --- */

    function openGame(id, name) {
      only(game, buildGame(id, name));
      browse.style.display = "none";
      game.style.display = "";

      // The scroll lives on .luaflipper-body, our parent, so an app opened from
      // halfway down the results would otherwise open halfway down the page.
      var scroller = wrap.parentNode;
      if (scroller && typeof scroller.scrollTop === "number") scroller.scrollTop = 0;
    }

    function showBrowse() {
      game.style.display = "none";
      browse.style.display = "";
      // Dropping the app page cancels nothing in flight, but it does mean an
      // install that finishes after this writes to nodes nobody can see.
      clear(game);
    }

    function buildGame(id, name) {
      var title = text(name) || ("App " + id);
      // Positioned, so the page background can be pinned behind the top of the
      // page and still scroll away with it. Spans the window rather than the
      // design's 706 column, and every split inside it is proportional, so
      // widening the page grows the gallery and the details block together.
      var v = style(el("div"), { position: "relative" });

      var back = backdrop(el);
      v.appendChild(back.node);

      // Everything else stacks above that art.
      var page = style(el("div"), { position: "relative", zIndex: "1" });
      v.appendChild(page);

      // One preferred-URL setter per image on the page. Each image starts its
      // CDN ladder immediately so art appears without waiting on a request;
      // these replay the store's own hashed URLs over them once one answers.
      // The icon keeps its own list because /api/assets has a real icon for it
      // and the header is only a square crop standing in until that lands.
      var art = [];
      var iconArt = [];

      /* -------------------------------------------------- title frame --- */

      // Steam's store has no Back button either: the breadcrumb is the way out
      // of an app page, so "All Games" is a link rather than decoration. The
      // design stacks it directly above the title, 13 tall over 22 (17 over 29
      // at 1:1).
      var crumbs = style(el("div"), {
        fontSize: "12px", lineHeight: "17px", marginBottom: "5px"
      });

      // The separators carry the dimming, not the line: opacity on the whole
      // crumb would take the blue out of the links with it.
      function crumbSep() {
        return style(el("span", null, " > "), { opacity: "0.45" });
      }

      var home = style(el("span", null, "All Games"), {
        cursor: "pointer", color: LINK_TEXT
      });
      home.title = "Back to browsing";
      home.addEventListener("click", showBrowse);
      crumbs.appendChild(home);
      crumbs.appendChild(crumbSep());
      // Empty until appdetails names a genre. Holding the slot keeps the crumb
      // in order without the line being rebuilt when that request answers.
      var genreCrumb = el("span");
      crumbs.appendChild(genreCrumb);
      crumbs.appendChild(style(el("span", null, title), { color: GRAY }));
      page.appendChild(crumbs);

      var titleRow = style(el("div"), {
        display: "flex", alignItems: "center", gap: "10px", marginBottom: "14px"
      });
      titleRow.appendChild(appIcon(el, id, iconArt));
      // inherit rather than the design's Light Gray: this line sits on the
      // theme's own surface wherever the background art has faded out, and a
      // hardcoded near-white would disappear on a light skin. On the stock
      // client inherit already resolves to it.
      titleRow.appendChild(style(el("div", null, title), {
        flex: "1", minWidth: "0", fontSize: "26px", fontWeight: "300",
        lineHeight: "29px"
      }));
      // A label, not a control. Nothing in this page can open a browser, so a
      // button shaped like a link that navigates nowhere would be a lie; it
      // sits here because the store's own app page has one in this corner.
      titleRow.appendChild(style(el("div", null, "SteamDB"), {
        flex: "0 0 auto", background: BLUE, color: "#ffffff", fontSize: "12px",
        fontWeight: "bold", padding: "0 14px", lineHeight: "26px",
        borderRadius: "2px", textShadow: "1px 1px rgba(0,0,0,0.2)"
      }));
      page.appendChild(titleRow);

      /* ---------------------------------------------------- main info --- */

      // The design's Main Info: Gallery 450 beside Game Overview 244 over a
      // 12px gutter. Grow factors rather than percentage bases, so that split
      // survives the gutter being subtracted from the row.
      var main = style(el("div"), {
        display: "flex", gap: GUTTER, alignItems: "flex-start",
        marginBottom: "18px"
      });
      var left = style(el("div"), {
        flexGrow: GAL_GROW, flexBasis: "0", minWidth: "0"
      });
      var right = style(el("div"), {
        flexGrow: OVR_GROW, flexBasis: "0", minWidth: "0"
      });
      main.appendChild(left);
      main.appendChild(right);
      page.appendChild(main);

      var gal = gallery(el, id);
      left.appendChild(gal.node);

      // The design's Game Cover, 244x114, which is header.jpg's own ratio. The
      // 460px cap is that header's native width: past it the art would upscale,
      // so on a very wide window the capsule stops growing and the details
      // under it keep the column.
      right.appendChild(slot(el, "460px", capsule(el, id, SHOT, COVER_RATIO, art)));

      // Replaced by the store's own blurb when appdetails answers. Until then,
      // and for good if it never does, it says the one thing this page exists
      // to say, so the column is never blank. Clamped to the design's 244x70
      // description box (five 14px rows); the full text goes under About below.
      var blurb = style(el("div", null,
        "Unlocking writes a manifest into config/stplug-in. Steam picks the " +
        "ownership up live; the depot keys inside that manifest are a " +
        "separate step."), {
        fontSize: "13px", lineHeight: ROW_H, color: GRAY, marginTop: "11px",
        display: "-webkit-box", WebkitBoxOrient: "vertical",
        WebkitLineClamp: "5", overflow: "hidden"
      });
      right.appendChild(blurb);

      // The design's Details block: 14px rows in groups, an 8-10px spacer
      // between them. No panel behind it, the way the store prints it straight
      // onto the page background.
      var details = style(el("div"), { marginTop: "11px" });
      // Both empty until their requests answer. Holding the slots here puts the
      // review line and the store's own rows above ours, in the order the store
      // prints them, without the block being torn down and rebuilt.
      var reviewSlot = el("div");
      var storeMeta = el("div");
      details.appendChild(reviewSlot);
      details.appendChild(storeMeta);
      details.appendChild(style(el("div"), { height: GROUP_GAP }));
      metaRow(el, details, "App ID", id);
      var foundVal = metaRow(el, details, "Sources found", "checking");
      var installedVal = metaRow(el, details, "Installed", "checking");
      right.appendChild(details);

      // Hidden until there are genres to put in it: an empty tag block reads as
      // "this app has no tags", which is not what a failed lookup means.
      var tagBox = style(el("div"), { display: "none", marginTop: GROUP_GAP });
      tagBox.appendChild(style(el("div", null,
        "Popular user-defined tags for this product:"), {
        fontSize: "12px", lineHeight: ROW_H, color: GRAY
      }));
      // The design sets the tag row 2px under its label and the pills 2px apart
      // (3 at 1:1), so the block reads as one row of the details table.
      var tags = style(el("div"), {
        display: "flex", flexWrap: "wrap", gap: "3px", marginTop: "3px"
      });
      tagBox.appendChild(tags);
      right.appendChild(tagBox);

      /* ---------------------------------------------- purchase panel --- */

      // The purchase area, the panel this page is recognised by: what you get
      // on the left, what it costs and the button on the right. The design's
      // Buy Widget is 462x54 (616x72 at 1:1) - one row, not a stack, which is
      // how the store draws it for anything that costs nothing.
      var buy = style(el("div"), {
        display: "flex", alignItems: "center", justifyContent: "space-between",
        gap: "16px", width: BUY_WIDTH, minHeight: "72px",
        boxSizing: "border-box", background: PANEL, borderRadius: "3px",
        padding: "0 14px"
      });
      var offer = style(el("div"), { flex: "1", minWidth: "0" });
      offer.appendChild(style(el("div", null, "Unlock " + title), {
        fontSize: "17px", fontWeight: "500", lineHeight: "22px"
      }));
      var sub = style(el("div", null, "Checking which sources carry this app..."), {
        fontSize: "12px", lineHeight: "17px", color: LINK_TEXT, marginTop: "2px"
      });
      offer.appendChild(sub);
      buy.appendChild(offer);
      var actions = style(el("div"), {
        display: "flex", justifyContent: "flex-end", flex: "0 0 auto"
      });
      buy.appendChild(actions);
      page.appendChild(buy);

      // Install results land under the panel, not inside it, so a long list of
      // rejected entries cannot push the buttons around. Above About rather
      // than below it, so the answer to a click is next to the button that was
      // clicked instead of past a screen of store copy. Same width as the
      // panel it reports on, so the two read as one block; empty, its margin
      // collapses into About's and costs nothing.
      var note = style(el("div"), {
        width: BUY_WIDTH, boxSizing: "border-box", marginTop: "14px"
      });
      page.appendChild(note);

      /* ------------------------------------------------------- about --- */

      // The design's About section under the Buy Widget, same 462 width, under
      // a 17px Section Header (23 at 1:1). Hidden until there is copy for it,
      // because a header over nothing reads as a store page that failed to
      // load rather than a lookup that did not answer.
      var about = style(el("div"), {
        display: "none", width: BUY_WIDTH, boxSizing: "border-box",
        marginTop: "18px"
      });
      about.appendChild(style(sectionHead(el, "About This Game"), {
        lineHeight: "23px", margin: "0 0 8px", paddingBottom: "4px",
        borderBottom: "1px solid rgba(103,193,245,0.4)"
      }));
      // pre-wrap: the store writes descriptions in paragraphs, and collapsing
      // them would run several of them into one wall of text.
      var aboutText = style(el("div"), {
        fontSize: "13px", lineHeight: ROW_H, color: GRAY, whiteSpace: "pre-wrap"
      });
      about.appendChild(aboutText);
      page.appendChild(about);

      // The price box's status line. Built here rather than inside addRow so
      // the manifest check can write to it whichever of the two answers first.
      var statusVal = style(el("div", null, "checking"), {
        background: DEEP, color: LINK_TEXT, fontSize: "13px", padding: "0 12px",
        lineHeight: "30px", whiteSpace: "nowrap"
      });

      function setInstalled(have) {
        installedVal.textContent = have ? "yes" : "no";
        statusVal.textContent = have ? "in library" : "not in library";
      }

      /* ---------------------------------------------------- requests --- */

      /*
       * Dispatch order is load bearing here, and this page got it wrong.
       *
       * The backend serves one connection at a time and closes each
       * (Connection: close), and /api/sources probes remote manifest hosts, so
       * it alone runs to about two seconds. Behind three requests that only
       * fetch art and copy it is the last thing to land, and until it does the
       * page shows no Add button and two rows reading "checking" - the one
       * state on this page a user cannot act on or learn anything from.
       *
       * So the two requests that drive the button go out first and alone, and
       * the three cosmetic ones are chained single file behind each other
       * rather than fired in a burst. That holds this page to three sockets,
       * which is what it opened before it needed art, and it puts the answer
       * the user is waiting for at the front of a queue instead of the back.
       */

      function json(r) { return r.json(); }
      function ignore() { /* every request below this line is optional */ }

      // The real app icon, once /api/assets hands it over. Latched, because
      // appdetails answers separately and its header would crop back over it.
      var haveIcon = false;

      function setIcon(url) {
        if (!url) return;
        haveIcon = true;
        iconArt[0](url);
      }

      /*
       * The store's own hashed art, replayed over every image already drawing
       * its CDN ladder. The flat CDN paths 404 for everything Steam published
       * after it moved that art to store_item_assets, so for newer apps these
       * are the only addresses that resolve at all.
       */
      function setHeader(url) {
        if (!url) return;
        art.forEach(function (set) { set(url); });
        if (!haveIcon) iconArt[0](url);
        gal.hero(url);
      }

      /*
       * "checking" is a promise, not a result.
       *
       * Every path below writes a real value on success and on failure, but
       * only if its promise settles at all: a backend restarted mid-request, or
       * a request CEF is holding in a full per-host socket pool, settles
       * neither way and would leave both rows saying "checking" for good. A
       * deadline is the only thing that makes "never stuck" a property of this
       * page rather than a hope about the network.
       */
      setTimeout(function () {
        if (foundVal.textContent === "checking") {
          foundVal.textContent = "no answer";
          sub.textContent = "Source lookup did not answer. Reopen the app to " +
                            "try again.";
        }
        if (installedVal.textContent === "checking") {
          installedVal.textContent = "no answer";
        }
        if (statusVal.textContent === "checking") {
          statusVal.textContent = "unknown";
        }
      }, 30000);

      fetch(API + "sources?appid=" + encodeURIComponent(id))
        .then(json)
        .then(function (res) {
          if (!res || typeof res !== "object") throw new Error("unreadable reply");
          if (res.error) {
            foundVal.textContent = "error";
            sub.textContent = "Source lookup failed.";
            only(note, el("div", "luaflipper-error", text(res.error)));
            return;
          }
          var found = arr(res.sources).filter(function (s) {
            return s && text(s.name);
          });
          foundVal.textContent = String(found.length);
          if (!found.length) {
            sub.textContent = "No source carries this app.";
            return;
          }
          sub.textContent = "Free via " + plural(found.length, "manifest source");
          // The names lost their own line when the panel took the store's two
          // row shape, and which sources answered is still worth reaching.
          sub.title = found.map(function (s) {
            var n = text(s.name);
            return installable(s) ? n : n + " (" + text(s.status) + ")";
          }).join(", ");
          actions.appendChild(addRow(id, found, note, setInstalled, statusVal));
        })
        .catch(function (e) {
          foundVal.textContent = "unknown";
          sub.textContent = "Source lookup failed.";
          only(note, el("div", "luaflipper-error", "Lookup failed: " + why(e)));
        });

      // INSTALLED is answered from the manifest list rather than remembered, so
      // it stays right for an app added in an earlier session or by hand.
      fetch(API + "manifests")
        .then(json)
        .then(function (res) {
          var have = false;
          arr(res && res.manifests).forEach(function (m) {
            if (m && appid(m.appid) === id) have = true;
          });
          setInstalled(have);
        })
        .catch(function () {
          installedVal.textContent = "unknown";
          statusVal.textContent = "unknown";
        });

      /*
       * Steam's own store copy, for the fields no CDN path can supply, and
       * first of the cosmetic three because it carries the most of the page:
       * the blurb, the tags and the screenshots.
       *
       * Everything it fills is optional. The page is already complete when this
       * is dispatched, so a failure leaves the default blurb, the hero art in
       * the carousel, no store rows and no tags rather than a stuck spinner or
       * six rows reading "unknown".
       */
      function useDetails(res) {
        if (!res || typeof res !== "object" || res.error) return;

        setHeader(text(res.header));

        var story = text(res.description);
        if (story) {
          blurb.textContent = story;
          aboutText.textContent = story;
          about.style.display = "";
        }

        // The carousel takes the frame over from here, so a shot the user moved
        // to cannot be undone by a header URL answering after them. The backend
        // caps the list at 12; the slice says so out loud.
        var shots = arr(res.screenshots).filter(function (s) {
          return s && (text(s.thumb) || text(s.full));
        });
        if (shots.length) gal.show(shots.slice(0, 12));

        // Appended, so these land in the store's own order above App ID. A
        // field the store left blank is skipped rather than printed empty.
        var made = arr(res.developers).join(", ");
        var sold = arr(res.publishers).join(", ");
        if (text(res.released)) {
          metaRow(el, storeMeta, "Release date", text(res.released));
        }
        if (made) metaRow(el, storeMeta, "Developer", made);
        if (sold) metaRow(el, storeMeta, "Publisher", sold);

        var genres = arr(res.genres).map(function (g) {
          return text(g && g.description);
        }).filter(function (g) { return g !== ""; });
        if (!genres.length) return;

        // The store puts the first genre in the breadcrumb and the whole set
        // in the tag strip, so one list feeds both.
        genreCrumb.appendChild(style(el("span", null, genres[0]), {
          color: LINK_TEXT
        }));
        genreCrumb.appendChild(crumbSep());
        genres.forEach(function (g) { tags.appendChild(tagPill(el, g)); });
        tagBox.style.display = "";
      }

      /*
       * The authoritative asset URLs, and the only source for the page
       * background. Without it each image keeps whatever its CDN ladder found
       * and the page simply has no backdrop.
       */
      function useAssets(res) {
        if (!res || typeof res !== "object" || res.error) return;
        back.set(text(res.page_background));
        setHeader(text(res.header));
        // 1920 wide and the closest thing to a screenshot the asset list has,
        // so it beats the header in the frame until real ones arrive.
        gal.hero(text(res.library_hero));
        setIcon(text(res.icon));
      }

      /*
       * The review summary, the one line the store leads its details block
       * with. Dropped entirely on a failure rather than printed as "unknown":
       * a score nobody can read is worse than no row at all.
       */
      function useReviews(res) {
        if (!res || typeof res !== "object" || res.error) return;
        var summary = text(res.summary);
        if (!summary) return;
        var all = num(res.total);
        metaRow(el, reviewSlot, "Reviews",
          all ? summary + " (" + comma(all) + ")" : summary);
      }

      // Single file, with a catch between each link so one failing does not
      // cancel the two behind it.
      fetch(API + "appdetails?appid=" + encodeURIComponent(id))
        .then(json).then(useDetails).catch(ignore)
        .then(function () {
          return fetch(API + "assets?appid=" + encodeURIComponent(id))
            .then(json).then(useAssets);
        }).catch(ignore)
        .then(function () {
          return fetch(API + "reviews?appid=" + encodeURIComponent(id))
            .then(json).then(useReviews);
        }).catch(ignore);

      return v;
    }

    /**
     * The store's price box: a FREE badge, the install status, and a single
     * green Add, butted together over a dark well the way Steam butts a
     * discount badge against a price against Add to Cart.
     *
     * One button rather than one per source, because picking a source is not a
     * decision the user has any basis to make: the sources carry the same packs
     * and differ only in whether they answer. So Add walks the list in order and
     * stops at the first that installs, which is what LuaTools does with its own
     * source list. A source reported "unavailable" is skipped, but "unknown" is
     * still tried, since that only means the probe host did not answer.
     */
    function addRow(id, sources, note, setInstalled, statusVal) {
      var live = sources.filter(installable);

      // 2px of padding and 2px gaps, so the well shows as hairlines between the
      // three parts and they read as one control rather than three.
      var action = style(el("div"), {
        display: "flex", alignItems: "center", gap: "2px", flex: "0 0 auto",
        background: DEEP, borderRadius: "2px", padding: "2px"
      });

      if (!live.length) {
        // Its own node rather than statusVal: the manifest check writes to that
        // one, and would overwrite this with an install state the user has no
        // way to act on from here.
        action.appendChild(style(el("div", null, "unavailable"), {
          background: DEEP, padding: "0 12px", lineHeight: "30px",
          fontSize: "13px", whiteSpace: "nowrap", color: LABEL
        }));
        return action;
      }

      // Steam's discount badge colours. Nothing on this page costs money, and
      // the badge is the fastest way to say so where a price would be.
      action.appendChild(style(el("div", null, "FREE"), {
        background: "#4c6b22", color: "#a4d007", fontWeight: "bold",
        fontSize: "13px", padding: "4px 8px", borderRadius: "2px",
        whiteSpace: "nowrap"
      }));
      action.appendChild(statusVal);

      var btn = storeBtn(el, "Add to Library", GREEN, GREEN_HOT);
      // Fixed width, so the elapsed counter counting past 9s does not resize
      // the button and shuffle the box under the pointer. The 2px radius is the
      // well's, so the button's own corner does not float inside it.
      style(btn, { minWidth: "150px", textAlign: "center", borderRadius: "2px" });
      var busy = false;

      btn.addEventListener("click", function () {
        if (busy) return;
        busy = true;
        style(btn, { pointerEvents: "none", opacity: "0.55" });

        // A pack is a few MB over plaintext HTTP and routinely takes tens of
        // seconds, with no progress events to report. An elapsed count is the
        // honest version of a progress bar: it proves the request is still
        // alive without inventing a percentage.
        var started = Date.now();
        var label = "Adding";
        var tick = setInterval(function () {
          btn.textContent = label + " " +
            Math.round((Date.now() - started) / 1000) + "s";
        }, 1000);
        btn.textContent = label + " 0s";

        function stop(t) {
          clearInterval(tick);
          btn.textContent = t;
        }
        function retry(t) {
          busy = false;
          style(btn, { pointerEvents: "auto", opacity: "1" });
          stop(t);
        }

        var failures = [];

        // Sequential on purpose. Running the sources in parallel would download
        // several multi-MB packs to install exactly one of them, and they write
        // to the same files.
        function attempt(i) {
          if (i >= live.length) {
            retry("Retry");
            report(note, false,
              "No source could provide this app. " + failures.join(" "), null);
            return;
          }
          var name = text(live[i].name);
          label = "Adding from " + name;

          // No timeout on purpose: cutting a multi-MB download short would
          // leave half a pack on disk and blame the network for it.
          fetch(API + "install?appid=" + encodeURIComponent(id) +
                      "&source=" + encodeURIComponent(name))
            .then(function (r) { return r.json(); })
            .then(function (res) {
              if (!res || typeof res !== "object") throw new Error("unreadable reply");
              if (res.error) {
                failures.push(name + ": " + text(res.error));
                attempt(i + 1);
                return;
              }
              stop("In Library");
              setInstalled(true);
              var n = num(res.installed);
              report(note, true,
                "Added " + plural(n, "file") + " from " + name +
                ". Ownership is live immediately; depot keys need " +
                "tools/sync_depot_keys.py run with Steam closed before this " +
                "app's content will decrypt.", res.rejected);
            })
            .catch(function (e) {
              failures.push(name + ": " + why(e));
              attempt(i + 1);
            });
        }
        attempt(0);
      });

      action.appendChild(btn);
      return action;
    }

    /**
     * The outcome of an install.
     *
     * `rejected` comes back on success too, where it is the only explanation
     * for a run that reported 200 and still skipped half the archive, so it is
     * shown either way. Entries wrap: they are sentences, and .luaflipper-meta
     * would otherwise clip them to one line.
     */
    function report(note, ok, head, rejected) {
      var box = el("div", ok ? null : "luaflipper-error");
      if (ok) {
        style(box, {
          padding: "12px 14px", borderRadius: "3px", background: INSET,
          boxShadow: "inset 3px 0 0 #75b022"
        });
      }
      box.appendChild(el("div", null, head));

      var bad = arr(rejected);
      if (bad.length) {
        box.appendChild(el("div", "luaflipper-sub",
          plural(bad.length, "entry", "entries") + " skipped:"));
        bad.forEach(function (line) {
          var e = el("div", "luaflipper-meta", text(line));
          e.style.whiteSpace = "normal";
          box.appendChild(e);
        });
      }
      only(note, box);
    }

    /* ------------------------------------------------------- wiring --- */

    go.addEventListener("click", search);
    input.addEventListener("keydown", function (ev) {
      if (ev.key === "Enter") search();
    });
    // Live search. Debounced because every call is proxied straight through to
    // Steam's store, so a fast typist would otherwise fire a dozen upstream
    // requests only to throw eleven of them away.
    input.addEventListener("input", function () {
      if (timer) clearTimeout(timer);
      timer = setTimeout(search, 300);
    });

    featuredCard();
    shelf("Top sellers", "top_sellers");
    shelf("New releases", "new_releases");
    showRecommended();
    return wrap;
  }

  /* ------------------------------------------------------------- manage --- */

  /**
   * The Manage page: everything a manifest added, shown the way the library
   * shows a game.
   *
   * The library is the page a user already has for "what do I have", so this
   * borrows its shape instead of inventing a third one. Portrait capsules on a
   * grid, art filling the tile edge to edge, the name carried only where there
   * is no art to carry it: that is what Steam does, and the geometry is read off
   * Steam's own library rather than guessed at (a 150% padding-top box, cover
   * fit, the same 0 4px 8px shadow under each tile).
   *
   * It is not Steam's library filtered down. Filtering the real library would
   * mean hiding games the user actually bought; this is the list of what
   * SteamFlipper put on the account, which is the only list where Remove and
   * Update mean anything.
   *
   * What the library has no equivalent for, the key and id counts, moves into
   * the panel that appears over a tile on hover, next to the two actions. A
   * manifest with no keys is called out on the tile itself, because it is the
   * one state where the app is in the library and still cannot be played.
   *
   * /api/manifests knows a file, an app id and two counts and nothing else, so
   * names and art are looked up per tile and filled in late.
   */
  function manage(data, el) {
    var err = errorEl(data, el);
    if (err) return err;

    // Falsy entries are dropped rather than rendered, the same guard the browse
    // view puts on a results list: a throw here is caught by luaflipper.js into
    // its "backend unavailable" box, which would blame the backend for being
    // unreachable when it answered, just not with the list expected.
    var list = arr(data && data.manifests).filter(function (m) { return !!m; });

    // The page's destructive colour, taken from .luaflipper-error's own
    // rgba(220,80,80). Solid rather than that rule's 12% wash: a confirm button
    // has to stop the eye, and an alpha fill over an unknown theme cannot
    // promise that it will.
    var RED = "linear-gradient(to bottom, #d05a5a 5%, #a03a3a 95%)";
    var RED_HOT = "linear-gradient(to bottom, #e06a6a 5%, #b04545 95%)";

    function json(r) { return r.json(); }

    // Steam's store body behind the top of the page, the same wash the browse
    // view paints, so moving between the two pages does not change skins. The
    // negative offsets are .luaflipper-body's own 18px/24px padding, which is
    // what this has to escape to reach the window edges.
    var wrap = style(el("div"), { position: "relative" });
    wrap.appendChild(style(el("div"), {
      position: "absolute", top: "-18px", left: "-24px",
      width: "calc(100% + 48px)", height: "900px", zIndex: "0",
      pointerEvents: "none", background: PAGE_WASH
    }));
    // Everything else stacks above that wash; without this it paints under.
    var page = style(el("div"), { position: "relative", zIndex: "1" });
    wrap.appendChild(page);

    if (!list.length) {
      page.appendChild(el("div", "luaflipper-empty",
        "Nothing added yet. The Unlocker page is where apps are found and " +
        "added; whatever is added there shows up here."));
      return wrap;
    }

    /* -------------------------------------------------------- toolbar --- */

    // No page title. The library does not announce itself either, and the one
    // thing worth saying up here is how many of these have usable keys.
    var bar = style(el("div"), {
      display: "flex", alignItems: "center", gap: "12px", marginBottom: "14px"
    });
    var field = style(el("div"), {
      display: "flex", alignItems: "stretch", flex: "0 1 360px",
      background: INSET, borderRadius: "3px", overflow: "hidden"
    });
    var input = el("input", "luaflipper-input");
    input.type = "text";
    input.placeholder = "filter by name or app id";
    // The field draws the fill and the rounding, so the input inside it must
    // draw neither or it shows through as a box inside a box.
    style(input, {
      border: "0", borderRadius: "0", background: "transparent",
      padding: "0 12px", height: "30px"
    });
    field.appendChild(input);
    bar.appendChild(field);

    var sub = style(el("div", "luaflipper-sub"), { margin: "0", flex: "1 1 auto" });
    bar.appendChild(sub);
    // Empty unless a filter is active. A filter whose effect cannot be seen is
    // a filter that gets blamed for the missing row it did not hide.
    var shown = style(el("div"), { fontSize: "12px", opacity: "0.6" });
    bar.appendChild(shown);
    page.appendChild(bar);

    /* ----------------------------------------------------------- grid --- */

    // auto-fill rather than a column count, so the grid reflows with the window
    // the way the library's does. 152px is close to the library's own capsules
    // at its default zoom, and the fraction unit spends whatever is left over
    // widening tiles instead of leaving a gutter down the right.
    // One receipt area for the page rather than one per tile: a note under a
    // tile would tear a hole in the grid. Above it rather than below, because
    // below a hundred-odd tiles is off the bottom of the window, and a removal
    // that reports nothing reads as a removal that did nothing. Every message
    // names its game, so nothing is lost by collecting them in one place.
    var note = style(el("div"), { display: "none", marginBottom: "16px" });
    page.appendChild(note);

    var grid = style(el("div"), {
      display: "grid", gap: "16px",
      gridTemplateColumns: "repeat(auto-fill, minmax(152px, 1fr))"
    });
    page.appendChild(grid);

    var nohit = el("div", "luaflipper-empty", "");
    nohit.style.display = "none";
    page.appendChild(nohit);

    var rows = [];

    function retally() {
      var n = 0, keyless = 0;
      rows.forEach(function (e) {
        if (!e.live) return;
        n++;
        if (!e.keys) keyless++;
      });
      // A manifest with keys 0 registers ownership and cannot decrypt a byte of
      // what it owns, which is what a download that starts and then dies looks
      // like. It leads the summary rather than hiding in a tile's key count.
      sub.textContent = plural(n, "manifest") + (keyless
        ? ", " + keyless + " with no decryption keys: those register ownership " +
          "but cannot decrypt content, so their downloads start and then fail."
        : ", all carrying decryption keys.");
    }

    function refilter() {
      var q = (input.value || "").trim().toLowerCase();
      var hits = 0, total = 0;
      rows.forEach(function (e) {
        var on = !q ||
          e.id.toLowerCase().indexOf(q) !== -1 ||
          e.file.toLowerCase().indexOf(q) !== -1 ||
          (e.name !== "" && e.name.toLowerCase().indexOf(q) !== -1);
        e.box.style.display = on ? "" : "none";
        if (!e.live) return;
        total++;
        if (on) hits++;
      });
      shown.textContent = q ? hits + " of " + total + " shown" : "";
      if (q && !hits) {
        nohit.textContent = "Nothing installed matches " + q + ".";
        nohit.style.display = "";
      } else {
        nohit.style.display = "none";
      }
      resort();
    }

    /**
     * Alphabetical, the way the library is.
     *
     * /api/manifests comes back in file order, which is app id order, and a
     * grid of a hundred games in app id order is a grid nobody can find a game
     * in. Names arrive one lookup at a time, so this runs from refilter(), which
     * already runs on every arrival, and it moves tiles with the grid's own
     * order property rather than reinserting nodes: the DOM stays put, and a
     * tile mid-download or mid-confirm is not torn out from under the pointer.
     */
    function resort() {
      var live = rows.slice().sort(function (a, b) {
        var x = (a.name || a.file).toLowerCase();
        var y = (b.name || b.file).toLowerCase();
        return x < y ? -1 : x > y ? 1 : 0;
      });
      live.forEach(function (e, i) { e.box.style.order = i; });
    }

    /* --------------------------------------------------------- names --- */

    /*
     * One name lookup at a time.
     *
     * The backend serves a single connection and closes each one, so firing a
     * request per tile makes none of them faster: it only puts the user's own
     * Update or Remove behind every tile in the grid. A queue holds the wait to
     * one request, and ask() means a name the Unlocker already fetched is free.
     */
    var pending = [];
    var working = false;

    function drain() {
      if (working) return;
      var job = pending.shift();
      if (!job) return;
      working = true;
      job(function () { working = false; drain(); });
    }

    function lookup(id, set) {
      pending.push(function (done) {
        ask("appdetails?appid=" + encodeURIComponent(id))
          .then(function (res) {
            var ok = usable(res);
            var name = ok ? text(ok.name) : "";
            if (name) { set(name); return null; }
            // appdetails answers for apps with a store page only. The browse
            // service behind /api/assets still names the ones without.
            return ask("assets?appid=" + encodeURIComponent(id))
              .then(function (a) {
                var got = usable(a);
                if (got) set(text(got.name));
              });
          })
          // A tile that could not be named keeps the file name it was drawn
          // with, so there is nothing to report and nothing to undo.
          .catch(skip)
          .then(done);
      });
      drain();
    }

    /* ----------------------------------------------------------- art --- */

    /* ---------------------------------------------------------- tiles --- */

    // Only one tile may be armed for removal at a time, and any other click in
    // the page puts it back. Bound to the page's own node rather than the
    // document, so the listener dies with the page instead of outliving it on
    // nodes nobody can see.
    var armed = null;

    function disarm() {
      var a = armed;
      armed = null;
      if (a) a.off();
    }

    wrap.addEventListener("click", function (ev) {
      if (armed && ev.target !== armed.node) disarm();
    }, true);

    /*
     * The outcome of an action.
     *
     * report() does this for the Unlocker but is closed over that renderer's
     * own `el`, so calling it from here would mean editing the call sites it
     * was signed off with. `rejected` shows on success too: it is the only
     * explanation for a run that reported 200 and still skipped half the
     * archive.
     */
    function say(ok, head, rejected) {
      var b = el("div", ok ? null : "luaflipper-error");
      if (ok) {
        style(b, {
          padding: "10px 12px", borderRadius: "3px", background: INSET,
          boxShadow: "inset 3px 0 0 #75b022", fontSize: "12.5px"
        });
      }
      b.appendChild(el("div", null, head));
      var bad = arr(rejected);
      if (bad.length) {
        b.appendChild(el("div", "luaflipper-sub",
          plural(bad.length, "entry", "entries") + " skipped:"));
        bad.forEach(function (line) {
          var e = el("div", "luaflipper-meta", text(line));
          // Sentences, not a column value: .luaflipper-meta clips to one line.
          e.style.whiteSpace = "normal";
          b.appendChild(e);
        });
      }
      note.style.display = "";
      only(note, b);
      return b;
    }

    function addManifest(m) {
      // The backend matches a manifest by its file stem, so the raw id is what
      // /api/remove and /api/update have to be handed. appid() strips it to
      // digits, which is right for a CDN path and wrong for that lookup.
      var raw = text(m.appid);
      var id = appid(raw);
      var file = text(m.file);

      // `live` is "counts towards the totals", which a manifest awaiting the
      // next Steam start does not: the loader never saw it.
      var wasRemoved = !!(m && m.removed);
      var entry = {
        live: !wasRemoved, id: raw, file: file, name: "",
        keys: num(m.keys), ids: num(m.ids), box: el("div")
      };
      rows.push(entry);
      var box = style(entry.box, { position: "relative" });
      grid.appendChild(box);

      // The library's own capsule: a 150% padding-top box so it holds a 2:3
      // shape before any art arrives, and the shadow Steam puts under each one.
      var cap = style(el("div"), {
        position: "relative", paddingTop: "150%", borderRadius: "3px",
        overflow: "hidden", background: INSET,
        boxShadow: "rgba(0,0,0,0.5) 0 4px 8px 0",
        transition: "transform 120ms ease, box-shadow 120ms ease"
      });
      box.appendChild(cap);

      var img = style(el("img"), {
        position: "absolute", top: "0", left: "0", width: "100%",
        height: "100%", objectFit: "cover", display: "block"
      });
      img.alt = "";
      cap.appendChild(img);

      // What stands in when there is no art, which is also what the library
      // falls back to: the name on the capsule itself, centred, wrapping.
      var plate = style(el("div", null, file), {
        position: "absolute", top: "0", left: "0", right: "0", bottom: "0",
        display: "none", alignItems: "center", justifyContent: "center",
        padding: "12px", textAlign: "center", fontSize: "13px",
        lineHeight: "1.35", wordBreak: "break-word"
      });
      cap.appendChild(plate);

      function blank() {
        img.style.display = "none";
        plate.style.display = "flex";
      }
      // A hand-named file has no app id to fetch art for, and asking anyway
      // spends a request per tile to be told so.
      if (id) libraryArt(img, id, "library_600x900.jpg",
                          "library_capsule", blank); else blank();

      // Dimmed and marked, because a removed manifest sitting in the same grid
      // as the live ones is the one thing on this page that could be misread as
      // still installed.
      var mark = null;
      if (wasRemoved) {
        style(cap, { opacity: "0.45" });
        mark = style(el("div", null, "removed"), {
          position: "absolute", top: "6px", left: "6px", padding: "2px 6px",
          borderRadius: "2px", fontSize: "11px", fontWeight: "bold",
          color: "#111318", background: "#c8ccd0"
        });
        cap.appendChild(mark);
      }

      // A keyless manifest is the one state where the app is in the library and
      // still cannot be played, so it is said on the tile rather than only in
      // the panel that has to be hovered to be read.
      var flag = style(el("div", null, "no keys"), {
        position: "absolute", top: "6px", right: "6px", display: "none",
        padding: "2px 6px", borderRadius: "2px", fontSize: "11px",
        fontWeight: "bold", color: "#ffffff", background: "rgba(176,60,60,0.92)"
      });
      cap.appendChild(flag);

      /* -------------------------------------------------------- panel --- */

      // Over the art on hover, the way the library reveals its play controls.
      // Everything the old row spelled out in columns lives here instead.
      var panel = style(el("div"), {
        position: "absolute", top: "0", left: "0", right: "0", bottom: "0",
        display: "flex", flexDirection: "column", justifyContent: "flex-end",
        gap: "6px", padding: "10px", opacity: "0", pointerEvents: "none",
        transition: "opacity 120ms ease",
        // Heavy enough to read small text over bright key art, which most of
        // these capsules are. Measured against the worst case in the grid, a
        // pastel capsule under an 11px key count.
        background: "linear-gradient(to bottom, rgba(0,0,0,0) 22%, " +
                    "rgba(0,0,0,0.72) 52%, rgba(0,0,0,0.94) 100%)"
      });
      cap.appendChild(panel);

      var title = style(el("div", null, file), {
        fontSize: "13px", fontWeight: "bold", color: "#ffffff",
        lineHeight: "1.25", maxHeight: "3.75em", overflow: "hidden"
      });
      title.title = file;
      panel.appendChild(title);

      var meta = style(el("div"), { fontSize: "11px", opacity: "0.85" });
      panel.appendChild(meta);

      // Monospace and Steam's own colour for a value, so an id stays scannable
      // against art of any brightness.
      var idLine = style(el("div", "luaflipper-appid", raw || "?"), {
        color: LINK, opacity: "1", fontSize: "11px"
      });
      panel.appendChild(idLine);

      function hover(on) {
        // A tile armed for removal keeps its panel: the pointer has to be able
        // to travel from Remove to Confirm remove without the target vanishing.
        if (!on && armed && armed.tile === box) return;
        panel.style.opacity = on ? "1" : "0";
        panel.style.pointerEvents = on ? "auto" : "none";
        style(cap, {
          transform: on ? "scale(1.03)" : "none",
          boxShadow: on ? "rgba(0,0,0,0.65) 0 6px 14px 0"
                        : "rgba(0,0,0,0.5) 0 4px 8px 0"
        });
      }
      box.addEventListener("mouseenter", function () { hover(true); });
      box.addEventListener("mouseleave", function () { hover(false); });

      function tally() {
        meta.textContent =
          (entry.keys ? plural(entry.keys, "key") : "no keys") + ", " +
          plural(entry.ids, "id");
        // "no keys" names the symptom, not the consequence. Say the
        // consequence, and say it in the colour the page uses for a fault.
        meta.title = entry.keys ? "" :
          "No decryption key in this manifest. Content downloads but cannot " +
          "be decrypted.";
        style(meta, {
          color: entry.keys ? "" : "#ff8a8a",
          opacity: entry.keys ? "0.85" : "1"
        });
        flag.style.display = entry.keys ? "none" : "block";
      }
      tally();

      var controls = style(el("div"), {
        display: "flex", gap: "6px", marginTop: "2px"
      });
      panel.appendChild(controls);

      var btnCss = {
        fontSize: "11.5px", lineHeight: "24px", padding: "0",
        borderRadius: "2px", textAlign: "center", width: "100%",
        boxSizing: "border-box", fontWeight: "normal"
      };
      // Fixed halves. Both labels change while a request runs, and a button
      // that resizes under the pointer takes its neighbour out from under it.
      var upSlot = style(el("div"), { flex: "1 1 0" });
      var rmSlot = style(el("div"), { flex: "1 1 0" });
      controls.appendChild(upSlot);
      controls.appendChild(rmSlot);

      var up = style(storeBtn(el, "Update", BLUE, BLUE_HOT), btnCss);
      upSlot.appendChild(up);


      // Two buttons swapped rather than one repainted: storeBtn closes over the
      // pair it was built with, so its own mouseleave would put an armed Remove
      // back to its resting fill the moment the pointer left it.
      // Not the page's usual DEEP secondary fill: that is a black wash, and it
      // disappears into the gradient this button sits on, leaving Remove
      // looking like a caption beside a real button. A light wash reads as a
      // control against the art without committing to a colour a theme owns.
      var rm = style(storeBtn(el, "Remove", "rgba(255,255,255,0.16)", LIFT), btnCss);
      var yes = style(storeBtn(el, "Confirm", RED, RED_HOT), btnCss);
      yes.style.display = "none";
      rmSlot.appendChild(rm);
      rmSlot.appendChild(yes);

      // A postscript on the message already showing, for something that went
      // wrong after the action itself succeeded.
      function add(line) {
        var b = note.firstChild;
        if (b) b.appendChild(el("div", "luaflipper-sub", line));
      }

      function named() { return entry.name || file; }

      var busy = false;

      function lock(on) {
        busy = on;
        [up, rm, yes].forEach(function (b) {
          if (!b || !b.parentNode) return;   // swapped out, or never shown
          style(b, {
            pointerEvents: on ? "none" : "auto", opacity: on ? "0.55" : "1"
          });
        });
      }

      /* ------------------------------------------------------ update --- */

      // The counts came from the list this page was drawn from, and an update
      // rewrites the file underneath them. Re-reading that list is the only way
      // a tile can stop claiming a key count the file on disk no longer has.
      function recount() {
        fetch(API + "manifests")
          .then(json)
          .then(function (res) {
            var found = null;
            arr(res && res.manifests).forEach(function (x) {
              if (x && text(x.appid) === raw) found = x;
            });
            if (!found) throw new Error("no longer in the manifest list");
            entry.keys = num(found.keys);
            entry.ids = num(found.ids);
            tally();
            retally();
          })
          .catch(function (e) {
            add("Key and id counts could not be refreshed (" + why(e) +
                "), so the numbers above are the ones from before the update.");
          });
      }

      up.addEventListener("click", function () {
        if (busy) return;
        // An update is a click somewhere other than an armed Confirm.
        disarm();
        lock(true);

        // Multi-MB over plaintext HTTP with no progress events, routinely tens
        // of seconds. An elapsed count is the honest version of a progress bar:
        // it proves the request is alive without inventing a percentage. No
        // timeout, for the same reason the Unlocker's Add has none: cutting a
        // download short leaves half a pack on disk and blames the network.
        var started = Date.now();
        var tick = setInterval(function () {
          up.textContent = Math.round((Date.now() - started) / 1000) + "s";
        }, 1000);
        up.textContent = "0s";

        // Back to a usable button on every path. An update that failed is worth
        // retrying, and one that worked can be run again later.
        function stop() {
          clearInterval(tick);
          up.textContent = "Update";
          lock(false);
        }

        fetch(API + "update?appid=" + encodeURIComponent(raw))
          .then(json)
          .then(function (res) {
            stop();
            if (!res || typeof res !== "object") throw new Error("unreadable reply");
            if (res.error) { say(false, named() + ": " + text(res.error), res.rejected); return; }
            say(true, named() + ": re-downloaded " +
                plural(num(res.installed), "file") +
                ". Ownership is live immediately; depot keys need " +
                "tools/sync_depot_keys.py run with Steam closed before this " +
                "app's content will decrypt.", res.rejected);
            recount();
          })
          .catch(function (e) {
            stop();
            say(false, named() + ": update failed: " + why(e), null);
          });
      });

      /* ------------------------------------------------------ remove --- */

      var armTimer = null;

      function unarm() {
        if (armTimer) { clearTimeout(armTimer); armTimer = null; }
        rm.style.display = "";
        yes.style.display = "none";
        // The panel was being held open for the confirm; let it close normally.
        hover(false);
      }

      // One click never removes anything. The second click is the removal, and
      // four seconds or a click anywhere else on the page takes the offer back.
      rm.addEventListener("click", function () {
        if (busy) return;
        disarm();
        rm.style.display = "none";
        yes.style.display = "";
        armTimer = setTimeout(disarm, 4000);
        armed = { node: yes, tile: box, off: unarm };
      });

      yes.addEventListener("click", function () {
        if (busy) return;
        // The arming is spent, and the button becomes this request's own
        // progress label rather than reverting under the pointer that hit it.
        if (armTimer) { clearTimeout(armTimer); armTimer = null; }
        armed = null;
        lock(true);
        yes.textContent = "Removing";

        function failed(msg) {
          lock(false);
          yes.textContent = "Confirm";
          unarm();
          say(false, msg, null);
        }

        fetch(API + "remove?appid=" + encodeURIComponent(raw))
          .then(json)
          .then(function (res) {
            if (!res || typeof res !== "object") throw new Error("unreadable reply");
            if (res.error) { failed(named() + ": " + text(res.error)); return; }

            // The tile goes, and the grid closes over the gap.
            grid.removeChild(box);
            entry.live = false;
            retally();
            refilter();
            say(true, named() + " removed. It is held as " +
                (text(res.kept) || (file + ".removed")) + " until Steam " +
                "restarts, and Restore on its tile puts it back until then. " +
                "That window exists because the file holds the only copy of " +
                "its depot keys on this machine. Steam keeps the ownership " +
                "for this session either way, since the loader has already " +
                "read the file.", null);
          })
          .catch(function (e) { failed(named() + ": remove failed: " + why(e)); });
      });

      /* ----------------------------------------------------- restore --- */

      // A removed manifest has one action, and it is the undo. Update and
      // Remove are both meaningless against a file the loader is not reading,
      // so they are replaced rather than disabled. This runs after both are
      // built: lock() paints all three buttons, and half of them not existing
      // yet is how that turns into a broken tile.
      if (wasRemoved) {
        var put = style(storeBtn(el, "Restore", BLUE, BLUE_HOT), btnCss);
        only(upSlot, put);
        rmSlot.style.display = "none";

        put.addEventListener("click", function () {
          if (busy) return;
          lock(true);
          put.textContent = "Restoring";
          fetch(API + "restore?appid=" + encodeURIComponent(raw))
            .then(json)
            .then(function (res) {
              if (!res || typeof res !== "object") throw new Error("unreadable reply");
              if (res.error) {
                lock(false);
                put.textContent = "Restore";
                say(false, named() + ": " + text(res.error), null);
                return;
              }
              // Nothing is re-read here: the loader collected its directories
              // at startup, so the file is back but the ownership is not until
              // Steam restarts. Saying otherwise would send someone to a
              // library that still does not have the game.
              cap.style.opacity = "";
              if (mark && mark.parentNode) mark.parentNode.removeChild(mark);
              put.textContent = "Restored";
              entry.live = true;
              retally();
              refilter();
              say(true, named() + " restored as " + (text(res.file) || file) +
                  ". Steam picks it up when it restarts, and it is no longer " +
                  "deleted at the next start.", null);
            })
            .catch(function (e) {
              lock(false);
              put.textContent = "Restore";
              say(false, named() + ": restore failed: " + why(e), null);
            });
        });
      }

      /* -------------------------------------------------------- name --- */

      // Only an all-digit stem is an app id. Anything else is a hand-named file
      // the store has never heard of, and asking about it spends a request per
      // tile to be told so.
      if (raw && raw === id) {
        lookup(raw, function (name) {
          if (!name) return;
          entry.name = name;
          title.textContent = name;
          title.title = name;
          plate.textContent = name;
          // A name landing after the user has typed can bring its tile into the
          // filter, so the filter is re-run rather than assumed still right.
          refilter();
        });
      }
    }

    list.forEach(addManifest);
    retally();
    refilter();

    // Live and undebounced: this filters what is already in the document, so
    // there is nothing upstream to spare by waiting.
    input.addEventListener("input", refilter);

    return wrap;
  }

  /* ------------------------------------------------------------ settings --- */

  /*
   * Steam's Settings dialog, measured off the live one.
   *
   * These are exact values read from the client's own settings window rather
   * than approximations, because the whole point of this page is that it is not
   * a second settings screen with its own opinions. Sampled rather than taken
   * from luaflipper.css for the same reason the store pages sample the store:
   * a theme that restyles Steam restyles the dialog these came from, and an
   * alpha wash over the surrounding surface tracks that where a fixed hex does
   * not. The few that are fixed are the dialog's identity, not its chrome.
   */
  var SET = {
    rail:     "rgba(0,0,0,0.24)",         // the left column behind the sections
    railText: "rgb(184,188,191)",         // a section that is not current
    railOn:   "rgba(103,193,245,0.14)",   // and the one that is
    rule:     "rgba(103,193,245,0.10)",   // hairline between rows and sections
    field:    "rgba(0,0,0,0.20)",         // a settings row
    label:    "rgb(220,222,223)",         // its title
    desc:     "rgb(139,146,154)"          // the line under the title
  };

  /**
   * The dialog's header: 22px bold white, the size Steam sets a settings page
   * in. Not sectionHead(), which is the store's small uppercase rule.
   */
  function dialogHead(el, label) {
    return style(el("div", null, label), {
      fontSize: "22px", fontWeight: "700", color: "#ffffff",
      margin: "8px 0 14px", lineHeight: "28px"
    });
  }

  /**
   * A group of settings rows.
   *
   * Steam draws consecutive rows as one rounded slab with hairlines between
   * them rather than as separate cards, so the group owns the background and
   * the rounding and each row only owns its separator.
   */
  function fieldGroup(el) {
    var g = style(el("div"), {
      background: SET.field, borderRadius: "3px", overflow: "hidden",
      marginBottom: "16px"
    });
    g.first = true;
    return g;
  }

  /**
   * One settings row: title and optional description on the left, whatever
   * control belongs to it on the right.
   *
   * Geometry from the live dialog: 12px padding, 12px gap, 14px/300 title,
   * 13px/400 description 4px under it. `control` may be a node or a string,
   * because half of these rows are a value rather than a button and Steam
   * renders those the same way.
   */
  function field(el, group, label, desc, control) {
    var r = style(el("div"), {
      display: "flex", alignItems: "center", gap: "12px", padding: "12px",
      borderTop: group.first ? "0" : "1px solid " + SET.rule
    });
    group.first = false;

    var left = style(el("div"), { flex: "1 1 auto", minWidth: "0" });
    left.appendChild(style(el("div", null, label), {
      fontSize: "14px", fontWeight: "300", color: SET.label, lineHeight: "18px"
    }));
    if (desc) {
      // A node as well as a string, so a description can carry a link without
      // every caller rebuilding this row.
      var d = style(el("div"), {
        fontSize: "13px", color: SET.desc, lineHeight: "18px", marginTop: "4px"
      });
      if (typeof desc === "string") d.textContent = desc;
      else d.appendChild(desc);
      left.appendChild(d);
    }
    r.appendChild(left);

    var right = style(el("div"), {
      flex: "0 0 auto", display: "flex", alignItems: "center", gap: "8px",
      maxWidth: "55%", minWidth: "0"
    });
    if (control) {
      right.appendChild(typeof control === "string"
        ? style(el("div", null, control), {
            fontSize: "13px", color: SET.desc, overflow: "hidden",
            textOverflow: "ellipsis", whiteSpace: "nowrap"
          })
        : control);
    }
    r.appendChild(right);

    group.appendChild(r);
    return right;
  }

  /**
   * Steam's toggle pill, 38x22 with a 16px radius, off grey and on blue.
   *
   * A real switch rather than a button that says "Enable ...": this page is the
   * settings dialog now, and every other row in that dialog that turns
   * something on is one of these. `set` is handed back so a caller whose
   * request failed can put the switch back where it was.
   */
  function toggle(el, on, onChange) {
    var t = style(el("div"), {
      position: "relative", width: "38px", height: "22px", flex: "0 0 38px",
      borderRadius: "16px", cursor: "pointer",
      transition: "background 120ms ease"
    });
    var knob = style(el("div"), {
      position: "absolute", top: "3px", width: "16px", height: "16px",
      borderRadius: "50%", background: "#ffffff",
      transition: "left 120ms ease"
    });
    t.appendChild(knob);

    var state = !!on;
    function paint() {
      t.style.background = state ? "#1a9fff" : "rgba(255,255,255,0.22)";
      knob.style.left = state ? "19px" : "3px";
    }
    paint();

    t.set = function (v) { state = !!v; paint(); };
    t.addEventListener("click", function () {
      if (t.busy) return;
      var next = !state;
      t.set(next);
      onChange(next, t);
    });
    return t;
  }

  /**
   * The installer command, as something that can actually be copied.
   *
   * user-select is set explicitly because Steam's frontend sets it to none
   * across the client, and a command nobody can select is a command nobody
   * will run. The webkit alias goes in beside it: this is CEF, and which of
   * the two a given client build honours is not worth finding out per build.
   */
  function shellLine(el, cmd) {
    return style(el("div", null, cmd), {
      marginTop: "8px", padding: "8px 10px", borderRadius: "3px",
      background: DEEP, color: LINK_TEXT, fontSize: "12px",
      fontFamily: "Consolas, 'Courier New', monospace",
      whiteSpace: "pre-wrap", wordBreak: "break-all",
      userSelect: "text", webkitUserSelect: "text", cursor: "text"
    });
  }

  /**
   * A settings button, in the dialog's own flat style.
   *
   * storeBtn is the store's green gradient, which is right on a purchase row
   * and wrong in a settings dialog: nothing in Steam's dialog is a gradient.
   */
  function dialogBtn(el, label) {
    var b = style(el("div", "luaflipper-button", label), {
      background: "rgba(255,255,255,0.14)", border: "0", color: "#ffffff",
      fontSize: "13px", fontWeight: "normal", lineHeight: "30px",
      padding: "0 14px", borderRadius: "2px", whiteSpace: "nowrap",
      textAlign: "center", cursor: "pointer"
    });
    // Inline styles beat the stylesheet, so .luaflipper-button's own hover rule
    // stops applying and the hover has to be wired here.
    b.addEventListener("mouseenter", function () {
      if (!b.disabledLook) b.style.background = "rgba(255,255,255,0.22)";
    });
    b.addEventListener("mouseleave", function () {
      if (!b.disabledLook) b.style.background = "rgba(255,255,255,0.14)";
    });
    b.busyLook = function (on) {
      b.disabledLook = on;
      style(b, { pointerEvents: on ? "none" : "auto", opacity: on ? "0.55" : "1" });
    };
    return b;
  }

  /* ------------------------------------------------------------ sections --- */

  /**
   * Updates.
   *
   * What this can and cannot do decides its whole wording. The module Steam has
   * mapped was built on this machine, so an update is a commit rather than a
   * download, and the button fast-forwards the source checkout and stops there.
   * Rebuilding needs the 32-bit toolchain and an installer that refuses to run
   * while Steam is up, which is exactly when this page exists, so the success
   * state hands over a command instead of claiming an update that has not
   * happened yet.
   */
  function updatesSection(data, el, pane) {
    function json(r) { return r.json(); }

    var sha = text(data && data.sha);
    var branch = text(data && data.branch);
    var known = !!sha && sha !== "unknown";

    pane.appendChild(dialogHead(el, "Updates"));

    var g = fieldGroup(el);
    pane.appendChild(g);

    // The version, not the commit. Someone deciding whether to update wants a
    // number they can compare, and a sha is only an answer to a question about
    // git.
    field(el, g, "Version", "This build, from the VERSION file at the repo root.",
      style(el("div", null, text(data && data.version) || "unknown"), {
        fontSize: "14px", color: "#ffffff", fontWeight: "bold"
      }));

    var latestVal = style(el("div", null, "not checked yet"), {
      fontSize: "13px", color: SET.desc, textAlign: "right"
    });
    var checkBtn = dialogBtn(el, "Check for updates");
    var latestRow = field(el, g, "Latest",
      "Read from the VERSION file on the tracked branch.", latestVal);
    latestRow.appendChild(checkBtn);

    if (known) {
      field(el, g, "Built from",
        "The commit this module was compiled at" +
        (branch ? ", tracking " + branch : "") + ".",
        sha.substring(0, 7));
    }

    // Under the group rather than in a row: outcomes here run to several
    // sentences and sometimes carry a command, and a row's right column is
    // sized for a value.
    var out = style(el("div"), {
      display: "none", padding: "12px", borderRadius: "3px",
      background: SET.field, fontSize: "13px", lineHeight: "19px",
      marginBottom: "16px"
    });
    pane.appendChild(out);

    function say(node) {
      out.style.display = "";
      only(out, node);
    }
    function tell(msg) { say(el("div", null, msg)); }

    checkBtn.addEventListener("click", function () {
      checkBtn.busyLook(true);
      checkBtn.textContent = "Checking";
      latestVal.textContent = "checking";

      function done(label) {
        checkBtn.busyLook(false);
        checkBtn.textContent = label;
      }

      fetch(API + "update/check").then(json).then(function (res) {
        if (!res || typeof res !== "object") throw new Error("unreadable reply");
        if (res.error) {
          done("Check again");
          latestVal.textContent = "unknown";
          tell(text(res.error));
          return;
        }

        // remote_version is the VERSION file on the branch; remote is the sha
        // it was read at, which is the only answer available when the branch
        // predates that file.
        latestVal.textContent =
          text(res.remote_version) || text(res.remote) || "unknown";

        // Four states, not two. Behind is the only one with an action, and
        // saying "up to date" to a checkout that is ahead or has diverged
        // would be a claim about work the user has not pushed.
        var rel = text(res.relation);
        if (rel === "current") {
          done("Check again");
          latestVal.style.color = "#a4d007";
          tell("This build is current.");
          return;
        }
        if (rel === "ahead") {
          done("Check again");
          // Commit counts only when there are commit counts. The check settles
          // the direction from the version file and returns, so it often has no
          // ancestry to report, and "ahead by 0 commits" is what that looked
          // like. The version is the comparison the user cares about anyway.
          tell("This build (" + (text(res.version) || "unknown") + ") is newer " +
               "than " + (branch || "the branch") + " (" +
               (text(res.remote_version) || text(res.remote) || "unknown") + ")." +
               (num(res.ahead) ? " " + plural(num(res.ahead), "commit") +
                                 " ahead." : "") +
               " There is nothing to pull.");
          return;
        }
        if (rel === "diverged") {
          done("Check again");
          tell("This checkout has diverged from " + (branch || "the branch") +
               (num(res.ahead) || num(res.behind_by)
                 ? ": " + plural(num(res.ahead), "commit") + " here that are " +
                   "not there, and " + plural(num(res.behind_by), "commit") +
                   " there that are not here"
                 : "") +
               ". Reconcile it in git rather than from a button.");
          return;
        }

        done("Check again");
        // Version first, commits only when the check actually counted them:
        // it settles the direction from the version file and returns without
        // ancestry in the common case, which used to render as "0 commits
        // behind" over a button offering to pull nothing.
        var behindBy = num(res.behind_by);
        var box = el("div");
        box.appendChild(el("div", null,
          "Version " + (text(res.remote_version) || "unknown") +
          " is available. This build is " + (text(res.version) || "unknown") +
          "." + (behindBy ? " " + plural(behindBy, "commit") + " behind." : "")));
        var apply = dialogBtn(el, behindBy
          ? "Pull " + plural(behindBy, "commit")
          : "Pull from " + (branch || "the branch"));
        style(apply, { display: "inline-block", marginTop: "10px" });
        box.appendChild(apply);
        say(box);

        apply.addEventListener("click", function () {
          apply.busyLook(true);
          apply.textContent = "Pulling";
          fetch(API + "update/apply").then(json).then(function (r2) {
            if (!r2 || typeof r2 !== "object") throw new Error("unreadable reply");
            if (r2.error) {
              apply.busyLook(false);
              apply.textContent = "Try again";
              tell(text(r2.error));
              return;
            }
            var line = el("div");
            line.appendChild(el("div", null,
              "Source updated. The module Steam has loaded is still the old " +
              "build; the installer cannot run while Steam is up. Close " +
              "Steam, then build and install it:"));
            line.appendChild(shellLine(el, text(r2.command) ||
              "./tools/install_linux.sh"));
            say(line);
          }).catch(function (e) {
            apply.busyLook(false);
            apply.textContent = "Try again";
            tell("Could not reach SteamFlipper: " + why(e));
          });
        });
      }).catch(function (e) {
        done("Check again");
        latestVal.textContent = "unknown";
        tell("Could not reach SteamFlipper: " + why(e));
      });
    });
  }

  /**
   * Cloud saves.
   *
   * Apps added by a Lua manifest are not on the account, so Valve's servers
   * answer their cloud uploads with Access Denied and those games end up with
   * no cloud at all. SteamFlipper answers the Cloud.* RPCs itself, out of a
   * folder on this machine, and that backend is compiled into the module.
   *
   * Which leaves nothing to install, so the section is a switch over the facts
   * that say whether it is doing anything.
   */
  function cloudSection(data, el, pane) {
    // A backend that answered {"error"} knows nothing about the state, so every
    // value reads "unknown" instead of "no", which would be a claim.
    var failed = (data && data.error) ? text(data.error) : "";
    function state(v, yes, no) { return failed ? "unknown" : (v ? yes : no); }

    var enabled = !failed && !!(data && data.enabled);
    var active = !failed && !!(data && data.active);
    var apps = failed ? 0 : num(data && data.apps);
    var storage = text(data && data.storage);
    var conf = text(data && data.config);

    pane.appendChild(dialogHead(el, "Cloud saves"));

    if (failed) {
      pane.appendChild(style(el("div", "luaflipper-error",
        "Cloud status unavailable: " + failed), { marginBottom: "16px" }));
    }

    var g = fieldGroup(el);
    pane.appendChild(g);

    // The switch first, because it is the only thing here that decides
    // anything; the rows under it only report. There is no separate "Enabled"
    // row: the switch is that row, and a value beside it saying the same thing
    // twice is one more place for the two to disagree.
    var told = null;

    var sw = toggle(el, enabled, function (next, t) {
      t.busy = true;
      fetch(API + (next ? "cloud/enable" : "cloud/disable"))
        .then(function (r) { return r.json(); })
        .then(function (res) {
          t.busy = false;
          if (!res || typeof res !== "object") throw new Error("unreadable reply");
          // Put the switch back: a refusal that leaves it looking flipped is a
          // switch that lies about the file it failed to write.
          if (res.error) { t.set(!next); note(text(res.error)); return; }
          note((next ? "Enabled" : "Disabled") + " in " +
               (text(res.config) || conf ||
                "steamflipper.toml in the Steam folder") +
               ". It takes effect when Steam restarts.");
        })
        .catch(function (e) {
          t.busy = false;
          t.set(!next);
          note("Could not reach SteamFlipper: " + why(e));
        });
    });

    field(el, g, "Answer cloud saves locally",
      "Only apps with a manifest in config/stplug-in. Games the account " +
      "genuinely owns keep using Valve's cloud and are never touched.", sw);

    // Enabled but not running means the flag was set after this Steam started,
    // which is the distinction this row exists to draw.
    var runVal = style(el("div", null, state(active, "yes", "no")), {
      fontSize: "13px", color: active ? "#a4d007" : SET.desc
    });
    field(el, g, "Running now",
      "Takes effect on the next Steam restart, so this can read no while the " +
      "switch above is on.", runVal);

    field(el, g, "Apps covered",
      "Manifests this backend is currently answering for.",
      state(apps, String(apps), "none"));

    var store = field(el, g, "Storage",
      "Where the saves are kept on this machine.",
      state(storage, storage, "unknown"));
    if (storage) store.title = storage;

    // Under the rows, where whoever just turned this on is reading: local
    // storage is the whole design, and the cost of it is the one thing they
    // need before they trust a save to it. Amber rather than
    // .luaflipper-error's red, because nothing has gone wrong; the class is
    // kept for its geometry and the colour is overridden.
    var warn = style(el("div", "luaflipper-error"), {
      background: "rgba(220,170,60,0.12)",
      border: "1px solid rgba(220,170,60,0.35)", marginBottom: "16px"
    });
    warn.appendChild(style(el("div", null,
      "Saves for these apps live only on this machine. Nothing copies them " +
      "anywhere else, so they are gone with the disk they are on."), {
      lineHeight: "18px"
    }));
    warn.appendChild(el("div", "luaflipper-sub",
      "Point a sync or backup client at the storage folder above to change " +
      "that."));
    pane.appendChild(warn);

    told = style(el("div"), {
      display: "none", padding: "12px", borderRadius: "3px",
      background: SET.field, fontSize: "13px", lineHeight: "19px"
    });
    pane.appendChild(told);

    function note(msg) {
      told.textContent = msg;
      told.style.display = "";
    }
  }

  /**
   * Sources.
   *
   * Two things are decided here. Which source is tried first, because every
   * install walks the list top down and takes the first that answers, so the
   * order is the preference. And the Hubcap key, because Hubcap is the one
   * source that is the user's own account rather than a shared endpoint.
   *
   * Both write straight into steamflipper.toml, which is hot-reloaded, so a
   * save is live without restarting Steam. The file is the record either way;
   * this page is a nicer way to edit it than a text editor, not a second store.
   */
  function hubcapSection(data, el, pane) {
    pane.appendChild(dialogHead(el, "Sources"));

    // Held so a save can rebuild without re-fetching, and so the reorder
    // buttons have something to reorder.
    var order = arr(data && data.order);
    if (!order.length) order = ["Ryuu", "Sushi", "Sadie (Hubcap)"];

    // Each source's own Discord, the same ones LuaTools lists, because "where
    // do I get a key" and "who do I ask when a source is down" have no other
    // answer and retyping an invite code off a settings page is a poor one.
    function about(name) {
      if (name === "Ryuu") {
        return sentence(el, [
          "No account needed. Plain HTTP to a bare IP, so the archive is only " +
          "as trustworthy as the network path to it.  ",
          ["Discord", "https://discord.gg/manifests"]]);
      }
      if (name === "Sushi") {
        return sentence(el, [
          "No account needed. Served from a GitHub repository.  ",
          ["Discord", "https://discord.gg/hMdv5dQhcN"]]);
      }
      if (name === "Luie" || name === "TwentyTwo Cloud" || name === "Skyflare") {
        return sentence(el, [
          "Served through lua.tools' proxy, so it needs the sign-in above. " +
          "Downloads count against the same 25 a day as fixes.  ",
          ["Discord", "https://discord.gg/luatools"]]);
      }
      if (name === "Sadie (Hubcap)") {
        return sentence(el, [
          "Your own account on ",
          ["hubcapmanifest.com", "https://hubcapmanifest.com"],
          ". Downloads count against this key's daily limit, not a shared one.  ",
          ["Discord", "https://discord.gg/hubcapsmanifest"]]);
      }
      return "A manifest source.";
    }

    // The same sign-in as the Fixes page. Several of the sources below are
    // served only through lua.tools' proxy and answer 401 without a session, so
    // this is the switch that turns them on.
    var acct = luaToolsAccount(el, function () { drawList(); });
    pane.appendChild(acct.node);

    var listHost = el("div");
    pane.appendChild(listHost);

    var said = style(el("div"), {
      display: "none", padding: "12px", borderRadius: "3px",
      background: SET.field, fontSize: "13px", lineHeight: "19px",
      marginBottom: "16px"
    });
    pane.appendChild(said);

    function tell(msg, good) {
      said.textContent = msg;
      said.style.display = "";
      said.style.color = good ? "#a4d007" : "#e7a94a";
    }

    function saveOrder() {
      fetch(API + "sources/order?order=" +
            encodeURIComponent(order.join(",")))
        .then(function (r) { return r.json(); })
        .then(function (res) {
          if (res && res.error) { tell(text(res.error), false); return; }
          tell("Order saved. Installs try " + order[0] + " first.", true);
        })
        .catch(function (e) {
          tell("Could not save: " + why(e), false);
        });
    }

    function move(i, by) {
      var to = i + by;
      if (to < 0 || to >= order.length) return;
      var tmp = order[i];
      order[i] = order[to];
      order[to] = tmp;
      drawList();
      saveOrder();
    }

    function drawList() {
      var g = fieldGroup(el);

      order.forEach(function (name, i) {
        var hub = name === "Sadie (Hubcap)";
        var proxied = name === "Luie" || name === "TwentyTwo Cloud" ||
                      name === "Skyflare";
        var state = hub ? hubcapState(data)
          : style(el("div", null, proxied ? "needs sign-in" : "no account needed"), {
              fontSize: "13px", color: SET.desc
            });
        var right = field(el, g, (i + 1) + ".  " + name, about(name), state);

        // Up and down rather than drag: three rows do not need a drag model,
        // and a button says what it will do before it is pressed.
        [["\u25B2", -1, i > 0], ["\u25BC", 1, i < order.length - 1]]
          .forEach(function (spec) {
            var b = dialogBtn(el, spec[0]);
            style(b, { padding: "0 10px", lineHeight: "26px", fontSize: "11px" });
            if (!spec[2]) b.busyLook(true);
            else b.addEventListener("click", function () { move(i, spec[1]); });
            right.appendChild(b);
          });
      });

      only(listHost, g);
    }

    function hubcapState(d) {
      var configured = !!(d && d.configured), valid = !!(d && d.valid);
      var label = !configured ? "no key"
                : (valid ? (num(d.used) + " of " + num(d.limit) + " today")
                         : "key rejected");
      return style(el("div", null, label), {
        fontSize: "13px",
        color: configured && valid ? "#a4d007" : (configured ? "#e7a94a" : SET.desc)
      });
    }

    drawList();

    /* ---------------------------------------------------------- key --- */

    pane.appendChild(style(el("div", null, "Hubcap key"), {
      fontSize: "16px", fontWeight: "700", color: "#ffffff",
      margin: "6px 0 10px"
    }));

    var g2 = fieldGroup(el);
    pane.appendChild(g2);

    // Never prefilled: the module does not send the key back, on purpose, so
    // there is nothing to fill it with. The placeholder carries the only fact
    // the page actually needs, which is whether one is already saved.
    var input = el("input", "luaflipper-input");
    input.type = "password";              // a credential, on a screen someone may be sharing
    input.placeholder = (data && data.configured)
      ? "a key is saved. Type a new one to replace it"
      : "smm_...";
    style(input, {
      border: "0", borderRadius: "2px", background: "rgba(0,0,0,0.35)",
      padding: "0 10px", height: "30px", width: "320px", fontSize: "13px"
    });

    var save = dialogBtn(el, "Save");
    var right = field(el, g2, "API key", sentence(el, [
      "Kept in steamflipper.toml, read at startup, and sent only to ",
      ["hubcapmanifest.com", "https://hubcapmanifest.com"],
      ". Save an empty field to remove it."]), input);
    right.appendChild(save);

    save.addEventListener("click", function () {
      var key = (input.value || "").trim();
      save.busyLook(true);
      save.textContent = "Saving";
      fetch(API + "hubcap/save?key=" + encodeURIComponent(key))
        .then(function (r) { return r.json(); })
        .then(function (res) {
          save.busyLook(false);
          save.textContent = "Save";
          if (res && res.error) { tell(text(res.error), false); return; }
          if (res && res.cleared) {
            tell("Key removed. Hubcap will report that it needs one.", true);
            data = { configured: false, order: order };
            input.value = "";
            input.placeholder = "smm_...";
            drawList();
            return;
          }
          // The verdict rides back with the save. Asking again separately used
          // to race the config file watcher and report a perfectly good key as
          // rejected, because the second request read a config that had not
          // reloaded yet.
          var st = res.stats || {};
          data = st;
          input.value = "";
          input.placeholder = "a key is saved. Type a new one to replace it";
          drawList();
          if (st.valid) {
            tell("Key accepted. " + num(st.used) + " of " + num(st.limit) +
                 " downloads used today.", true);
          } else {
            tell(text(st.error) || "Hubcap did not accept this key.", false);
          }
        })
        .catch(function (e) {
          save.busyLook(false);
          save.textContent = "Save";
          tell("Could not save: " + why(e), false);
        });
    });

    if (data && data.configured && data.valid && text(data.expires)) {
      field(el, g2, "Key expires", "As reported by Hubcap.",
            text(data.expires).substring(0, 10));
    }

    pane.appendChild(style(sentence(el, [
      "Hubcap is a separate account. Get a key from ",
      ["their Discord", "https://discord.gg/hubcapsmanifest"],
      ", paste it above and save; nothing else needs a restart."]), {
      display: "block", fontSize: "13px", color: SET.desc, marginTop: "4px"
    }));
  }

  /**
   * A section that is nothing but reported facts: what the module loaded, and
   * what it found.
   *
   * /api/config and /api/status both answer {rows:[{label,value}]}, so one
   * function draws both. There is no description line because the backend does
   * not send one, and inventing one per row would mean this file deciding what
   * every setting means.
   */
  function rowsSection(el, pane, title, data, empty) {
    pane.appendChild(dialogHead(el, title));

    if (data && data.error) {
      pane.appendChild(el("div", "luaflipper-error", text(data.error)));
      return;
    }
    var list = arr(data && data.rows);
    if (!list.length) {
      pane.appendChild(el("div", "luaflipper-empty", empty));
      return;
    }
    var g = fieldGroup(el);
    pane.appendChild(g);
    list.forEach(function (r) {
      var value = text(r.value);
      var right = field(el, g, text(r.label), "", linkify(el, value) || value);
      right.title = value;
    });
  }

  /**
   * A row value that is an address, as something clickable.
   *
   * The API row is the one anybody actually wants to open, and reading a
   * host:port off a settings page to retype it into a browser is a silly way to
   * spend a minute. Returns null for values that are not addresses, so the
   * caller falls back to plain text and a path or a count is never dressed up
   * as a link.
   *
   * steam:// rather than window.open: the client resolves that itself and shows
   * it in its own browser window, where window.open from this context either
   * opens nothing or takes over the view the UI is sitting in.
   */
  /**
   * Open a URL in the system's default browser.
   *
   * openurl_external, not openurl: the second hands it to Steam's own built-in
   * browser, which is not where anyone wants a Discord invite or a login page
   * to land. SteamClient.System.OpenInSystemBrowser would say it more directly
   * but lives only in SharedJSContext, a different CEF target from the window
   * this UI is injected into, so the protocol handler is the route that exists
   * from here.
   *
   * Assigning location rather than window.open: from this context window.open
   * either does nothing or replaces the view the UI is sitting in, and a
   * steam:// assignment is intercepted by the client without navigating.
   */
  function openUrl(url) {
    try { window.location.href = "steam://openurl_external/" + url; } catch (e) {}
  }

  /** A clickable label. Same treatment wherever a link appears on these pages. */
  function link(el, label, url) {
    var a = style(el("span", null, label), {
      color: LINK_TEXT, cursor: "pointer",
      textDecoration: "underline", textDecorationColor: "rgba(102,192,244,0.4)"
    });
    a.title = "Open " + url;
    a.addEventListener("click", function (ev) {
      ev.stopPropagation();
      openUrl(url);
    });
    return a;
  }

  /**
   * A sentence with links in it.
   *
   * `parts` alternates plain strings and [label, url] pairs, which is enough
   * for a description line and stops short of an HTML parser: nothing here
   * should be building markup out of text.
   */
  function sentence(el, parts) {
    var box = el("span");
    parts.forEach(function (part) {
      box.appendChild(typeof part === "string"
        ? document.createTextNode(part)
        : link(el, part[0], part[1]));
    });
    return box;
  }

  function linkify(el, value) {
    var url = /^https?:\/\//.test(value) ? value
            : (/^\d{1,3}(\.\d{1,3}){3}:\d+$/.test(value) ? "http://" + value : "");
    if (!url) return null;

    var a = style(el("div", null, value), {
      color: LINK_TEXT, cursor: "pointer", fontSize: "13px",
      textDecoration: "underline", textDecorationColor: "rgba(102,192,244,0.4)"
    });
    a.title = "Open " + url;
    a.addEventListener("click", function () { openUrl(url); });
    return a;
  }

  /* ------------------------------------------------------------- config --- */

  /**
   * The Config page: Steam's settings dialog, for this module.
   *
   * Updates, cloud saves and status used to be three entries in the dropdown,
   * which made the dropdown a list of pages rather than a list of places. They
   * are one thing, the module's own settings, and Steam already has a shape for
   * that: a rail of sections on the left, one page of rows on the right. This
   * is that dialog, laid out from measurements off the live one, so the page
   * reads as part of the client rather than as a panel bolted to it.
   *
   * Only Updates and Configuration are drawn from the reply this page was
   * loaded with. Cloud and Status have their own endpoints and are fetched when
   * their section is first opened: they report live state, so fetching all
   * three up front would put two stale panels behind a rail nobody has clicked.
   */
  function configPage(data, el) {
    var wrap = style(el("div"), { position: "relative" });
    wrap.appendChild(style(el("div"), {
      position: "absolute", top: "-18px", left: "-24px",
      width: "calc(100% + 48px)", height: "1400px", zIndex: "0",
      pointerEvents: "none", background: PAGE_WASH
    }));

    // The dialog itself: rail and pane, filling the page the way the settings
    // window fills its own. The negative margins escape .luaflipper-body's
    // padding, because the rail runs to the window edge in Steam's dialog and a
    // rail floating in from the left would read as a card, not a dialog.
    var shell = style(el("div"), {
      position: "relative", zIndex: "1", display: "flex",
      margin: "-18px -24px", minHeight: "calc(100vh - 120px)"
    });
    wrap.appendChild(shell);

    var rail = style(el("div"), {
      flex: "0 0 198px", background: SET.rail, padding: "24px 0 12px"
    });
    shell.appendChild(rail);

    rail.appendChild(style(el("div", null, "LUAFLIPPER"), {
      color: LINK_TEXT, fontSize: "15px", fontWeight: "bold",
      letterSpacing: "0.5px", padding: "0 24px", marginBottom: "14px"
    }));

    var pane = style(el("div"), {
      flex: "1 1 auto", minWidth: "0", padding: "18px 24px 32px",
      maxWidth: "760px"
    });
    shell.appendChild(pane);

    /*
     * The sections, and what each needs before it can draw.
     *
     * `load` is the endpoint whose reply the section wants; sections without
     * one are drawn from the page's own reply. Each is built once and then
     * shown and hidden, so a check that is running or a switch that was just
     * flipped survives a trip to another section and back.
     */
    var SECTIONS = [
      { name: "Updates",
        draw: function (p) { updatesSection(data, el, p); } },
      { name: "Cloud saves", load: "cloud",
        draw: function (p, got) { cloudSection(got, el, p); } },
      { name: "Sources", load: "hubcap/stats",
        draw: function (p, got) { hubcapSection(got, el, p); } },
      { name: "Configuration",
        draw: function (p) {
          rowsSection(el, p, "Configuration", data,
            "No configuration loaded. steamflipper.toml sits in the Steam " +
            "directory.");
        } },
      { name: "Status", load: "status",
        draw: function (p, got) {
          rowsSection(el, p, "Status", got, "No status reported.");
        } }
    ];

    var built = {};
    var items = {};
    var current = null;

    function select(name) {
      if (current === name) return;
      current = name;

      SECTIONS.forEach(function (s) {
        var it = items[s.name];
        style(it, {
          background: s.name === name ? SET.railOn : "transparent",
          color: s.name === name ? "#ffffff" : SET.railText
        });
        if (built[s.name]) built[s.name].style.display =
          s.name === name ? "" : "none";
      });

      if (built[name]) return;

      var sec = null;
      SECTIONS.forEach(function (s) { if (s.name === name) sec = s; });
      var host = el("div");
      built[name] = host;
      pane.appendChild(host);

      if (!sec.load) { sec.draw(host); return; }

      host.appendChild(el("div", "luaflipper-loading", "Loading…"));
      fetch(API + sec.load)
        .then(function (r) { return r.json(); })
        .then(function (got) { host.innerHTML = ""; sec.draw(host, got); })
        .catch(function (e) {
          host.innerHTML = "";
          host.appendChild(dialogHead(el, name));
          host.appendChild(el("div", "luaflipper-error",
            "Could not reach SteamFlipper: " + why(e)));
        });
    }

    SECTIONS.forEach(function (s) {
      var it = style(el("div", null, s.name), {
        padding: "10px 8px 10px 24px", fontSize: "14px", cursor: "pointer",
        color: SET.railText
      });
      it.addEventListener("mouseenter", function () {
        if (current !== s.name) it.style.background = "rgba(255,255,255,0.06)";
      });
      it.addEventListener("mouseleave", function () {
        if (current !== s.name) it.style.background = "transparent";
      });
      it.addEventListener("click", function () { select(s.name); });
      items[s.name] = it;
      rail.appendChild(it);
    });

    // Updates first: it is the only section with something to decide, and the
    // rest are reference.
    select(SECTIONS[0].name);
    return wrap;
  }

  /* ------------------------------------------------------------- export --- */

  // Assigned once, at the end, so a parse error higher up leaves the global
  // absent rather than half-built: luaflipper.js can then fall back instead of
  // calling a renderer that is not there.
  window.LUAFlipperPages = {
    // Unlocker finds and adds. Manage is what is already on disk, and owns the
    // two actions that can change it, so the destructive one lives there rather
    // than beside a page whose whole job is adding.
    manage: manage,
    unlocker: addPage,
    fixes: fixes,
    // Config is the settings dialog, and cloud saves, updates and status are
    // sections inside it rather than pages of their own: they are all answers
    // to "how is this module set up", and three dropdown entries for one
    // question made the dropdown a list of pages instead of a list of places.
    config: configPage
  };
})();
