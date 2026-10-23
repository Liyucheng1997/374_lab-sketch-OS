#!/usr/bin/env python3
"""
检查截图 PNG 是否有效（只用标准库）：

    python3 tests/web/check_png.py 文件.png [宽x高] [--min-colors N]

检查项：PNG 签名、每个数据块的 CRC、IHDR 尺寸（给了宽x高时必须一致）、
解压后的像素数据长度正确、画面不是一整块纯色（至少 N 种颜色，默认 2）。
tools/v86_harness.mjs 写出的 PNG 是 8 位 RGB、每行过滤类型 0；其它过滤类型也能解码。
"""
import argparse
import struct
import sys
import zlib


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def unfilter(raw, width, height, bpp):
    """按 PNG 规范还原每一行的过滤（0 无、1 Sub、2 Up、3 Average、4 Paeth）"""
    stride = width * bpp
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += stride + 1
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                upleft = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + paeth(left, prev[i], upleft)) & 0xFF
        elif ftype != 0:
            fail("未知的行过滤类型 %d" % ftype)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def main():
    ap = argparse.ArgumentParser(description="检查截图 PNG")
    ap.add_argument("png")
    ap.add_argument("size", nargs="?", help="期望的尺寸，例如 1024x768")
    ap.add_argument("--min-colors", type=int, default=2)
    a = ap.parse_args()
    path, want, min_colors = a.png, a.size, a.min_colors
    try:
        data = open(path, "rb").read()
    except OSError as e:
        fail("打不开 %s: %s" % (path, e))
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail("%s 不是 PNG 文件" % path)
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        if pos + 8 > len(data):
            fail("数据块头被截断")
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])[0]
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            fail("数据块 %s 的 CRC 不对" % ctype.decode("latin1"))
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length
    if not ihdr:
        fail("没有 IHDR")
    width, height, depth, ctype_, _, _, interlace = ihdr
    if want and want != "%dx%d" % (width, height):
        fail("尺寸是 %dx%d，期望 %s" % (width, height, want))
    if depth != 8 or ctype_ not in (2, 6) or interlace:
        print("OK %s %dx%d（非 8 位 RGB/RGBA，只检查了文件结构）" % (path, width, height))
        return
    bpp = 3 if ctype_ == 2 else 4
    raw = zlib.decompress(idat)
    if len(raw) != (width * bpp + 1) * height:
        fail("像素数据长度不对：%d" % len(raw))
    pixels = unfilter(raw, width, height, bpp)
    colors = set()
    step = max(1, (width * height) // 200000)   # 大图抽样统计颜色
    for i in range(0, width * height, step):
        colors.add(bytes(pixels[i * bpp:i * bpp + 3]))
        if len(colors) >= 4096:
            break
    if len(colors) < min_colors:
        fail("%s 只有 %d 种颜色（画面是空白的？）" % (path, len(colors)))
    print("OK %s %dx%d，至少 %d 种颜色" % (path, width, height, len(colors)))


if __name__ == "__main__":
    main()
