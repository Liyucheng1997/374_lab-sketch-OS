#!/usr/bin/env node
/**
 * SketchOS 网页版自动化测试工具：在 Node.js 里无界面地运行 v86 模拟器（和网页版是同一个模拟器），
 * 通过串口、键盘、鼠标控制系统并截图。用法和 tools/qemu_harness.py 对应。
 *
 * 在 Windows 上用 node 运行（WSL 里没有 node）：
 *     node tools/v86_harness.mjs [选项] [--script test.txt]
 * 先运行 bash scripts/web.sh --lib-only 准备好 web/lib/（libv86.mjs、v86.wasm、BIOS）。
 *
 * 选项：
 *     --kernel 文件       内核，默认 out/bzImage
 *     --initrd 文件       initramfs，默认 out/initramfs.cpio.gz
 *     --append "参数"     追加到内核命令行
 *     --cmdline "参数"    替换整条内核命令行（默认和网页版相同）
 *     --mem MB            内存大小，默认 256；--vga-mem MB 显存，默认 16
 *     --lib 目录          v86 文件所在目录，默认 web/lib
 *     --script 文件       测试脚本（见下）；不给脚本时进入交互模式：键盘输入直接发到串口，Ctrl+] 退出
 *     --log 文件          串口日志保存路径
 *     --audio-wav 文件    把 SB16 声卡放出的声音录成 WAV 文件（停机时写出）
 *     --quiet             不把串口输出打印到终端（默认会实时打印）
 *     --timeout 秒        脚本跑完后再保持运行的秒数（交互模式下表示最长运行时间）
 *     --screen WxH        文本模式下鼠标坐标换算用的屏幕大小，默认 1024x768（图形模式自动用真实分辨率）
 * 路径可以写 Windows 路径，也可以写 WSL 路径（/mnt/f/... 或 /root/...，会自动换算）。
 *
 * 脚本每行一个命令（# 开头是注释），和 qemu_harness.py 相同：
 *     wait <正则> [超时秒]        等待串口输出匹配（JavaScript 正则，从开机或上一个 mark 算起）
 *     mark                        之后的 wait 只在此刻以后的串口输出里找（例如重启之后）
 *     sleep <秒>
 *     serial <文本>               向串口写入（\n 回车，\xNN 任意字节，\e 是 ESC）
 *     run <命令>                  在串口 shell 执行并打印输出（先等串口安静 0.2 秒；靠唯一的结束标记识别输出结尾）
 *     shot <文件.png>             截屏：直接读取模拟显卡的显存；文本模式用显卡里的字库画出字符
 *     text                        打印 VGA 文本模式屏幕上的文字
 *     wait_text <正则> [超时秒]   等待 VGA 文本模式屏幕上出现匹配的文字
 *     move <x> <y>                鼠标移动到屏幕坐标
 *     click <x> <y> [left|right|middle]
 *     dclick <x> <y>              双击
 *     drag <x1> <y1> <x2> <y2>
 *     wheel <x> <y> <格数>        正数向上
 *     key <组合键>                QEMU qcode 名称，例如 ret、ctrl-alt-t、alt-f4、shift-a
 *     type <文本>                 逐字符输入（美式键盘能打出的 ASCII，\n=回车）
 *     expect_shot_differs <a.png> <b.png>   两张截图必须不同
 *
 * 也可以作为库使用：
 *     import { V86VM } from "./v86_harness.mjs";
 *     const vm = await new V86VM({ initrd: "test.cpio.gz" }).start();
 *     await vm.waitSerial(/# $/m, 60);
 *     console.log(await vm.runSerial("uname -a"));
 *     await vm.click(500, 300); await vm.key("ctrl-alt-t"); await vm.typeText("ls\n");
 *     await vm.screenshot("shot.png");
 *     await vm.stop();
 *
 * 输入是怎么送进虚拟机的（和浏览器里 v86 的 src/browser/mouse.js、keyboard.js 走同一条“总线”）：
 *     键盘  "keyboard-code"：PS/2 第 1 套扫描码，按下 = 码，松开 = 码 | 0x80，扩展键先发 0xE0
 *     鼠标  "mouse-delta" [dx, dy]：PS/2 相对位移包（y 向上为正），会触发 12 号中断(IRQ12)
 *           "mouse-absolute" [x, y, 宽, 高]：VMware 后门端口里的绝对坐标(0..65535)
 *           "mouse-click" [左, 中, 右]：按键状态，PS/2 和 VMware 两边都会收到
 *     VMware 绝对坐标本身不产生中断：客户机驱动是在每次 PS/2 鼠标中断时去后门端口取坐标的，
 *     所以移动时两种事件都要发（浏览器里也是这样）。
 */
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath, pathToFileURL } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const DEFAULT_CMDLINE =
    "console=tty1 console=ttyS0 tsc=reliable mitigations=off random.trust_cpu=on loglevel=4 sketch.mode=1024x768";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/* ---------- 路径：允许在 Windows 的 node 里直接写 WSL 路径 ---------- */
export function hostPath(p) {
    // 只有单个 / 开头的才是 WSL 路径；//server/share 这样的 UNC 路径原样交给 Node
    if (process.platform === "win32" && p.startsWith("/") && !p.startsWith("//")) {
        const m = /^\/mnt\/([a-zA-Z])(\/.*)?$/.exec(p);
        if (m) {
            return m[1].toUpperCase() + ":" + (m[2] || "/").replace(/\//g, "\\");
        }
        const distro = process.env.SKETCHOS_WSL_DISTRO || "Ubuntu";
        return "\\\\wsl.localhost\\" + distro + p.replace(/\//g, "\\");
    }
    return path.resolve(p);
}

/* ---------- 键盘：QEMU qcode 名称 → PS/2 第 1 套扫描码 ---------- */
// 大于 0xFF 的表示带 0xE0 前缀的扩展键
const SCANCODES = {
    esc: 0x01, 1: 0x02, 2: 0x03, 3: 0x04, 4: 0x05, 5: 0x06, 6: 0x07, 7: 0x08, 8: 0x09, 9: 0x0A, 0: 0x0B,
    minus: 0x0C, equal: 0x0D, backspace: 0x0E, tab: 0x0F,
    q: 0x10, w: 0x11, e: 0x12, r: 0x13, t: 0x14, y: 0x15, u: 0x16, i: 0x17, o: 0x18, p: 0x19,
    bracket_left: 0x1A, bracket_right: 0x1B, ret: 0x1C, ctrl: 0x1D,
    a: 0x1E, s: 0x1F, d: 0x20, f: 0x21, g: 0x22, h: 0x23, j: 0x24, k: 0x25, l: 0x26,
    semicolon: 0x27, apostrophe: 0x28, grave_accent: 0x29, shift: 0x2A, backslash: 0x2B,
    z: 0x2C, x: 0x2D, c: 0x2E, v: 0x2F, b: 0x30, n: 0x31, m: 0x32, comma: 0x33, dot: 0x34, slash: 0x35,
    shift_r: 0x36, kp_multiply: 0x37, alt: 0x38, spc: 0x39, caps_lock: 0x3A,
    f1: 0x3B, f2: 0x3C, f3: 0x3D, f4: 0x3E, f5: 0x3F, f6: 0x40, f7: 0x41, f8: 0x42, f9: 0x43, f10: 0x44,
    num_lock: 0x45, scroll_lock: 0x46,
    kp_7: 0x47, kp_8: 0x48, kp_9: 0x49, kp_subtract: 0x4A, kp_4: 0x4B, kp_5: 0x4C, kp_6: 0x4D,
    kp_add: 0x4E, kp_1: 0x4F, kp_2: 0x50, kp_3: 0x51, kp_0: 0x52, kp_decimal: 0x53,
    less: 0x56, f11: 0x57, f12: 0x58,
    kp_enter: 0xE01C, ctrl_r: 0xE01D, kp_divide: 0xE035, sysrq: 0xE037, print: 0xE037, alt_r: 0xE038,
    home: 0xE047, up: 0xE048, pgup: 0xE049, left: 0xE04B, right: 0xE04D, end: 0xE04F,
    down: 0xE050, pgdn: 0xE051, insert: 0xE052, delete: 0xE053,
    meta_l: 0xE05B, meta_r: 0xE05C, menu: 0xE05D,
};
// 常见别名，方便手写脚本
const KEY_ALIASES = {
    enter: "ret", return: "ret", space: "spc", escape: "esc", del: "delete", ins: "insert",
    pageup: "pgup", pagedown: "pgdn", control: "ctrl", lctrl: "ctrl", rctrl: "ctrl_r",
    lalt: "alt", ralt: "alt_r", altgr: "alt_r", lshift: "shift", rshift: "shift_r",
    win: "meta_l", super: "meta_l", meta: "meta_l", asterisk: "kp_multiply", period: "dot",
};
// 字符 → (qcode, 是否需要 Shift)，美式键盘
const SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right", "|": "backslash",
    ":": "semicolon", "\"": "apostrophe", "<": "comma", ">": "dot", "?": "slash", "~": "grave_accent",
};
const PLAIN = {
    " ": "spc", "\n": "ret", "\r": "ret", "\t": "tab", "-": "minus", "=": "equal", "[": "bracket_left",
    "]": "bracket_right", "\\": "backslash", ";": "semicolon", "'": "apostrophe", ",": "comma",
    ".": "dot", "/": "slash", "`": "grave_accent", "\b": "backspace", "\x1b": "esc",
};

export function keyCode(name) {
    const n = name.toLowerCase();
    const k = KEY_ALIASES[n] || n;
    const code = SCANCODES[k];
    if (code === undefined) {
        throw new Error("未知按键名: " + name);
    }
    return code;
}

export function charToKey(ch) {
    if (/^[a-z0-9]$/.test(ch)) return [ch, false];
    if (/^[A-Z]$/.test(ch)) return [ch.toLowerCase(), true];
    if (ch in PLAIN) return [PLAIN[ch], false];
    if (ch in SHIFTED) return [SHIFTED[ch], true];
    throw new Error("无法用键盘输入字符 " + JSON.stringify(ch) + "（中文等请用 serial 命令）");
}

/** 'ctrl-alt-t' → [码...]；'ctrl--' 表示 ctrl + minus */
export function parseCombo(combo) {
    let parts = combo.split("-");
    if (combo.endsWith("--")) {
        parts = parts.slice(0, -2).concat(["minus"]);
    }
    return parts.filter(Boolean).map(keyCode);
}

/* ---------- 文本转义：\n \t \e \xNN \uNNNN → 字节 ---------- */
export function unescapeToBytes(s) {
    const out = [];
    const enc = new TextEncoder();
    const pushText = (t) => { for (const b of enc.encode(t)) out.push(b); };
    const map = { n: 10, r: 13, t: 9, e: 27, a: 7, b: 8, 0: 0, "\\": 92 };
    let i = 0;
    while (i < s.length) {
        if (s[i] === "\\" && i + 1 < s.length) {
            const n = s[i + 1];
            if (n in map) {
                out.push(map[n]);
                i += 2;
            } else if (n === "x" && /^[0-9a-fA-F]{2}$/.test(s.slice(i + 2, i + 4))) {
                out.push(parseInt(s.slice(i + 2, i + 4), 16));   // \xNN：原样的一个字节
                i += 4;
            } else if (n === "u" && /^[0-9a-fA-F]{4}$/.test(s.slice(i + 2, i + 6))) {
                pushText(String.fromCharCode(parseInt(s.slice(i + 2, i + 6), 16)));
                i += 6;
            } else {
                pushText(n);                                     // 未知转义：保留字符本身
                i += 2;
            }
        } else {
            const ch = String.fromCodePoint(s.codePointAt(i));   // 按码点走，汉字、emoji 都不会被拆坏
            pushText(ch);
            i += ch.length;
        }
    }
    return Uint8Array.from(out);
}

/* ---------- PNG 编码（node:zlib 负责 deflate 压缩） ---------- */
const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        t[n] = c >>> 0;
    }
    return t;
})();

function crc32(buf) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
}

function pngChunk(type, data) {
    const head = Buffer.alloc(8);
    head.writeUInt32BE(data.length, 0);
    head.write(type, 4, "latin1");
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(Buffer.concat([head.subarray(4), data])), 0);
    return Buffer.concat([head, data, crc]);
}

/** rgb: 每像素 3 字节；note: 写进 PNG 的 iTXt 注释（UTF-8） */
export function encodePNG(width, height, rgb, note) {
    const stride = width * 3;
    const raw = Buffer.alloc((stride + 1) * height);
    for (let y = 0; y < height; y++) {
        raw[y * (stride + 1)] = 0;   // 每行开头的过滤类型：0 = 不过滤
        raw.set(rgb.subarray(y * stride, (y + 1) * stride), y * (stride + 1) + 1);
    }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(width, 0);
    ihdr.writeUInt32BE(height, 4);
    ihdr[8] = 8;    // 每通道 8 位
    ihdr[9] = 2;    // 颜色类型 2 = RGB
    const chunks = [Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]), pngChunk("IHDR", ihdr)];
    if (note) {
        chunks.push(pngChunk("iTXt", Buffer.concat([Buffer.from("Comment\0\0\0\0\0", "latin1"),
            Buffer.from(note, "utf8")])));
    }
    chunks.push(pngChunk("IDAT", zlib.deflateSync(raw, { level: 6 })), pngChunk("IEND", Buffer.alloc(0)));
    return Buffer.concat(chunks);
}

/* ---------- 显卡画面 ---------- */

/** VBE/bochs 线性帧缓冲（bochs-drm、sketchd 用的就是它）：直接按像素格式读显存 */
function grabSvga(cpu, vga) {
    const w = vga.svga_width, h = vga.svga_height, bpp = vga.svga_bpp;
    const bytesPP = bpp === 15 ? 2 : bpp >> 3;
    // 显存是 wasm 线性内存里的一段；每次都重新建视图，防止内存扩容后旧视图失效
    const mem = new Uint8Array(cpu.wasm_memory.buffer, vga.svga_memory.byteOffset, vga.vga_memory_size);
    // svga_offset 是“从第几个像素开始显示”（bochs 的 Y 偏移，用来做双缓冲翻页）
    const base = vga.svga_offset * bytesPP;
    const stride = w * bytesPP;
    const rgb = new Uint8Array(w * h * 3);
    const pal = vga.vga256_palette;
    for (let y = 0; y < h; y++) {
        let s = base + y * stride;
        if (s + stride > mem.length) break;
        let d = y * w * 3;
        for (let x = 0; x < w; x++, s += bytesPP, d += 3) {
            let r, g, b;
            if (bpp === 32 || bpp === 24) {
                b = mem[s]; g = mem[s + 1]; r = mem[s + 2];       // 小端：B G R (X)
            } else if (bpp === 16) {
                const v = mem[s] | mem[s + 1] << 8;               // RGB565
                r = (v >> 11 & 31) * 255 / 31; g = (v >> 5 & 63) * 255 / 63; b = (v & 31) * 255 / 31;
            } else if (bpp === 15) {
                const v = mem[s] | mem[s + 1] << 8;               // RGB555
                r = (v >> 10 & 31) * 255 / 31; g = (v >> 5 & 31) * 255 / 31; b = (v & 31) * 255 / 31;
            } else {
                const c = pal[mem[s]];                            // 8 位调色板模式
                r = c >> 16 & 255; g = c >> 8 & 255; b = c & 255;
            }
            rgb[d] = r; rgb[d + 1] = g; rgb[d + 2] = b;
        }
    }
    return { width: w, height: h, rgb, mode: `图形 ${w}x${h}x${bpp}` };
}

/** VGA 文本模式：显存里每个字符占 2 字节（字符码 + 颜色属性），字形在显卡第 2 平面的字库里。
 *  这里按真实 VGA 的规则把字符画出来，所以开机早期（BIOS、内核 vgacon）也能截图。 */
function grabText(vga) {
    const cols = Math.min(vga.max_cols, 256), rows = Math.min(vga.max_rows, 128);
    const charH = (vga.max_scan_line & 0x1F) + 1;
    const dbl = (vga.clocking_mode & 0x08) !== 0;
    const nine = !dbl && !(vga.clocking_mode & 0x01);
    const charW = dbl ? 16 : nine ? 9 : 8;
    const width = cols * charW, height = rows * charH;
    const rgb = new Uint8Array(width * height * 3);
    const mem = vga.vga_memory, plane2 = vga.plane2, pal = vga.vga256_palette;

    // 字库页：字符映射选择寄存器选出 A/B 两页（每页 8KB，按 VGA 的非线性编号换算）
    const linear = [0, 2, 4, 6, 1, 3, 5, 7];
    const cms = vga.character_map_select;
    const pageA = linear[((cms & 0b1100) >> 2) | ((cms & 0b100000) >> 3)] * 8192;
    const pageB = linear[(cms & 0b11) | ((cms & 0b10000) >> 2)] * 8192;
    const abEnabled = !!vga.font_page_ab_enabled;
    const blink = (vga.attribute_mode & 0x08) !== 0;
    const copy8th = (vga.attribute_mode & 0x04) !== 0;
    const fgMask = abEnabled ? 7 : 0xF, bgMask = blink ? 7 : 0xF;
    const color = (nibble) => pal[vga.dac_mask & vga.dac_map[nibble]];
    const splitRow = typeof vga.scan_line_to_screen_row === "function"
        ? vga.scan_line_to_screen_row(vga.line_compare) : -1;
    const rowSkip = Math.max(0, (vga.offset_register * 2 - vga.max_cols) * 2);
    const lines = [];

    const put = (px, py, c) => {
        const d = (py * width + px) * 3;
        rgb[d] = c >> 16 & 255; rgb[d + 1] = c >> 8 & 255; rgb[d + 2] = c & 255;
    };
    let addr = vga.start_address << 1;
    for (let row = 0; row < rows; row++) {
        if (row === splitRow) addr = 0;
        let text = "";
        for (let col = 0; col < cols; col++, addr += 2) {
            const chr = mem[addr] | 0, attr = mem[addr | 1] | 0;
            text += chr >= 0x20 && chr < 0x7F ? String.fromCharCode(chr) : chr === 0 ? " " : ".";
            const fg = color(attr & fgMask), bg = color(attr >> 4 & bgMask);
            const font = (abEnabled && !(attr & 0x08) ? pageB : pageA) + chr * 32;
            for (let line = 0; line < charH; line++) {
                const bits = plane2[font + line];
                const y = row * charH + line;
                for (let i = 0; i < charW; i++) {
                    let on;
                    if (dbl) on = bits >> (7 - (i >> 1)) & 1;
                    else if (i < 8) on = bits >> (7 - i) & 1;
                    else on = copy8th && chr >= 0xC0 && chr <= 0xDF ? bits & 1 : 0;   // 第 9 列
                    put(col * charW + i, y, on ? fg : bg);
                }
            }
        }
        addr += rowSkip;
        lines.push(text.replace(/\s+$/, ""));
    }
    // 光标：扫描线起始寄存器的第 5 位为 1 表示关闭光标
    if (!(vga.cursor_scanline_start & 0x20)) {
        const perRow = Math.max(vga.max_cols, vga.offset_register * 2);
        const pos = vga.cursor_address - vga.start_address;
        const crow = Math.floor(pos / perRow), ccol = pos % perRow;
        if (pos >= 0 && crow < rows && ccol < cols) {
            const attr = mem[(vga.start_address + crow * perRow + ccol) << 1 | 1] | 0;
            const fg = color(attr & fgMask);
            const s = Math.min(vga.cursor_scanline_start & 0x1F, charH - 1);
            const e = Math.min(vga.cursor_scanline_end & 0x1F, charH - 1);
            for (let line = s; line <= e; line++)
                for (let i = 0; i < charW; i++) put(ccol * charW + i, crow * charH + line, fg);
        }
    }
    return { width, height, rgb, mode: `文本 ${cols}x${rows}`, lines };
}

/** 传统 VGA 图形模式（16 色平面模式、mode 13h 等）在 Node 里 v86 不会渲染，画一张“便签”说明 */
function noteImage(message) {
    const width = 640, height = 400;
    const rgb = new Uint8Array(width * height * 3);
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const d = (y * width + x) * 3;
            const edge = x < 6 || y < 6 || x >= width - 6 || y >= height - 6;
            const hatch = (x + y) % 16 < 2;                       // 斜线排线
            const c = edge ? [0x2B, 0x2B, 0x2B] : hatch ? [0xF2, 0xC9, 0x4C] : [0xFF, 0xFD, 0xF7];
            rgb[d] = c[0]; rgb[d + 1] = c[1]; rgb[d + 2] = c[2];
        }
    }
    return { width, height, rgb, mode: "传统 VGA 图形（无法读取）", note: message };
}

/* ---------- 虚拟机 ---------- */
export class V86VM {
    constructor(opts = {}) {
        this.kernel = hostPath(opts.kernel || path.join(ROOT, "out", "bzImage"));
        this.initrd = hostPath(opts.initrd || path.join(ROOT, "out", "initramfs.cpio.gz"));
        this.cmdline = ((opts.cmdline || DEFAULT_CMDLINE) + " " + (opts.append || "")).trim();
        this.mem = opts.mem || 256;
        this.vgaMem = opts.vgaMem || 16;
        this.lib = hostPath(opts.lib || path.join(ROOT, "web", "lib"));
        this.logPath = opts.log ? hostPath(opts.log) : null;
        this.echo = opts.echo !== false;
        this.screenW = opts.screenW || 1024;
        this.screenH = opts.screenH || 768;
        this.audioWav = opts.audioWav ? hostPath(opts.audioWav) : null;
        this._audio = [];                        // 录下的声音：[左声道 Float32Array, 右声道, 采样率] 的列表
        this.emulator = null;
        this._buf = "";
        this._decoder = new TextDecoder("utf-8");
        this._logFd = null;
        this._pendingLog = "";
        this._buttons = [false, false, false];   // 左、中、右
        this._mouse = null;                      // 上一次的鼠标位置
        this._echoCol = 0;                       // 串口回显当前行已经输出了多少字符
        this._lastOutputAt = 0;                  // 最近一次收到串口输出的时间
        this.resets = 0;
    }

    async start() {
        for (const [what, f] of [["内核", this.kernel], ["initramfs", this.initrd],
            ["v86", path.join(this.lib, "libv86.mjs")], ["v86.wasm", path.join(this.lib, "v86.wasm")],
            ["SeaBIOS", path.join(this.lib, "seabios.bin")], ["VGA BIOS", path.join(this.lib, "vgabios.bin")]]) {
            if (!fs.existsSync(f)) {
                const hint = what.startsWith("v86") || what.includes("BIOS")
                    ? "（先在 WSL 里运行 bash scripts/web.sh --lib-only）" : "";
                throw new Error(`找不到${what}: ${f}${hint}`);
            }
        }
        this._V86 = (await import(pathToFileURL(path.join(this.lib, "libv86.mjs")).href)).V86;
        if (this.logPath) {
            fs.mkdirSync(path.dirname(this.logPath), { recursive: true });
            this._logFd = fs.openSync(this.logPath, "w");
        }
        const file = (f) => {
            const b = fs.readFileSync(f);
            return b.buffer.slice(b.byteOffset, b.byteOffset + b.length);
        };
        this._files = {
            bios: file(path.join(this.lib, "seabios.bin")),
            vga_bios: file(path.join(this.lib, "vgabios.bin")),
            bzimage: file(this.kernel),
            initrd: file(this.initrd),
        };
        await this._boot();
        return this;
    }

    /** 创建一台新的模拟 PC 并开机。每次都传入镜像的新副本：v86 会把它们拷进模拟内存，
     *  内核启动后会就地解压、覆盖自己，所以原始数据必须留在外面，冷启动时才能再用。 */
    async _boot() {
        const copy = (b) => ({ buffer: b.slice(0) });
        const emulator = new this._V86({
            wasm_path: path.join(this.lib, "v86.wasm"),
            bios: copy(this._files.bios),
            vga_bios: copy(this._files.vga_bios),
            bzimage: copy(this._files.bzimage),
            initrd: copy(this._files.initrd),
            cmdline: this.cmdline,
            memory_size: this.mem * 1024 * 1024,
            vga_memory_size: this.vgaMem * 1024 * 1024,
            acpi: false,
            autostart: true,
            disable_speaker: true,
            // Node 里没有 window：v86 自己的键盘/鼠标适配器什么也不会做，输入全部由我们发到总线上
        });
        this.emulator = emulator;
        this._buttons = [false, false, false];
        this._mouse = null;
        emulator.add_listener("serial0-output-byte", (b) => this._onSerialByte(b));
        this._attachFakeDac(emulator);
        await new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error("v86 初始化超时（60 秒）")), 60000);
            emulator.add_listener("emulator-started", () => { clearTimeout(timer); resolve(); });
        });
        // 客户机重启（reboot 命令让内核通过键盘控制器复位 CPU）：v86 只在初始化时把 bzImage 放进
        // 内存，复位后 BIOS 会跳回那块早已被覆盖的内存，模拟器随即崩溃。所以这里截住复位，
        // 先停下旧的模拟器，再用原始镜像冷启动一台新的——网页版的“重启”也是这样做的。
        const cpu = emulator.v86 && emulator.v86.cpu;
        if (cpu && typeof cpu.reboot_internal === "function") {
            const orig = cpu.reboot_internal.bind(cpu);
            let fired = false;
            cpu.reboot_internal = () => {
                orig();                      // 先完成复位，CPU 从 BIOS 入口重新执行（不会跑到坏内存）
                if (fired) return;
                fired = true;
                emulator.v86.stop();         // 本轮时间片结束后停下
                this.resets++;
                this._appendSerial("\n[v86_harness] 客户机请求重启：冷启动一台新的模拟器\n");
                this._rebooting = (async () => {
                    try {
                        await Promise.race([emulator.destroy(), sleep(3000)]);
                    } catch (e) {
                        // 旧模拟器已经停了
                    }
                    if (this.emulator === emulator) await this._boot();
                    this._rebooting = null;
                })();
            };
        }
    }

    /** 假的声音输出端。v86 的 SB16 不按自己的时钟播放：浏览器里是 Web Audio 播放器在缓冲快空的时候
     *  发 "dac-request-data"，SB16 才通过 DMA 从客户机内存取下一块采样、用 "dac-send-data" 交出来，
     *  取完一整段再发中断。Node 里没有 Web Audio（disable_speaker），没人要数据，客户机的声卡驱动就会
     *  一直等下去。这里用定时器按真实速度“消耗”采样、不够了就要，和浏览器里的节奏一样；
     *  给了 audioWav（--audio-wav）时把收到的采样录下来，stop() 时写成 WAV 文件。 */
    _attachFakeDac(emulator) {
        const bus = emulator.bus;
        const dac = { rate: 22050, enabled: false, queued: 0, last: Date.now() };
        bus.register("dac-tell-sampling-rate", (r) => { dac.rate = r; });
        bus.register("dac-enable", () => { dac.enabled = true; dac.last = Date.now(); });
        bus.register("dac-disable", () => { dac.enabled = false; });
        bus.register("dac-send-data", (data) => {
            dac.queued += data[0].length;
            if (this.audioWav) this._audio.push([data[0], data[1], dac.rate]);
        });
        if (this._dacTimer) clearInterval(this._dacTimer);
        this._dacTimer = setInterval(() => {
            const now = Date.now();
            dac.queued = Math.max(0, dac.queued - (now - dac.last) * dac.rate / 1000);
            dac.last = now;
            // 保持 0.1 秒的“已缓冲”声音（每次最多要 8 块，防止 DMA 回调是异步的时候一下要太多）
            for (let i = 0; i < 8 && dac.enabled && dac.queued < dac.rate * 0.1 && this.emulator === emulator; i++) {
                const before = dac.queued;
                bus.send("dac-request-data");
                if (dac.queued === before) break;
            }
        }, 10);
    }

    _writeAudio() {
        if (!this.audioWav) return;
        const rate = this._audio.length ? this._audio[this._audio.length - 1][2] : 22050;
        const frames = this._audio.reduce((n, a) => n + a[0].length, 0);
        const buf = Buffer.alloc(44 + frames * 4);
        buf.write("RIFF", 0); buf.writeUInt32LE(36 + frames * 4, 4); buf.write("WAVEfmt ", 8);
        buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(2, 22);
        buf.writeUInt32LE(rate, 24); buf.writeUInt32LE(rate * 4, 28); buf.writeUInt16LE(4, 32);
        buf.writeUInt16LE(16, 34); buf.write("data", 36); buf.writeUInt32LE(frames * 4, 40);
        let off = 44;
        const s16 = (v) => Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
        for (const [l, r] of this._audio) {
            for (let i = 0; i < l.length; i++, off += 4) {
                buf.writeInt16LE(s16(l[i]), off);
                buf.writeInt16LE(s16(r[i]), off + 2);
            }
        }
        fs.writeFileSync(this.audioWav, buf);
    }

    async stop() {
        if (this._rebooting) await this._rebooting;
        if (this._dacTimer) {
            clearInterval(this._dacTimer);
            this._dacTimer = null;
        }
        this._writeAudio();
        if (this.emulator) {
            const emu = this.emulator;
            this.emulator = null;
            try {
                await Promise.race([emu.destroy(), sleep(3000)]);
            } catch (e) {
                // 已经停了也没关系
            }
        }
        this._flushLog();
        if (this._logFd !== null) {
            fs.closeSync(this._logFd);
            this._logFd = null;
        }
    }

    /* ----- 串口 ----- */
    _onSerialByte(b) {
        this._appendSerial(this._decoder.decode(Uint8Array.of(b), { stream: true }));
    }

    /** 打印 harness 自己的信息：串口回显停在半行时先换行，免得和客户机的输出挤在同一行 */
    note(text) {
        if (this.echo && this._echoCol) process.stdout.write("\n");
        this._echoCol = 0;
        console.log(text);
    }

    _appendSerial(text) {
        if (!text) return;
        this._buf += text;
        this._lastOutputAt = Date.now();
        if (this.echo) {
            process.stdout.write(text);
            const nl = text.lastIndexOf("\n");
            this._echoCol = nl < 0 ? this._echoCol + text.length : text.length - nl - 1;
        }
        if (this._logFd !== null) {
            if (!this._pendingLog) setImmediate(() => this._flushLog());
            this._pendingLog += text;
        }
    }

    _flushLog() {
        if (this._logFd !== null && this._pendingLog) {
            fs.writeSync(this._logFd, this._pendingLog);
        }
        this._pendingLog = "";
    }

    serialOutput() {
        return this._buf;
    }

    _ensureRunning() {
        if (!this.emulator) throw new Error("虚拟机没有在运行");
    }

    /** 等待串口输出（从偏移 start 开始）匹配正则；返回 { index, match } */
    async waitSerial(pattern, timeout = 60, start = 0) {
        const rx = pattern instanceof RegExp
            ? new RegExp(pattern.source, pattern.flags.replace("g", "") + "g")
            : new RegExp(pattern, "gm");
        const deadline = Date.now() + timeout * 1000;
        for (;;) {
            rx.lastIndex = start;
            const m = rx.exec(this._buf);
            if (m) return m;
            if (Date.now() > deadline) {
                throw new Error(`等待 ${rx} 超时（${timeout} 秒），串口输出结尾:\n${this._buf.slice(-3000)}`);
            }
            this._ensureRunning();
            await sleep(20);
        }
    }

    /** 等到串口连续 quietMs 毫秒没有新输出（最多等 maxMs） */
    async waitQuiet(quietMs = 200, maxMs = 3000) {
        const deadline = Date.now() + maxMs;
        while (Date.now() < deadline && Date.now() - this._lastOutputAt < quietMs) {
            await sleep(20);
        }
    }

    serialWrite(data) {
        this._ensureRunning();
        const bytes = typeof data === "string" ? new TextEncoder().encode(data) : data;
        this.emulator.serial_send_bytes(0, bytes);
    }

    /** 在串口 shell 里执行命令，返回输出。结束标记写成 __SKDO''NE_…：
     *  回显的命令行里带着引号，不会被误认；shell 执行 echo 时去掉引号，才输出完整标记。 */
    async runSerial(command, timeout = 30) {
        // 先等串口安静下来（shell 已经打印完提示符、正在等输入），再发命令：
        // 如果命令和前面刚发的内容挤在同一次 read() 里，有的行编辑器会把第二行丢掉
        await this.waitQuiet(200, 3000);
        const marker = `__SKDONE_${Date.now()}__`;
        const start = this._buf.length;
        this.serialWrite(`${command}; echo ${marker.slice(0, 6)}''${marker.slice(6)}\n`);
        const m = await this.waitSerial(marker.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), timeout, start);
        let lines = this._buf.slice(start, m.index).replace(/\r/g, "").split("\n");
        if (lines.length && lines[0].includes(command)) lines = lines.slice(1);   // 去掉回显的命令行
        return lines.join("\n").replace(/^\n+|\n+$/g, "");
    }

    /* ----- 屏幕 ----- */
    _vga() {
        this._ensureRunning();
        const vga = this.emulator.v86.cpu.devices.vga;
        if (!vga) throw new Error("模拟器里没有显卡设备");
        return vga;
    }

    /** 当前画面 { width, height, rgb, mode, lines? } */
    grabScreen() {
        const vga = this._vga();
        if (vga.svga_enabled) return grabSvga(this.emulator.v86.cpu, vga);
        if (!vga.graphical_mode) return grabText(vga);
        return noteImage("当前是传统 VGA 图形模式（不是 VBE 线性帧缓冲），Node 里的 v86 不渲染这种模式。");
    }

    async screenshot(file) {
        const shot = this.grabScreen();
        const out = hostPath(file);
        fs.mkdirSync(path.dirname(out), { recursive: true });
        fs.writeFileSync(out, encodePNG(shot.width, shot.height, shot.rgb,
            `SketchOS v86 screenshot: ${shot.mode}` + (shot.note ? ` — ${shot.note}` : "")));
        return { file: out, width: shot.width, height: shot.height, mode: shot.mode, note: shot.note };
    }

    textScreen() {
        const vga = this._vga();
        if (vga.graphical_mode || vga.svga_enabled) return null;
        return grabText(vga).lines;
    }

    /** 等待 VGA 文本屏幕上出现匹配的文字（只适用于文本模式，例如 BIOS 阶段或 nomodeset 启动） */
    async waitText(pattern, timeout = 30) {
        const rx = pattern instanceof RegExp ? pattern : new RegExp(pattern, "m");
        const deadline = Date.now() + timeout * 1000;
        for (;;) {
            const lines = this.textScreen();
            if (lines && rx.test(lines.join("\n"))) return true;
            if (Date.now() > deadline) {
                throw new Error(`等待屏幕文字 ${rx} 超时（${timeout} 秒），当前屏幕:\n` +
                    (lines ? lines.join("\n") : "（图形模式）"));
            }
            await sleep(100);
        }
    }

    screenSize() {
        const vga = this._vga();
        if (vga.svga_enabled && vga.svga_width && vga.svga_height) return [vga.svga_width, vga.svga_height];
        return [this.screenW, this.screenH];
    }

    /* ----- 鼠标 ----- */
    /** 屏幕坐标 → VMware 绝对坐标 0..65535。向上取整，保证客户机按 raw*(w-1)/65535
     *  或 raw*w/65536 向下取整换算回像素时，都正好落在 x 这一列上。 */
    _abs(x, y) {
        const [w, h] = this.screenSize();
        const cx = Math.max(0, Math.min(w - 1, Math.round(x)));
        const cy = Math.max(0, Math.min(h - 1, Math.round(y)));
        return {
            px: cx, py: cy,
            ax: Math.min(0xFFFF, Math.ceil(cx * 0xFFFF / Math.max(1, w - 1))),
            ay: Math.min(0xFFFF, Math.ceil(cy * 0xFFFF / Math.max(1, h - 1))),
        };
    }

    async mouseMove(x, y) {
        this._ensureRunning();
        const bus = this.emulator.bus;
        const p = this._abs(x, y);
        const last = this._mouse || { px: p.px, py: p.py };
        let dx = p.px - last.px, dy = p.py - last.py;
        if (!this._mouse) dx = 1;    // 第一次移动：至少产生一个 PS/2 包，让客户机来取绝对坐标
        // 绝对坐标（vmware.js 会按 data[0]/data[2]*0xFFFF 换算，这里直接给好 0..65535）
        bus.send("mouse-absolute", [p.ax, p.ay, 0xFFFF, 0xFFFF]);
        // PS/2 相对包：每包的位移只有 9 位有符号数，大幅移动拆成几包；y 轴向上为正
        while (dx !== 0 || dy !== 0) {
            const sx = Math.max(-100, Math.min(100, dx)), sy = Math.max(-100, Math.min(100, dy));
            bus.send("mouse-delta", [sx, -sy]);
            dx -= sx;
            dy -= sy;
        }
        this._mouse = p;
        await sleep(30);
    }

    async mouseButton(button = "left", down = true) {
        this._ensureRunning();
        const idx = { left: 0, middle: 1, right: 2 }[button];
        if (idx === undefined) throw new Error("未知鼠标按键: " + button);
        this._buttons[idx] = down;
        this.emulator.bus.send("mouse-click", this._buttons.slice());
        await sleep(30);
    }

    async click(x, y, button = "left") {
        await this.mouseMove(x, y);
        await sleep(50);
        await this.mouseButton(button, true);
        await sleep(50);
        await this.mouseButton(button, false);
        await sleep(50);
    }

    async doubleClick(x, y) {
        await this.mouseMove(x, y);
        for (let i = 0; i < 2; i++) {
            await this.mouseButton("left", true);
            await sleep(30);
            await this.mouseButton("left", false);
            await sleep(60);
        }
    }

    async drag(x1, y1, x2, y2, steps = 12) {
        await this.mouseMove(x1, y1);
        await this.mouseButton("left", true);
        for (let i = 1; i <= steps; i++) {
            await this.mouseMove(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
            await sleep(20);
        }
        await this.mouseButton("left", false);
    }

    async wheel(x, y, clicks) {
        await this.mouseMove(x, y);
        for (let i = 0; i < Math.abs(clicks | 0); i++) {
            // 和浏览器一致：+1 = 向上滚
            this.emulator.bus.send("mouse-wheel", [clicks > 0 ? 1 : -1, 0]);
            await sleep(40);
        }
    }

    /* ----- 键盘 ----- */
    _sendCode(code, down) {
        const bus = this.emulator.bus;
        if (code > 0xFF) bus.send("keyboard-code", code >> 8);          // 0xE0 前缀
        bus.send("keyboard-code", (code & 0xFF) | (down ? 0 : 0x80));   // 松开 = 按下码 | 0x80
    }

    /** combo 例如 'ctrl-alt-t'、'ret'、'shift-a'：依次按下，保持 hold 秒，再倒序松开 */
    async key(combo, hold = 0.05) {
        this._ensureRunning();
        const codes = parseCombo(combo);
        for (const c of codes) this._sendCode(c, true);
        await sleep(hold * 1000);
        for (const c of codes.slice().reverse()) this._sendCode(c, false);
        await sleep(40);
    }

    async typeText(text, delay = 0.02) {
        this._ensureRunning();
        for (const ch of text) {
            const [name, shift] = charToKey(ch);
            const code = keyCode(name);
            if (shift) this._sendCode(SCANCODES.shift, true);
            this._sendCode(code, true);
            await sleep(15);
            this._sendCode(code, false);
            if (shift) this._sendCode(SCANCODES.shift, false);
            await sleep(delay * 1000);
        }
    }
}

/* ---------- 脚本 ---------- */
function unescapeText(s) {
    return new TextDecoder().decode(unescapeToBytes(s));
}

export async function runScript(vm, lines) {
    let markOffset = 0;    // wait 从这个位置开始找（mark 命令更新它）
    for (const raw of lines) {
        const line = raw.trim();
        if (!line || line.startsWith("#")) continue;
        const sp = line.indexOf(" ");
        const cmd = sp < 0 ? line : line.slice(0, sp);
        const rest = sp < 0 ? "" : line.slice(sp + 1);
        const a = rest.split(/\s+/).filter(Boolean);
        const num = (i) => {
            const v = Number(a[i]);
            if (!Number.isFinite(v)) throw new Error(`${cmd}: 第 ${i + 1} 个参数不是数字: ${a[i]}`);
            return v;
        };
        vm.note(">> " + line);
        // “正则 [超时秒]”：最后一个数字是超时
        const patternAndTimeout = (def) => {
            const m = /^(.*)\s+(\d+(?:\.\d+)?)$/.exec(rest);
            return m ? [m[1], Number(m[2])] : [rest, def];
        };
        switch (cmd) {
            case "wait": {
                const [pat, timeout] = patternAndTimeout(60);
                await vm.waitSerial(pat, timeout, markOffset);
                break;
            }
            case "mark": markOffset = vm.serialOutput().length; break;
            case "wait_text": {
                const [pat, timeout] = patternAndTimeout(30);
                await vm.waitText(pat, timeout);
                break;
            }
            case "sleep": await sleep(num(0) * 1000); break;
            case "serial": vm.serialWrite(unescapeToBytes(rest)); break;
            case "run": {
                const out = await vm.runSerial(rest);
                vm.note(out.split("\n").map((l) => "   | " + l).join("\n"));
                break;
            }
            case "shot": {
                const r = await vm.screenshot(rest);
                vm.note(`   截图: ${r.file} (${r.width}x${r.height}，${r.mode})` + (r.note ? "，" + r.note : ""));
                break;
            }
            case "text": {
                const t = vm.textScreen();
                vm.note(t ? t.map((l) => "   | " + l).join("\n") : "   （当前是图形模式，没有文本屏幕）");
                break;
            }
            case "move": await vm.mouseMove(num(0), num(1)); break;
            case "click": await vm.click(num(0), num(1), a[2] || "left"); break;
            case "dclick": await vm.doubleClick(num(0), num(1)); break;
            case "drag": await vm.drag(num(0), num(1), num(2), num(3)); break;
            case "wheel": await vm.wheel(num(0), num(1), num(2)); break;
            case "key": await vm.key(a[0]); break;
            case "type": await vm.typeText(unescapeText(rest)); break;
            case "expect_shot_differs": {
                const [x, y] = [fs.readFileSync(hostPath(a[0])), fs.readFileSync(hostPath(a[1]))];
                if (x.equals(y)) throw new Error(`截图 ${a[0]} 与 ${a[1]} 完全相同`);
                break;
            }
            default: throw new Error("未知脚本命令: " + cmd);
        }
    }
}

/* ---------- 命令行 ---------- */
function parseArgs(argv) {
    const opts = { echo: true, timeout: 0 };
    const need = (i) => {
        if (i + 1 >= argv.length) throw new Error(`${argv[i]} 需要一个参数`);
        return argv[i + 1];
    };
    for (let i = 0; i < argv.length; i++) {
        const k = argv[i];
        switch (k) {
            case "--kernel": opts.kernel = need(i); i++; break;
            case "--initrd": opts.initrd = need(i); i++; break;
            case "--append": opts.append = need(i); i++; break;
            case "--cmdline": opts.cmdline = need(i); i++; break;
            case "--mem": opts.mem = Number(need(i)); i++; break;
            case "--vga-mem": opts.vgaMem = Number(need(i)); i++; break;
            case "--lib": opts.lib = need(i); i++; break;
            case "--script": opts.script = need(i); i++; break;
            case "--log": opts.log = need(i); i++; break;
            case "--audio-wav": opts.audioWav = need(i); i++; break;
            case "--timeout": opts.timeout = Number(need(i)); i++; break;
            case "--quiet": opts.echo = false; break;
            case "--screen": {
                const m = /^(\d+)x(\d+)$/.exec(need(i));
                if (!m) throw new Error("--screen 的格式是 宽x高");
                opts.screenW = Number(m[1]);
                opts.screenH = Number(m[2]);
                i++;
                break;
            }
            case "-h": case "--help": opts.help = true; break;
            default: throw new Error("未知参数: " + k);
        }
    }
    return opts;
}

function printUsage() {
    const src = fs.readFileSync(fileURLToPath(import.meta.url), "utf8");
    const doc = /\/\*\*([\s\S]*?)\*\//.exec(src)[1].replace(/^ \* ?/gm, "");
    console.log(doc.trim());
}

/** 交互模式：终端里的按键原样发到串口，Ctrl+] 退出（和 telnet 一样） */
async function interactive(vm, timeout) {
    return new Promise((resolve) => {
        const stdin = process.stdin;
        const done = () => {
            if (stdin.isTTY) stdin.setRawMode(false);
            stdin.pause();
            resolve();
        };
        if (stdin.isTTY) {
            stdin.setRawMode(true);
            process.stderr.write("[v86_harness] 交互模式：输入直接发到串口 ttyS0，按 Ctrl+] 退出\n");
        }
        stdin.on("data", (d) => {
            if (d.includes(0x1D)) return done();
            vm.serialWrite(new Uint8Array(d));
        });
        stdin.on("end", () => { if (!timeout) setTimeout(done, 2000); });
        if (timeout) setTimeout(done, timeout * 1000);
    });
}

async function main() {
    let opts;
    try {
        opts = parseArgs(process.argv.slice(2));
    } catch (e) {
        console.error(e.message);
        process.exit(2);
    }
    if (opts.help) {
        printUsage();
        return 0;
    }
    const vm = new V86VM(opts);
    let code = 0;
    try {
        await vm.start();
        if (opts.script) {
            const text = fs.readFileSync(hostPath(opts.script), "utf8");
            await runScript(vm, text.split(/\r?\n/));
            if (opts.timeout) await sleep(opts.timeout * 1000);
            console.log("\n测试脚本执行完毕");
        } else {
            await interactive(vm, opts.timeout);
        }
    } catch (e) {
        console.error("\n测试失败: " + (e && e.message ? e.message : e));
        code = 1;
    } finally {
        await vm.stop();
    }
    return code;
}

/** 退出进程。v86 的 JIT 会在运行时不断生成新的 WebAssembly 模块，V8 在后台线程里编译它们；
 *  如果模拟器刚停就立刻 process.exit()，后台编译完成时会往已经关闭的 libuv 句柄发消息，
 *  Windows 上的 Node 会因此断言失败（"Assertion failed: !(handle->flags & UV_HANDLE_CLOSING)"）。
 *  所以先等后台任务收尾，让事件循环自然结束；真有东西卡住，5 秒后再强制退出。 */
async function finish(code) {
    process.exitCode = code;
    await sleep(500);
    setTimeout(() => process.exit(code), 5000).unref();
}

const invokedDirectly = process.argv[1] &&
    path.resolve(process.argv[1]).toLowerCase() === fileURLToPath(import.meta.url).toLowerCase();
if (invokedDirectly) {
    main().then(finish, (e) => {
        console.error(e);
        return finish(1);
    });
}
