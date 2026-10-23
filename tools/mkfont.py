#!/usr/bin/env python3
"""
mkfont.py — 把 TrueType 字体预先光栅化成 SketchOS 自己的 .skf 位图字体

用法（通常由 scripts/fonts.sh 调用）：
    python3 tools/mkfont.py --src <TTF 所在目录> --root <仓库根目录> \\
        --out assets/fonts --builtin userspace/gui/libsketch/font_builtin.c
    python3 tools/mkfont.py --src <TTF 目录> --compare weights.png   # 只画一张字重对比图

为什么要这样做？
    TrueType 字体里存的是字形“轮廓”（二次贝塞尔曲线）。屏幕上显示文字之前，必须有人把
    曲线“光栅化”成像素——桌面 Linux 用的是 FreeType 库，有几十万行代码。SketchOS 只用
    4 个固定字号，于是我们在构建时（宿主机上，借 Pillow 调 FreeType）把每个字都画成
    8 位灰度覆盖率图（0=完全没墨，255=完全被笔画盖住），存进一个简单的二进制文件。
    系统运行时只需 mmap 文件、二分查找、按覆盖率混合颜色——几百行 C 就够了。

.skf 文件格式见 userspace/gui/include/sketch/font.h（全部小端）：
    32 字节文件头 + 按码点升序排列的 16 字节索引项 + 位图数据区。

本脚本是确定性的：同样的输入（TTF、源码里的字符串）永远生成逐字节相同的输出，
不写时间戳，所有集合都排序后再输出。
"""
import argparse
import math
import os
import re
import struct
import sys
import unicodedata

from PIL import Image, ImageDraw, ImageFont

FONT_VERSION = "v1.530"

# ---------------------------------------------------------------------------
# 输出字体的配置
#
# 字重选择（渲染对比图后人工判断，见 --compare 和 assets/fonts/README.md）：
#   16px 正文 / 20px 标题：Regular 的笔画在 16px 下只有约 1 像素宽，抗锯齿后变成浅灰色，
#     在暖白色“纸”背景上发虚；Medium 笔画更实，中文小字也清楚，选 Medium。
#   32px 大字：Regular 已经足够黑，而且保留了楷书的轻盈笔意（像铅笔写的）；Medium 显得
#     像马克笔，太重，选 Regular。
#   等宽 16px（终端）：同 16px 正文的理由，选 Mono Medium。
# ---------------------------------------------------------------------------
FONTS = [
    dict(name="sans-16", ttf="LXGWWenKai-Medium.ttf", size=16, mono=False,
         sets=("base", "box", "gb2312", "scan", "docs")),
    dict(name="sans-20", ttf="LXGWWenKai-Medium.ttf", size=20, mono=False,
         sets=("base", "box", "gb2312", "scan", "docs")),
    dict(name="sans-32", ttf="LXGWWenKai-Regular.ttf", size=32, mono=False,
         sets=("base", "scan", "large")),
    # 等宽字体里“半角”字符的字形太宽时，改用同字重的比例字体里的字形（见 fit_cell）
    dict(name="mono-16", ttf="LXGWWenKaiMono-Medium.ttf", size=16, mono=True,
         narrow_ttf="LXGWWenKai-Medium.ttf",
         sets=("base", "box", "gb2312", "scan", "docs")),
]
BUILTIN_FROM = "mono-16"        # 内置后备字体从哪个字体里取 ASCII

# 字体家族的显示名（只用于文档；按 OFL 要求，转换后的字体本身不使用保留名称）
TTF_DESC = {
    "LXGWWenKai-Regular.ttf": "霞鹜文楷 LXGW WenKai Regular",
    "LXGWWenKai-Medium.ttf": "霞鹜文楷 LXGW WenKai Medium",
    "LXGWWenKaiMono-Regular.ttf": "霞鹜文楷等宽 LXGW WenKai Mono Regular",
    "LXGWWenKaiMono-Medium.ttf": "霞鹜文楷等宽 LXGW WenKai Mono Medium",
}

# ---------------------------------------------------------------------------
# 字符集
# ---------------------------------------------------------------------------
def rng(a, b):
    return list(range(a, b + 1))


SET_ASCII = rng(0x20, 0x7E)
SET_LATIN1 = rng(0xA0, 0xFF)
SET_GENERAL_PUNCT = rng(0x2010, 0x205E)          # ‐ – — ‘ ’ “ ” • … ‰ ′ ″ ‹ › ※ ‼ ⁇ …
SET_ARROWS = rng(0x2190, 0x2199) + [0x21B5, 0x21D0, 0x21D2, 0x21D4]   # ← ↑ → ↓ ↔ ↕ ↖ ↗ ↘ ↙ ↵ ⇐ ⇒ ⇔
SET_MATH = [0x00D7, 0x00F7, 0x00B1, 0x2212, 0x2215, 0x2217, 0x221A, 0x221D, 0x221E, 0x2220,
            0x2227, 0x2228, 0x2229, 0x222A, 0x222B, 0x2234, 0x2235, 0x2236, 0x2237, 0x2248,
            0x2260, 0x2261, 0x2264, 0x2265, 0x2211, 0x220F, 0x2208, 0x2203, 0x2200, 0x2205,
            0x2206, 0x2207, 0x2282, 0x2283, 0x2286, 0x2287, 0x22A5, 0x2016]
SET_LETTERLIKE = [0x2103, 0x2109, 0x2116, 0x2122, 0x2160, 0x2161, 0x2162, 0x2163, 0x2164,
                  0x2165, 0x2166, 0x2167, 0x2168, 0x2169, 0x216A, 0x216B]
SET_GEOMETRIC = [0x25A0, 0x25A1, 0x25AA, 0x25AB, 0x25B2, 0x25B3, 0x25B6, 0x25B7, 0x25BC,
                 0x25BD, 0x25C0, 0x25C1, 0x25C6, 0x25C7, 0x25CB, 0x25CE, 0x25CF, 0x25E2,
                 0x25E3, 0x25E4, 0x25E5, 0x2605, 0x2606, 0x2610, 0x2611, 0x2612, 0x2640,
                 0x2642, 0x2660, 0x2661, 0x2663, 0x2665, 0x2666, 0x266A, 0x266B,
                 0x2713, 0x2714, 0x2717, 0x2718, 0x2460, 0x2461, 0x2462, 0x2463, 0x2464,
                 0x2465, 0x2466, 0x2467, 0x2468, 0x2469]
SET_CJK_PUNCT = rng(0x3000, 0x303F)
SET_FULLWIDTH = rng(0xFF01, 0xFF5E) + rng(0xFFE0, 0xFFE6)
SET_SPECIAL = [0xFFFD]                            # 解码失败时显示的“替换字符” �
SET_BOX = rng(0x2500, 0x257F)                     # 制表符（终端里画表格、边框）
SET_BLOCK = rng(0x2580, 0x259F)                   # 方块元素（进度条、字符画）

# 大字（32px）额外保证的字：时钟、启动画面、日期
LARGE_EXTRA = ("0123456789:：年月日时分秒星期一二三四五六七八九十天上午下午早晚"
               "手绘操作系统欢迎使用正在启动关机重启请稍候")

SCAN_EXTS = (".c", ".h")


def gb2312_chars():
    """枚举 GB2312：符号区（0xA1-0xA9 行）和全部 6763 个汉字（0xB0-0xF7 行）。
    GB2312 用两个字节编码一个字：第一个字节是“区”，第二个字节是“位”，各 0xA1..0xFE。"""
    symbols, hanzi = [], []
    for row in range(0xA1, 0xF8):
        if 0xAA <= row <= 0xAF:
            continue                               # 这几区是空的
        for col in range(0xA1, 0xFF):
            try:
                ch = bytes([row, col]).decode("gb2312")
            except UnicodeDecodeError:
                continue                           # 空位（例如 D7FA-D7FE）
            (hanzi if row >= 0xB0 else symbols).append(ord(ch))
    return symbols, hanzi


def c_string_literals(text):
    """从 C 源码里提取字符串/字符字面量的内容（跳过注释）。
    界面上显示的文字一定写在字符串里；注释里的中文不需要进 32px 大字体。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif text[i] in "\"'":
            q = text[i]
            j = i + 1
            buf = []
            while j < n and text[j] != q and text[j] != "\n":
                if text[j] == "\\":
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            out.append("".join(buf))
            i = j + 1
        else:
            i += 1
    return out


def read_text(path):
    """读 UTF-8 文本文件；二进制文件（含 NUL 或不是合法 UTF-8）返回 None。"""
    try:
        with open(path, "rb") as fp:
            data = fp.read()
    except OSError:
        return None
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def interesting(cp):
    """扫描到的字符里哪些值得放进字体：非 ASCII、非控制字符、非代理/私用区"""
    if cp < 0x80:
        return False
    cat = unicodedata.category(chr(cp))
    return cat not in ("Cc", "Cs", "Co", "Cn")


def walk_files(top):
    result = []
    for dirpath, dirnames, filenames in os.walk(top):
        dirnames.sort()
        for fn in sorted(filenames):
            result.append(os.path.join(dirpath, fn))
    return result


def scan_ui_chars(root):
    """扫描 userspace/**/*.c|h 的字符串字面量（不含 gui/tests/）+ rootfs/** 的全部文本 → 界面会用到的字符"""
    chars = set()
    nfiles = 0
    us = os.path.join(root, "userspace")
    tests = os.path.join(us, "gui", "tests") + os.sep      # 宿主机测试不进系统，它们的字符串不算界面文字
    if os.path.isdir(us):
        for p in walk_files(us):
            if p.endswith(SCAN_EXTS) and not p.startswith(tests):
                t = read_text(p)
                if t is None:
                    continue
                nfiles += 1
                for s in c_string_literals(t):
                    chars.update(ord(c) for c in s)
    rf = os.path.join(root, "rootfs")
    if os.path.isdir(rf):
        for p in walk_files(rf):
            t = read_text(p)
            if t is None:
                continue
            nfiles += 1
            chars.update(ord(c) for c in t)
    return sorted(c for c in chars if interesting(c)), nfiles


def scan_doc_chars(root):
    """docs/*.md 会被复制进系统（/usr/share/doc/sketchos），用记事本阅读"""
    chars = set()
    d = os.path.join(root, "docs")
    if os.path.isdir(d):
        for p in walk_files(d):
            if p.endswith(".md"):
                t = read_text(p)
                if t:
                    chars.update(ord(c) for c in t)
    return sorted(c for c in chars if interesting(c))


# ---------------------------------------------------------------------------
# 读 TrueType 的 cmap 表：字体里到底有哪些字
# （Pillow 不提供这个接口；缺字时 FreeType 会画一个“豆腐块” .notdef，
#   我们需要事先知道哪些字缺失，才能跳过或自己合成。）
# ---------------------------------------------------------------------------
def read_cmap(path):
    with open(path, "rb") as fp:
        data = fp.read()
    num_tables = struct.unpack_from(">H", data, 4)[0]
    tables = {}
    for i in range(num_tables):
        tag, _csum, off, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        tables[tag] = (off, length)
    if b"cmap" not in tables:
        raise SystemExit("%s: 没有 cmap 表" % path)
    base = tables[b"cmap"][0]
    _ver, nsub = struct.unpack_from(">HH", data, base)
    subs = []
    for i in range(nsub):
        pid, eid, off = struct.unpack_from(">HHI", data, base + 4 + 8 * i)
        fmt = struct.unpack_from(">H", data, base + off)[0]
        subs.append((pid, eid, fmt, base + off))
    cps = set()
    # 优先用 format 12（32 位码点，覆盖 BMP 之外的字），其次 format 4（只有 BMP）
    for pid, eid, fmt, so in subs:
        if fmt == 12 and (pid, eid) in ((3, 10), (0, 4), (0, 6)):
            ngroups = struct.unpack_from(">I", data, so + 12)[0]
            for g in range(ngroups):
                start, end, gid = struct.unpack_from(">III", data, so + 16 + 12 * g)
                for cp in range(start, end + 1):
                    if gid + (cp - start) != 0:
                        cps.add(cp)
            return cps
    for pid, eid, fmt, so in subs:
        if fmt == 4 and (pid, eid) in ((3, 1), (0, 3), (0, 4)):
            seg2 = struct.unpack_from(">H", data, so + 6)[0]
            ends = so + 14
            starts = ends + seg2 + 2
            deltas = starts + seg2
            ranges = deltas + seg2
            for i in range(seg2 // 2):
                end = struct.unpack_from(">H", data, ends + 2 * i)[0]
                start = struct.unpack_from(">H", data, starts + 2 * i)[0]
                delta = struct.unpack_from(">H", data, deltas + 2 * i)[0]
                ro = struct.unpack_from(">H", data, ranges + 2 * i)[0]
                for c in range(start, end + 1):
                    if c == 0xFFFF:
                        continue
                    if ro == 0:
                        gid = (c + delta) & 0xFFFF
                    else:
                        addr = ranges + 2 * i + ro + 2 * (c - start)
                        gid = struct.unpack_from(">H", data, addr)[0]
                        if gid:
                            gid = (gid + delta) & 0xFFFF
                    if gid:
                        cps.add(c)
            return cps
    raise SystemExit("%s: 找不到可用的 Unicode cmap 子表" % path)


# ---------------------------------------------------------------------------
# 字符宽度（终端里占几格）。表格从 font.c 里读出来，保证 C 代码的 sk_wcwidth()
# 和这里给等宽字体定的前进宽度完全一致（“单一事实来源”）。
# ---------------------------------------------------------------------------
class WcWidth:
    def __init__(self, font_c_path):
        with open(font_c_path, encoding="utf-8") as fp:
            src = fp.read()
        self.zero = self._table(src, "WCWIDTH_ZERO")
        self.wide = self._table(src, "WCWIDTH_WIDE")

    @staticmethod
    def _table(src, name):
        m = re.search(r"/\* %s-BEGIN \*/(.*?)/\* %s-END \*/" % (name, name), src, re.S)
        if not m:
            raise SystemExit("font.c 里找不到 %s 表的标记" % name)
        pairs = re.findall(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\}", m.group(1))
        tab = [(int(a, 16), int(b, 16)) for a, b in pairs]
        if not tab or any(lo > hi for lo, hi in tab) or \
                any(tab[i][1] >= tab[i + 1][0] for i in range(len(tab) - 1)):
            raise SystemExit("font.c 的 %s 表为空、未排序或区间重叠" % name)
        return tab

    @staticmethod
    def _in(tab, cp):
        lo, hi = 0, len(tab) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if cp < tab[mid][0]:
                hi = mid - 1
            elif cp > tab[mid][1]:
                lo = mid + 1
            else:
                return True
        return False

    def __call__(self, cp):
        if cp < 0x20 or 0x7F <= cp < 0xA0:
            return 0
        if cp < 0x300:
            return 1
        if self._in(self.zero, cp):
            return 0
        if self._in(self.wide, cp):
            return 2
        return 1


# ---------------------------------------------------------------------------
# 光栅化
# ---------------------------------------------------------------------------
class Glyph:
    __slots__ = ("w", "h", "left", "top", "adv", "data", "synth")

    def __init__(self, w, h, left, top, adv, data, synth=False):
        self.w, self.h, self.left, self.top, self.adv = w, h, left, top, adv
        self.data, self.synth = data, synth


def iround(x):
    return int(math.floor(x + 0.5))


# 覆盖率量化成 16 级（0, 17, 34 … 255），仍然按 8 位存储，文件格式不变。
# 抗锯齿边缘有 16 级灰度时人眼已经分辨不出差别（早年 FreeType 的灰度模式也只有 5~17 级），
# 但不同取值少了，gzip 压缩后的字体小 40% 左右——网页版要通过网络下载整个系统，这很重要。
QUANT_LEVELS = 16
_q = 255.0 / (QUANT_LEVELS - 1)
QUANT_TABLE = bytes(iround(iround(v / _q) * _q) for v in range(256))


def crop_glyph(img, origin_x, baseline, adv, synth=False):
    """把画好的灰度图裁到“有墨的最小矩形”，换算出相对笔位/基线的偏移"""
    img = img.point(list(QUANT_TABLE))           # 先量化，再裁剪：被量化成 0 的淡边不占空间
    bbox = img.getbbox()
    if bbox is None:
        return Glyph(0, 0, 0, 0, adv, b"", synth)
    x0, y0, x1, y1 = bbox
    return Glyph(x1 - x0, y1 - y0, x0 - origin_x, baseline - y0, adv,
                 img.crop(bbox).tobytes(), synth)


class Raster:
    def __init__(self, path, size):
        # 固定用 BASIC 布局引擎（不用 raqm），保证不同机器上结果一致
        self.font = ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)
        self.size = size
        self.cmap = read_cmap(path)
        self.ascent, self.descent = self.font.getmetrics()
        self.height = self.font.font.height

    def has(self, cp):
        return cp in self.cmap

    def render(self, cp):
        s = self.size
        img = Image.new("L", (s * 4, s * 4), 0)
        ox, base = s, s * 3
        ImageDraw.Draw(img).text((ox, base), chr(cp), font=self.font, fill=255, anchor="ls")
        adv = iround(self.font.getlength(chr(cp)))
        return crop_glyph(img, ox, base, adv)


def fit_cell(g, cp, target, narrow):
    """等宽字体：让字形的前进宽度恰好是 target（= 格数 × 格宽）。
    霞鹜文楷等宽里很多“东亚歧义宽度”字符（“”…—αβ 等）是按全角画的，
    但终端按 Unicode 标准把它们算作 1 格，所以要把字形塞进 1 格里：
      墨迹本来就够窄 → 水平居中；
      太宽 → 先试比例字体里的同一个字（通常是窄的西文样式）；
      还是太宽 → 缩小：扁长的（— …）只压扁宽度，其它的（● ★）等比例缩小保持形状。"""
    if g.adv == target or target == 0:
        g.adv = target
        return g
    if g.w > target and narrow is not None and narrow.has(cp):
        alt = narrow.render(cp)
        if alt.w < g.w:
            g = alt
    if g.w <= target:
        g.left = (target - g.w) // 2
        g.adv = target
        return g
    room = max(1, target - 1)
    img = Image.frombytes("L", (g.w, g.h), g.data)
    if g.w >= 2 * g.h:
        img = img.resize((room, g.h), Image.LANCZOS)
        top = g.top
    else:
        nh = max(1, iround(g.h * room / float(g.w)))
        img = img.resize((room, nh), Image.LANCZOS)
        top = g.top - (g.h - nh) // 2              # 保持垂直方向的中心不变
    return Glyph(img.width, img.height, (target - img.width) // 2, top, target, img.tobytes())


# ---------------------------------------------------------------------------
# 合成制表符和方块元素
#
# 终端用这些字符拼表格/边框，相邻格子的线条必须严丝合缝地连起来：
# 竖线要从格子顶端一直画到底端（整整一个行高），横线要从格子左边画到右边。
# 字体里的这些字是按全角设计的，放不进半角格子，所以等宽字体里全部由程序按像素画出。
# ---------------------------------------------------------------------------
# U+2500..U+254B 和 U+2550..U+256C、U+2574..U+257F：四个方向（上 右 下 左）的线型，
# 0=无 1=细 2=粗 3=双线。None 表示特殊字符（虚线、圆角、斜线），另行处理。
BOX_ARMS = {}
_box_codes = (
    # 2500-250F
    "0101 0202 1010 2020 - - - - - - - - 0110 0210 0120 0220 "
    # 2510-251F
    "0011 0012 0021 0022 1100 1200 2100 2200 1001 1002 2001 2002 1110 1210 2110 1120 "
    # 2520-252F
    "2120 2210 1220 2220 1011 1012 2011 1021 2021 2012 1022 2022 0111 0112 0211 0212 "
    # 2530-253F
    "0121 0122 0221 0222 1101 1102 1201 1202 2101 2102 2201 2202 1111 1112 1211 1212 "
    # 2540-254F
    "2111 1121 2121 2112 2211 1122 1221 2212 1222 2122 2221 2222 - - - - "
    # 2550-255F
    "0303 3030 0310 0130 0330 0013 0031 0033 1300 3100 3300 1003 3001 3003 1310 3130 "
    # 2560-256F
    "3330 1013 3031 3033 0313 0131 0333 1303 3101 3303 1313 3131 3333 - - - "
    # 2570-257F
    "- - - - 0001 1000 0100 0010 0002 2000 0200 0020 0201 1020 0102 2010"
).split()
for _i, _c in enumerate(_box_codes):
    BOX_ARMS[0x2500 + _i] = None if _c == "-" else tuple(int(x) for x in _c)
assert len(_box_codes) == 128
# 虚线：码点 → (方向 'h'/'v', 粗细 1/2, 段数)
BOX_DASH = {
    0x2504: ("h", 1, 3), 0x2505: ("h", 2, 3), 0x2506: ("v", 1, 3), 0x2507: ("v", 2, 3),
    0x2508: ("h", 1, 4), 0x2509: ("h", 2, 4), 0x250A: ("v", 1, 4), 0x250B: ("v", 2, 4),
    0x254C: ("h", 1, 2), 0x254D: ("h", 2, 2), 0x254E: ("v", 1, 2), 0x254F: ("v", 2, 2),
}
UP, RIGHT, DOWN, LEFT = 0, 1, 2, 3


class BoxPainter:
    """在 W×H 的格子里按像素画制表符。坐标：x 向右，y 向下，格子左上角为 (0,0)。"""

    def __init__(self, w, h, size):
        self.W, self.H = w, h
        self.t1 = max(1, iround(size / 16.0))       # 细线粗细
        self.t2 = self.t1 * 2                       # 粗线粗细
        # 细线所在的像素（格子正中），竖线的 x、横线的 y
        self.px = self._span(w / 2.0, self.t1)[0]
        self.py = self._span(h / 2.0, self.t1)[0]
        # 双线：两条细线，以细线为中心对称、中间空 t1 像素（和 VGA 字体的 ═ 一样）
        self.dbl_v = (self.px - self.t1, self.px + self.t1)     # 双竖线的两个 x
        self.dbl_h = (self.py - self.t1, self.py + self.t1)     # 双横线的两个 y

    @staticmethod
    def _span(mid, t):
        a = int(math.floor(mid - t / 2.0 + 0.5))
        return a, a + t - 1

    def stroke(self, weight, vertical):
        """细线/粗线占据的像素范围（竖线返回 x 范围，横线返回 y 范围），粗线以细线为中心加粗"""
        t = self.t1 if weight == 1 else self.t2
        c = self.px if vertical else self.py
        return self._span(c + self.t1 / 2.0, t)

    def paint(self, arms):
        """arms = (上, 右, 下, 左) 的线型。每条“臂”从格子边缘画向中心，
        关键是决定它朝中心画到哪里，才能和别的臂接成干净的拐角/丁字/十字。"""
        W, H, t1 = self.W, self.H, self.t1
        img = Image.new("L", (W, H), 0)
        dr = ImageDraw.Draw(img)

        for a in (UP, RIGHT, DOWN, LEFT):
            w = arms[a]
            if w == 0:
                continue
            vertical = a in (UP, DOWN)
            from_low = a in (UP, LEFT)            # 从格子的上边/左边画过来
            # 和这条臂垂直的两条臂：竖臂对应(左, 右)，横臂对应(上, 下)
            perp = (arms[LEFT], arms[RIGHT]) if vertical else (arms[UP], arms[DOWN])
            opposite = arms[{UP: DOWN, DOWN: UP, LEFT: RIGHT, RIGHT: LEFT}[a]]
            center = self.py if vertical else self.px          # 沿臂方向的中心像素
            cross_dbl = self.dbl_h if vertical else self.dbl_v  # 垂直方向双线的位置

            def reach(line_pos):
                """“画到并盖住 line_pos 处那条细线”时，沿臂方向的终点坐标"""
                return line_pos + t1 - 1 if from_low else line_pos

            if w in (1, 2):
                lo, hi = self.stroke(w, vertical)
                if opposite and 3 in perp:
                    end = reach(center)            # 单线穿过双线（╪ ╫）：直通
                elif 3 in perp:
                    if perp[0] == 3 and perp[1] == 3:
                        # 双线从这里穿过（╤ ╧）：接到近的那条
                        end = reach(cross_dbl[0] if from_low else cross_dbl[1])
                    else:
                        # 双线在这里拐弯（╒ ╕）：接到远的那条，把两条双线的端点都连上
                        end = reach(cross_dbl[1] if from_low else cross_dbl[0])
                else:
                    # 细/粗线之间：盖满中心的“交汇块”（所有垂直笔画的范围）
                    exts = [self.stroke(p, not vertical) for p in perp if p]
                    if exts:
                        end = max(e[1] for e in exts) if from_low else min(e[0] for e in exts)
                    else:
                        end = reach(center)
                pieces = [(lo, hi, end)]
            else:
                # 双线臂：两条细线（side 0 = 左/上那条，side 1 = 右/下那条）各自决定终点
                lines = self.dbl_v if vertical else self.dbl_h
                pieces = []
                for side in (0, 1):
                    p_same, p_other = perp[side], perp[1 - side]
                    if p_same == 3:
                        # 同侧也是双线（╔ 的内侧线、╬）：停在它离我近的那条线上
                        end = reach(cross_dbl[0] if from_low else cross_dbl[1])
                    elif p_same:
                        end = reach(center)        # 同侧是单线（╞ ╟）：碰到那条单线
                    elif opposite:
                        end = reach(center)        # 对面有同方向的臂（═ ╠）：直通
                    elif p_other == 3:
                        # 拐角的外侧线（╔ 的外框）：延伸到另一侧双线的远线
                        end = reach(cross_dbl[1] if from_low else cross_dbl[0])
                    else:
                        end = reach(center)        # 另一侧是单线（╒ ╓）或什么都没有
                    pieces.append((lines[side], lines[side] + t1 - 1, end))

            for lo, hi, end in pieces:
                if a == UP:
                    box = (lo, 0, hi, end)
                elif a == DOWN:
                    box = (lo, end, hi, H - 1)
                elif a == LEFT:
                    box = (0, lo, end, hi)
                else:
                    box = (end, lo, W - 1, hi)
                if box[2] >= box[0] and box[3] >= box[1]:
                    dr.rectangle(box, fill=255)
        return img

    def dashed(self, direction, weight, n):
        W, H = self.W, self.H
        img = Image.new("L", (W, H), 0)
        dr = ImageDraw.Draw(img)
        lo, hi = self.stroke(weight, direction == "v")
        length = W if direction == "h" else H
        seg = length / float(n)
        dash = max(1, iround(seg / 2.0))
        for i in range(n):
            a = int(math.floor(i * seg + (seg - dash) / 2.0 + 0.5))
            b = min(length - 1, a + dash - 1)
            if direction == "h":
                dr.rectangle((a, lo, b, hi), fill=255)
            else:
                dr.rectangle((lo, a, hi, b), fill=255)
        return img

    def smooth(self, cp):
        """圆角 ╭╮╯╰ 和斜线 ╱╲╳：先在 8 倍分辨率上画，再缩小得到抗锯齿效果；
        直线部分对齐到像素边界，缩小后仍然是精确的整像素，能和邻格接上。"""
        W, H, k = self.W, self.H, 8
        img = Image.new("L", (W * k, H * k), 0)
        dr = ImageDraw.Draw(img)
        t = self.t1 * k
        cx = (self.px + self.t1 / 2.0) * k          # 竖线中心（连续坐标）
        cy = (self.py + self.t1 / 2.0) * k          # 横线中心
        if cp in (0x256D, 0x256E, 0x256F, 0x2570):
            r = min(cx, W * k - cx, cy, H * k - cy) - k / 2.0
            sx = 1 if cp in (0x256D, 0x2570) else -1     # 横线往右(+1)还是往左(-1)
            sy = 1 if cp in (0x256D, 0x256E) else -1     # 竖线往下(+1)还是往上(-1)
            ox, oy = cx + sx * r, cy + sy * r            # 圆心
            start = {0x256D: 180, 0x256E: 270, 0x256F: 0, 0x2570: 90}[cp]
            dr.arc((ox - r - t / 2.0, oy - r - t / 2.0, ox + r + t / 2.0, oy + r + t / 2.0),
                   start, start + 90, fill=255, width=t)
            # 竖直段：从圆弧端点到格子上/下边
            x0 = self.px * k
            if sy > 0:
                dr.rectangle((x0, int(cy + r), x0 + t - 1, H * k - 1), fill=255)
            else:
                dr.rectangle((x0, 0, x0 + t - 1, int(cy - r)), fill=255)
            y0 = self.py * k
            if sx > 0:
                dr.rectangle((int(cx + r), y0, W * k - 1, y0 + t - 1), fill=255)
            else:
                dr.rectangle((0, y0, int(cx - r), y0 + t - 1), fill=255)
        else:
            lw = int(t * 1.2)
            if cp in (0x2571, 0x2573):
                dr.line((W * k, 0, 0, H * k), fill=255, width=lw)
            if cp in (0x2572, 0x2573):
                dr.line((0, 0, W * k, H * k), fill=255, width=lw)
        return img.resize((W, H), Image.BOX)

    def block(self, cp):
        W, H = self.W, self.H
        img = Image.new("L", (W, H), 0)
        dr = ImageDraw.Draw(img)

        def rect(x0, y0, x1, y1, v=255):
            if x1 >= x0 and y1 >= y0:
                dr.rectangle((x0, y0, x1, y1), fill=v)

        hx = W // 2                 # 左右分界
        hy = (H + 1) // 2           # 上下分界（上半多一行时，▀ 和 ▄ 仍然正好拼满）
        if cp == 0x2580:
            rect(0, 0, W - 1, hy - 1)
        elif 0x2581 <= cp <= 0x2588:            # 下 1/8 .. 全满
            n = cp - 0x2580
            hh = iround(H * n / 8.0)
            rect(0, H - hh, W - 1, H - 1)
        elif 0x2589 <= cp <= 0x258F:            # 左 7/8 .. 左 1/8
            n = 0x2590 - cp
            ww = max(1, iround(W * n / 8.0))
            rect(0, 0, ww - 1, H - 1)
        elif cp == 0x2590:
            rect(hx, 0, W - 1, H - 1)
        elif cp in (0x2591, 0x2592, 0x2593):    # 浅/中/深阴影：均匀的覆盖率
            rect(0, 0, W - 1, H - 1, {0x2591: 0x40, 0x2592: 0x80, 0x2593: 0xC0}[cp])
        elif cp == 0x2594:
            rect(0, 0, W - 1, max(1, iround(H / 8.0)) - 1)
        elif cp == 0x2595:
            rect(W - max(1, iround(W / 8.0)), 0, W - 1, H - 1)
        else:
            # 象限块 2596-259F：(左上, 右上, 左下, 右下)
            quads = {
                0x2596: (0, 0, 1, 0), 0x2597: (0, 0, 0, 1), 0x2598: (1, 0, 0, 0),
                0x2599: (1, 0, 1, 1), 0x259A: (1, 0, 0, 1), 0x259B: (1, 1, 1, 0),
                0x259C: (1, 1, 0, 1), 0x259D: (0, 1, 0, 0), 0x259E: (0, 1, 1, 0),
                0x259F: (0, 1, 1, 1),
            }[cp]
            if quads[0]:
                rect(0, 0, hx - 1, hy - 1)
            if quads[1]:
                rect(hx, 0, W - 1, hy - 1)
            if quads[2]:
                rect(0, hy, hx - 1, H - 1)
            if quads[3]:
                rect(hx, hy, W - 1, H - 1)
        return img

    def glyph(self, cp, ascent):
        if 0x2580 <= cp <= 0x259F:
            img = self.block(cp)
        elif cp in BOX_DASH:
            img = self.dashed(*BOX_DASH[cp])
        elif BOX_ARMS.get(cp) is None:
            img = self.smooth(cp)
        else:
            img = self.paint(BOX_ARMS[cp])
        # 格子顶边在基线上方 ascent 像素处
        return crop_glyph(img, 0, ascent, self.W, synth=True)


# ---------------------------------------------------------------------------
# 生成一个字体
# ---------------------------------------------------------------------------
class FontBuild:
    def __init__(self, cfg, srcdir, wcw, charsets):
        self.cfg = cfg
        self.name = cfg["name"]
        self.raster = Raster(os.path.join(srcdir, cfg["ttf"]), cfg["size"])
        self.narrow = None
        if cfg.get("narrow_ttf"):
            self.narrow = Raster(os.path.join(srcdir, cfg["narrow_ttf"]), cfg["size"])
        r = self.raster
        self.size = cfg["size"]
        self.mono = cfg["mono"]
        self.ascent, self.descent = r.ascent, r.descent
        if self.mono:
            # 等宽字体的行高必须正好是 ascent+descent：终端一行就是一个格子高，
            # 合成的竖线画满这个高度，上下两行的线才能接上。
            self.line_height = self.ascent + self.descent
            self.cell = iround(r.font.getlength("M"))
        else:
            self.line_height = max(self.ascent + self.descent, r.height)
            self.cell = 0
        self.wcw = wcw
        self.wanted = set()
        for s in cfg["sets"]:
            self.wanted.update(charsets[s])
        self.glyphs = {}
        self.missing = []        # 想要但字体里没有的字
        self.synth = 0

    def build(self):
        r = self.raster
        box_cell = self.cell if self.mono else max(1, iround(self.size / 2.0))
        painter = BoxPainter(box_cell, self.line_height, self.size)
        for cp in sorted(self.wanted):
            is_box = 0x2500 <= cp <= 0x259F
            if is_box and (self.mono or not r.has(cp)):
                g = painter.glyph(cp, self.ascent)
                self.synth += 1
            elif r.has(cp):
                g = r.render(cp)
                if self.mono:
                    g = fit_cell(g, cp, self.wcw(cp) * self.cell, self.narrow)
            elif cp == 0xA0 and r.has(0x20):
                g = r.render(0x20)                  # 不换行空格 = 空格
                if self.mono:
                    g.adv = self.cell
            else:
                self.missing.append(cp)
                continue
            self.glyphs[cp] = g
        return self

    def check(self):
        bad = []
        for cp, g in self.glyphs.items():
            if g.adv > 255:
                g.adv = 255                         # 格式里 advance 是 uint8
            if not (0 <= g.w <= 255 and 0 <= g.h <= 255 and -128 <= g.left <= 127
                    and -128 <= g.top <= 127):
                bad.append(cp)
            if self.mono and not g.synth and g.adv != self.wcw(cp) * self.cell:
                bad.append(cp)
        if bad:
            raise SystemExit("%s: 字形超出格式范围: %s" % (self.name, ", ".join("U+%04X" % c for c in bad[:20])))

    def ink_extents(self):
        """统计实际墨迹的最高/最低点（检查 ascent/descent 是否够用）"""
        top = max((g.top for g in self.glyphs.values() if g.h), default=0)
        bot = max((g.h - g.top for g in self.glyphs.values() if g.h), default=0)
        return top, bot

    def serialize(self, only=None):
        cps = sorted(c for c in self.glyphs if only is None or c in only)
        index = bytearray()
        bitmaps = bytearray()
        seen = {}                                   # 相同位图只存一份（例如各种空格）
        for cp in cps:
            g = self.glyphs[cp]
            bits = g.data.translate(QUANT_TABLE)
            if g.w * g.h == 0:
                off = 0
            elif bits in seen:
                off = seen[bits]
            else:
                off = len(bitmaps)
                seen[bits] = off
                bitmaps += bits
            index += struct.pack("<IIBBbbB3x", cp, off, g.w, g.h, g.left, g.top, g.adv)
        index_off = 32
        bitmap_off = index_off + len(index)
        header = struct.pack("<4sHhhHIIIHHI", b"SKF1", self.size, self.ascent, self.descent,
                             self.line_height, len(cps), index_off, bitmap_off,
                             1 if self.mono else 0, self.cell, 0)
        assert len(header) == 32
        return bytes(header + index + bitmaps)


# ---------------------------------------------------------------------------
# 输出：内置后备字体（C 数组）、README
# ---------------------------------------------------------------------------
def write_builtin(path, fb, data):
    ttf = TTF_DESC[fb.cfg["ttf"]]
    lines = []
    lines.append("/*")
    lines.append(" * font_builtin.c — 编译进 libsketch 的后备字体（只含 ASCII 0x20..0x7E）")
    lines.append(" *")
    lines.append(" * !!! 本文件由 tools/mkfont.py 自动生成，请不要手工修改；重新生成：bash scripts/fonts.sh !!!")
    lines.append(" *")
    lines.append(" * 来源：%s %s，%dpx，FreeType(Pillow) 8 位灰度光栅化。" % (ttf, FONT_VERSION, fb.size))
    lines.append(" * 许可证：SIL Open Font License 1.1（全文见 assets/fonts/OFL.txt，系统里是 /usr/share/fonts/OFL.txt）。")
    lines.append(" *   Copyright 2021-2026 LXGW (https://github.com/lxgw/LxgwWenKai)")
    lines.append(" *   Copyright 2020 The Klee Project Authors (https://github.com/fontworks-fonts/Klee)")
    lines.append(" *   按 OFL 的要求，转换后的字体不使用保留字体名称，这里称为“SketchOS 内置等宽字体”。")
    lines.append(" *")
    lines.append(" * 为什么要内置一份字体？字体文件放在 /usr/share/fonts/，万一文件丢失或损坏，")
    lines.append(" * 界面不能因此变成一片空白——sk_font_get() 承诺“永远不返回 NULL”，靠的就是它。")
    lines.append(" * 数组里就是一个完整的 .skf 文件（格式见 sketch/font.h），font.c 用和读文件完全相同的")
    lines.append(" * 代码解析它：对程序来说，“文件”不过是一串字节，放在磁盘上还是编译进程序里都一样。")
    lines.append(" *")
    lines.append(" * 度量：等宽，格宽 %d，行高 %d（ascent %d + descent %d），%d 个字形，共 %d 字节。"
                 % (fb.cell, fb.line_height, fb.ascent, fb.descent, len([c for c in SET_ASCII if c in fb.glyphs]), len(data)))
    lines.append(" */")
    lines.append("#include <stddef.h>")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("extern const uint8_t sk_font_builtin_data[];")
    lines.append("extern const size_t sk_font_builtin_size;")
    lines.append("")
    lines.append("/* 16 字节对齐：和 mmap 出来的文件一样，按 4 字节读取索引项不会跨对齐边界 */")
    lines.append("__attribute__((aligned(16))) const uint8_t sk_font_builtin_data[%d] = {" % len(data))
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + " ".join("0x%02x," % b for b in chunk))
    lines.append("};")
    lines.append("const size_t sk_font_builtin_size = sizeof(sk_font_builtin_data);")
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(lines))


def fmt_size(n):
    if n >= 1024 * 1024:
        return "%.2f MB" % (n / 1048576.0)
    return "%.1f KB" % (n / 1024.0)


def write_readme(path, builds, sizes, stats):
    L = []
    L.append("# SketchOS 系统字体")
    L.append("")
    L.append("> 本文件由 `tools/mkfont.py` 自动生成（`bash scripts/fonts.sh`），请不要手工修改。")
    L.append("")
    L.append("## 字体与许可证")
    L.append("")
    L.append("- 来源字体：**霞鹜文楷 / 霞鹜文楷等宽**（LXGW WenKai / LXGW WenKai Mono）%s，"
             "作者 LXGW，基于 Fontworks 的 Klee One。手写楷体风格，和 SketchOS 的手绘界面很配。" % FONT_VERSION)
    L.append("- 下载地址：<https://github.com/lxgw/LxgwWenKai/releases/tag/%s>" % FONT_VERSION)
    L.append("- 许可证：**SIL Open Font License 1.1**，全文见同目录的 [`OFL.txt`](OFL.txt)"
             "（系统里安装在 `/usr/share/fonts/OFL.txt`）。OFL 允许自由使用、修改、嵌入和再分发，"
             "条件是附上版权声明和许可证、不单独出售字体。")
    L.append("- 这里的 `.skf` 文件是从原字体**转换格式**得到的位图字体，属于 OFL 所说的“修改版本”。"
             "原字体声明了保留字体名称（“霞鹜”“LXGW”等），所以转换后的字体不使用这些名字，"
             "统称为 **SketchOS Sans / SketchOS Mono**；“霞鹜文楷”只用来说明来源。")
    L.append("")
    L.append("## 文件")
    L.append("")
    L.append("| 文件 | 用途 (`enum sk_font_id`) | 来源字形 | 字号 | ascent | descent | 行高 | 格宽 | 字形数 | 大小 |")
    L.append("|---|---|---|---|---|---|---|---|---|---|")
    usage = {"sans-16": "`SK_FONT_UI` 正文", "sans-20": "`SK_FONT_TITLE` 标题/按钮",
             "sans-32": "`SK_FONT_LARGE` 大字（启动画面、时钟）", "mono-16": "`SK_FONT_MONO` 终端"}
    for b in builds:
        L.append("| `%s.skf` | %s | %s | %dpx | %d | %d | %d | %s | %d | %s |" % (
            b.name, usage[b.name], TTF_DESC[b.cfg["ttf"]], b.size, b.ascent, b.descent,
            b.line_height, str(b.cell) if b.mono else "-", len(b.glyphs), fmt_size(sizes[b.name])))
    L.append("")
    L.append("此外 `userspace/gui/libsketch/font_builtin.c` 里编译进了一份只含 ASCII 的等宽后备字体"
             "（%s），字体文件缺失或损坏时使用，大小 %s。" % (stats["builtin_desc"], fmt_size(stats["builtin_size"])))
    L.append("")
    L.append("## 字重的选择")
    L.append("")
    L.append("生成前把 Regular 和 Medium 在各个字号下渲染在“纸”色背景上对比（`python3 tools/mkfont.py --src <字体目录> --compare 对比图.png`）：")
    L.append("")
    L.append("- **16px（正文、终端）选 Medium**：Regular 的笔画在 16px 下只有 1 像素左右，抗锯齿后成了浅灰色，"
             "在暖白色纸面上发虚、看久了累眼；Medium 笔画更实，小号汉字的结构也更清楚。")
    L.append("- **20px（标题、按钮）选 Medium**：和正文保持同一字重，按钮文字更醒目。")
    L.append("- **32px（大字）选 Regular**：字号大了以后 Regular 已经足够黑，还保留楷书轻盈的笔意，"
             "像铅笔写的；Medium 在这个尺寸显得像马克笔，太重。")
    L.append("")
    L.append("## 字符覆盖")
    L.append("")
    L.append("| 字符集 | 范围 | sans-16 / sans-20 / mono-16 | sans-32 |")
    L.append("|---|---|---|---|")
    L.append("| ASCII | U+0020–007E | ✓ | ✓ |")
    L.append("| Latin-1 | U+00A0–00FF | ✓ | ✓ |")
    L.append("| 常用标点 | U+2010–205E（‘’“”…—–•‰‹› 等） | ✓ | ✓ |")
    L.append("| 箭头、数学符号、几何图形 | ←↑→↓↔ ×÷±≤≥≠∞√ ■□▲△▼▽●○◆◇★☆ ①… | ✓ | ✓ |")
    L.append("| 中文标点 | U+3000–303F | ✓ | ✓ |")
    L.append("| 全角字符 | U+FF01–FF5E、U+FFE0–FFE6 | ✓ | ✓ |")
    L.append("| 制表符、方块元素 | U+2500–257F、U+2580–259F | ✓（mono-16 里由程序合成） | — |")
    L.append("| GB2312 符号区 | 01–09 区（希腊/俄文字母、假名、拼音、注音、序号…）共 %d 个 | ✓ | — |" % stats["gb_symbols"])
    L.append("| GB2312 全部汉字 | 16–87 区，一级 + 二级共 %d 个 | ✓ | — |" % stats["gb_hanzi"])
    L.append("| 界面文字 | 扫描 `userspace/**/*.c,*.h`（不含 `gui/tests/`）的字符串和 `rootfs/**` 的文本得到 %d 个非 ASCII 字符 | ✓ | ✓ |" % stats["scan"])
    L.append("| 文档文字 | `docs/*.md` 里出现的字符 | ✓ | — |")
    L.append("| 大字补充 | 时钟和启动画面用字：`%s` | — | ✓ |" % LARGE_EXTRA)
    L.append("")
    L.append("缺字时 libsketch 会沿回退链查找（`sans-32 → sans-20 → sans-16 → 内置`，`mono-16 → sans-16`），"
             "都没有就画一个手绘小方框。所以 sans-32 里没有的汉字会用 20px 显示，而不是变成方框。")
    L.append("")
    L.append("**注意**：sans-32 只含扫描到的界面用字。新增界面文字后请重新运行 `bash scripts/fonts.sh`。")
    if stats["missing"]:
        L.append("")
        L.append("源字体里没有、因而跳过的字符：%s" % " ".join("U+%04X" % c for c in stats["missing"][:60]))
    L.append("")
    L.append("## 生成方法")
    L.append("")
    L.append("1. `scripts/fonts.sh` 从 GitHub 下载 4 个 TTF 和 OFL.txt 到 `$WORK/fonts/`（已缓存就跳过）。")
    L.append("2. `tools/mkfont.py` 用 Pillow（内部是 FreeType）把每个字画到灰度图上，"
             "裁成“有墨的最小矩形”，记下相对笔位的左偏移、相对基线的上偏移和前进宽度，"
             "写成 `.skf`：32 字节文件头 + 按码点排序的 16 字节索引 + 8 位覆盖率位图（格式见 `sketch/font.h`）。"
             "内容相同的位图只存一份。")
    L.append("3. ascent/descent 取自字体的度量（FreeType 的 size metrics）；比例字体的行高取 "
             "max(ascent+descent, 字体推荐行高)；等宽字体的行高严格等于 ascent+descent。")
    L.append("4. 等宽字体：格宽 = 字母 M 的前进宽度；每个字的前进宽度 = `sk_wcwidth(码点) × 格宽`"
             "（宽度表直接从 `font.c` 里读，保证和 C 代码一致）。汉字、全角字符正好两格；"
             "“东亚歧义宽度”字符（“”…—αβ●→ 等）按 Unicode 标准占一格——原字形太宽时，先换用比例字体的窄字形，"
             "还放不下就缩小：扁长的（— …）只压窄，其它的（● ★ →）等比例缩小以保持形状。")
    L.append("5. 制表符和方块元素在等宽字体里按像素合成：竖线画满整个行高、横线画满整个格宽，"
             "相邻格子拼起来是连续的线；圆角和斜线 8 倍超采样后缩小，带抗锯齿。")
    L.append("6. 覆盖率量化成 %d 级（0、17、34 … 255），仍按 8 位存储、格式不变：抗锯齿边缘有 16 级灰度时"
             "肉眼看不出差别，但取值少了，gzip 压缩后字体小约 40%%（网页版要通过网络下载整个系统）。" % QUANT_LEVELS)
    L.append("")
    L.append("## 已知的细节")
    L.append("")
    L.append("- 少数带重音的大写字母（À É Ñ Й 等）比 ascent 高 1 像素（FreeType 对度量取整造成），"
             "在终端里可能和上一行重叠 1 像素。")
    L.append("- 源字体没有组合附加符号（U+0300–036F），这些字符宽度为 0、不显示；"
             "Latin-1 里的预组合字母（é ñ ü …）正常显示。")
    L.append("- 等宽字体里歧义宽度的几何符号和箭头被缩进一个半角格子，显得比较小——这是"
             "“歧义宽度占一格”这条终端惯例的代价。")
    L.append("")
    L.append("## 等宽字体的格子约定（终端用）")
    L.append("")
    L.append("- 一个格子宽 `sk_font_cell_width()`、高 `sk_font_line_height()`；")
    L.append("- 在格子里画字时，基线 = 格子顶边 + `sk_font_ascent()`；")
    L.append("- 双宽字符（`sk_wcwidth()==2`）占两个格子，字形的前进宽度正好是两格。")
    L.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(L))


def compare_weights(srcdir, out):
    """画一张 Regular / Medium 对比图，帮助选择字重"""
    lines = ["手绘操作系统 SketchOS：文件 终端 记事本 画板 设置",
             "进程是正在运行的程序；内核负责调度、内存管理和文件系统。",
             "The quick brown fox jumps over the lazy dog. 0123456789",
             "root@sketchos:~# ls -l /usr/share/fonts  (gjpqy) [{}]"]
    rows = [("LXGWWenKai", s) for s in (16, 20, 32)] + [("LXGWWenKaiMono", 16)]
    img = Image.new("RGB", (1130, 1000), (0xFF, 0xFD, 0xF7))
    dr = ImageDraw.Draw(img)
    label = ImageFont.truetype(os.path.join(srcdir, "LXGWWenKai-Regular.ttf"), 13)
    y = 8
    for fam, size in rows:
        for weight in ("Regular", "Medium"):
            f = ImageFont.truetype(os.path.join(srcdir, "%s-%s.ttf" % (fam, weight)), size)
            dr.text((8, y), "%s %s %dpx" % (fam.replace("LXGW", ""), weight, size), font=label,
                    fill=(0xE0, 0x60, 0x5A))
            for t in (lines if size == 16 else lines[:2]):
                dr.text((200, y), t, font=f, fill=(0x2B, 0x2B, 0x2B))
                y += int(size * 1.35)
            y += 6
    img.crop((0, 0, 1130, y + 4)).save(out)
    print(out)


def main():
    ap = argparse.ArgumentParser(description="TrueType → SketchOS .skf 位图字体")
    ap.add_argument("--src", required=True, help="TTF 所在目录（scripts/env.sh 的 FONT_CACHE）")
    ap.add_argument("--root", help="仓库根目录（扫描界面文字、读取 font.c 的宽度表）")
    ap.add_argument("--out", help="输出目录（assets/fonts）")
    ap.add_argument("--builtin", help="内置后备字体 C 文件的输出路径")
    ap.add_argument("--compare", help="只生成字重对比图到这个 PNG")
    a = ap.parse_args()

    if a.compare:
        compare_weights(a.src, a.compare)
        return
    if not (a.root and a.out):
        ap.error("需要 --root 和 --out")

    wcw = WcWidth(os.path.join(a.root, "userspace", "gui", "libsketch", "font.c"))
    gb_symbols, gb_hanzi = gb2312_chars()
    if len(gb_hanzi) != 6763:
        raise SystemExit("GB2312 汉字数不对: %d" % len(gb_hanzi))
    scan, nfiles = scan_ui_chars(a.root)
    docs = scan_doc_chars(a.root)
    base = (SET_ASCII + SET_LATIN1 + SET_GENERAL_PUNCT + SET_ARROWS + SET_MATH + SET_LETTERLIKE
            + SET_GEOMETRIC + SET_CJK_PUNCT + SET_FULLWIDTH + SET_SPECIAL)
    charsets = {
        "base": set(base),
        "box": set(SET_BOX + SET_BLOCK),
        "gb2312": set(gb_symbols + gb_hanzi),
        "scan": set(scan),
        "docs": set(docs),
        "large": set(ord(c) for c in LARGE_EXTRA),
    }
    print("扫描了 %d 个源码/配置文件，界面用到 %d 个非 ASCII 字符；文档 %d 个" % (nfiles, len(scan), len(docs)))

    os.makedirs(a.out, exist_ok=True)
    builds, sizes = [], {}
    missing_all = set()
    for cfg in FONTS:
        b = FontBuild(cfg, a.src, wcw, charsets).build()
        b.check()
        data = b.serialize()
        path = os.path.join(a.out, cfg["name"] + ".skf")
        with open(path, "wb") as fp:
            fp.write(data)
        sizes[cfg["name"]] = len(data)
        top, bot = b.ink_extents()
        print("%-8s %5d 字形  %8s  ascent=%d descent=%d 行高=%d 格宽=%d  合成=%d 缺字=%d  墨迹最高=%d 最低=%d"
              % (cfg["name"], len(b.glyphs), fmt_size(len(data)), b.ascent, b.descent,
                 b.line_height, b.cell, b.synth, len(b.missing), top, bot))
        missing_all.update(c for c in b.missing if c not in charsets["box"])
        builds.append(b)

    builtin_size = 0
    builtin_desc = ""
    fb = next(b for b in builds if b.name == BUILTIN_FROM)
    if a.builtin:
        data = fb.serialize(only=set(SET_ASCII))
        write_builtin(a.builtin, fb, data)
        builtin_size = len(data)
        print("内置后备字体 → %s（%d 字节）" % (a.builtin, builtin_size))
    builtin_desc = "%s %dpx，格宽 %d，行高 %d" % (TTF_DESC[fb.cfg["ttf"]], fb.size, fb.cell, fb.line_height)

    # 许可证原文跟着字体走（OFL 的要求）
    ofl_src = os.path.join(a.src, "OFL.txt")
    if os.path.exists(ofl_src):
        with open(ofl_src, "rb") as fp:
            ofl = fp.read().replace(b"\r\n", b"\n")
        with open(os.path.join(a.out, "OFL.txt"), "wb") as fp:
            fp.write(ofl)
    else:
        print("警告：%s 不存在，没有复制许可证" % ofl_src, file=sys.stderr)

    stats = dict(gb_symbols=len(gb_symbols), gb_hanzi=len(gb_hanzi), scan=len(scan),
                 missing=sorted(missing_all), builtin_size=builtin_size, builtin_desc=builtin_desc)
    write_readme(os.path.join(a.out, "README.md"), builds, sizes, stats)
    if missing_all:
        print("源字体缺少的字符（已跳过）: %s" % " ".join("U+%04X" % c for c in sorted(missing_all)[:40]))


if __name__ == "__main__":
    main()
