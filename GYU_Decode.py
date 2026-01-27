#!/usr/bin/env python3
"""
GYU Image Decoder for Retouch Engine
Supports 8-bit palette, 24-bit, and 32-bit images with separate alpha channel.
"""
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    Image = None
    print("Warning: PIL not installed")


class MersenneTwister:
    """MT19937 RNG"""
    def __init__(self):
        self.mt = [0] * 624
        self.index = 624

    def seed(self, seed):
        """Seed the RNG (matches sub_1016F690)"""
        a = seed
        idx = 0

        for _ in range(104):
            for _ in range(6):
                if idx >= 624:
                    break
                val = a & 0xFFFF0000
                a = (69069 * a + 1) & 0xFFFFFFFF
                val |= (a >> 16)
                self.mt[idx] = val
                a = (69069 * a + 1) & 0xFFFFFFFF
                idx += 1

        self._twist()

    def _twist(self):
        for i in range(227):
            y = (self.mt[i] & 0x80000000) | (self.mt[i + 1] & 0x7FFFFFFF)
            self.mt[i] = self.mt[i + 397] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)

        for i in range(227, 623):
            y = (self.mt[i] & 0x80000000) | (self.mt[i + 1] & 0x7FFFFFFF)
            self.mt[i] = self.mt[i - 227] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)

        y = (self.mt[623] & 0x80000000) | (self.mt[0] & 0x7FFFFFFF)
        self.mt[623] = self.mt[396] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)
        self.index = 0

    def next(self):
        """Get next random number"""
        if self.index >= 624:
            self._twist()

        y = self.mt[self.index]
        self.index += 1

        y ^= (y >> 11)
        y ^= ((y << 7) & 0x9D2C5680)
        y ^= ((y << 15) & 0xEFC60000)
        y ^= (y >> 18)

        return y & 0xFFFFFFFF

    def rand(self, max_val):
        """Random int in [0, max_val)"""
        if max_val <= 0:
            return 0
        return self.next() % max_val


def shuffle_compressed(data: bytearray, key: int, size: int):
    """Shuffle compressed data (FORWARD order) - before decompression"""
    rng = MersenneTwister()
    rng.seed(key)

    # Generate 10 pairs and swap in FORWARD order (as game does)
    for _ in range(10):
        idx1 = rng.rand(size)
        idx2 = rng.rand(size)
        if idx1 < len(data) and idx2 < len(data):
            data[idx1], data[idx2] = data[idx2], data[idx1]

    return data


def unshuffle_decompressed(data: bytearray, key: int, size: int):
    """Unshuffle decompressed data (REVERSE order) - after decompression"""
    rng = MersenneTwister()
    rng.seed(key)

    # Generate 10 pairs
    pairs = []
    for _ in range(10):
        idx1 = rng.rand(size)
        idx2 = rng.rand(size)
        pairs.append((idx1, idx2))

    # Swap in REVERSE order (matches sub_1008CD40)
    for idx1, idx2 in reversed(pairs):
        if idx1 < len(data) and idx2 < len(data):
            data[idx1], data[idx2] = data[idx2], data[idx1]

    return data


def decompress_lzss(data: bytes, output_size: int = None) -> bytes:
    """LZSS decompression (Retouch Engine variant)"""
    window = bytearray(4096)
    win_pos = 4078  # 0xFEE

    output = bytearray()
    pos = 0
    flags = 0

    while pos < len(data):
        flags >>= 1
        if (flags & 0x100) == 0:
            if pos >= len(data):
                break
            flags = data[pos] | 0xFF00
            pos += 1

        if pos >= len(data):
            break

        if flags & 1:
            # Literal byte
            byte = data[pos]
            pos += 1
            output.append(byte)
            window[win_pos] = byte
            win_pos = (win_pos + 1) & 0xFFF
        else:
            # Back-reference
            if pos + 1 >= len(data):
                break
            b1 = data[pos]
            b2 = data[pos + 1]
            pos += 2

            offset = ((b2 & 0xF0) << 4) | b1
            length = (b2 & 0x0F) + 3

            for i in range(length):
                byte = window[(offset + i) & 0xFFF]
                output.append(byte)
                window[win_pos] = byte
                win_pos = (win_pos + 1) & 0xFFF
            if output_size and len(output) >= output_size:
                break

    return bytes(output[:output_size] if output_size else output)

def decompress_lzss2(data: bytes, output_size: int) -> bytes:
    """LZSS2 / ungyu decompression"""
    output = bytearray()
    out_end = output_size

    # Bit buffer class
    pos = 0
    saved_bits = 0
    saved_count = 0

    def get_bits(bits):
        nonlocal pos, saved_bits, saved_count
        while bits > saved_count:
            if pos >= len(data):
                return 0
            saved_bits = (saved_bits << 8) | data[pos]
            pos += 1
            saved_count += 8

        extra_bits = saved_count - bits
        mask = 0xFFFFFFFF << extra_bits
        val = (saved_bits & mask) >> extra_bits
        saved_bits &= ~mask
        saved_count -= bits
        return val

    def get_next_byte():
        nonlocal pos
        if pos >= len(data):
            return 0
        b = data[pos]
        pos += 1
        return b

    # First byte is literal
    output.append(get_next_byte())

    while len(output) < out_end:
        if get_bits(1):
            # Literal
            output.append(get_next_byte())
        else:
            n = 0
            p = 0

            if get_bits(1):
                # Long back-reference
                b1 = get_next_byte()
                b2 = get_next_byte()
                n = 0xFFFF0000 | (b1 << 8) | b2
                p = (n >> 3) | 0xFFFFE000
                n &= 7

                if n:
                    n += 1
                else:
                    n = get_next_byte()
                    if n == 0:
                        break
            else:
                # Short back-reference
                n = get_bits(2) + 1
                p = get_next_byte() | 0xFFFFFF00

            n += 1

            # Convert to signed for negative indexing
            if p >= 0x80000000:
                p = p - 0x100000000

            while n > 0:
                if len(output) >= out_end:
                    break
                src = len(output) + p
                if 0 <= src < len(output):
                    output.append(output[src])
                else:
                    output.append(0)
                n -= 1

    return bytes(output[:output_size])


def decompress_gyu(compressed: bytes, output_size: int, mode: int) -> bytes:
    """Decompress based on mode flags"""
    mode_byte = (mode >> 24) & 0xFF

    if mode_byte == 0x01:
        # No compression
        return compressed[:output_size]
    elif mode_byte in (0x02, 0x04):
        # LZSS mode 0
        return decompress_lzss(compressed, output_size)
    elif mode_byte == 0x08:
        # LZSS2 mode 3
        return decompress_lzss2(compressed, output_size)
    else:
        print(f"  Unknown compression mode: 0x{mode_byte:02X}")
        return decompress_lzss(compressed, output_size)  # Try default


def decode_gyu(filepath: str) -> dict:
    with open(filepath, 'rb') as f:
        data = f.read()

    if data[0:4] != b'GYU\x1a':
        raise ValueError("Not a GYU file")

    flags = struct.unpack('<H', data[4:6])[0]
    type_ = struct.unpack('<H', data[6:8])[0]
    key = struct.unpack('<I', data[8:12])[0]
    bpp = struct.unpack('<I', data[12:16])[0]
    width = struct.unpack('<I', data[16:20])[0]
    height = struct.unpack('<I', data[20:24])[0]
    data_size = struct.unpack('<I', data[24:28])[0]
    alpha_size = struct.unpack('<I', data[28:32])[0]
    pal_colors = struct.unpack('<I', data[32:36])[0]

    print(f"GYU: {filepath}")
    print(f"  Size: {width} x {height}")
    print(f"  BPP: {bpp}")
    print(f"  Flags: 0x{flags:04X}, Type: 0x{type_:04X}")
    print(f"  Key: 0x{key:08X}")
    print(f"  Data: {data_size}, Alpha: {alpha_size}, Palette: {pal_colors}")

    header_size = 36

    # Parse palette (BGRA format, 4 bytes each)
    palette = None
    pal_size = pal_colors * 4
    if pal_colors > 0:
        palette = []
        for i in range(pal_colors):
            offset = header_size + i * 4
            b = data[offset]
            g = data[offset + 1]
            r = data[offset + 2]
            # a = data[offset + 3]  # Usually 0, ignore
            palette.append((r, g, b))

    compressed_start = header_size + pal_size
    compressed = bytearray(data[compressed_start:compressed_start + data_size])

    alpha_compressed = None
    if alpha_size > 0:
        alpha_start = compressed_start + data_size
        alpha_compressed = bytearray(data[alpha_start:alpha_start + alpha_size])

    # Row stride
    bytes_per_pixel = bpp // 8 if bpp >= 8 else 1
    row_stride = ((width * bytes_per_pixel) + 3) & ~3
    expected_size = row_stride * height

    # Unscramble
    if key != 0:
        print("  Unscrambling compressed data...")
        unscramble(compressed, key, len(compressed))

    # Decompress RGB
    TYPE_GYU = 0x0800

    if data_size == expected_size:
        rgb_data = bytes(compressed)
    elif type_ == TYPE_GYU:
        print("  Decompressing with ungyu (LZSS2)...")
        rgb_data = decompress_lzss2(bytes(compressed[4:]), expected_size)
    else:
        print("  Decompressing with LZSS...")
        rgb_data = decompress_lzss(bytes(compressed), expected_size)

    print(f"  Decompressed: {len(rgb_data)} bytes")

    # Decompress alpha
    alpha_data = None
    if alpha_compressed:
        alpha_stride = (width + 3) & ~3
        alpha_expected = alpha_stride * height

        if alpha_size == alpha_expected:
            alpha_data = bytes(alpha_compressed)
        else:
            print("  Decompressing alpha...")
            alpha_data = decompress_lzss(bytes(alpha_compressed), alpha_expected)

    return {
        'width': width,
        'height': height,
        'bpp': bpp,
        'row_stride': row_stride,
        'rgb': rgb_data,
        'alpha': alpha_data,
        'flags': flags,
        'palette': palette
    }

def unscramble(data: bytearray, seed: int, size: int):
    """Unscramble - swap10 pairs using MT19937"""
    rng = MersenneTwister()
    rng.seed(seed)

    for _ in range(10):
        idx1 = rng.rand(size)
        idx2 = rng.rand(size)
        if idx1 < len(data) and idx2 < len(data):
            data[idx1], data[idx2] = data[idx2], data[idx1]

def gyu_to_png(gyu_path: str, png_path: str = None):
    if Image is None:
        print("PIL not available!")
        return

    info = decode_gyu(gyu_path)

    if png_path is None:
        png_path = str(Path(gyu_path).with_suffix('.png'))

    width = info['width']
    height = info['height']
    bpp = info['bpp']
    stride = info['row_stride']
    rgb = info['rgb']
    alpha = info['alpha']
    palette = info.get('palette')
    flags = info.get('flags', 0)

    # Wide alpha: flags == 0x0003 (not &)
    wide_alpha = (flags == 0x0003)

    if bpp == 8:
        # Paletted image
        if not palette:
            print(f"  Error: 8-bit image but no palette!")
            return

        # Alpha stride = (width + 3) & ~3
        alpha_stride = (width + 3) & ~3

        if alpha:
            img = Image.new('RGBA', (width, height))
        else:
            img = Image.new('RGB', (width, height))

        pixels = []

        for y in range(height - 1, -1, -1):  # Bottom-up
            row_start = y * stride
            alpha_row_start = y * alpha_stride

            for x in range(width):
                # RGB from palette
                idx_offset = row_start + x
                if idx_offset < len(rgb):
                    pal_idx = rgb[idx_offset]
                    if pal_idx < len(palette):
                        r, g, b = palette[pal_idx]
                    else:
                        r, g, b = 0, 0, 0
                else:
                    r, g, b = 0, 0, 0

                if alpha:
                    a_offset = alpha_row_start + x
                    if a_offset < len(alpha):
                        a = alpha[a_offset]
                # Alpha scaling based on wide_alpha flag
                        if not wide_alpha:
                            if a < 16:
                                a = a * 16
                            else:
                                a = 255
                    else:
                        a = 255
                    pixels.append((r, g, b, a))
                else:
                    pixels.append((r, g, b))

        img.putdata(pixels)

    elif bpp == 24:
        img = Image.new('RGB', (width, height))
        pixels = []

        for y in range(height - 1, -1, -1):
            row_start = y * stride
            for x in range(width):
                offset = row_start + x * 3
                if offset + 2 < len(rgb):
                    b, g, r = rgb[offset], rgb[offset + 1], rgb[offset + 2]
                    pixels.append((r, g, b))
                else:
                    pixels.append((0, 0, 0))

        img.putdata(pixels)

        if alpha:
            img = img.convert('RGBA')
            alpha_stride = (width + 3) & ~3
            alpha_pixels = []

            for y in range(height - 1, -1, -1):
                row_start = y * alpha_stride
                for x in range(width):
                    offset = row_start + x
                    if offset < len(alpha):
                        a = alpha[offset]
                        if not wide_alpha:
                            if a < 16:
                                a = a * 16
                            else:
                                a = 255
                    else:
                        a = 255
                    alpha_pixels.append(a)

            alpha_img = Image.new('L', (width, height))
            alpha_img.putdata(alpha_pixels)
            img.putalpha(alpha_img)

    elif bpp == 32:
        img = Image.new('RGBA', (width, height))
        pixels = []

        for y in range(height - 1, -1, -1):
            row_start = y * stride
            for x in range(width):
                offset = row_start + x * 4
                if offset + 3 < len(rgb):
                    b, g, r, a = rgb[offset], rgb[offset+1], rgb[offset+2], rgb[offset+3]
                    pixels.append((r, g, b, a))
                else:
                    pixels.append((0, 0, 0, 255))

        img.putdata(pixels)

    else:
        print(f"Unsupported BPP: {bpp}")
        return

    img.save(png_path)
    print(f"  Saved: {png_path}")

if __name__ == '__main__':
    import os
    import glob
    import argparse

    parser = argparse.ArgumentParser(description='GYU Image Decoder')
    parser.add_argument('inputs', nargs='+', help='GYU files or folders to convert')
    parser.add_argument('-o', '--output', help='Output directory (preserves folder structure)')
    args = parser.parse_args()

    files_to_process = []
    input_base = None  # Track base for relative path calculation

    for arg in args.inputs:
        if os.path.isdir(arg):
            if input_base is None:
                input_base = arg
            for root, dirs, files in os.walk(arg):
                for f in files:
                    if f.lower().endswith('.gyu'):
                        files_to_process.append(os.path.join(root, f))
        elif os.path.isfile(arg):
            files_to_process.append(arg)
        else:
            files_to_process.extend(glob.glob(arg, recursive=True))

    print(f"Found {len(files_to_process)} GYU file(s)")

    for path in files_to_process:
        try:
            # Determine output path
            if args.output:
                if input_base:
                    rel_path = os.path.relpath(path, input_base)
                else:
                    rel_path = os.path.basename(path)
                out_path = os.path.join(args.output, os.path.splitext(rel_path)[0] + '.png')
                os.makedirs(os.path.dirname(out_path), exist_ok=True)
            else:
                out_path = None  # Same dir as input

            gyu_to_png(path, out_path)
        except Exception as e:
            print(f"Error processing {path}: {e}")
            import traceback
            traceback.print_exc()
