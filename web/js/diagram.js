/*
 * diagram.js — 页面下方的“系统分层图”，用 rough.js 按容器宽度现场画出来（手机上会自动换行）。
 *
 * 从上到下：应用 → 我们写的系统程序 → Linux 内核 → v86 模拟出来的硬件 → 浏览器。
 * 每两层之间的箭头上写着它们“怎么说话”：套接字和共享内存、系统调用、I/O 端口和中断……
 * 这正是学操作系统时最该看清楚的东西：每一层只和相邻的一层打交道。
 */
(function (global) {
    "use strict";

    const LAYERS = [
        {
            id: "apps", color: "var(--yellow)", title: "图形应用", sub: "每个应用是一个独立进程",
            chips: ["skterm 终端", "skfiles 文件", "sknote 记事本", "skpaint 画板", "skmon 监视器", "skclock 时钟", "skabout 关于"],
        },
        {
            id: "user", color: "var(--blue)", title: "系统程序（我们自己写的）", sub: "用户态 · 静态链接 musl libc",
            chips: ["sinit 1 号进程", "sksh 命令行", "skbox 工具箱", "sketchd 显示服务器 + 窗口管理器", "libsketch 手绘图形库"],
        },
        {
            id: "kernel", color: "var(--green)", title: "Linux 6.12 内核（原版，未修改）", sub: "内核态 · 只是自己裁剪了配置",
            chips: ["进程调度", "虚拟内存", "tmpfs / proc / sysfs", "bochs-drm 显卡驱动", "evdev 输入", "TTY 与 8250 串口"],
        },
        {
            id: "v86", color: "var(--orange)", title: "v86 模拟器", sub: "WebAssembly · 把 x86 指令即时编译执行",
            chips: ["x86 CPU + JIT", "256 MB 内存", "Bochs VBE 显卡", "PS/2 键盘鼠标", "16550 串口", "VMware 鼠标后门"],
        },
        {
            id: "browser", color: "var(--purple)", title: "浏览器（就是这个页面）", sub: "",
            chips: ["<canvas> 显示画面", "键盘 / 鼠标 / 触摸事件", "串口控制台面板"],
        },
    ];
    const LINKS = [
        "Unix 域套接字 + 共享内存（memfd）",
        "系统调用（open / read / mmap / ioctl …）",
        "I/O 端口 · 中断 · 显存",
        "画面帧 · 输入事件 · 串口字节",
    ];

    const FONT_FAMILY = '"LXGW WenKai", "LXGW WenKai Screen", "KaiTi", "STKaiti", serif';
    let measureCtx = null;
    function textWidth(text, px, bold) {
        if (!measureCtx) measureCtx = document.createElement("canvas").getContext("2d");
        measureCtx.font = `${bold ? "bold " : ""}${px}px ${FONT_FAMILY}`;
        return measureCtx.measureText(text).width;
    }

    function text(svg, x, y, str, cls) {
        const t = Sketch.el("text", { x: x, y: y, class: cls }, svg);
        t.textContent = str;
        return t;
    }

    function render(host) {
        if (!host.clientWidth) return;            // 看不见（比如后台标签页）时先不画，等有了宽度再说
        const W = Math.max(280, Math.floor(host.clientWidth));
        if (host._width === W) return;
        host._width = W;
        const narrow = W < 520;
        const pad = narrow ? 10 : 16;
        const chipH = narrow ? 26 : 28, chipGap = 7, chipPx = narrow ? 13 : 14;
        const gapH = narrow ? 52 : 46;
        const svg = Sketch.el("svg", { class: "arch-svg", role: "img", "aria-labelledby": "arch-title" });
        Sketch.el("title", { id: "arch-title" }, svg).textContent =
            "SketchOS 分层结构：应用、系统程序、Linux 内核、v86 模拟器、浏览器";

        let y = 4;
        LAYERS.forEach((layer, li) => {
            const pen = new Sketch.Pen("arch-" + layer.id);
            const x0 = 2, w = W - 4;
            // 先排版：标题一行，下面的“标签”按宽度自动换行
            const titleY = y + pad + 16;
            const subOnOwnLine = narrow || textWidth(layer.title, 17, true) + textWidth(layer.sub, 13) + 40 > w - pad * 2;
            let cy = titleY + (layer.sub && subOnOwnLine ? 22 : 0) + 14;
            let cx = x0 + pad;
            const chips = [];
            layer.chips.forEach((c) => {
                const cw = Math.min(w - pad * 2, textWidth(c, chipPx) + 20);
                if (cx + cw > x0 + w - pad && cx > x0 + pad) {
                    cx = x0 + pad;
                    cy += chipH + chipGap;
                }
                chips.push({ x: cx, y: cy, w: cw, label: c });
                cx += cw + chipGap;
            });
            const h = cy + chipH + pad - y;

            const g = Sketch.el("g", { class: "layer layer-" + layer.id }, svg);
            Sketch.path(g, pen.hachureRect(x0 + 3, y + 3, w - 6, h - 6, 7, -38), "hatch", { style: `stroke:${layer.color}` });
            Sketch.path(g, pen.rect(x0, y, w, h), "ink");
            text(g, x0 + pad, titleY, layer.title, "arch-title");
            if (layer.sub) {
                if (subOnOwnLine) text(g, x0 + pad, titleY + 21, layer.sub, "arch-sub");
                else text(g, x0 + pad + textWidth(layer.title, 17, true) + 14, titleY, layer.sub, "arch-sub");
            }
            chips.forEach((c, i) => {
                const cp = new Sketch.Pen(`chip-${layer.id}-${i}`, { roughness: 0.7, jitter: 1.4 });
                Sketch.el("rect", { x: c.x, y: c.y, width: c.w, height: chipH, rx: 3, class: "chip-bg" }, g);
                Sketch.path(g, cp.rect(c.x, c.y, c.w, chipH), "ink chip-ink");
                text(g, c.x + 10, c.y + chipH / 2 + chipPx * 0.36, c.label, "arch-chip");
            });
            y += h;
            if (li < LINKS.length) {
                // 两层之间的双向箭头 + 说明文字
                const ax = x0 + (narrow ? 26 : 44);
                const ap = new Sketch.Pen("arrow-" + li, { roughness: 0.9 });
                Sketch.path(svg, ap.arrow(ax, y + 6, ax, y + gapH - 6, 8) + ap.arrow(ax, y + gapH - 6, ax, y + 6, 8), "ink arrow");
                text(svg, ax + 16, y + gapH / 2 + 5, LINKS[li], "arch-link");
                y += gapH;
            }
        });
        svg.setAttribute("width", W);
        svg.setAttribute("height", Math.ceil(y + 4));
        svg.setAttribute("viewBox", `0 0 ${W} ${Math.ceil(y + 4)}`);
        host.replaceChildren(svg);
        // 图的高度变了，外面那张纸片的手绘边框也要跟着重画
        if (global.Sketch && Sketch.refreshAll) Sketch.refreshAll();
    }

    function mount(host) {
        if (!host) return;
        render(host);
        if ("ResizeObserver" in global) {
            let t = 0;
            new ResizeObserver(() => {
                clearTimeout(t);
                t = setTimeout(() => render(host), 80);
            }).observe(host);
        }
        // 字体（比如系统装了霞鹜文楷）可能稍后才生效，生效后按新的字宽再排一次
        if (document.fonts && document.fonts.ready) {
            document.fonts.ready.then(() => {
                host._width = 0;
                render(host);
            });
        }
    }

    global.SketchDiagram = { mount };
})(window);
