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
SKY         = 0xFF89DCEB
PINK        = 0xFFF5C2E7
FLAMINGO    = 0xFFF38BA8
ORANGE      = 0xFFFAB387  # alias of PEACH, used where "orange tile" reads clearer

def create_blank(bg=TRANSPARENT):
    return [bg] * (32 * 32)

def set_pixel(buf, x, y, color):
    if 0 <= x < 32 and 0 <= y < 32:
        buf[y * 32 + x] = color

def fill_circle(buf, cx, cy, r, color):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                set_pixel(buf, x, y, color)

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

def gen_paint():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 8, 24, 20, 10, MAUVE)
    fill_circle(buf, 10, 14, 3, RED)
    fill_circle(buf, 16, 11, 3, YELLOW)
    fill_circle(buf, 22, 14, 3, GREEN)
    fill_circle(buf, 12, 20, 3, BLUE)
    # brush handle
    for i in range(9):
        set_pixel(buf, 24 + i, 6 - i // 2, SURFACE1)
    fill_circle(buf, 25, 5, 2, TEXT)
    return buf

def gen_audioplayer():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 2, 28, 28, 6, BASE)
    fill_circle(buf, 11, 23, 4, FLAMINGO)
    fill_circle(buf, 23, 20, 4, FLAMINGO)
    fill_rect(buf, 14, 8, 2, 15, FLAMINGO)
    fill_rect(buf, 26, 5, 2, 15, FLAMINGO)
    fill_rect(buf, 14, 8, 14, 2, FLAMINGO)
    return buf

def gen_fetch():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 2, 28, 28, 6, BASE)
    fill_rect(buf, 5, 5, 10, 10, LAVENDER)
    fill_rect(buf, 17, 5, 10, 10, BLUE)
    fill_rect(buf, 5, 17, 10, 10, GREEN)
    fill_rect(buf, 17, 17, 10, 10, PEACH)
    return buf

def gen_screenshot():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 6, 28, 20, 4, SURFACE0)
    fill_rounded_rect(buf, 4, 8, 24, 16, 2, BASE)
    fill_rect(buf, 11, 2, 10, 5, SURFACE1)
    fill_circle(buf, 16, 16, 6, SAPPHIRE)
    fill_circle(buf, 16, 16, 3, BASE)
    return buf

def gen_minesweeper():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 2, 28, 28, 3, SURFACE1)
    for gx in range(3):
        for gy in range(3):
            fill_rect(buf, 4 + gx * 9, 4 + gy * 9, 8, 8, SURFACE0)
    fill_circle(buf, 16, 16, 6, TEXT)
    fill_circle(buf, 16, 16, 4, SURFACE1)
    for dx, dy in ((-6, 0), (6, 0), (0, -6), (0, 6), (-4, -4), (4, -4), (-4, 4), (4, 4)):
        set_pixel(buf, 16 + dx, 16 + dy, TEXT)
    fill_circle(buf, 14, 14, 1, BASE)
    return buf

def gen_2048():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 2, 28, 28, 4, SURFACE0)
    fill_rounded_rect(buf, 4, 4, 12, 12, 2, ORANGE)
    fill_rounded_rect(buf, 18, 4, 10, 12, 2, YELLOW)
    fill_rounded_rect(buf, 4, 18, 10, 10, 2, RED)
    fill_rounded_rect(buf, 16, 18, 12, 10, 2, GREEN)
    return buf

def gen_snake():
    buf = create_blank()
    fill_rounded_rect(buf, 2, 2, 28, 28, 4, BASE)
    segs = [(6, 22), (10, 22), (14, 22), (14, 18), (14, 14), (18, 14), (22, 14), (22, 10)]
    for i, (sx, sy) in enumerate(segs):
        col = GREEN if i < len(segs) - 1 else TEAL  # head a shade lighter
        fill_rect(buf, sx, sy, 4, 4, col)
    fill_circle(buf, 25, 7, 2, RED)  # apple
    return buf

def gen_xclock():
    buf = create_blank()
    fill_rounded_rect(buf, 3, 3, 26, 26, 13, SKY)
    fill_rounded_rect(buf, 5, 5, 22, 22, 11, BASE)
    for y in range(9, 16):
        set_pixel(buf, 16, y, TEXT)
    for x in range(16, 22):
        set_pixel(buf, x, 16, TEXT)
    fill_circle(buf, 16, 16, 1, SKY)
    return buf

def gen_xeyes():
    buf = create_blank()
    fill_rounded_rect(buf, 1, 8, 30, 16, 6, PINK)
    fill_circle(buf, 10, 16, 6, TEXT)
    fill_circle(buf, 22, 16, 6, TEXT)
    fill_circle(buf, 11, 16, 2, BASE)
    fill_circle(buf, 23, 16, 2, BASE)
    return buf

def gen_xcalc():
    buf = create_blank()
    fill_rounded_rect(buf, 4, 2, 24, 28, 4, BASE)
    fill_rect(buf, 7, 5, 18, 6, YELLOW)
    colors = [SAPPHIRE, MAUVE, PEACH, GREEN]
    for row in range(3):
        for col in range(3):
            fill_rect(buf, 7 + col * 6, 14 + row * 5, 4, 3, colors[(row + col) % len(colors)])
    return buf

def gen_xgui_demo():
    buf = create_blank()
    fill_rounded_rect(buf, 5, 8, 22, 20, 3, SURFACE0)
    fill_rounded_rect(buf, 2, 4, 22, 20, 3, LAVENDER)
    fill_rect(buf, 2, 4, 22, 5, SURFACE1)
    fill_rect(buf, 5, 12, 16, 2, BASE)
    fill_rect(buf, 5, 16, 10, 2, BASE)
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
        'userland/apps/paint/paint.icn': gen_paint,
        'userland/apps/audioplayer/audioplayer.icn': gen_audioplayer,
        'userland/apps/fetch/fetch.icn': gen_fetch,
        'userland/apps/screenshot/screenshot.icn': gen_screenshot,
        'userland/apps/minesweeper/minesweeper.icn': gen_minesweeper,
        'userland/apps/2048/2048.icn': gen_2048,
        'userland/apps/snake/snake.icn': gen_snake,
        'userland/apps/xclock/xclock.icn': gen_xclock,
        'userland/apps/xeyes/xeyes.icn': gen_xeyes,
        'userland/apps/xcalc/xcalc.icn': gen_xcalc,
        'userland/apps/xgui_demo/xgui_demo.icn': gen_xgui_demo,
    }
    for path, gen_fn in generators.items():
        buf = gen_fn()
        save_icn(path, buf)

if __name__ == '__main__':
    main()
