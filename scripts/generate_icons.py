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
MANTLE      = 0xFF181825

def create_blank(bg=TRANSPARENT):
    return [bg] * (32 * 32)

new_icon = create_blank

def set_pixel(buf, x, y, color):
    if 0 <= x < 32 and 0 <= y < 32:
        buf[y * 32 + x] = color

def draw_line(buf, x0, y0, x1, y1, color):
    dx = abs(x1 - x0)
    dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        set_pixel(buf, x0, y0, color)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy

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

def gen_imageviewer():
    buf = create_blank()
    # Outer picture frame
    fill_rounded_rect(buf, 3, 3, 26, 26, 4, BASE)
    fill_rounded_rect(buf, 5, 5, 22, 22, 2, SURFACE0)
    # Sky
    fill_rect(buf, 6, 6, 20, 12, SAPPHIRE)
    # Sun
    fill_circle(buf, 21, 10, 3, YELLOW)
    # Mountains / Hills
    for y in range(14, 25):
        for x in range(6, 26):
            if y >= 25 - (x - 6): # Left slope
                set_pixel(buf, x, y, TEAL)
            if y >= 25 - (25 - x) * 0.8: # Right slope
                set_pixel(buf, x, y, GREEN)
    # Frame accent border
    fill_rect(buf, 3, 3, 26, 2, MAUVE)
    return buf

def gen_notes():
    buf = create_blank()
    # Sticky note pad (Yellow pastel)
    fill_rounded_rect(buf, 4, 3, 24, 26, 3, YELLOW)
    # Top adhesive strip
    fill_rect(buf, 4, 3, 24, 5, PEACH)
    # Horizontal ruled lines representing notes
    fill_rect(buf, 8, 12, 16, 2, SURFACE1)
    fill_rect(buf, 8, 16, 14, 2, SURFACE1)
    fill_rect(buf, 8, 20, 10, 2, SURFACE1)
    # Folded bottom-right dog-ear corner
    for dy in range(6):
        for dx in range(6 - dy):
            set_pixel(buf, 27 - dx, 28 - dy, BASE)
    for dy in range(6):
        set_pixel(buf, 22 + dy, 28 - dy, PEACH)
    return buf

def gen_ide():
    """Azami Code Studio (IDE) icon: window frame with code brackets and run badge."""
    buf = new_icon()
    # Dark window canvas
    fill_rect(buf, 2, 2, 28, 28, MANTLE)
    # Header bar
    fill_rect(buf, 2, 2, 28, 6, SURFACE0)
    # Window dots
    set_pixel(buf, 5, 5, RED)
    set_pixel(buf, 8, 5, YELLOW)
    set_pixel(buf, 11, 5, GREEN)
    # Left sidebar divider
    draw_line(buf, 9, 8, 9, 29, SURFACE1)
    # Code brackets: '<'
    draw_line(buf, 14, 13, 11, 17, SAPPHIRE)
    draw_line(buf, 11, 17, 14, 21, SAPPHIRE)
    # '/'
    draw_line(buf, 16, 21, 19, 13, MAUVE)
    # '>'
    draw_line(buf, 21, 13, 24, 17, SAPPHIRE)
    draw_line(buf, 24, 17, 21, 21, SAPPHIRE)
    # Green run triangle badge at bottom right
    for r in range(5):
        draw_line(buf, 24, 23 + r, 24 + (4 - abs(2 - r)), 23 + r, GREEN)
    return buf

def gen_fontviewer():
    """Font Viewer icon: large stylized 'A' with typography grid marks."""
    buf = new_icon()
    fill_rect(buf, 2, 2, 28, 28, SURFACE0)
    # Baseline & cap-height guideline marks
    draw_line(buf, 4, 8, 27, 8, SURFACE1)
    draw_line(buf, 4, 25, 27, 25, SURFACE1)
    # Big capital 'A' in Mauve & Text
    # Left stem
    draw_line(buf, 15, 9, 9, 24, TEXT)
    draw_line(buf, 16, 9, 10, 24, MAUVE)
    # Right stem
    draw_line(buf, 16, 9, 22, 24, TEXT)
    draw_line(buf, 17, 9, 23, 24, MAUVE)
    # Crossbar
    draw_line(buf, 11, 19, 21, 19, PEACH)
    draw_line(buf, 11, 20, 21, 20, PEACH)
    # Serifs at base
    draw_line(buf, 7, 24, 12, 24, MAUVE)
    draw_line(buf, 20, 24, 25, 24, MAUVE)
    return buf

def gen_hexedit():
    """Terminal Hex Editor icon: 0x prefix badge with hex grid."""
    buf = new_icon()
    fill_rect(buf, 2, 2, 28, 28, BASE)
    fill_rect(buf, 2, 2, 28, 6, SURFACE0)
    # '0x' in Yellow
    # '0'
    draw_line(buf, 5, 11, 5, 17, YELLOW)
    draw_line(buf, 9, 11, 9, 17, YELLOW)
    draw_line(buf, 5, 11, 9, 11, YELLOW)
    draw_line(buf, 5, 17, 9, 17, YELLOW)
    # 'x'
    draw_line(buf, 12, 13, 16, 17, YELLOW)
    draw_line(buf, 12, 17, 16, 13, YELLOW)
    # Hex byte dots in Sapphire and Mauve
    for y in (20, 23, 26):
        fill_rect(buf, 5, y, 4, 2, SAPPHIRE)
        fill_rect(buf, 11, y, 4, 2, MAUVE)
        fill_rect(buf, 18, y, 4, 2, TEAL)
        fill_rect(buf, 24, y, 4, 2, TEXT)
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
        'userland/apps/imageviewer/imageviewer.icn': gen_imageviewer,
        'userland/apps/notes/notes.icn': gen_notes,
        'userland/apps/ide/ide.icn': gen_ide,
        'userland/apps/fontviewer/fontviewer.icn': gen_fontviewer,
        'userland/apps/hexedit/hexedit.icn': gen_hexedit,
    }
    for path, gen_fn in generators.items():
        buf = gen_fn()
        save_icn(path, buf)

if __name__ == '__main__':
    main()
