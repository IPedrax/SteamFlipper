#!/usr/bin/env python3
"""Runnable check for the injector's choice of CEF target.

    ./tools/client_target_test.py

The injector finds the main client window by searching /json/list for a needle
and then reading the webSocketDebuggerUrl that follows it. Which needle matches
decides whether the UI is injected at all, and getting it wrong is silent: the
module loads, serves its API, reports "UI injected: not yet" forever, and every
check that can be run from outside looks the same as a client that has no nav.

The fixtures are real captures, not hand-written: one from the desktop client
and one taken while Big Picture was open. Big Picture is the reason this exists.
It replaces the main window rather than joining it, so switching into it leaves
no target called "Steam" and no createflags=18 either, and the two needles that
existed before both missed.

Mirrors the needle order in LuaFlipperUI.cpp. If that order changes, change it
here: this asserts the fixtures resolve, not that the C++ says so.
"""
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"

# The order matters and is the order in the C++: the exact desktop title first,
# Big Picture second, and the createflags shape only as a last resort.
NEEDLES = ['"Steam"', '"Steam Big Picture Mode"', "createflags=18"]

failures = 0


def check(ok, what):
    global failures
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures += 1


def resolve(doc: str):
    """The injector's own logic: first needle that hits, then the ws url after it."""
    for needle in NEEDLES:
        at = doc.find(needle)
        if at != -1:
            m = re.search(r'"webSocketDebuggerUrl"\s*:\s*"([^"]+)"', doc[at:])
            return needle, (m.group(1) if m else None)
    return None, None


def title_of(doc: str, ws: str):
    for t in json.loads(doc):
        if t.get("webSocketDebuggerUrl") == ws:
            return t.get("title")
    return None


for name, expect_title in (("json_list_desktop.json", "Steam"),
                           ("json_list_bigpicture.json", "Steam Big Picture Mode")):
    path = FIXTURES / name
    print(f"{name}")
    if not path.is_file():
        check(False, "fixture is present")
        continue

    doc = path.read_text()
    needle, ws = resolve(doc)
    check(ws is not None, f"a target is found (matched on {needle})")
    check(ws is not None and ws.startswith("ws://"),
          "and it yields a websocket url to inject through")
    got = title_of(doc, ws) if ws else None
    check(got == expect_title,
          f"it is the client window, not a menu or popup (got {got!r})")

# The regression this was written for. Before Big Picture was in the list, both
# original needles missed it and the injector logged "target was not found".
bp = FIXTURES / "json_list_bigpicture.json"
if bp.is_file():
    doc = bp.read_text()
    print("Big Picture, with the pre-fix needles only")
    old = [n for n in ('"Steam"', "createflags=18") if n in doc]
    check(not old, f"neither old needle matches, which is the bug ({old})")

print("\nall passed" if not failures else f"\n{failures} FAILED")
sys.exit(1 if failures else 0)
