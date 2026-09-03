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

  /** Fixes the loader applied, one row each: kind, file it touched, detail. */
  function fixes(data, el) {
    var err = errorEl(data, el);
    if (err) return err;

    var list = arr(data && data.fixes);
    var wrap = el("div");

    if (!list.length) {
      wrap.appendChild(el("div", "luaflipper-empty",
        "No fixes applied. Nothing needed patching."));
      return wrap;
    }

    wrap.appendChild(el("div", "luaflipper-sub", plural(list.length, "fix", "fixes")));
    list.forEach(function (f) {
      wrap.appendChild(row(el, text(f.kind), text(f.file), text(f.detail)));
    });
    return wrap;
  }

  /**
   * Label/value pages. Cloud, config and status answer the identical {"rows"}
   * shape, so they share a renderer and differ only in what an empty response
   * means on that page.
   *
   * The label takes the flexible column and the value the fixed one: values are
   * paths and counts that lose their meaning when truncated, labels do not.
   */
  function rowsPage(data, el, empty) {
    var err = errorEl(data, el);
    if (err) return err;

    var list = arr(data && data.rows);
    var wrap = el("div");

    if (!list.length) {
      wrap.appendChild(el("div", "luaflipper-empty", empty));
      return wrap;
    }
    list.forEach(function (r) {
      wrap.appendChild(row(el, "", text(r.label), text(r.value)));
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
              return s && text(s.name) && text(s.status) !== "unavailable";
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
            return text(s.status) === "unavailable" ? n + " (unavailable)" : n;
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
      var live = sources.filter(function (s) {
        return text(s.status) !== "unavailable";
      });

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
   * The Manage page: everything already added, in the store's own list shape.
   *
   * Same visual language as the browse view, because these are the same apps
   * the user found there and a second skin for the second half of one workflow
   * reads as a second application.
   *
   * /api/manifests knows a file, an app id and two counts and nothing else, so
   * names and art are looked up per row and filled in late. Until a name lands
   * the row shows the file name, which is the one label that is always true.
   *
   * Rows are deliberately not clickable. openGame() lives inside the Unlocker
   * renderer's closure and cannot be reached from here, and a second app page
   * built to make these rows clickable would be a second copy of the one that
   * is already signed off.
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

    page.appendChild(bigHead(el, "Installed manifests"));

    if (!list.length) {
      page.appendChild(el("div", "luaflipper-empty",
        "Nothing added yet. The Unlocker page is where apps are found and " +
        "added; whatever is added there shows up here."));
      return wrap;
    }

    var sub = el("div", "luaflipper-sub");
    page.appendChild(sub);

    /* -------------------------------------------------------- filter --- */

    // The store's search field, minus the button: nothing here leaves the page,
    // so there is nothing to submit and nothing to wait for.
    var bar = style(el("div"), {
      display: "flex", alignItems: "center", gap: "12px", marginBottom: "16px"
    });
    var field = style(el("div"), {
      display: "flex", alignItems: "stretch", flex: "0 1 420px",
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
    // Empty unless a filter is active. A filter whose effect cannot be seen is
    // a filter that gets blamed for the missing row it did not hide.
    var shown = style(el("div"), { fontSize: "12px", opacity: "0.6" });
    bar.appendChild(shown);
    page.appendChild(bar);

    var listBox = el("div");
    page.appendChild(listBox);

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
      // like. It leads the summary rather than hiding in a row's key count.
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
        // A removed row leaves its receipt behind. That is a record of what
        // just happened, not a listing, so it counts towards neither total.
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
    }

    /* --------------------------------------------------------- names --- */

    /*
     * One name lookup at a time.
     *
     * The backend serves a single connection and closes each one, so firing a
     * request per row makes none of them faster: it only puts the user's own
     * Update or Remove behind every row in the list. A queue holds the wait to
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
          // A row that could not be named keeps the file name it was drawn
          // with, so there is nothing to report and nothing to undo.
          .catch(skip)
          .then(done);
      });
      drain();
    }

    /* ----------------------------------------------------------- rows --- */

    // Only one row may be armed for removal at a time, and any other click in
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

    function addManifest(m) {
      // The backend matches a manifest by its file stem, so the raw id is what
      // /api/remove and /api/update have to be handed. appid() strips it to
      // digits, which is right for a CDN path and wrong for that lookup.
      var raw = text(m.appid);
      var id = appid(raw);
      var file = text(m.file);

      var entry = {
        live: true, id: raw, file: file, name: "",
        keys: num(m.keys), ids: num(m.ids), box: el("div")
      };
      rows.push(entry);
      var box = entry.box;
      listBox.appendChild(box);

      var r = el("div", "luaflipper-row");
      box.appendChild(r);

      // Same rescue as the browse view's rows: anything Steam published after
      // it moved store art behind hashed URLs 404s on the flat CDN path, and a
      // library of recent additions would come up as a column of empty boxes.
      var setters = [];
      var cap = capsule(el, id, CAP, "46.58%", setters);
      // A hand-named file has no app id to rescue art for, and asking anyway
      // spends a request per row to be told so.
      if (id) rescueArt(cap, id, setters[0]);
      r.appendChild(slot(el, "120px", cap));

      // The file name until a real one lands: always true, and it is what the
      // user would see in the folder. Titled because the column clips.
      // min-width 0 matters here. A flex item's automatic minimum size is its
      // content, so this column refuses to shrink past a long game name and
      // pushes the controls off the right edge: measured live, the Remove
      // button's container was clipped to 97px against 250px of buttons and the
      // page scrolled 117px sideways. The ellipsis needs this too, since it
      // cannot engage on a column that never narrows.
      // flex 1 1 auto, not the stylesheet's `flex: 1` (which is `1 1 0%`).
      // Shrinking is weighted by base size, so a basis of 0 contributes nothing
      // to absorbing negative space: the column kept its full width and pushed
      // the controls off the right edge instead. min-width 0 is still needed for
      // the ellipsis, since that cannot engage on a column that never narrows.
      var label = style(el("span", "luaflipper-name", file), {
        fontSize: "14px", minWidth: "0", flex: "1 1 auto"
      });
      label.title = file;
      r.appendChild(label);

      // Monospace and a fixed column, so the ids line up down a list this page
      // exists to be scanned. LINK is the store's own colour for a value.
      r.appendChild(style(el("span", "luaflipper-appid", raw || "?"), {
        color: LINK, opacity: "1"
      }));

      var meta = el("span", "luaflipper-meta", "");
      r.appendChild(meta);

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
          color: entry.keys ? "" : "#e06a6a",
          opacity: entry.keys ? "" : "1"
        });
      }
      tally();

      // Explicit basis rather than `auto`. Measured live, the container resolved
      // to 97px while holding 250px of buttons, which clipped Remove off the
      // row; stating the width leaves nothing to resolve.
      var controls = style(el("div"), {
        display: "flex", alignItems: "center", gap: "6px", flex: "0 0 250px"
      });
      r.appendChild(controls);

      var btnCss = {
        fontSize: "12.5px", lineHeight: "28px", padding: "0",
        borderRadius: "2px", textAlign: "center", width: "100%",
        boxSizing: "border-box"
      };
      // Fixed slots. Both labels change while a request runs, and a button that
      // resizes under the pointer takes its neighbour out from under it.
      var upSlot = style(el("div"), { flex: "0 0 112px" });
      var rmSlot = style(el("div"), { flex: "0 0 132px" });
      controls.appendChild(upSlot);
      controls.appendChild(rmSlot);

      var up = style(storeBtn(el, "Update", BLUE, BLUE_HOT), btnCss);
      upSlot.appendChild(up);

      // Two buttons swapped rather than one repainted: storeBtn closes over the
      // pair it was built with, so its own mouseleave would put an armed Remove
      // back to its resting fill the moment the pointer left it.
      var rm = style(storeBtn(el, "Remove", DEEP, LIFT), btnCss);
      var yes = style(storeBtn(el, "Confirm remove", RED, RED_HOT), btnCss);
      yes.style.display = "none";
      rmSlot.appendChild(rm);
      rmSlot.appendChild(yes);

      // Under the row rather than in it, so a long list of rejected entries
      // cannot push the buttons around, and so a removal's receipt can stay
      // exactly where the row it describes used to be.
      var note = style(el("div"), { display: "none", margin: "0 0 8px" });
      box.appendChild(note);

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
      }

      // A postscript on the message already showing, for something that went
      // wrong after the action itself succeeded.
      function add(line) {
        var b = note.firstChild;
        if (b) b.appendChild(el("div", "luaflipper-sub", line));
      }

      var busy = false;

      function lock(on) {
        busy = on;
        [up, rm, yes].forEach(function (b) {
          style(b, {
            pointerEvents: on ? "none" : "auto", opacity: on ? "0.55" : "1"
          });
        });
      }

      /* ------------------------------------------------------ update --- */

      // The counts came from the list this page was drawn from, and an update
      // rewrites the file underneath them. Re-reading that list is the only way
      // a row can stop claiming a key count the file on disk no longer has.
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
        // An update is a click somewhere other than an armed Confirm remove.
        disarm();
        lock(true);

        // Multi-MB over plaintext HTTP with no progress events, routinely tens
        // of seconds. An elapsed count is the honest version of a progress bar:
        // it proves the request is alive without inventing a percentage. No
        // timeout, for the same reason the Unlocker's Add has none: cutting a
        // download short leaves half a pack on disk and blames the network.
        var started = Date.now();
        var tick = setInterval(function () {
          up.textContent = "Updating " +
            Math.round((Date.now() - started) / 1000) + "s";
        }, 1000);
        up.textContent = "Updating 0s";

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
            if (res.error) { say(false, text(res.error), res.rejected); return; }
            say(true, "Re-downloaded " + plural(num(res.installed), "file") +
                ". Ownership is live immediately; depot keys need " +
                "tools/sync_depot_keys.py run with Steam closed before this " +
                "app's content will decrypt.", res.rejected);
            recount();
          })
          .catch(function (e) {
            stop();
            say(false, "Update failed: " + why(e), null);
          });
      });

      /* ------------------------------------------------------ remove --- */

      var armTimer = null;

      function unarm() {
        if (armTimer) { clearTimeout(armTimer); armTimer = null; }
        rm.style.display = "";
        yes.style.display = "none";
      }

      // One click never removes anything. The second click is the removal, and
      // four seconds or a click anywhere else on the page takes the offer back.
      rm.addEventListener("click", function () {
        if (busy) return;
        disarm();
        rm.style.display = "none";
        yes.style.display = "";
        armTimer = setTimeout(disarm, 4000);
        armed = { node: yes, off: unarm };
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
          yes.textContent = "Confirm remove";
          unarm();
          say(false, msg, null);
        }

        fetch(API + "remove?appid=" + encodeURIComponent(raw))
          .then(json)
          .then(function (res) {
            if (!res || typeof res !== "object") throw new Error("unreadable reply");
            if (res.error) { failed(text(res.error)); return; }

            // The row goes, its receipt stays where the row was.
            box.removeChild(r);
            entry.live = false;
            retally();
            refilter();
            say(true, (entry.name || file) + " removed. The manifest was kept " +
                "as " + (text(res.kept) || (file + ".removed")) + " rather " +
                "than deleted, because it holds the only copy of its depot " +
                "keys: rename it back to .lua to put it back. Steam keeps the " +
                "ownership until it restarts, since the loader has already " +
                "read the file.", null);
          })
          .catch(function (e) { failed("Remove failed: " + why(e)); });
      });

      /* -------------------------------------------------------- name --- */

      // Only an all-digit stem is an app id. Anything else is a hand-named file
      // the store has never heard of, and asking about it spends a request per
      // row to be told so.
      if (raw && raw === id) {
        lookup(raw, function (name) {
          if (!name) return;
          entry.name = name;
          label.textContent = name;
          label.title = name;
          // A name landing after the user has typed can bring its row into the
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

  /* -------------------------------------------------------------- cloud --- */

  /**
   * The Cloud saves page.
   *
   * Apps added by a Lua manifest are not on the account, so Valve's servers
   * answer their cloud uploads with Access Denied and those games end up with
   * no cloud at all. SteamFlipper answers the Cloud.* RPCs itself, out of a
   * folder on this machine, and that backend (src/Utils/CloudSaves) is compiled
   * into the module.
   *
   * Which leaves nothing to install, so the page is a status panel over one
   * switch. It used to carry a three step ladder for CloudRedirect: a curl
   * installer, a TOML snippet and an SLSsteam aside. None of that is loaded any
   * more, and keeping it would document a component this build does not have.
   *
   * Same store surfaces as Unlocker and Manage, because this is the third page
   * of one application and a second skin would read as a second program.
   */
  function cloudPage(data, el) {
    // Steam's discount green, the colour this page already prints "free" in.
    // Used for a condition that is met; an unmet one stays LABEL rather than
    // going red, because a switch that is off is not a fault.
    var GOOD = "#a4d007";

    // The panel and the prose stop here rather than filling a maximised window:
    // a value pinned to the far right of a 2000px row stops reading as the
    // partner of the label on the left.
    var COL = "760px";

    // A backend that answered {"error"} knows nothing about the state, so every
    // value reads "unknown" instead of "no" - which would be a claim.
    var failed = (data && data.error) ? text(data.error) : "";

    function state(v, yes, no) { return failed ? "unknown" : (v ? yes : no); }

    // `installed` is still in the reply and is now always true, so it says
    // nothing; `builtin` is what the Backend row reports.
    var enabled = !failed && !!(data && data.enabled);
    var active = !failed && !!(data && data.active);
    var apps = failed ? 0 : num(data && data.apps);
    var storage = failed ? "" : text(data && data.storage);
    // Kept only as the fallback for the toggle's reply: /api/cloud/disable
    // answers without a config path when there was no [cloud] section to edit.
    var conf = text(data && data.config);

    // Steam's store body behind the top of the page, the same wash Unlocker and
    // Manage paint. The negative offsets are .luaflipper-body's own 18px/24px
    // padding, which is what this has to escape to reach the window edges.
    var wrap = style(el("div"), { position: "relative" });
    wrap.appendChild(style(el("div"), {
      position: "absolute", top: "-18px", left: "-24px",
      width: "calc(100% + 48px)", height: "900px", zIndex: "0",
      pointerEvents: "none", background: PAGE_WASH
    }));
    // Everything else stacks above that wash; without this it paints under.
    var page = style(el("div"), { position: "relative", zIndex: "1" });
    wrap.appendChild(page);

    page.appendChild(bigHead(el, "Cloud saves"));
    page.appendChild(el("div", "luaflipper-sub",
      "Apps added by a Lua manifest get no Steam Cloud: the account does not " +
      "own them, so Steam refuses their uploads with Access Denied. " +
      "SteamFlipper answers those cloud requests itself and keeps the files " +
      "on this machine."));

    // Above the panel, not instead of it: the rows below still say which facts
    // are unknown, and the switch under them is worth reaching either way.
    if (failed) {
      page.appendChild(style(el("div", "luaflipper-error",
        "Cloud status unavailable: " + failed), {
        maxWidth: COL, marginBottom: "16px"
      }));
    }

    /* --------------------------------------------------------- status --- */

    var panel = style(el("div"), {
      background: PANEL, borderRadius: "3px", padding: "4px 14px",
      maxWidth: COL, marginBottom: "16px"
    });
    page.appendChild(panel);

    var firstFact = true;

    /**
     * One fact: dim uppercase label left, value right, hairline between rows.
     * Returns the value span, so a row the switch below invalidates can be
     * corrected instead of left contradicting the button.
     *
     * Its own row rather than metaRow's. That one is signed off for the app
     * page's details table and sets no min-width on its value, so a filesystem
     * path in it cannot shrink past its own content: the ellipsis never engages
     * and the row overflows instead. Both properties are needed here, and
     * flex 1 1 auto rather than the stylesheet's `flex: 1` (which is `1 1 0%`),
     * because shrinking is weighted by base size and a 0 basis cannot absorb
     * the negative space a long path creates.
     */
    function fact(label, value, ok, full) {
      var r = style(el("div"), {
        display: "flex", alignItems: "baseline", gap: "16px", padding: "9px 0",
        borderTop: firstFact ? "0" : "1px solid rgba(103,193,245,0.10)"
      });
      firstFact = false;
      r.appendChild(style(el("span", null, label), {
        flex: "0 0 auto", color: LABEL, textTransform: "uppercase",
        letterSpacing: "0.5px", fontSize: "11px", whiteSpace: "nowrap"
      }));
      var v = style(el("span", null, value), {
        flex: "1 1 auto", minWidth: "0", textAlign: "right", fontSize: "12.5px",
        color: ok ? GOOD : LABEL,
        overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
      });
      // The column clips by design, so anything that can be long has to stay
      // reachable some other way.
      if (full) v.title = full;
      r.appendChild(v);
      panel.appendChild(r);
      return v;
    }

    // Not routed through state(): this one is a fact about the build, not about
    // the reply, so a status lookup that failed does not make it unknown.
    fact("Backend", "built in", true);

    var enabledVal = fact("Enabled", state(enabled, "yes", "no"), enabled);
    // Enabled but not running means the flag was set after this Steam started,
    // which is the distinction this row exists to draw.
    fact("Running", state(active, "yes", "no"), active);
    // Counted by the backend, so it is 0 until the backend is up. "Running: no"
    // above is what explains a "none" here.
    fact("Apps covered", state(apps, String(apps), "none"), apps > 0);
    fact("Storage", state(storage, storage, "unknown"), !!storage, storage);

    /* -------------------------------------------------------- control --- */

    var control = style(el("div"), { maxWidth: COL, marginBottom: "18px" });
    page.appendChild(control);

    // The button is replaced rather than relabelled when the state flips:
    // storeBtn captures its resting and hover colours at construction, and the
    // two states are different colours, so relabelling in place would leave the
    // hover handlers painting the previous pair.
    var hold = el("div");
    control.appendChild(hold);

    // Every outcome lands here in LABEL, not in .luaflipper-error's red. The
    // expected failure is a config file that could not be written, which is
    // worth reading rather than alarming about.
    var told = style(el("div"), {
      display: "none", fontSize: "12px", lineHeight: "18px", color: LABEL,
      margin: "8px 0 0", wordBreak: "break-word"
    });
    control.appendChild(told);

    function say(msg) {
      told.textContent = msg;
      told.style.display = "";
    }

    /**
     * Draw the switch for a state: a neutral "Disable cloud saves" when `on`, a
     * green "Enable cloud saves" when not.
     *
     * Both endpoints only flip a flag in steamflipper.toml, which Initialize()
     * reads at startup, so neither direction takes effect until Steam restarts.
     *
     * An unknown state arms to enable. That is what someone whose status box
     * failed came here to do, and the endpoint is harmless against a flag that
     * is already true.
     */
    function arm(on) {
      var btn = only(hold, storeBtn(el,
        on ? "Disable cloud saves" : "Enable cloud saves",
        on ? DEEP : GREEN, on ? LIFT : GREEN_HOT));
      // .luaflipper-button is a div, and unlike every other storeBtn on these
      // pages this one is not in a flex row that shrinks it to fit, so without
      // this it stretches the full 760.
      style(btn, { display: "inline-block" });

      btn.addEventListener("click", function () {
        // The in-flight guard: pointer-events lands synchronously, before any
        // second click could be dispatched, and every path below ends in a
        // fresh button rather than this one re-armed.
        style(btn, { pointerEvents: "none", opacity: "0.55" });
        btn.textContent = on ? "Disabling" : "Enabling";

        fetch(API + (on ? "cloud/disable" : "cloud/enable"))
          .then(function (r) { return r.json(); })
          .then(function (res) {
            if (!res || typeof res !== "object") {
              throw new Error("unreadable reply");
            }
            // Same state, pressable again, so a refusal is answered by trying
            // again rather than by a label stuck on "Enabling".
            if (res.error) { arm(on); say(text(res.error)); return; }

            arm(!on);
            // The panel would otherwise still read "Enabled: no" directly above
            // a button now offering to disable it.
            enabledVal.textContent = on ? "no" : "yes";
            enabledVal.style.color = on ? LABEL : GOOD;
            say((on ? "Disabled" : "Enabled") + " in " +
                (text(res.config) || conf ||
                 "steamflipper.toml in the Steam folder") +
                ". It takes effect when Steam restarts.");
          })
          // Named as a transport failure: in the same muted line as the config
          // answers above, an unlabelled one would read as one of them.
          .catch(function (e) {
            arm(on);
            say("Could not reach SteamFlipper: " + why(e));
          });
      });
    }

    arm(enabled);

    /* ------------------------------------------------------- coverage --- */

    // The scope, stated plainly because it is the safety property: an owned
    // game answered locally would be a save the account's real cloud never
    // sees, and the manifest list is what keeps that from happening.
    page.appendChild(style(el("div", null,
      "Only apps with a manifest in config/stplug-in are answered here. " +
      "Games the account genuinely owns keep using Valve's cloud and are " +
      "never touched."), {
      fontSize: "13px", lineHeight: "20px", maxWidth: COL, marginBottom: "18px"
    }));

    /* -------------------------------------------------------- warning --- */

    // Under the switch, where whoever just turned this on is reading: local
    // storage is the whole design, and the cost of it is the one thing they
    // need before they trust a save to it. Amber rather than
    // .luaflipper-error's red, because nothing has gone wrong; the class is
    // kept for its geometry and the colour is overridden.
    var warn = style(el("div", "luaflipper-error"), {
      background: "rgba(220,170,60,0.12)",
      border: "1px solid rgba(220,170,60,0.35)",
      maxWidth: COL, marginBottom: "18px"
    });
    warn.appendChild(style(el("div", null,
      "Saves for these apps live only on this machine. Nothing copies them " +
      "anywhere else, so they are gone with the disk they are on."), {
      lineHeight: "18px"
    }));
    warn.appendChild(el("div", "luaflipper-sub",
      "Point a sync or backup client at the storage folder above to change " +
      "that."));
    page.appendChild(warn);

    return wrap;
  }

  /* -------------------------------------------------------------- config --- */

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
   * The update panel of the Config page.
   *
   * Split out of the renderer so the label/value rows below it still draw when
   * this has nothing to work with: version, sha and branch are top-level
   * fields of /api/config, and a backend that does not send them still has a
   * config page.
   *
   * Same store surfaces as the Cloud page, because this is the second switch
   * in one application and a second skin would read as a second program.
   *
   * What this panel can and cannot do decides its whole wording. The module
   * Steam has mapped was built on this machine, so an update is a commit
   * rather than a download, and the green button fast-forwards the source
   * checkout and stops there. Rebuilding needs the 32-bit toolchain and an
   * installer that refuses to run while Steam is up - which is exactly when
   * this page exists - so the success state hands over a command instead of
   * claiming an update that has not happened yet.
   */
  function updatePanel(data, el) {
    // Steam's discount green, the colour the Cloud page prints a met condition
    // in. An unmet one stays LABEL rather than going red: being behind the
    // branch is not a fault.
    var GOOD = "#a4d007";
    // The panel stops here rather than filling a maximised window, for the
    // reason the Cloud page's does: a value pinned to the far right of a
    // 2000px row stops reading as the partner of the label on the left.
    var COL = "760px";

    var sha = text(data && data.sha);
    var branch = text(data && data.branch);
    var known = !!sha && sha !== "unknown";

    var box = style(el("div"), { maxWidth: COL, marginBottom: "24px" });
    box.appendChild(sectionHead(el, "Updates"));

    var panel = style(el("div"), {
      background: PANEL, borderRadius: "3px", padding: "4px 14px",
      marginBottom: "12px"
    });
    box.appendChild(panel);

    var firstFact = true;

    /**
     * One fact: dim uppercase label left, value right, hairline between rows.
     * Returns the value span, so a reply that arrives later can correct a row
     * instead of leaving it contradicting the button under it.
     *
     * Its own row rather than metaRow's, for the reason the Cloud page gives:
     * that one is signed off for the app page's details table and sets no
     * min-width on its value, so a long value cannot shrink past its own
     * content, the ellipsis never engages and the row overflows instead. Both
     * properties are needed, and flex 1 1 auto rather than the stylesheet's
     * `flex: 1` (which is `1 1 0%`), because shrinking is weighted by base
     * size and a 0 basis cannot absorb the negative space.
     */
    function fact(label, value, ok, full) {
      var r = style(el("div"), {
        display: "flex", alignItems: "baseline", gap: "16px", padding: "9px 0",
        borderTop: firstFact ? "0" : "1px solid rgba(103,193,245,0.10)"
      });
      firstFact = false;
      r.appendChild(style(el("span", null, label), {
        flex: "0 0 auto", color: LABEL, textTransform: "uppercase",
        letterSpacing: "0.5px", fontSize: "11px", whiteSpace: "nowrap"
      }));
      var v = style(el("span", null, value), {
        flex: "1 1 auto", minWidth: "0", textAlign: "right", fontSize: "12.5px",
        color: ok ? GOOD : LABEL,
        overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"
      });
      // The column clips by design, so anything that can be long has to stay
      // reachable some other way.
      if (full) v.title = full;
      r.appendChild(v);
      panel.appendChild(r);
      return v;
    }

    fact("Version", text(data && data.version) || "unknown", false);
    fact("Built from", known ? (sha + (branch ? " on " + branch : ""))
                             : "no commit, built outside a git tree", known);
    var headVal = fact("Branch head", "not checked yet", false);

    // Appended on the first answer that carries one, rather than reserved
    // empty: a blank row above the button would read as a fact we are missing.
    var msgVal = null;
    function commit(m) {
      if (msgVal) {
        msgVal.textContent = m;
        msgVal.title = m;
        return;
      }
      msgVal = fact("Latest commit", m, false, m);
    }

    /* --------------------------------------------------------- control --- */

    // The button is replaced rather than relabelled when the state flips:
    // storeBtn captures its resting and hover colours at construction, and the
    // two states are different colours, so relabelling in place would leave
    // the hover handlers painting the previous pair.
    var hold = el("div");
    box.appendChild(hold);

    // Every outcome lands here in LABEL, not in .luaflipper-error's red. The
    // expected ones are "up to date" and "the tree has local changes", both
    // worth reading and neither worth alarming about.
    var told = style(el("div"), {
      display: "none", fontSize: "12px", lineHeight: "18px", color: LABEL,
      margin: "10px 0 0", wordBreak: "break-word"
    });
    box.appendChild(told);

    function say(msg) {
      clear(told);
      told.appendChild(el("div", null, msg));
      told.style.display = "";
      return told;
    }

    function json(r) { return r.json(); }

    /** Blue "check", the resting state and the one every failure returns to. */
    function armCheck(label) {
      var btn = only(hold, storeBtn(el, label || "Check for updates",
                                    BLUE, BLUE_HOT));
      // .luaflipper-button is a div, and unlike the storeBtns on the store
      // pages this one is not in a flex row that shrinks it to fit, so without
      // this it stretches the full column.
      style(btn, { display: "inline-block" });

      btn.addEventListener("click", function () {
        // The in-flight guard: pointer-events lands synchronously, before any
        // second click could be dispatched, and every path below ends in a
        // fresh button rather than this one re-armed.
        style(btn, { pointerEvents: "none", opacity: "0.55" });
        btn.textContent = "Checking";

        fetch(API + "update/check").then(json).then(function (res) {
          if (!res || typeof res !== "object") {
            throw new Error("unreadable reply");
          }
          if (res.error) { armCheck(); say(text(res.error) + "."); return; }

          // A build with no commit behind it. Nothing was compared, so the
          // row must not claim a verdict either way.
          if (res.reason) {
            headVal.textContent = "cannot compare";
            armCheck();
            say(text(res.reason) + ".");
            return;
          }

          headVal.textContent = text(res.remote);
          if (res.message) commit(text(res.message));

          if (!res.behind) {
            headVal.style.color = GOOD;
            armCheck();
            say("SteamFlipper is up to date.");
            return;
          }
          headVal.style.color = LINK_TEXT;
          armApply();
          say("The " + (text(res.branch) || "remote") + " branch has moved on " +
              "since this build. Updating pulls the source; it does not touch " +
              "the running client.");
        }).catch(function (e) {
          // Named as a transport failure: in the same muted line as the
          // answers above, an unlabelled one would read as one of them.
          armCheck();
          say("Could not reach SteamFlipper: " + why(e));
        });
      });
    }

    /** Green "update", offered only once a check said the branch has moved. */
    function armApply() {
      var btn = only(hold, storeBtn(el, "Update source", GREEN, GREEN_HOT));
      style(btn, { display: "inline-block" });

      btn.addEventListener("click", function () {
        style(btn, { pointerEvents: "none", opacity: "0.55" });
        btn.textContent = "Updating";

        fetch(API + "update/apply").then(json).then(function (res) {
          if (!res || typeof res !== "object") {
            throw new Error("unreadable reply");
          }
          // Back to Check rather than back to Update, in both directions.
          // Every refusal here (local changes, a branch that cannot
          // fast-forward, no source tree configured) is fixed outside this
          // window, and a success has nothing left to pull.
          armCheck("Check again");

          if (res.error) { say(text(res.error) + "."); return; }

          var line = say(res.status === "already-current"
            ? "The source was already at " + text(res.pulled) + "."
            : "Source updated to " + text(res.pulled) + ". The running client " +
              "is unchanged.");
          line.appendChild(el("div", "luaflipper-sub",
            "Close Steam, then build and install it:"));
          line.appendChild(shellLine(el, text(res.command) ||
            "./tools/install_linux.sh"));
        }).catch(function (e) {
          armCheck("Check again");
          say("Could not reach SteamFlipper: " + why(e));
        });
      });
    }

    armCheck();
    return box;
  }

  /**
   * The Config page: the panel that keeps this install current, over the
   * label/value rows describing what it loaded.
   *
   * The panel goes first because it is the only thing on the page with a
   * button, and reference does not outrank an action.
   */
  function configPage(data, el) {
    var err = errorEl(data, el);
    if (err) return err;

    var wrap = el("div");
    wrap.appendChild(updatePanel(data, el));
    wrap.appendChild(sectionHead(el, "Configuration"));
    wrap.appendChild(rowsPage(data, el,
      "No configuration loaded. steamflipper.toml sits in the Steam directory."));
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
    // Cloud saves is a switch with a status panel over it, not a label/value
    // dump: the backend is compiled in, so the only thing to decide on that
    // page is whether it is on.
    cloud: cloudPage,
    // Config carries the self-update panel as well as the settings rows: it is
    // the page you are already on when you want to know what this build is,
    // and "what build is this" and "is it current" are one question.
    config: configPage,
    status: function (data, el) {
      return rowsPage(data, el, "No status reported.");
    }
  };
})();
