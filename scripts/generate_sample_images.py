#!/usr/bin/env python3
"""
AzamiOS — Sample Image Generator for Image Viewer
Generates sample PPM (P6 and P3), BMP, TGA, and QOI images.
"""

import sys
import os
import struct
import math

def generate_ppm_p6(filename, width=256, height=256):
    """Generate a Catppuccin Mocha gradient PPM (P6 format)."""
    header = f"P6\n{width} {height}\n255\n".encode('ascii')
    pixels = bytearray()
    cx, cy = width / 2.0, height / 2.0
    sun_r = 45.0

    for y in range(height):
        v = y / float(height)
        for x in range(width):
            u = x / float(width)
            dx = x - cx
            dy = y - (cy - 20)
            dist = math.sqrt(dx*dx + dy*dy)

            r = int(30 * (1 - v) + 203 * v * (1 - u) + 250 * v * u)
            g = int(30 * (1 - v) + 166 * v * (1 - u) + 179 * v * u)
            b = int(46 * (1 - v) + 247 * v * (1 - u) + 135 * v * u)

            if dist < sun_r:
                factor = 1.0 - (dist / sun_r) * 0.4
                r = int(250 * factor + r * (1 - factor))
                g = int(179 * factor + g * (1 - factor))
                b = int(135 * factor + b * (1 - factor))
            elif dist < sun_r + 15:
                glow = (1.0 - (dist - sun_r) / 15.0) * 0.5
                r = int(min(255, r + 150 * glow))
                g = int(min(255, g + 100 * glow))
                b = int(min(255, b + 50 * glow))

            pixels.extend((min(255, max(0, r)), min(255, max(0, g)), min(255, max(0, b))))

    with open(filename, 'wb') as f:
        f.write(header)
        f.write(pixels)
    print(f"  ✓  Generated {filename} (P6, {width}x{height})")

def generate_bmp_24(filename, width=128, height=128):
    """Generate an uncompressed 24bpp BMP file with Azami crest."""
    row_bytes = (width * 3 + 3) & ~3
    image_size = row_bytes * height
    file_size = 54 + image_size

    bmp_header = struct.pack('<2sIHHI', b'BM', file_size, 0, 0, 54)
    dib_header = struct.pack('<IIIHHIIIIII', 40, width, height, 1, 24, 0, image_size, 2835, 2835, 0, 0)
    pixels = bytearray(image_size)
    cx, cy = width / 2.0, height / 2.0

    for y in range(height):
        row_offset = y * row_bytes
        for x in range(width):
            dx = x - cx
            dy = y - cy
            dist = math.sqrt(dx*dx + dy*dy)
            angle = math.atan2(dy, dx)
            petal_r = 38.0 * (1.0 + 0.32 * math.cos(6.0 * angle))

            if dist < petal_r:
                if dist < 12:
                    b, g, r = 175, 226, 249
                else:
                    b, g, r = 247, 166, 203
            else:
                b, g, r = 37, 24, 24

            px_offset = row_offset + x * 3
            pixels[px_offset] = b
            pixels[px_offset + 1] = g
            pixels[px_offset + 2] = r

    with open(filename, 'wb') as f:
        f.write(bmp_header)
        f.write(dib_header)
        f.write(pixels)
    print(f"  ✓  Generated {filename} (BMP 24bpp, {width}x{height})")

def generate_tga_24(filename, width=128, height=128):
    """Generate an uncompressed 24bpp Truevision TGA image (Type 2)."""
    # TGA Header (18 bytes)
    # id_len(1), colormap_type(1), image_type(1=2), colormap_spec(5),
    # x_origin(2), y_origin(2), width(2), height(2), bpp(1=24), desc(1=0x20 top-left origin)
    header = struct.pack('<BBB5sHHHHBB', 0, 0, 2, b'\x00'*5, 0, 0, width, height, 24, 0x20)
    pixels = bytearray()

    cx, cy = width / 2.0, height / 2.0
    for y in range(height):
        for x in range(width):
            dx = (x - cx) / cx
            dy = (y - cy) / cy
            d = math.sqrt(dx*dx + dy*dy)

            # Sapphire & Teal concentric rings
            ring = int(128.0 + 120.0 * math.sin(d * 12.0))
            r = int(min(255, max(0, 116 + ring // 3)))
            g = int(min(255, max(0, 199 + ring // 4)))
            b = int(min(255, max(0, 236)))
            pixels.extend((b, g, r)) # BGR order

    with open(filename, 'wb') as f:
        f.write(header)
        f.write(pixels)
    print(f"  ✓  Generated {filename} (TGA 24bpp, {width}x{height})")

def generate_qoi(filename, width=64, height=64):
    """Generate a valid QOI (Quite OK Image) format file."""
    # Header: 'qoif' (4B), width (4B BE), height (4B BE), channels (1B=3), colorspace (1B=0)
    header = struct.pack('>4sIIBB', b'qoif', width, height, 3, 0)
    stream = bytearray()

    # Encode with QOI_OP_RGB (0xFE)
    for y in range(height):
        for x in range(width):
            r = int((x / float(width)) * 203 + 30)
            g = int((y / float(height)) * 166 + 30)
            b = int(247)
            stream.append(0xFE)
            stream.extend((r, g, b))

    # QOI 8-byte end padding
    padding = b'\x00\x00\x00\x00\x00\x00\x00\x01'

    with open(filename, 'wb') as f:
        f.write(header)
        f.write(stream)
        f.write(padding)
    print(f"  ✓  Generated {filename} (QOI, {width}x{height})")

def generate_ppm_p3(filename, width=64, height=48):
    """Generate an ASCII P3 PPM color swatch."""
    with open(filename, 'w') as f:
        f.write(f"P3\n# Catppuccin Mocha Color Swatches\n{width} {height}\n255\n")
        colors = [
            (243, 139, 168), # Red
            (250, 179, 135), # Peach
            (249, 226, 175), # Yellow
            (166, 227, 161), # Green
            (137, 180, 250), # Blue
            (203, 166, 247), # Mauve
        ]
        swatch_w = width // len(colors)
        for y in range(height):
            line = []
            for x in range(width):
                ci = min(x // swatch_w, len(colors) - 1)
                r, g, b = colors[ci]
                line.append(f"{r} {g} {b}")
            f.write(" ".join(line) + "\n")
    print(f"  ✓  Generated {filename} (P3 ASCII, {width}x{height})")

def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out_dir, exist_ok=True)

    generate_ppm_p6(os.path.join(out_dir, "sunset.ppm"))
    generate_bmp_24(os.path.join(out_dir, "azami_logo.bmp"))
    generate_tga_24(os.path.join(out_dir, "rings.tga"))
    generate_qoi(os.path.join(out_dir, "cyber.qoi"))
    generate_ppm_p3(os.path.join(out_dir, "palette.ppm"))

if __name__ == "__main__":
    main()
