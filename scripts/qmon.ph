#!/usr/bin/env python3
"""Drive a running AzamiOS guest through QEMU's HMP monitor socket.

    qmon.py shot <out.png>        screenshot to PNG
    qmon.py cmd "<hmp command>"   raw monitor command
    qmon.py move <x> <y>          put the pointer at absolute (x,y)
    qmon.py click <x> <y>         move there, then left-click
    qmon.py rclick <x> <y>        move there, then right-click
    qmon.py dclick <x> <y>        move there, then double-click
    qmon.py drag <x1> <y1> <x2> <y2>
    qmon.py key <k> [k...]        sendkey (e.g. ret, esc, ctrl-alt-t)
    qmon.py type "text"           type a string

Positioning note: azwm accelerates a mouse packet whose delta magnitude is
over 6px (dx = dx*14/10, see azwm/main.c), so every move here is made out of
<=6px steps, which are passed through 1:1. The pointer is first pinned to
(0,0) with a few oversized negative moves (they clamp), so absolute
coordinates are exact without needing to know where the cursor started.
All of it goes down one monitor connection in a single write, which is what
makes a few hundred small steps take about a second instead of a minute.
"""
import os
import socket
import struct
import sys
import time
import zlib

SOCK = os.environ.get(
    "QMON_SOCK",
    "/tmp/claude-1000/-home-a0wsvg-AzamiOS/92b2ea9c-e514-424b-9dcc-d3d5db7ae574/scratchpad/qemu-mon.sock")
STEP = 6            # largest delta azwm does not accelerate
SETTLE = float(os.environ.get("QMON_SETTLE", "0.6"))


def send(cmds, settle=SETTLE, read=True):
    """Send a list of HMP commands over one connection."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(20)
    s.connect(SOCK)
    time.sleep(0.1)
    try:
        s.recv(1 << 16)
    except socket.timeout:
        pass
    s.sendall(("\n".join(cmds) + "\n").encode())
    out = b""
    if read:
        time.sleep(settle)
        try:
            while True:
                chunk = s.recv(1 << 16)
                if not chunk:
                    break
                out += chunk
                if out.rstrip().endswith(b"(qemu)"):
                    break
        except socket.timeout:
            pass
    s.close()
    return out.decode(errors="replace")


def steps_to(x, y):
    """Pin to the origin, then walk to (x,y) in <=STEP px increments."""
    cmds = ["mouse_move -400 -400"] * 6
    dx, dy = int(x), int(y)
    while dx > 0 or dy > 0:
        sx, sy = min(STEP, dx), min(STEP, dy)
        cmds.append(f"mouse_move {sx} {sy}")
        dx -= sx
        dy -= sy
    return cmds


def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, "rb") as f:
        data = f.read()
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = fields
    pix = data[i:i + w * h * 3]
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pix[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    with open(png_path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" +
                chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 6)) +
                chunk(b"IEND", b""))
    return w, h


def shot(out):
    ppm = out + ".ppm"
    if os.path.exists(ppm):
        os.unlink(ppm)
    send([f"screendump {ppm}"], settle=1.0)
    for _ in range(60):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
            size = os.path.getsize(ppm)
            time.sleep(0.2)
            if os.path.getsize(ppm) == size:
                break
        time.sleep(0.2)
    w, h = ppm_to_png(ppm, out)
    os.unlink(ppm)
    print(f"{out} {w}x{h}")


KEYNAMES = {" ": "spc", "-": "minus", "=": "equal", "/": "slash", "\\": "backslash",
            ".": "dot", ",": "comma", ";": "semicolon", "'": "apostrophe",
            "[": "bracket_left", "]": "bracket_right", "`": "grave_accent",
            "\n": "ret", "\t": "tab",
            "_": "shift-minus", ":": "shift-semicolon", '"': "shift-apostrophe",
            "?": "shift-slash", "|": "shift-backslash", "~": "shift-grave_accent",
            "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4",
            "%": "shift-5", "^": "shift-6", "&": "shift-7", "*": "shift-8",
            "(": "shift-9", ")": "shift-0", "+": "shift-equal",
            "<": "shift-comma", ">": "shift-dot", "{": "shift-bracket_left",
            "}": "shift-bracket_right"}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    a = sys.argv
    what = a[1]

    if what == "shot":
        shot(a[2])
    elif what == "cmd":
        print(send([a[2]]))
    elif what == "move":
        send(steps_to(a[2], a[3]))
        print(f"pointer at {a[2]},{a[3]}")
    elif what in ("click", "rclick", "dclick"):
        btn = "2" if what == "rclick" else "1"
        cmds = steps_to(a[2], a[3])
        send(cmds)
        time.sleep(0.3)
        if what == "dclick":
            send(["mouse_button 1", "mouse_button 0",
                  "mouse_button 1", "mouse_button 0"], settle=0.5)
        else:
            send([f"mouse_button {btn}"], settle=0.25)
            send(["mouse_button 0"], settle=0.5)
        print(f"{what} {a[2]},{a[3]}")
    elif what == "drag":
        x1, y1, x2, y2 = (int(v) for v in a[2:6])
        send(steps_to(x1, y1))
        time.sleep(0.2)
        send(["mouse_button 1"], settle=0.3)
        dx, dy = x2 - x1, y2 - y1
        cmds = []
        while dx or dy:
            sx = max(-STEP, min(STEP, dx))
            sy = max(-STEP, min(STEP, dy))
            cmds.append(f"mouse_move {sx} {sy}")
            dx -= sx
            dy -= sy
        send(cmds, settle=0.4)
        send(["mouse_button 0"], settle=0.5)
        print(f"drag {x1},{y1} -> {x2},{y2}")
    elif what == "key":
        send([f"sendkey {k}" for k in a[2:]], settle=0.4)
        print("keys:", " ".join(a[2:]))
    elif what == "type":
        cmds = []
        for ch in a[2]:
            k = KEYNAMES.get(ch)
            if k is None:
                k = ("shift-" + ch.lower()) if ch.isupper() else ch
            cmds.append("sendkey " + k)
        send(cmds, settle=0.5)
        print("typed:", a[2])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
