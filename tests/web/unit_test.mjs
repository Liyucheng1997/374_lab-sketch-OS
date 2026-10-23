/**
 * tests/web/unit_test.mjs — 网页版 JavaScript 的单元测试（不需要浏览器，用 Node 运行）：
 *     node tests/web/unit_test.mjs
 *
 *   - web/js/terminal.js：串口终端对 \r \b、ANSI 转义序列、颜色、UTF-8、超长日志的处理
 *   - web/js/rough.js：同一个种子画出的线条必须完全一样（界面重绘不闪烁）
 *   - tools/v86_harness.mjs：按键表、转义、PNG 编码，以及鼠标坐标换算——
 *     对每个像素检查：客户机不管用“四舍五入”还是“向下取整”把 0..65535 换算回屏幕坐标，都正好落回原处
 */
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";
import { V86VM, unescapeToBytes, parseCombo, charToKey, keyCode, encodePNG, hostPath } from "../../tools/v86_harness.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
let pass = 0, fail = 0;
function check(name, ok, detail) {
    if (ok) pass++;
    else fail++;
    console.log(`  ${ok ? "通过" : "失败"} ${name}${!ok && detail !== undefined ? "  → " + JSON.stringify(detail) : ""}`);
}

/** 在一个假的“浏览器全局环境”里运行网页脚本（只提供这些脚本在非 DOM 路径上用到的东西） */
function loadBrowserScript(file, extra) {
    const window = { addEventListener() {} };
    const ctx = vm.createContext(Object.assign({ window, requestAnimationFrame: () => 0, console }, extra || {}));
    vm.runInContext(fs.readFileSync(path.join(ROOT, file), "utf8"), ctx, { filename: file });
    return window;
}

/* ---------- terminal.js ---------- */
console.log("terminal.js");
{
    const { SerialTerminal } = loadBrowserScript("web/js/terminal.js");
    const run = (input) => {
        const t = new SerialTerminal({});
        t.write(input);
        return t;
    };
    check("换行与回车", run("hello\r\nworld").text() === "hello\nworld");
    check("退格后覆盖", run("abc\b\bX").text() === "aXc");
    check("ESC[K 清除到行尾（shell 重画当前行）", run("prompt# ls -l\r\x1b[Kprompt# cd").text() === "prompt# cd");
    check("ESC[nD / ESC[nC 光标左右移动", run("abcdef\x1b[3Dxy\x1b[1Cz").text() === "abcxyfz");
    check("ESC[nG 光标移到第 n 列", run("hello\x1b[1Gj").text() === "jello");
    check("OSC 标题序列被忽略", run("\x1b]0;window title\x07ok").text() === "ok");
    check("中文按字符计列", run("你好世界\b\b朋友").text() === "你好朋友");
    {
        const t = run("\x1b[1;32mgreen\x1b[0m plain \x1b[38;5;196mred256\x1b[39m");
        const line = t.lines[0];
        check("SGR 颜色与粗体", line[0].st.fg === 2 && line[0].st.bold && line[6].st.fg === -1 &&
            line[12].st.rgbFg === "rgb(255,0,0)", line.map((c) => c.st.fg).join(","));
    }
    {
        const t = new SerialTerminal({});
        for (let i = 0; i < 3500; i++) t.write(`line ${i}\r\n`);
        const lines = t.text().split("\n");
        check("超长日志只保留最后 3000 行", t.lines.length <= 3000 && lines[lines.length - 2] === "line 3499",
            [t.lines.length, lines[lines.length - 2]]);
    }
    check("ESC[2J 清屏后从新的一屏开始", run("old\x1b[2J\x1b[Hnew").text().trim().endsWith("new"));
    check("不完整的转义序列不会吞掉后面的字", run("a\x1b[").text() === "a" && run("a\x1b[31").text() === "a");
}

/* ---------- rough.js ---------- */
console.log("rough.js");
{
    const { Sketch } = loadBrowserScript("web/js/rough.js");
    const a = new Sketch.Pen("button-1").rect(1, 1, 100, 40);
    const b = new Sketch.Pen("button-1").rect(1, 1, 100, 40);
    const c = new Sketch.Pen("button-2").rect(1, 1, 100, 40);
    check("同一个种子画出完全相同的线条", a === b);
    check("不同种子画出不同的线条", a !== c);
    const hatch = new Sketch.Pen(7).hachureRect(0, 0, 200, 100, 8);
    const strokes = (hatch.match(/M/g) || []).length;
    check("排线条数合理（200x100、间距 8）", strokes > 15 && strokes < 60, strokes);
    check("椭圆路径非空", new Sketch.Pen(3).ellipse(50, 50, 80, 40).length > 50);
    check("路径里没有 NaN", !/NaN/.test(a + hatch + new Sketch.Pen(1).squiggle(0, 100, 10)));
    check("哈希稳定", Sketch.hash("SketchOS") === Sketch.hash("SketchOS") && Sketch.hash("a") !== Sketch.hash("b"));
}

/* ---------- v86_harness.mjs ---------- */
console.log("v86_harness.mjs");
{
    const bytes = Array.from(unescapeToBytes("a\\nb\\x03\\e中"));
    check("转义：\\n \\xNN \\e 和中文 UTF-8", JSON.stringify(bytes) === JSON.stringify([97, 10, 98, 3, 27, 0xE4, 0xB8, 0xAD]), bytes);
    check("组合键 ctrl-alt-t", JSON.stringify(parseCombo("ctrl-alt-t")) === JSON.stringify([0x1D, 0x38, 0x14]));
    check("组合键 ctrl-- 是 ctrl + minus", JSON.stringify(parseCombo("ctrl--")) === JSON.stringify([0x1D, 0x0C]));
    check("扩展键带 0xE0 前缀", keyCode("up") === 0xE048 && keyCode("delete") === 0xE053);
    check("别名 enter / esc", keyCode("enter") === 0x1C && keyCode("escape") === 0x01);
    check("字符 → 按键", JSON.stringify(charToKey("A")) === JSON.stringify(["a", true]) &&
        JSON.stringify(charToKey("?")) === JSON.stringify(["slash", true]));
    let threw = false;
    try { charToKey("中"); } catch (e) { threw = true; }
    check("键盘打不出的字符会报错", threw);

    // PNG：编码后用 zlib 解回来，像素必须一致
    const w = 5, h = 3, rgb = new Uint8Array(w * h * 3).map((_, i) => (i * 37) & 255);
    const png = encodePNG(w, h, rgb, "注释");
    const idat = [];
    for (let p = 8; p < png.length;) {
        const len = png.readUInt32BE(p), type = png.toString("latin1", p + 4, p + 8);
        if (type === "IDAT") idat.push(png.subarray(p + 8, p + 8 + len));
        p += 12 + len;
    }
    const raw = zlib.inflateSync(Buffer.concat(idat));
    let same = raw.length === (w * 3 + 1) * h;
    for (let y = 0; same && y < h; y++) {
        same = raw[y * (w * 3 + 1)] === 0 &&
            Buffer.compare(raw.subarray(y * (w * 3 + 1) + 1, (y + 1) * (w * 3 + 1)), Buffer.from(rgb.subarray(y * w * 3, (y + 1) * w * 3))) === 0;
    }
    check("PNG 编码可以无损解回", png.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])) && same);

    // 鼠标坐标：对常见分辨率的每一个像素，检查两种换算方式都落回原坐标
    const vmx = new V86VM({});
    const bad = [];
    for (const [W, H] of [[640, 480], [800, 600], [1024, 768], [1280, 800], [1920, 1080]]) {
        vmx.screenSize = () => [W, H];
        for (let x = 0; x < Math.max(W, H); x++) {
            const p = vmx._abs(Math.min(x, W - 1), Math.min(x, H - 1));
            for (const [v, size, want] of [[p.ax, W, Math.min(x, W - 1)], [p.ay, H, Math.min(x, H - 1)]]) {
                const rounded = Math.floor((v * (size - 1) + 0x7FFF) / 0xFFFF);   // sketchd 的写法
                const floored = Math.floor(v * (size - 1) / 0xFFFF);             // 向下取整的写法
                const linux = Math.floor(v * size / 0x10000);                    // 按 65536 等分的写法
                if (v < 0 || v > 0xFFFF || rounded !== want || floored !== want || linux !== want) {
                    bad.push({ W, H, want, v, rounded, floored, linux });
                }
            }
        }
    }
    check("鼠标绝对坐标对每个像素都精确（5 种分辨率）", bad.length === 0, bad.slice(0, 3));

    if (process.platform === "win32") {
        check("WSL 路径换算 /mnt/f/...", hostPath("/mnt/f/a/b.txt") === "F:\\a\\b.txt");
        check("WSL 路径换算 /root/...", /^\\\\wsl\.localhost\\[^\\]+\\root\\x$/.test(hostPath("/root/x")));
    }
}

console.log(`\n单元测试：${pass} 通过，${fail} 失败`);
process.exit(fail ? 1 : 0);
