#!/usr/bin/env python3
"""Ask the running Steam client which of LUAFlipper's anchors actually exist.

    ./tools/ui_probe.py          # with Steam running

The injected UI hangs off a handful of Steam class names, and when one of them
is missing the module still loads, still serves its API, and still reports
"UI injected: yes". From outside, a client whose markup we cannot attach to is
indistinguishable from one where everything worked. This prints the counts, so
the difference takes one command instead of a week.

What the numbers mean:

    .SuperNavBar 1, .ContentFrame 1     the desktop client, as expected. If the
                                        tab is still missing, the fault is after
                                        attachment, not before it.
    all zero                            the Deck UI, which shares none of this
                                        markup. The corner launcher is the way
                                        in there.
    .SuperNavBar 1, .ContentFrame 0     the bad middle. A nav exists but the
                                        page has nowhere to mount, so the
                                        launcher appears and its panel is drawn
                                        under Steam's browser views.

Speaks CDP over a raw socket because Steam's debugger is a websocket and no
websocket library can be assumed present. Reads only; evaluates nothing that
changes the page.
"""
import base64
import json
import os
import socket
import struct
import sys
import urllib.request

PORT = 8080


def targets():
    url = f"http://127.0.0.1:{PORT}/json/list"
    with urllib.request.urlopen(url, timeout=5) as r:
        return json.load(r)


class Debugger:
    """Just enough of the protocol to evaluate an expression and read the reply."""

    def __init__(self, ws_url):
        rest = ws_url.split("://", 1)[1]
        hostport, path = rest.split("/", 1)
        host, port = hostport.split(":")
        self.sock = socket.create_connection((host, int(port)), timeout=15)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            (f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\n"
             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
             f"Sec-WebSocket-Key: {key}\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("the debugger closed during the handshake")
            buf += chunk
        if b"101" not in buf.split(b"\r\n")[0]:
            raise RuntimeError("the debugger refused the upgrade")
        self.rest = buf.split(b"\r\n\r\n", 1)[1]
        self.seq = 0

    def _read(self, n):
        out, self.rest = self.rest[:n], self.rest[n:]
        while len(out) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("the debugger closed mid-frame")
            need = n - len(out)
            out += chunk[:need]
            self.rest = chunk[need:]
        return out

    def evaluate(self, expression):
        self.seq += 1
        payload = json.dumps({
            "id": self.seq, "method": "Runtime.evaluate",
            "params": {"expression": expression, "returnByValue": True},
        }).encode()

        # Client frames must be masked; the protocol requires it.
        mask = os.urandom(4)
        header = b"\x81"
        if len(payload) < 126:
            header += bytes([0x80 | len(payload)])
        else:
            header += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
        self.sock.sendall(
            header + mask +
            bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

        while True:
            b0, b1 = self._read(2)
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            if b1 & 0x80:
                self._read(4)
            data = self._read(length)
            if (b0 & 0x0F) not in (1, 2):     # not text or binary
                continue
            message = json.loads(data.decode("utf-8", "replace"))
            if message.get("id") == self.seq:
                return message.get("result", {}).get("result", {}).get("value")


# Both names the injector itself looks for, in its order, so this cannot
# disagree with what the module does.
CLIENT_TITLES = ("Steam", "Steam Big Picture Mode")

# Why each one is here, printed alongside the count so a zero explains itself.
ANCHORS = (
    (".SuperNavBar",             "the nav bar the tab attaches to"),
    (".SuperNavBar .MenuButton", "the stock tabs it is cloned from"),
    (".MenuWrapper",             "the wrapper carrying the nav's own styling"),
    (".ContentFrame",            "where a page is mounted"),
    (".LocalContentContainer",   "Steam's own view, hidden while a page is open"),
    (".BrowserWrapper",          "the browser view, which paints over any DOM"),
)


def main():
    try:
        found = targets()
    except Exception as e:
        sys.exit(f"no answer from the CEF debugger on 127.0.0.1:{PORT}: {e}\n"
                 "Is Steam running, and does <Steam>/.cef-enable-remote-debugging exist?")

    window = None
    for title in CLIENT_TITLES:
        window = next((t for t in found if t.get("title") == title), None)
        if window:
            break
    if not window:
        print("titles present:", [t.get("title") for t in found])
        sys.exit("no client window among them; the module has nothing to inject into")

    print(f"client window  : {window.get('title')}")
    dbg = Debugger(window["webSocketDebuggerUrl"])
    print(f"url            : {str(dbg.evaluate('location.href'))[:70]}")

    for selector, why in ANCHORS:
        count = dbg.evaluate(
            f"document.querySelectorAll({selector!r}).length")
        print(f"{selector:26} {str(count):>4}   {why}")

    injected = dbg.evaluate("typeof window.__luaflipperCleanup")
    has_tab = dbg.evaluate("!!document.getElementById('luaflipper-tab')")
    has_launcher = dbg.evaluate("!!document.getElementById('luaflipper-deck-btn')")
    print(f"{'script injected':26} {str(injected):>4}")
    print(f"{'nav tab':26} {str(has_tab):>4}")
    print(f"{'corner launcher':26} {str(has_launcher):>4}")

    # The combination that produces "a panel appears behind Steam's content".
    if has_launcher and not has_tab:
        print("\nThe corner launcher is up on a client that has a nav bar.")
        print("Its panel is a fixed overlay, and Steam's browser views paint")
        print("over any DOM regardless of stacking, so it renders underneath.")


if __name__ == "__main__":
    main()
