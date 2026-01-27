#!/usr/bin/env python3
"""
GYU Image Decoder for Retouch Engine
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

    # Correct header parsing!
    flags = struct.unpack('<H', data[4:6])[0] # 16-bit
    type_ = struct.unpack('<H', data[6:8])[0] # 16-bit
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
    pal_size = pal_colors * 4
    compressed_start = header_size + pal_size

    compressed = bytearray(data[compressed_start:compressed_start + data_size])

    bytes_per_pixel = bpp // 8
    row_stride = ((width * bytes_per_pixel) + 3) & ~3
    expected_size = row_stride * height

    if key != 0:
        print(f"  Unscrambling compressed data...")
        unscramble(compressed, key, len(compressed))

    TYPE_GYU_COMPRESSION = 0x0800

    if data_size == expected_size:
        # No compression
        rgb_data = bytes(compressed)
    elif type_ == TYPE_GYU_COMPRESSION:
        # Skip first 4 bytes, use ungyu
        print("  Decompressing with ungyu (LZSS2)...")
        rgb_data = decompress_lzss2(bytes(compressed[4:]), expected_size)
    else:
        # Standard LZSS
        print("  Decompressing with LZSS...")
        rgb_data = decompress_lzss(bytes(compressed), expected_size)

    print(f"  Decompressed: {len(rgb_data)} bytes")

    return {
        'width': width,
        'height': height,
        'bpp': bpp,
        'row_stride': row_stride,
        'rgb': rgb_data,
        'alpha': None,
        'flags': flags
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
    """Convert GYU to PNG"""
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

    if bpp == 24:
        img = Image.new('RGB', (width, height))
        pixels = []

        for y in range(height - 1, -1, -1):  # Bottom-up
            row_start = y * stride
            for x in range(width):
                offset = row_start + x * 3
                if offset + 2 < len(rgb):
                    b, g, r = rgb[offset], rgb[offset + 1], rgb[offset + 2]
                    pixels.append((r, g, b))
                else:
                    pixels.append((0, 0, 0))

        img.putdata(pixels)

        # Add alpha
        if alpha:
            img = img.convert('RGBA')
            alpha_stride = (width + 3) & ~3
            alpha_pixels = []

            for y in range(height - 1, -1, -1):
                row_start = y * alpha_stride
                for x in range(width):
                    offset = row_start + x
                    alpha_pixels.append(alpha[offset] if offset < len(alpha) else 255)

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
    if len(sys.argv) < 2:
        print("Usage: gyu_decode.py <file.gyu> [output.png]")
        sys.exit(1)

    for path in sys.argv[1:]:
        if path.endswith('.png'):
            continue
        try:
            gyu_to_png(path)
        except Exception as e:
            print(f"Error: {e}")
            import traceback
            traceback.print_exc()
