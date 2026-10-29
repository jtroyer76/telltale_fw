"""Talks to a running Telltale's HTTP API (src/api.h).

    python tools/telltale.py <host> status
    python tools/telltale.py <host> get-config [out.json]
    python tools/telltale.py <host> validate <config.json>
    python tools/telltale.py <host> put-config <config.json>
    python tools/telltale.py <host> reset-config          (back to the built-in config)
    python tools/telltale.py <host> widgets
    python tools/telltale.py <host> screen <index>
    python tools/telltale.py <host> restart
    python tools/telltale.py <host> screenshot <out.png> [screen index]

<host> is e.g. telltale.local or an IP. Commands that change things need the device password in
the TELLTALE_OTA_PASS environment variable (= the device password, under Settings on the screen).
"""

import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request
import zlib


def call(host, method, path, body=None, auth=False):
    headers = {"Content-Type": "application/json"}
    if auth:
        password = os.environ.get("TELLTALE_OTA_PASS")
        if not password:
            sys.exit("Set TELLTALE_OTA_PASS (the device password, under Settings on the screen)")
        headers["X-OTA-Password"] = password
    req = urllib.request.Request(f"http://{host}{path}", data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, dict(resp.headers), resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode()
    except OSError as e:
        sys.exit(f"{host}: {e}")


def show(status, text):
    try:
        print(json.dumps(json.loads(text), indent=2))
    except ValueError:
        print(text.strip())
    if status >= 400:
        sys.exit(1)


def bmp565_to_png(bmp):
    """The display's screenshot (16-bit BMP, RGB565, top row first) as PNG bytes."""
    if bmp[:2] != b"BM":
        sys.exit("not a screenshot: " + bmp[:200].decode(errors="replace"))
    offset, = struct.unpack_from("<I", bmp, 10)
    w, h = struct.unpack_from("<ii", bmp, 18)
    h = abs(h)
    raw = bytearray()
    for y in range(h):
        raw.append(0)   # PNG filter: none
        row = memoryview(bmp)[offset + y * w * 2: offset + (y + 1) * w * 2].cast("H")
        for p in row:
            r, g, b = p >> 11, (p >> 5) & 0x3F, p & 0x1F
            raw += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def screenshot(host, out, index=None):
    if index is not None:
        call(host, "POST", f"/api/screen?index={index}")
        time.sleep(3)   # the swipe animation, and fresh values
    try:
        with urllib.request.urlopen(f"http://{host}/api/screenshot", timeout=30) as resp:
            bmp = resp.read()
    except OSError as e:
        sys.exit(f"{host}: {e}")
    with open(out, "wb") as f:
        f.write(bmp565_to_png(bmp) if out.lower().endswith(".png") else bmp)
    print(out)


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    host, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3:]

    if cmd == "status":
        show(*call(host, "GET", "/api/status")[::2])
    elif cmd == "get-config":
        status, headers, text = call(host, "GET", "/api/config")
        print(f"source: {headers.get('X-Config-Source', '?')}", file=sys.stderr)
        if args:
            with open(args[0], "w", encoding="utf-8") as f:
                f.write(text)
        else:
            show(status, text)
    elif cmd == "validate" and args:
        show(*call(host, "POST", "/api/config/validate", read_file(args[0]))[::2])
    elif cmd == "put-config" and args:
        show(*call(host, "PUT", "/api/config", read_file(args[0]), auth=True)[::2])
    elif cmd == "reset-config":
        show(*call(host, "DELETE", "/api/config", auth=True)[::2])
    elif cmd == "widgets":
        show(*call(host, "GET", "/api/widgets")[::2])
    elif cmd == "screen" and args:
        show(*call(host, "POST", f"/api/screen?index={int(args[0])}")[::2])
    elif cmd == "screenshot" and args:
        screenshot(host, args[0], int(args[1]) if len(args) > 1 else None)
    elif cmd == "restart":
        show(*call(host, "POST", "/api/restart", b"", auth=True)[::2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
