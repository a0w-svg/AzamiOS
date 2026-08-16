#!/usr/bin/env python3
import os
import struct

# Catppuccin Mocha colors in 0xAARRGGBB format
TRANSPARENT = 0x00000000
BASE        = 0xFF1E1E2E
SURFACE0    = 0xFF313244
SURFACE1    = 0xFF45475A
TEXT        = 0xFFCDD6F4
MAUVE       = 0xFFCBA6F7
BLUE        = 0xFF89B4FA
SAPPHIRE    = 0xFF74C7EC
GREEN       = 0xFFA6E3A1
YELLOW      = 0xFFF9E2AF
PEACH       = 0xFFFAB387
RED         = 0xFFF38BA8
TEAL        = 0xFF94E2D5
LAVENDER    = 0xFFB4BEFE

def create_blank(bg=TRANSPARENT):
    return [bg] * (32 * 32)

def set_pixel(buf, x, y, color):
    if 0 <= x < 32 and 0 <= y < 32:
        buf[y * 32 + x] = color

def fill_rect(buf, rx, ry, rw, rh, color):
    for y in range(ry, ry + rh):
        for x in range(rx, rx + rw):
            set_pixel(buf, x, y, color)

def fill_rounded_rect(buf, rx, ry, rw, rh, r, color):
    for y in range(ry, ry + rh):
        for x in range(rx, rx + rw):
            if x < rx + r and y < ry + r and (x - (rx + r))**2 + (y - (ry + r))**2 > r**2:
                continue
            if x >= rx + rw - r and y < ry + r and (x - (rx + rw - r - 1))**2 + (y - (ry + r))**2 > r**2:
                continue
            if x < rx + r and y >= ry + rh - r and (x - (rx + r))**2 + (y - (ry + rh - r - 1))**2 > r**2:
                continue
            if x >= rx + rw - r and y >= ry + rh - r and (x - (rx + rw - r - 1))**2 + (y - (ry + rh - r - 1))**2 > r**2:
                continue
            set_pixel(buf, x, y, color)

def save_icn(filepath, buf):
    os.makedirs(os.path.dirname(filepath), exist_ok=True)
    with open(filepath, 'wb') as f:
        for p in buf:
            f.write(struct.pack('<I', p))
    print(f"Generated {filepath}")

def gen_texteditor():
    buf = create_blank()
    fill_rounded_rect(buf, 4, 2, 24, 28, 4, BASE)
    fill_rounded_rect(buf, 6, 4, 20, 24, 2, SURFACE0)
    fill_rect(buf, 9, 8, 14, 2, MAUVE)
    fill_rect(buf, 9, 13, 14, 2, TEXT)
    fill_rect(buf, 9, 17, 10, 2, TEXT)
    fill_rect(buf, 9, 21, 12, 2, LAVENDER)
    return buf

def gen_terminal():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 4, 28, 24, 4, BASE)
    fill_rect(buf, 2, 4, 28, 6, SURFACE0)
    set_pixel(buf, 5, 7, RED)
    set_pixel(buf, 8, 7, YELLOW)
    set_pixel(buf, 11, 7, GREEN)
    set_pixel(buf, 7, 14, GREEN)
    set_pixel(buf, 8, 15, GREEN)
    set_pixel(buf, 7, 16, GREEN)
    fill_rect(buf, 11, 16, 6, 2, TEXT)
    return buf

def gen_filemanager():
    buf = create_blank()
    fill_rounded_rect(buf, 4, 6, 12, 6, 2, PEACH)
    fill_rounded_rect(buf, 4, 10, 24, 18, 4, BLUE)
    fill_rect(buf, 6, 14, 20, 12, SURFACE0)
    return buf

def gen_calculator():
    buf = create_blank()
    fill_rounded_rect(buf, 4, 2, 24, 28, 4, BASE)
    fill_rect(buf, 7, 5, 18, 6, TEAL)
    colors = [MAUVE, SAPPHIRE, PEACH, GREEN]
    for row in range(3):
        for col in range(3):
            fill_rect(buf, 7 + col * 6, 14 + row * 5, 4, 3, colors[(row + col) % len(colors)])
    return buf

def gen_sysmon():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 4, 28, 24, 4, BASE)
    fill_rounded_rect(buf, 4, 6, 24, 20, 2, SURFACE0)
    points = [(5,16), (9,16), (12,9), (15,22), (18,12), (22,16), (26,16)]
    for i in range(len(points)-1):
        x1, y1 = points[i]
        x2, y2 = points[i+1]
        for x in range(x1, x2+1):
            t = (x - x1) / max(1, (x2 - x1))
            y = int(y1 + t * (y2 - y1))
            set_pixel(buf, x, y, GREEN)
            set_pixel(buf, x, y+1, GREEN)
    return buf

def gen_settings():
    buf = create_blank()
    fill_rounded_rect(buf, 4, 4, 24, 24, 12, SURFACE1)
    fill_rounded_rect(buf, 10, 10, 12, 12, 6, BASE)
    teeth = [(15, 2), (16, 2), (15, 29), (16, 29), (2, 15), (2, 16), (29, 15), (29, 16)]
    for tx, ty in teeth:
        set_pixel(buf, tx, ty, MAUVE)
    return buf

def gen_clock():
    buf = create_blank()
    fill_rounded_rect(buf, 3, 3, 26, 26, 13, SAPPHIRE)
    fill_rounded_rect(buf, 5, 5, 22, 22, 11, BASE)
    for y in range(11, 16):
        set_pixel(buf, 15, y, TEXT)
        set_pixel(buf, 16, y, TEXT)
    for x in range(16, 21):
        set_pixel(buf, x, 15, RED)
        set_pixel(buf, x, 16, RED)
    return buf

def gen_about():
    buf = create_blank()
    fill_rounded_rect(buf, 3, 3, 26, 26, 13, LAVENDER)
    fill_rounded_rect(buf, 5, 5, 22, 22, 11, BASE)
    fill_rect(buf, 14, 9, 4, 3, LAVENDER)
    fill_rect(buf, 14, 14, 4, 9, LAVENDER)
    return buf

def main():
    generators = {
        'userland/apps/texteditor/texteditor.icn': gen_texteditor,
        'userland/apps/terminal/terminal.icn': gen_terminal,
        'userland/apps/filemanager/filemanager.icn': gen_filemanager,
        'userland/apps/calculator/calculator.icn': gen_calculator,
        'userland/apps/sysmon/sysmon.icn': gen_sysmon,
        'userland/apps/settings/settings.icn': gen_settings,
        'userland/apps/clock/clock.icn': gen_clock,
        'userland/apps/about/about.icn': gen_about,
    }
    for path, gen_fn in generators.items():
        buf = gen_fn()
        save_icn(path, buf)

if __name__ == '__main__':
    main()
