#!/usr/bin/env python3
"""
GYU Image Encoder for Retouch Engine
Supports 8-bit palette, 24-bit, and 32-bit images with separate alpha channel.
"""
import struct
import os
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    raise ImportError("PIL required: pip install Pillow")


class MersenneTwister:
    """MT19937 RNG"""
    def __init__(self):
        self.mt = [0] * 624
        self.index = 624

    def seed(self, seed):
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
        if max_val <= 0:
            return 0
        return self.next() % max_val


def scramble(data: bytearray, seed: int):
    """
    Scramble data - FORWARD order (same as unscramble since swapping is symmetric).
    """
    rng = MersenneTwister()
    rng.seed(seed)

    size = len(data)
    for _ in range(10):
        idx1 = rng.rand(size)
        idx2 = rng.rand(size)
        if idx1 < size and idx2 < size:
            data[idx1], data[idx2] = data[idx2], data[idx1]


def compress_lzss(data: bytes) -> bytes:
    """
    LZSS compression (Retouch Engine variant)
    """
    WINDOW_SIZE = 4096
    WINDOW_MASK = 0xFFF
    MIN_MATCH = 3
    MAX_MATCH = 18

    window = bytearray(WINDOW_SIZE)
    win_pos = 4078  # 0xFEE - standard initial position

    output = bytearray()
    pos = 0
    flag_byte = 0
    flag_bit = 0
    pending = bytearray()

    def flush():
        nonlocal flag_byte, flag_bit, pending
        if flag_bit > 0:
            output.append(flag_byte)
            output.extend(pending)
        flag_byte = 0
        flag_bit = 0
        pending = bytearray()

    while pos < len(data):
        # Find best match in window
        best_len = 0
        best_off = 0

        for back in range(1, min(pos + 1, WINDOW_SIZE)):
            match_len = 0
            while (match_len < MAX_MATCH and
                   pos + match_len < len(data) and
                   window[(win_pos - back + match_len) & WINDOW_MASK] == data[pos + match_len]):
                match_len += 1
            if match_len >= MIN_MATCH and match_len > best_len:
                best_len = match_len
                best_off = (win_pos - back) & WINDOW_MASK

        if best_len >= MIN_MATCH:
            # Back-reference: 2 bytes
            pending.append(best_off & 0xFF)
            pending.append(((best_off >> 4) & 0xF0) | ((best_len - 3) & 0x0F))
            for i in range(best_len):
                window[win_pos] = data[pos + i]
                win_pos = (win_pos + 1) & WINDOW_MASK
            pos += best_len
        else:
            # Literal byte
            pending.append(data[pos])
            flag_byte |= (1 << flag_bit)
            window[win_pos] = data[pos]
            win_pos = (win_pos + 1) & WINDOW_MASK
            pos += 1

        flag_bit += 1
        if flag_bit == 8:
            flush()

    flush()
    return bytes(output)


def read_original_gyu(gyu_path: str) -> dict:
    """Read header from existing GYU file to preserve key and type"""
    with open(gyu_path, 'rb') as f:
        data = f.read(36)

    if data[0:4] != b'GYU\x1a':
        return None

    return {
        'flags': struct.unpack('<H', data[4:6])[0],
        'type': struct.unpack('<H', data[6:8])[0],
        'key': struct.unpack('<I', data[8:12])[0],
        'bpp': struct.unpack('<I', data[12:16])[0],
    }


def generate_key() -> int:
    """Generate random scrambling key"""
    import random
    return random.randint(0x10000000, 0xFFFFFFFF)


def png_to_gyu(png_path: str, gyu_path: str = None, reference_gyu: str = None):
    """
    Convert PNG to GYU format.

    Supports:
    - 8-bit paletted (P mode) with optional alpha
    - 24-bit RGB
    - 32-bit RGBA (with separate alpha channel)

    Args:
        png_path: Input PNG file
        gyu_path: Output GYU file (default: same name with .gyu)
        reference_gyu: Original GYU to copy key from
    """
    img = Image.open(png_path)

    if gyu_path is None:
        gyu_path = str(Path(png_path).with_suffix('.gyu'))

    width, height = img.size

    print(f"Encoding: {png_path}")
    print(f"  Size: {width} x {height}")
    print(f"  Mode: {img.mode}")

    # Load reference if provided
    ref = None
    if reference_gyu and os.path.exists(reference_gyu):
        ref = read_original_gyu(reference_gyu)
        if ref:
            print(f"  Reference: {reference_gyu} (key=0x{ref['key']:08X}, bpp={ref['bpp']})")

    # Determine target format
    if ref and ref['bpp'] == 8:
        # Re-encode as 8-bit paletted
        bpp = 8
        if img.mode != 'P':
            print("  Quantizing to 256 colors...")
            if img.mode == 'RGBA':
                # Extract alpha before quantizing
                alpha_channel = img.split()[3]
                img = img.convert('RGB').quantize(colors=256)
            else:
                img = img.convert('RGB').quantize(colors=256)
                alpha_channel = None
        else:
            alpha_channel = None
            if 'transparency' in img.info:
                # Has transparency info
                pass
    elif img.mode == 'RGBA' or (img.mode == 'P' and 'transparency' in img.info):
        bpp = 24  # RGB data + separate alpha
        if img.mode == 'P':
            img = img.convert('RGBA')
    elif img.mode == 'P':
        bpp = 8
        alpha_channel = None
    else:
        bpp = 24
        img = img.convert('RGB')

    print(f"  Output BPP: {bpp}")

    # Build raw data
    palette_data = b''
    alpha_data = b''
    pal_colors = 0

    if bpp == 8:
        # 8-bit paletted
        pal = img.getpalette()
        if pal:
            pal_colors = 256
            palette_data = bytearray()
            for i in range(256):
                r = pal[i * 3] if i * 3 < len(pal) else 0
                g = pal[i * 3 + 1] if i * 3 + 1 < len(pal) else 0
                b = pal[i * 3 + 2] if i * 3 + 2 < len(pal) else 0
                palette_data.extend([b, g, r, 0])  # BGRA

        # Build index data (bottom-up)
        row_stride = (width + 3) & ~3
        raw_data = bytearray()
        for y in range(height - 1, -1, -1):
            row = bytearray()
            for x in range(width):
                idx = img.getpixel((x, y))
                row.append(idx)
            while len(row) < row_stride:
                row.append(0)
            raw_data.extend(row)

        # Handle alpha if present
        if 'alpha_channel' in dir() and alpha_channel is not None:
            alpha_stride = (width + 3) & ~3
            alpha_raw = bytearray()
            for y in range(height - 1, -1, -1):
                row = bytearray()
                for x in range(width):
                    a = alpha_channel.getpixel((x, y))
                    row.append(a)
                while len(row) < alpha_stride:
                    row.append(0)
                alpha_raw.extend(row)
            alpha_data = compress_lzss(bytes(alpha_raw))

    elif bpp == 24:
        # 24-bit RGB (or RGBA with separate alpha)
        has_alpha = img.mode == 'RGBA'

        bytes_per_pixel = 3
        row_stride = ((width * bytes_per_pixel) + 3) & ~3

        raw_data = bytearray()
        alpha_raw = bytearray() if has_alpha else None
        alpha_stride = (width + 3) & ~3

        for y in range(height - 1, -1, -1):
            row = bytearray()
            alpha_row = bytearray() if has_alpha else None

            for x in range(width):
                pixel = img.getpixel((x, y))
                if has_alpha:
                    r, g, b, a = pixel
                    alpha_row.append(a)
                else:
                    r, g, b = pixel
                row.extend([b, g, r])

            while len(row) < row_stride:
                row.append(0)
            raw_data.extend(row)

            if has_alpha:
                while len(alpha_row) < alpha_stride:
                    alpha_row.append(0)
                alpha_raw.extend(alpha_row)

        if has_alpha and alpha_raw:
            alpha_data = compress_lzss(bytes(alpha_raw))

    else:
        raise ValueError(f"Unsupported BPP: {bpp}")

    print(f"  Raw data: {len(raw_data)} bytes")

    # Compress RGB data
    compressed = compress_lzss(bytes(raw_data))
    ratio = 100 * len(compressed) // len(raw_data) if len(raw_data) > 0 else 0
    print(f"  Compressed: {len(compressed)} bytes ({ratio}%)")

    # Get key
    if ref:
        key = ref['key']
    else:
        key = generate_key()
    print(f"  Key: 0x{key:08X}")

    # Scramble
    compressed = bytearray(compressed)
    scramble(compressed, key)
    print("  Scrambled RGB data")

    if alpha_data:
        alpha_data = bytearray(alpha_data)
        scramble(alpha_data, key)
        print("  Scrambled alpha data")

    # Build header
    # flags: 0x0003 if wide alpha (8-bit), 0x0000 otherwise
    flags = 0x0003 if alpha_data else 0x0000
    type_ = 0x0000  # Standard LZSS

    header = struct.pack('<4sHHIIIIIII',
        b'GYU\x1a',
        flags,
        type_,
        key,
        bpp,
        width,
        height,
        len(compressed),
        len(alpha_data) if alpha_data else 0,
        pal_colors
    )

    # Write output
    with open(gyu_path, 'wb') as f:
        f.write(header)
        f.write(palette_data)
        f.write(compressed)
        if alpha_data:
            f.write(alpha_data)

    final_size = len(header) + len(palette_data) + len(compressed) + len(alpha_data)
    print(f"  Saved: {gyu_path} ({final_size} bytes)")

    return gyu_path


if __name__ == '__main__':
    import argparse
    import glob

    parser = argparse.ArgumentParser(description='GYU Image Encoder')
    parser.add_argument('inputs', nargs='+', help='PNG files or folders to convert')
    parser.add_argument('-o', '--output', help='Output directory')
    parser.add_argument('--ref', help='Reference GYU folder (to copy keys from original files)')
    args = parser.parse_args()

    files_to_process = []
    input_base = None

    for arg in args.inputs:
        if os.path.isdir(arg):
            if input_base is None:
                input_base = arg
            for root, dirs, files in os.walk(arg):
                for f in files:
                    if f.lower().endswith('.png'):
                        files_to_process.append(os.path.join(root, f))
        elif os.path.isfile(arg):
            files_to_process.append(arg)
        else:
            files_to_process.extend(glob.glob(arg, recursive=True))

    print(f"Found {len(files_to_process)} PNG file(s)")

    for png_path in files_to_process:
        try:
            # Determine output path
            if args.output:
                if input_base:
                    rel_path = os.path.relpath(png_path, input_base)
                else:
                    rel_path = os.path.basename(png_path)
                out_path = os.path.join(args.output, os.path.splitext(rel_path)[0] + '.gyu')
                os.makedirs(os.path.dirname(out_path), exist_ok=True)
            else:
                out_path = str(Path(png_path).with_suffix('.gyu'))

            # Find reference GYU if available
            ref_gyu = None
            if args.ref:
                gyu_name = os.path.splitext(os.path.basename(png_path))[0] + '.gyu'
                if input_base:
                    rel_path = os.path.relpath(png_path, input_base)
                    ref_gyu = os.path.join(args.ref, os.path.splitext(rel_path)[0] + '.gyu')
                else:
                    ref_gyu = os.path.join(args.ref, gyu_name)
                if not os.path.exists(ref_gyu):
                    ref_gyu = None

            png_to_gyu(png_path, out_path, ref_gyu)
            print()

        except Exception as e:
            print(f"Error processing {png_path}: {e}")
            import traceback
            traceback.print_exc()
