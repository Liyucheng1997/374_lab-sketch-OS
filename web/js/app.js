/*
 * app.js — SketchOS 网页版主程序：检查文件 → 下载 → 启动 v86 → 处理屏幕、键盘、鼠标、串口
 *
 * v86 的使用方式：new V86({...}) 创建一台虚拟 PC，它自己下载 BIOS 和镜像，
 * 然后像 QEMU 的 -kernel 参数那样把 bzImage 和 initramfs 直接放进内存启动 Linux。
 * 这里额外做的事：
 *   - 启动前检查文件在不在，缺了就告诉用户怎么构建，而不是让模拟器默默卡住；
 *   - 用 download-progress 事件画手绘进度条，用串口里的“日志里程碑”显示开机进度；
 *   - 键盘只在点了屏幕之后才交给虚拟机（否则网页上的输入框、快捷键都没法用）；
 *   - 客户机重启时“冷启动”一台新的模拟器（原因见 hookGuestReboot）。
 *
 * 网址参数：?autoboot=0 不自动开机；?mem=128 内存大小(MB)；?append=sketch.nogui 追加内核参数
 */
(function () {
    "use strict";

    /* ---------- 配置 ---------- */
    const CMDLINE = "console=tty1 console=ttyS0 tsc=reliable mitigations=off random.trust_cpu=on loglevel=4 sketch.mode=1024x768";
    const URLS = {
        lib: "lib/libv86.js",
        wasm: "lib/v86.wasm",
        bios: "lib/seabios.bin",
        vgaBios: "lib/vgabios.bin",
        bzimage: "images/bzImage",
        initrd: "images/initramfs.cpio.gz",
        manifest: "images/manifest.json",
    };
    // 下载顺序就是 v86 加载的顺序：先 wasm，再 BIOS、内核、initramfs
    const DOWNLOADS = [
        { url: URLS.wasm, label: "v86 模拟器核心（WebAssembly）" },
        { url: URLS.bios, label: "SeaBIOS（主板 BIOS）" },
        { url: URLS.vgaBios, label: "VGA BIOS（显卡 BIOS）" },
        { url: URLS.bzimage, label: "Linux 内核 bzImage" },
        { url: URLS.initrd, label: "根文件系统 initramfs" },
    ];
    const BUILD_HINT = "bash scripts/web.sh";

    const params = new URLSearchParams(location.search);
    const OPTS = {
        autoboot: params.get("autoboot") !== "0",
        memoryMB: clampInt(params.get("mem"), 64, 1024, 256),
        // 只允许常见的内核参数字符，防止网址里塞进奇怪的东西
        append: (params.get("append") || "").replace(/[^\w.=,:/+\- ]/g, "").slice(0, 200).trim(),
    };

    /* ---------- 页面元素 ---------- */
    const $ = (id) => document.getElementById(id);
    const ui = {
        power: $("btn-power"), fullscreen: $("btn-fullscreen"), shot: $("btn-shot"),
        serialBtn: $("btn-serial"), scale: $("btn-scale"), osk: $("btn-osk"), exitFs: $("btn-exit-fs"),
        lamp: $("lamp"), statusText: $("status-text"), steps: $("boot-steps"),
        progress: $("progress"), progressSvg: $("progress-svg"), progressLabel: $("progress-label"),
        notebook: $("notebook"), rings: $("rings"), stage: $("stage"), viewport: $("viewport"),
        frame: $("screen-frame"), sizer: $("sizer"), screen: $("screen_container"),
        overlay: $("overlay"), overlayBody: $("overlay-body"), flash: $("flash"),
        kbdState: $("kbd-state"), kbdText: $("kbd-text"), kbdIcon: $("kbd-icon"),
        phoneKbd: $("phone-keyboard"), caption: $("caption"),
        serialPanel: $("serial-panel"), serialLog: $("serial-log"), serialForm: $("serial-form"),
        serialInput: $("serial-input"), serialCtrlC: $("serial-ctrlc"), serialClear: $("serial-clear"),
        buildInfo: $("build-info"), logo: $("logo"), diagram: $("arch-diagram"),
    };

    /* ---------- 状态 ---------- */
    const S = {
        phase: "idle",        // idle | checking | loading | booting | running | off | error | restarting
        emu: null,
        gen: 0,               // 第几次开机；旧模拟器迟到的事件按这个过滤掉
        manifest: null,
        sizes: {},            // url → 字节数
        dl: {},               // url → { loaded, total, done }
        lastProgressAt: 0,
        startedAt: 0,
        desktopReady: false,
        servicesAt: 0,
        mode: null,           // { graphical, w, h }
        mouseMode: "",
        captured: false,
        scaleMode: "fit",
        serialOpen: false,
        lineBuf: "",
        instr: { last: 0, at: 0, mips: 0 },
        confirmUntil: 0,
        rebootHooked: false,
        libPromise: null,
    };
    const term = new SerialTerminal(ui.serialLog);
    const decoder = { gen: -1, d: null };
    const history = { items: [], pos: 0 };

    /* ---------- 小工具 ---------- */
    function clampInt(v, lo, hi, def) {
        const n = parseInt(v, 10);
        return Number.isFinite(n) ? Math.max(lo, Math.min(hi, n)) : def;
    }
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

    function fmtBytes(n) {
        if (!Number.isFinite(n) || n <= 0) return "?";
        if (n < 1024) return n + " B";
        if (n < 1024 * 1024) return (n / 1024).toFixed(0) + " KB";
        return (n / 1024 / 1024).toFixed(1) + " MB";
    }

    function fmtTime(sec) {
        const s = Math.floor(sec), m = Math.floor(s / 60), h = Math.floor(m / 60);
        const pad = (x) => String(x).padStart(2, "0");
        return h ? `${h}:${pad(m % 60)}:${pad(s % 60)}` : `${m}:${pad(s % 60)}`;
    }

    /** 小型 DOM 构造器：h("p", {class: "x"}, "文字", 子元素...)；文字一律当纯文本，不解析 HTML */
    function h(tag, attrs, ...kids) {
        const e = document.createElement(tag);
        for (const k in attrs || {}) {
            if (k === "class") e.className = attrs[k];
            else if (k.startsWith("data-") || k === "type" || k === "id" || k === "title") e.setAttribute(k, attrs[k]);
            else e[k] = attrs[k];
        }
        for (const kid of kids.flat()) {
            if (kid === null || kid === undefined || kid === false) continue;
            e.appendChild(typeof kid === "string" ? document.createTextNode(kid) : kid);
        }
        return e;
    }

    function store(key, value) {
        try {
            if (value === undefined) return localStorage.getItem("sketchos." + key);
            localStorage.setItem("sketchos." + key, value);
        } catch (e) {
            // 隐私模式等情况下 localStorage 不能用：只是记不住偏好，不影响使用
        }
        return null;
    }

    function isFullscreen() {
        return (document.fullscreenElement || document.webkitFullscreenElement) === ui.stage;
    }

    /* ---------- 手绘装饰（一次性） ---------- */
    function drawLogo() {
        // 一支斜放的铅笔，笔尖拖出一道蓝色的线
        const svg = Sketch.el("svg", { viewBox: "0 0 72 72", width: 72, height: 72 });
        const p = new Sketch.Pen("logo", { roughness: 0.9, jitter: 1.5 });
        const body = [[14, 46], [46, 14], [56, 24], [24, 56]];
        Sketch.path(svg, p.hachure(body, 4.2, 45), "hatch-yellow");
        Sketch.path(svg, p.hachure([[46, 14], [52, 8], [62, 18], [56, 24]], 3, 45), "hatch-pink");
        Sketch.path(svg, p.hachure([[14, 46], [24, 56], [7, 63]], 3, 0), "hatch-wood");
        Sketch.path(svg, p.polyline(body, true) + p.polyline([[46, 14], [52, 8], [62, 18], [56, 24]]) +
            p.polyline([[14, 46], [7, 63], [24, 56]]) + p.line(9.5, 57, 12.5, 60.5) + p.line(19, 51, 51, 19), "ink");
        Sketch.path(svg, p.curve([[7, 66], [20, 69], [34, 64], [48, 68], [64, 63]]), "squiggle");
        ui.logo.appendChild(svg);
    }

    function drawRings() {
        const w = ui.rings.clientWidth, hgt = 44;
        if (!w || ui.rings._w === w) return;
        ui.rings._w = w;
        const svg = Sketch.el("svg", { width: w, height: hgt, viewBox: `0 0 ${w} ${hgt}` });
        const n = Math.max(4, Math.floor(w / 46));
        const step = w / n;
        for (let i = 0; i < n; i++) {
            const x = step * (i + 0.5);
            const p = new Sketch.Pen("ring" + i, { roughness: 0.6, jitter: 1 });
            Sketch.el("ellipse", { cx: x, cy: 31, rx: 4.2, ry: 4.6, class: "hole" }, svg);
            // 螺旋圈：从孔里穿出来，绕到纸的上沿后面
            const coil = p.curve([[x - 1, 31], [x - 7, 22], [x - 4, 7], [x + 4, 3], [x + 7, 12]]);
            Sketch.path(svg, coil, "coil");
            Sketch.path(svg, p.curve([[x - 5, 18], [x - 3, 8], [x + 3, 5]]), "coil-hi");
        }
        ui.rings.replaceChildren(svg);
    }

    function drawLamp() {
        const svg = Sketch.el("svg", { width: 18, height: 18, viewBox: "0 0 18 18" });
        const p = new Sketch.Pen("lamp", { roughness: 0.7, jitter: 1 });
        Sketch.path(svg, p.hachure([[3, 5], [5, 3], [13, 3], [15, 5], [15, 13], [13, 15], [5, 15], [3, 13]], 2.6, -45), "hatch");
        Sketch.path(svg, p.ellipse(9, 9, 14, 14), "ink");
        ui.lamp.appendChild(svg);
    }

    function drawStepDots() {
        ui.steps.querySelectorAll("li").forEach((li) => {
            const name = li.getAttribute("data-step");
            const svg = Sketch.el("svg", { class: "step-dot", width: 18, height: 18, viewBox: "0 0 18 18", "aria-hidden": "true" });
            const p = new Sketch.Pen("step-" + name, { roughness: 0.8, jitter: 1 });
            Sketch.path(svg, p.hachure([[4, 4], [14, 4], [14, 14], [4, 14]], 2.8, -45), "hatch");
            Sketch.path(svg, p.ellipse(9, 9, 14, 14), "ring");
            Sketch.path(svg, p.polyline([[4.5, 9.5], [8, 13], [15.5, 3.5]]), "check");
            li.insertBefore(svg, li.firstChild);
            li.setAttribute("data-state", "pending");
        });
    }

    function drawIcons() {
        document.querySelectorAll(".sk-btn[data-icon]").forEach((b) => {
            b.insertBefore(Sketch.icon(b.getAttribute("data-icon"), b.id), b.querySelector(".label"));
        });
        ui.kbdIcon.appendChild(Sketch.icon("keyboard", "state"));
    }

    function drawProgress(fraction) {
        const svg = ui.progressSvg;
        const w = svg.clientWidth || ui.progress.clientWidth, hgt = 26;
        if (!w) return;
        if (svg._w !== w) {
            svg._w = w;
            svg.setAttribute("viewBox", `0 0 ${w} ${hgt}`);
            const p = new Sketch.Pen("progress", { roughness: 0.9 });
            const clipId = "progress-clip";
            svg.replaceChildren();
            const defs = Sketch.el("defs", null, svg);
            const cp = Sketch.el("clipPath", { id: clipId }, defs);
            svg._clip = Sketch.el("rect", { x: 0, y: 0, width: 0, height: hgt }, cp);
            Sketch.path(svg, p.hachureRect(3, 3, w - 6, hgt - 6, 5, -50), "hatch", { "clip-path": `url(#${clipId})` });
            Sketch.path(svg, p.rect(1, 1, w - 2, hgt - 2), "ink");
        }
        svg._clip.setAttribute("width", Math.max(0, Math.min(1, fraction)) * w);
    }

    /* ---------- 状态显示 ---------- */
    /** hold 毫秒内，每秒刷新的“运行中 · …”不会盖掉这条消息（Infinity = 一直保留到下次开机） */
    function setStatus(text, lamp, hold) {
        ui.statusText.textContent = text;
        if (lamp) ui.lamp.setAttribute("data-state", lamp);
        S.statusHold = hold ? Date.now() + hold : 0;
    }

    function step(name, state) {
        const li = ui.steps.querySelector(`li[data-step="${name}"]`);
        if (!li) return;
        const order = { pending: 0, active: 1, done: 2 };
        // 步骤只能往前走（同一次开机里不会从“完成”退回“进行中”）
        if (state !== "pending" && order[li.getAttribute("data-state")] >= order[state]) return;
        li.setAttribute("data-state", state);
    }

    function resetSteps() {
        ui.steps.querySelectorAll("li").forEach((li) => li.setAttribute("data-state", "pending"));
    }

    function setPowerButton() {
        const running = ["loading", "booting", "running"].includes(S.phase);
        const label = ui.power.querySelector(".label");
        const confirming = S.confirmUntil > Date.now();
        label.textContent = !running ? "开机" : confirming ? "再点一次确认重启" : "重启";
        ui.power.classList.toggle("confirm", running && confirming);
        // 下载中途不许重启：v86 没法取消已经发出去的下载，半路扔掉的模拟器会在后台自己开机
        ui.power.disabled = ["checking", "loading", "restarting"].includes(S.phase);
        if (S.phase === "loading") label.textContent = "下载中……";
        ui.power.setAttribute("data-accent", running ? "var(--orange)" : "var(--green)");
        const deco = ui.power.querySelector(".sk-deco");
        if (deco) deco.style.setProperty("--accent", running ? "var(--orange)" : "var(--green)");
        const icon = ui.power.querySelector(".sk-icon");
        const want = running ? "restart" : "power";
        if (icon && icon._name !== want) {
            const fresh = Sketch.icon(want, "power");
            fresh._name = want;
            icon.replaceWith(fresh);
        }
        const canUse = running && !!S.emu;
        ui.shot.disabled = !canUse;
    }

    function setPhase(phase) {
        S.phase = phase;
        const hasScreen = ["booting", "running", "off"].includes(phase) ||
            (phase === "error" && S.startedAt > 0) || (phase === "restarting" && S.startedAt > 0);
        ui.viewport.classList.toggle("empty", !hasScreen);
        ui.progress.hidden = phase !== "loading";
        setPowerButton();
        layoutScreen();
    }

    /* ---------- 屏幕上的提示卡片 ---------- */
    function showOverlay(kids, opts) {
        ui.overlayBody.replaceChildren(...kids.filter(Boolean));
        ui.overlay.hidden = false;
        ui.overlay.classList.toggle("dim", !!(opts && opts.dim));
        Sketch.decorate(ui.overlay);
    }

    function hideOverlay() {
        ui.overlay.hidden = true;
    }

    function actionButton(label, action, accent, big) {
        return h("button", {
            type: "button", class: "sk-btn" + (big ? " big" : ""), "data-sketch": "button",
            "data-accent": accent || "var(--green)", "data-action": action, id: "ov-" + action,
        }, h("span", { class: "label" }, label));
    }

    function showError(title, message, opts) {
        const o = opts || {};
        if (S.emu && o.stopEmulator !== false) {
            const emu = S.emu;
            S.emu = null;
            S.gen++;
            Promise.resolve().then(() => emu.destroy()).catch(() => {});
        }
        release();
        setPhase("error");
        setStatus(title, "error", Infinity);
        showOverlay([
            h("h2", null, title),
            h("p", null, message),
            o.command ? h("pre", null, o.command) : null,
            o.detail ? h("p", { class: "detail" }, o.detail) : null,
            h("div", { class: "actions" }, actionButton(o.retryLabel || "重试", "boot", "var(--green)")),
        ], { dim: S.startedAt > 0 });
    }

    function renderLoadingCard() {
        let loaded = 0, total = 0;
        const items = DOWNLOADS.map((d) => {
            const st = S.dl[d.url] || { loaded: 0, total: S.sizes[d.url] || 0, done: false };
            const t = st.total || S.sizes[d.url] || 0;
            const l = st.done ? t : Math.min(st.loaded, t || st.loaded);
            loaded += l;
            total += t;
            const amount = st.done ? "✓ " + fmtBytes(t) : st.loaded ? `${fmtBytes(l)} / ${fmtBytes(t)}` : fmtBytes(t);
            return h("li", { "data-done": st.done ? "true" : "false" }, h("span", { class: "name" }, d.label),
                h("span", { class: "amount" }, amount));
        });
        const frac = total ? loaded / total : 0;
        drawProgress(frac);
        ui.progressLabel.textContent = `${Math.round(frac * 100)}%`;
        const card = ui.overlayBody.querySelector(".file-list");
        if (card && S.phase === "loading") {
            card.replaceChildren(...items);
        } else {
            showOverlay([
                h("h2", null, "正在把电脑搬进浏览器……"),
                h("p", null, `一共要下载约 ${fmtBytes(total)}，只在第一次比较慢。`),
                h("ul", { class: "file-list" }, items),
            ]);
        }
        return frac;
    }

    /* ---------- 开机前检查 ---------- */
    function loadLib() {
        if (window.V86) return Promise.resolve();
        if (S.libPromise) return S.libPromise;
        S.libPromise = new Promise((resolve, reject) => {
            const s = document.createElement("script");
            s.src = URLS.lib;
            s.onload = () => (window.V86 ? resolve() : reject(new Error("libv86.js 加载了，但里面没有 V86")));
            s.onerror = () => reject(new Error("找不到 " + URLS.lib));
            document.head.appendChild(s);
        }).catch((e) => {
            S.libPromise = null;
            throw e;
        });
        return S.libPromise;
    }

    async function headSize(url) {
        const r = await fetch(url, { method: "HEAD", cache: "no-store" });
        if (!r.ok) return -1;
        const n = parseInt(r.headers.get("Content-Length") || "0", 10);
        return Number.isFinite(n) ? n : 0;
    }

    /** 返回 true 表示可以开机；否则已经在屏幕上显示了原因 */
    async function preflight() {
        if (location.protocol === "file:") {
            showError("请通过本地服务器打开这个页面",
                "浏览器不允许直接打开的本地网页（file://）读取文件，而模拟器需要下载内核和系统镜像。",
                { command: "Windows：双击 windows\\启动网页版.bat\n或者：python3 tools/serve.py --open", retryLabel: "再试一次" });
            return false;
        }
        if (typeof WebAssembly !== "object") {
            showError("这个浏览器不支持 WebAssembly", "v86 模拟器需要 WebAssembly。请换用新版的 Chrome、Edge、Firefox 或 Safari。");
            return false;
        }
        setPhase("checking");
        setStatus("正在检查系统文件……", "busy");
        showOverlay([h("h2", null, "正在检查系统文件……"), h("p", null, "看看内核和 BIOS 都在不在。")]);

        let manifest = null;
        try {
            const r = await fetch(URLS.manifest, { cache: "no-store" });
            if (r.ok) manifest = await r.json();
        } catch (e) {
            // 读不到清单：下面逐个检查文件时会给出具体原因
        }
        const missing = [];
        try {
            await Promise.all(DOWNLOADS.map(async (d) => {
                const n = await headSize(d.url);
                if (n < 0) missing.push(d.url);
                else S.sizes[d.url] = n;
            }));
        } catch (e) {
            showError("连不上网页服务器", "检查文件时网络请求失败了。服务器是不是已经关掉了？", { detail: String(e) });
            return false;
        }
        if (manifest && manifest.files) {
            const f = manifest.files;
            if (f.bzImage && !S.sizes[URLS.bzimage]) S.sizes[URLS.bzimage] = f.bzImage.size;
            if (f["initramfs.cpio.gz"] && !S.sizes[URLS.initrd]) S.sizes[URLS.initrd] = f["initramfs.cpio.gz"].size;
        }
        S.manifest = manifest;
        showBuildInfo();
        const libMissing = missing.some((u) => u.startsWith("lib/"));
        const imgMissing = missing.some((u) => u.startsWith("images/"));
        if (missing.length) {
            showError(imgMissing && !libMissing ? "还没有构建系统镜像" : "网页版的文件还没有准备好",
                "缺少：" + missing.join("、") + "。请先在 WSL（Ubuntu）里，于项目根目录运行下面的命令，然后点「重试」：",
                { command: imgMissing ? "bash scripts/kernel.sh        # 编译内核（只需一次）\nbash scripts/userspace.sh && bash scripts/initramfs.sh\n" + BUILD_HINT : BUILD_HINT + " --lib-only",
                  detail: "scripts/web.sh 会下载 v86 模拟器和 BIOS 放进 web/lib/，并把 out/ 里的内核和 initramfs 复制到 web/images/。" });
            return false;
        }
        try {
            await loadLib();
        } catch (e) {
            showError("模拟器加载失败", "libv86.js 没能运行。", { command: BUILD_HINT + " --lib-only", detail: String(e.message || e) });
            return false;
        }
        return true;
    }

    function showBuildInfo() {
        const m = S.manifest;
        if (!m) {
            ui.buildInfo.textContent = "";
            return;
        }
        const f = m.files || {};
        const parts = [];
        if (m.built) parts.push("镜像构建于 " + String(m.built).replace("T", " ").replace(/[+-]\d{4}$/, ""));
        if (m.kernel_version) parts.push("Linux " + m.kernel_version);
        if (m.v86_version) parts.push("v86 " + m.v86_version);
        if (f.bzImage) parts.push("bzImage " + fmtBytes(f.bzImage.size));
        if (f["initramfs.cpio.gz"]) parts.push("initramfs " + fmtBytes(f["initramfs.cpio.gz"].size));
        ui.buildInfo.textContent = parts.join(" · ");
    }

    /* ---------- 开机 ---------- */
    async function startMachine() {
        if (["checking", "loading", "booting", "running"].includes(S.phase)) return;
        if (S.emu) {
            const old = S.emu;
            S.emu = null;
            S.gen++;
            try { await Promise.race([old.destroy(), sleep(3000)]); } catch (e) { /* 旧模拟器已停 */ }
        }
        if (!(await preflight())) return;
        boot();
    }

    function boot() {
        const gen = ++S.gen;
        S.dl = {};
        S.lastProgressAt = Date.now();
        S.startedAt = 0;
        S.desktopReady = false;
        S.servicesAt = 0;
        S.mode = null;
        S.mouseMode = "";
        S.lineBuf = "";
        S.rebootHooked = false;
        S.instr = { last: 0, at: 0, mips: 0 };
        resetSteps();
        step("download", "active");
        setPhase("loading");
        setStatus("正在下载模拟器和系统镜像……", "busy");
        renderLoadingCard();

        let emu;
        try {
            emu = new V86({
                wasm_path: URLS.wasm,
                bios: { url: URLS.bios },
                vga_bios: { url: URLS.vgaBios },
                bzimage: { url: URLS.bzimage },
                initrd: { url: URLS.initrd },
                cmdline: (CMDLINE + " " + OPTS.append).trim(),
                memory_size: OPTS.memoryMB * 1024 * 1024,
                vga_memory_size: 16 * 1024 * 1024,
                screen_container: ui.screen,
                acpi: false,
                autostart: true,
                disable_speaker: true,
            });
        } catch (e) {
            showError("模拟器创建失败", "new V86() 抛出了异常。", { detail: String(e && e.message || e) });
            return;
        }
        S.emu = emu;
        const mine = (fn) => (...args) => { if (gen === S.gen) fn(...args); };
        // 已经被放弃的模拟器（下载失败后重试、重启……）如果还是加载完成并开了机，立刻销毁它
        const orphan = () => {
            if (gen === S.gen) return false;
            emu.destroy().catch(() => {});
            return true;
        };

        emu.add_listener("download-progress", mine(onDownloadProgress));
        emu.add_listener("download-error", mine((e) => {
            showError("下载失败", `下载 ${e.file_name} 时服务器返回了错误（${e.request && e.request.status}）。`,
                { command: BUILD_HINT });
        }));
        emu.add_listener("emulator-loaded", () => { if (!orphan()) onLoaded(emu, gen); });
        emu.add_listener("emulator-started", () => { if (!orphan()) onStarted(); });
        emu.add_listener("serial0-output-byte", mine(onSerialByte));
        emu.add_listener("screen-set-size", mine(onScreenSize));
        emu.add_listener("vmware-absolute-mouse", mine((on) => {
            S.mouseMode = on ? "绝对坐标（VMware 后门）" : "PS/2 相对移动";
            updateKbdState();
        }));
        // 键盘适配器是在 wasm 下载完以后才创建的：等它出现，立刻按“是否点过屏幕”设置开关
        const wait = setInterval(() => {
            if (gen !== S.gen) return clearInterval(wait);
            if (emu.keyboard_adapter) {
                clearInterval(wait);
                emu.keyboard_set_enabled(S.captured);
            }
        }, 50);
    }

    function onDownloadProgress(e) {
        const url = String(e.file_name || "");
        const key = DOWNLOADS.find((d) => url === d.url || url.endsWith("/" + d.url) || url.endsWith(d.url))?.url || url;
        // 文件按顺序下载：轮到后面的文件时，前面的一定已经下完了
        const idx = DOWNLOADS.findIndex((d) => d.url === key);
        for (let i = 0; i < idx; i++) {
            const u = DOWNLOADS[i].url;
            S.dl[u] = Object.assign(S.dl[u] || { loaded: 0, total: S.sizes[u] || 0 }, { done: true });
        }
        const st = S.dl[key] || (S.dl[key] = { loaded: 0, total: S.sizes[key] || 0, done: false });
        st.loaded = e.loaded || 0;
        if (e.lengthComputable && e.total) st.total = e.total;
        if (st.total && st.loaded >= st.total) st.done = true;
        S.lastProgressAt = Date.now();
        if (S.phase === "loading") {
            const frac = renderLoadingCard();
            const label = (DOWNLOADS[idx] || { label: key }).label;
            setStatus(`正在下载 ${label} · 总进度 ${Math.round(frac * 100)}%`, "busy");
        }
    }

    /* 注意顺序：autostart 时 v86 先开机（emulator-started），然后才发 emulator-loaded */
    function onLoaded(emu, gen) {
        DOWNLOADS.forEach((d) => {
            S.dl[d.url] = Object.assign(S.dl[d.url] || { loaded: 0, total: S.sizes[d.url] || 0 }, { done: true });
        });
        if (S.phase === "loading") renderLoadingCard();
        step("download", "done");
        hookGuestReboot(emu, gen);
    }

    function onStarted() {
        if (S.phase === "running") return;   // 暂停后继续也会触发这个事件
        S.startedAt = Date.now();
        step("download", "done");
        step("bios", "active");
        setPhase("booting");
        hideOverlay();
        setStage("BIOS 开机自检中");
        layoutScreen();
        if (isFullscreen()) capture();
    }

    /** 开机阶段的说明文字；每秒刷新的状态行会把它和分辨率、速度拼在一起 */
    function setStage(text) {
        S.stage = text;
        setStatus(text + "……", "busy");
    }

    function kernelStarted() {
        step("bios", "done");
        step("kernel", "active");
        if (S.stage === "BIOS 开机自检中") setStage("Linux 内核启动中");
    }

    /* 客户机重启：v86 只在初始化时把 bzImage 和 initramfs 放进模拟内存。内核启动后会解压、覆盖它们，
     * 所以 CPU 复位以后 BIOS 再去执行那块内存只会跑飞（模拟器直接崩溃）。真机重启时内核是从硬盘
     * 重新读出来的；我们模仿这一点：截住 CPU 复位，换一台新的模拟器，用原始镜像重新开机。 */
    function hookGuestReboot(emu, gen) {
        const cpu = emu.v86 && emu.v86.cpu;
        if (!cpu || typeof cpu.reboot_internal !== "function") return;
        const original = cpu.reboot_internal.bind(cpu);
        let fired = false;
        cpu.reboot_internal = function () {
            original();                // 先完成复位：CPU 回到 BIOS 入口，暂时不会跑到坏内存
            if (fired || gen !== S.gen) return;
            fired = true;
            try { emu.v86.stop(); } catch (e) { /* 已经停了 */ }
            setTimeout(() => coldRestart("guest"), 0);
        };
        S.rebootHooked = true;
    }

    async function coldRestart(reason) {
        if (S.phase === "restarting") return;
        const old = S.emu;
        S.emu = null;
        S.gen++;
        S.confirmUntil = 0;
        setPhase("restarting");
        setStatus(reason === "guest" ? "SketchOS 请求重启，正在重新开机……" : "正在重启……", "busy");
        showOverlay([h("h2", null, "正在重启……"), h("p", null, "换一台全新的虚拟电脑，从 BIOS 重新开始。")], { dim: true });
        if (old) {
            try { await Promise.race([old.destroy(), sleep(3000)]); } catch (e) { /* 旧模拟器已停 */ }
        }
        S.phase = "idle";
        await startMachine();
    }

    async function powerOff() {
        if (!S.emu || S.phase === "off") return;
        const emu = S.emu;
        setPhase("off");
        setStatus("SketchOS 已关机", "off");
        release();
        try { await emu.stop(); } catch (e) { /* 已经停了 */ }
        showOverlay([
            h("h2", null, "SketchOS 已经关机了"),
            h("p", null, "内核执行完关机流程后让 CPU 停了下来。点下面的按钮重新开机。"),
            h("div", { class: "actions" }, actionButton("开机", "boot", "var(--green)", true)),
        ], { dim: true });
    }

    /* ---------- 串口 ---------- */
    let pendingBytes = [];
    let flushScheduled = false;

    function onSerialByte(b) {
        pendingBytes.push(b);
        if (!flushScheduled) {
            flushScheduled = true;
            // 一个字节一个事件太碎了：攒到下一个宏任务一起解码、显示
            setTimeout(flushSerial, 0);
        }
    }

    function flushSerial() {
        flushScheduled = false;
        if (!pendingBytes.length) return;
        const bytes = Uint8Array.from(pendingBytes);
        pendingBytes = [];
        if (decoder.gen !== S.gen) {
            decoder.gen = S.gen;
            decoder.d = new TextDecoder("utf-8");
            if (term.lines.length > 1 || term.lines[0].length) term.write("\r\n");
            term.write(`\x1b[2m── 第 ${S.gen} 次开机 ──\x1b[0m\r\n`);
        }
        const text = decoder.d.decode(bytes, { stream: true });
        term.write(text);
        scanMilestones(text);
    }

    /** 日志里程碑（docs/开发规范.md 4.6）：按行匹配，推进开机步骤 */
    function scanMilestones(text) {
        // SeaBIOS 不往串口打印，串口上出现的第一个字一定来自内核：BIOS 阶段结束了
        kernelStarted();
        S.lineBuf += text;
        let nl;
        while ((nl = S.lineBuf.indexOf("\n")) >= 0) {
            const line = S.lineBuf.slice(0, nl).replace(/\r/g, "");
            S.lineBuf = S.lineBuf.slice(nl + 1);
            onSerialLine(line);
        }
        if (S.lineBuf.length > 4096) S.lineBuf = S.lineBuf.slice(-1024);
    }

    function onSerialLine(line) {
        let m;
        if ((m = /sinit: SketchOS (\S+) starting/.exec(line))) {
            ["bios", "kernel"].forEach((s) => step(s, "done"));
            step("sinit", "active");
            setStage(`sinit ${m[1]} 正在启动系统服务`);
        } else if (/sinit: services started/.test(line)) {
            ["bios", "kernel", "sinit"].forEach((s) => step(s, "done"));
            step("desktop", "active");
            S.servicesAt = Date.now();
            setStage("系统服务已启动，正在打开图形桌面");
        } else if ((m = /sketchd: ready (\d+)x(\d+) backend=(\S+) mouse=(\S+)/.exec(line))) {
            ["bios", "kernel", "sinit", "desktop"].forEach((s) => step(s, "done"));
            S.desktopReady = true;
            S.stage = "运行中";
            S.phase = "running";
            setPowerButton();
            setStatus(`桌面已就绪（${m[1]}×${m[2]}，显示后端 ${m[3]}，鼠标 ${m[4]}）`, "ok", 4000);
        } else if (/reboot: Restarting system/.test(line)) {
            // 正常情况下 hookGuestReboot 会截住 CPU 复位；万一没装上，这里兜底
            if (!S.rebootHooked) {
                const gen = S.gen;
                setTimeout(() => { if (gen === S.gen) coldRestart("guest"); }, 1500);
            }
        } else if (/reboot: (Power down|System halted|Power off not available)/.test(line)) {
            powerOff();
        } else if (/Kernel panic - not syncing/.test(line)) {
            setStatus("内核崩溃了（kernel panic）——打开串口控制台看看原因", "error", Infinity);
            if (!S.serialOpen) toggleSerial(true);
        }
    }

    function sendSerial(text) {
        if (!S.emu || !["booting", "running"].includes(S.phase)) {
            setStatus("虚拟机还没有开机，串口没有人在听", S.phase === "error" ? "error" : undefined, 3000);
            return false;
        }
        const bytes = new TextEncoder().encode(text);
        if (typeof S.emu.serial_send_bytes === "function") S.emu.serial_send_bytes(0, bytes);
        else S.emu.serial0_send(text);
        return true;
    }

    function toggleSerial(open) {
        S.serialOpen = open === undefined ? !S.serialOpen : open;
        ui.serialPanel.hidden = !S.serialOpen;
        ui.serialBtn.setAttribute("aria-pressed", String(S.serialOpen));
        store("serial", S.serialOpen ? "1" : "0");
        if (S.serialOpen) {
            Sketch.decorate(ui.serialPanel);
            ui.serialLog.scrollTop = ui.serialLog.scrollHeight;
        }
    }

    /* ---------- 屏幕尺寸与缩放 ---------- */
    function onScreenSize(size) {
        const [w, hgt, bpp] = size;
        S.mode = bpp ? { graphical: true, w: w, h: hgt, bpp: bpp } : { graphical: false, w: w, h: hgt };
        // 切到图形模式说明内核的显卡驱动（bochs-drm）已经接管了屏幕
        if (bpp && S.startedAt) kernelStarted();
        setTimeout(layoutScreen, 0);
    }

    /** 把画面缩放到合适大小。用 CSS transform 缩放整个 screen_container：
     *  v86 计算鼠标位置用的是 getBoundingClientRect()，它已经包含了缩放，所以坐标依然准确。 */
    function layoutScreen() {
        const nw = ui.screen.offsetWidth, nh = ui.screen.offsetHeight;
        if (!nw || !nh) return;
        const fs = isFullscreen();
        let k = 1;
        if (S.scaleMode === "fit" || fs) {
            const availW = ui.viewport.clientWidth - 12;
            const barH = fs ? (ui.stage.querySelector(".stage-bar").offsetHeight + 36) : 0;
            const availH = fs ? ui.stage.clientHeight - barH : Math.max(240, window.innerHeight - 36);
            k = Math.min(availW / nw, availH / nh);
            if (!fs) k = Math.min(1, k);
            k = Math.max(0.1, k);
        }
        const w = Math.round(nw * k), hgt = Math.round(nh * k);
        ui.viewport.classList.toggle("scroll", !fs && S.scaleMode === "1x");
        if (ui.sizer._k === k && ui.sizer._w === w && ui.sizer._h === hgt) return;
        ui.sizer._k = k; ui.sizer._w = w; ui.sizer._h = hgt;
        ui.sizer.style.width = w + "px";
        ui.sizer.style.height = hgt + "px";
        ui.screen.style.transform = k === 1 ? "" : `scale(${k})`;
        ui.frame.classList.toggle("smooth", Math.abs(k - Math.round(k)) > 0.001);
    }

    function setScaleMode(mode) {
        S.scaleMode = mode === "1x" ? "1x" : "fit";
        ui.scale.querySelector(".label").textContent = S.scaleMode === "fit" ? "缩放：适应" : "缩放：1:1";
        ui.scale.setAttribute("aria-pressed", String(S.scaleMode === "1x"));
        store("scale", S.scaleMode);
        layoutScreen();
    }

    /* ---------- 键盘：点了屏幕才交给虚拟机 ---------- */
    function capture() {
        if (S.captured) return;
        S.captured = true;
        if (S.emu && S.emu.keyboard_adapter) S.emu.keyboard_set_enabled(true);
        updateKbdState();
    }

    function release() {
        if (!S.captured) return;
        // 先让 v86 把“还按着的键”全部松开，否则虚拟机会以为 Ctrl 一直按着
        window.dispatchEvent(new Event("blur"));
        S.captured = false;
        if (S.emu && S.emu.keyboard_adapter) S.emu.keyboard_set_enabled(false);
        updateKbdState();
    }

    function updateKbdState() {
        ui.kbdState.setAttribute("data-captured", String(S.captured));
        ui.notebook.setAttribute("data-captured", String(S.captured));
        const mouse = S.mouseMode ? ` · 鼠标：${S.mouseMode}` : "";
        ui.kbdText.textContent = S.captured
            ? "键盘已交给 SketchOS（点屏幕外面，或按一下 Ctrl+Alt 收回）" + mouse
            : "点一下屏幕，键盘就交给 SketchOS" + mouse;
    }

    // Ctrl+Alt 同时按下再松开（中间没按别的键）= 收回键盘，和 QEMU 的习惯类似
    const chord = { ctrl: false, alt: false, other: false };
    function onKeyDown(e) {
        const isCtrl = e.key === "Control", isAlt = e.key === "Alt" || e.key === "AltGraph";
        if ((isCtrl || isAlt) && !chord.ctrl && !chord.alt) chord.other = false;   // 新的一次组合开始
        if (isCtrl) chord.ctrl = true;
        else if (isAlt) chord.alt = true;
        else chord.other = true;
        if (!S.captured && document.activeElement === ui.screen && (e.key === "Enter" || e.key === " ")) {
            capture();   // 用 Tab 键移到屏幕上，再按回车也能把键盘交过去
            e.preventDefault();
        }
    }
    function onKeyUp(e) {
        if (e.key === "Control" || e.key === "Alt" || e.key === "AltGraph") {
            if (chord.ctrl && chord.alt && !chord.other && S.captured) release();
            if (e.key === "Control") chord.ctrl = false; else chord.alt = false;
            if (!chord.ctrl && !chord.alt) chord.other = false;
        }
    }

    /* ---------- 触摸：把手指变成鼠标 ---------- */
    // v86 自带的触摸处理只发相对位移，这里换成和鼠标一样的“绝对坐标 + PS/2 中断”
    const touch = { active: false, x0: 0, y0: 0, x: 0, y: 0, dragging: false, long: false, timer: 0 };

    function touchPoint(t) {
        const r = ui.screen.getBoundingClientRect();
        return { x: t.clientX - r.left, y: t.clientY - r.top, w: r.width, h: r.height };
    }

    function sendPointer(p) {
        if (!S.emu) return;
        const bus = S.emu.bus;
        const dx = Math.round(p.x - touch.x), dy = Math.round(p.y - touch.y);
        bus.send("mouse-absolute", [p.x, p.y, p.w, p.h]);
        // PS/2 包会触发鼠标中断，驱动这时才去后门端口取绝对坐标；位移不能为 0，否则不发包
        bus.send("mouse-delta", [dx || 1, -dy]);
        if (!dx) bus.send("mouse-delta", [-1, 0]);
        touch.x = p.x;
        touch.y = p.y;
    }

    function sendButtons(left, right) {
        if (S.emu) S.emu.bus.send("mouse-click", [left, false, right]);
    }

    function onTouchStart(e) {
        if (!S.emu) return;
        e.preventDefault();
        e.stopPropagation();       // 别让 v86 在 window 上的触摸处理也收到
        capture();
        if (e.touches.length !== 1) {
            touchCancel();
            return;
        }
        const p = touchPoint(e.touches[0]);
        Object.assign(touch, { active: true, x0: p.x, y0: p.y, dragging: false, long: false });
        sendPointer(p);
        clearTimeout(touch.timer);
        touch.timer = setTimeout(() => {            // 长按 = 右键
            if (!touch.active || touch.dragging) return;
            touch.long = true;
            sendButtons(false, true);
            setTimeout(() => sendButtons(false, false), 80);
        }, 550);
    }

    function onTouchMove(e) {
        if (!touch.active) return;
        e.preventDefault();
        e.stopPropagation();
        const p = touchPoint(e.touches[0]);
        if (!touch.dragging && !touch.long && Math.hypot(p.x - touch.x0, p.y - touch.y0) > 10) {
            touch.dragging = true;                  // 移动超过 10 像素：按住左键拖动
            clearTimeout(touch.timer);
            sendButtons(true, false);
        }
        if (touch.dragging) sendPointer(p);
    }

    function onTouchEnd(e) {
        if (!touch.active) return;
        e.preventDefault();
        e.stopPropagation();
        clearTimeout(touch.timer);
        if (touch.dragging) sendButtons(false, false);
        else if (!touch.long) {                     // 轻点 = 左键单击
            sendButtons(true, false);
            setTimeout(() => sendButtons(false, false), 60);
        }
        touch.active = false;
    }

    function touchCancel() {
        clearTimeout(touch.timer);
        if (touch.active && touch.dragging) sendButtons(false, false);
        touch.active = false;
    }

    /* ---------- 工具栏动作 ---------- */
    function onPowerClick() {
        const running = ["loading", "booting", "running"].includes(S.phase);
        if (!running) {
            startMachine();
            return;
        }
        // 重启会丢掉内存里的一切：第一次点只是“确认一下”
        if (S.confirmUntil > Date.now()) {
            coldRestart("user");
        } else {
            S.confirmUntil = Date.now() + 3000;
            setPowerButton();
            setTimeout(setPowerButton, 3100);
        }
    }

    async function toggleFullscreen() {
        try {
            if (isFullscreen()) {
                await (document.exitFullscreen ? document.exitFullscreen() : document.webkitExitFullscreen());
            } else {
                const req = ui.stage.requestFullscreen || ui.stage.webkitRequestFullscreen;
                await req.call(ui.stage, { navigationUI: "hide" });
            }
        } catch (e) {
            setStatus("浏览器拒绝了全屏请求：" + (e.message || e));
        }
    }

    function onFullscreenChange() {
        const fs = isFullscreen();
        ui.fullscreen.setAttribute("aria-pressed", String(fs));
        if (fs) {
            capture();
            // Chrome/Edge：全屏时锁定键盘，Esc、Ctrl+W 等也能交给虚拟机（长按 Esc 退出全屏）
            if (navigator.keyboard && navigator.keyboard.lock) navigator.keyboard.lock().catch(() => {});
        } else if (navigator.keyboard && navigator.keyboard.unlock) {
            navigator.keyboard.unlock();
        }
        ui.sizer._k = 0;
        requestAnimationFrame(layoutScreen);
        Sketch.decorate(ui.stage);
    }

    function takeScreenshot() {
        if (!S.emu) return;
        let img;
        try {
            img = S.emu.screen_make_screenshot();
        } catch (e) {
            setStatus("截图失败：" + (e.message || e), "error");
            return;
        }
        if (!img || !img.src) return;
        const d = new Date();
        const pad = (x) => String(x).padStart(2, "0");
        const name = `SketchOS-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-` +
            `${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.png`;
        fetch(img.src).then((r) => r.blob()).then((blob) => {
            const url = URL.createObjectURL(blob);
            const a = h("a", { href: url, download: name });
            document.body.appendChild(a);
            a.click();
            a.remove();
            setTimeout(() => URL.revokeObjectURL(url), 10000);
        });
        ui.flash.classList.remove("on");
        void ui.flash.offsetWidth;               // 重新触发动画
        ui.flash.classList.add("on");
        setStatus(`已保存截图 ${name}`, undefined, 2500);
    }

    function openPhoneKeyboard() {
        capture();
        ui.phoneKbd.value = " ";                  // 留一个字符，退格键才会产生“删除”事件
        ui.phoneKbd.focus({ preventScroll: true });
    }

    /* ---------- 每秒刷新一次状态行 ---------- */
    function tick() {
        const now = Date.now();
        // 保险：ResizeObserver 和 requestAnimationFrame 在后台页面里不会触发，
        // 这里每秒确认一次画面缩放和手绘边框（尺寸没变时两者都会立刻返回，几乎不花时间）
        layoutScreen();
        drawRings();
        Sketch.refreshAll();
        if (S.phase === "loading" && now - S.lastProgressAt > 25000 && !(S.statusHold > now)) {
            setStatus("下载好像卡住了……（模拟器会自动重试；也可以检查一下网页服务器）", "busy", 5000);
        }
        if (!S.emu || !S.startedAt || !["booting", "running"].includes(S.phase)) return;
        if (typeof S.emu.get_instruction_counter === "function") {
            const ic = S.emu.get_instruction_counter() >>> 0;
            if (S.instr.at) {
                const delta = (ic - S.instr.last) >>> 0;          // 计数器是 32 位的，会回绕
                const mips = delta / ((now - S.instr.at) / 1000) / 1e6;
                S.instr.mips = S.instr.mips ? S.instr.mips * 0.6 + mips * 0.4 : mips;
            }
            S.instr.last = ic;
            S.instr.at = now;
        }
        // 没有 sketchd 的系统（比如 sketch.nogui）：服务启动 8 秒后，或者开机 40 秒后，就算“运行中”
        if (S.phase === "booting" && (S.desktopReady || (S.servicesAt && now - S.servicesAt > 8000) ||
            now - S.startedAt > 40000)) {
            S.phase = "running";
            setPowerButton();
        }
        if (!(S.statusHold > now)) {
            const running = S.phase === "running";
            const mode = !S.mode ? "" : S.mode.graphical ? `图形 ${S.mode.w}×${S.mode.h}` : `文本 ${S.mode.w}×${S.mode.h}`;
            // MIPS = 每秒执行的百万条 x86 指令。系统空闲时内核执行 hlt 让 CPU 睡觉，所以会接近 0
            const speed = !S.instr.at ? "" : S.instr.mips < 0.5 ? "CPU 空闲" : `约 ${Math.round(S.instr.mips)} MIPS`;
            const parts = [running ? "运行中" : S.stage + "……", mode, speed,
                "已运行 " + fmtTime((now - S.startedAt) / 1000)].filter(Boolean);
            setStatus(parts.join(" · "), running ? "ok" : "busy");
        }
    }

    /* ---------- 全局错误：模拟器崩溃、内存不够…… ---------- */
    /** 什么样的错误算“机器坏了”：
     *   - WebAssembly 陷阱（RuntimeError: unreachable / memory access out of bounds）：模拟器的主循环
     *     在 wasm 里崩溃，CPU 不会再执行任何指令；
     *   - 内存分配失败：虚拟机根本建不起来；
     *   - 下载/初始化阶段来自 libv86.js 的任何异常：开机流程断了。
     * 其它错误（某个键盘/鼠标事件处理函数里的异常、浏览器的 ResizeObserver 提示）不影响正在运行的
     * 虚拟机，只记到控制台。 */
    function onGlobalError(message, source) {
        const msg = String(message || "未知错误");
        if (/ResizeObserver loop/.test(msg)) return;
        const fromEmulator = /libv86|v86\.wasm|wasm-function/.test(msg + " " + (source || ""));
        const trap = /RuntimeError/.test(msg);
        // “RuntimeError: memory access out of bounds” 是模拟器跑飞，不是内存不够
        const memory = !trap && /RangeError|allocat|out of memory/i.test(msg);
        const fatal = trap || memory || (S.phase === "loading" && fromEmulator);
        if (!["loading", "booting", "running"].includes(S.phase) || !fatal) {
            console.warn("SketchOS 网页：", msg);
            return;
        }
        showError(memory ? "浏览器没能分配足够的内存" : "模拟器出错了",
            memory ? `这台虚拟电脑需要约 ${OPTS.memoryMB + 16} MB 内存。可以关掉一些标签页，或者在网址后面加上 ?mem=128 再试。`
                : "v86 在运行时遇到了无法继续的错误。可以点「重试」重新开机；串口控制台里也许有线索。",
            { detail: msg });
    }

    /* ---------- 初始化 ---------- */
    function wire() {
        ui.power.addEventListener("click", onPowerClick);
        ui.fullscreen.addEventListener("click", toggleFullscreen);
        ui.exitFs.addEventListener("click", toggleFullscreen);
        ui.shot.addEventListener("click", takeScreenshot);
        ui.serialBtn.addEventListener("click", () => toggleSerial());
        ui.scale.addEventListener("click", () => setScaleMode(S.scaleMode === "fit" ? "1x" : "fit"));
        ui.osk.addEventListener("click", openPhoneKeyboard);
        ui.overlay.addEventListener("click", (e) => {
            const b = e.target.closest("[data-action]");
            if (b && b.getAttribute("data-action") === "boot") startMachine();
        });

        // 点屏幕 = 把键盘交给虚拟机；点别处 = 收回
        document.addEventListener("pointerdown", (e) => {
            if (ui.screen.contains(e.target)) capture();
            else if (!ui.osk.contains(e.target) && e.target !== ui.phoneKbd) release();
        }, true);
        ui.screen.addEventListener("focus", () => {
            if (!S.captured) ui.kbdText.textContent = "按 Enter 把键盘交给 SketchOS";
        });
        ui.screen.addEventListener("blur", updateKbdState);
        window.addEventListener("keydown", onKeyDown, true);
        window.addEventListener("keyup", onKeyUp, true);
        ui.phoneKbd.addEventListener("input", () => {
            // v86 已经通过 input 事件把字送进去了；把输入框复位成一个空格，避免越积越多
            setTimeout(() => {
                ui.phoneKbd.value = " ";
            }, 0);
        });

        ui.screen.addEventListener("touchstart", onTouchStart, { passive: false });
        ui.screen.addEventListener("touchmove", onTouchMove, { passive: false });
        ui.screen.addEventListener("touchend", onTouchEnd, { passive: false });
        ui.screen.addEventListener("touchcancel", touchCancel, { passive: false });
        ui.screen.addEventListener("contextmenu", (e) => e.preventDefault());

        // 串口输入框：这里打的字只走串口。获得焦点时把键盘从虚拟机那边收回来
        ui.serialInput.addEventListener("focus", release);
        ui.serialForm.addEventListener("submit", (e) => {
            e.preventDefault();
            const v = ui.serialInput.value;
            if (sendSerial(v + "\n")) {
                if (v && history.items[history.items.length - 1] !== v) history.items.push(v);
                if (history.items.length > 200) history.items.shift();
                history.pos = history.items.length;
                ui.serialInput.value = "";
            }
        });
        ui.serialInput.addEventListener("keydown", (e) => {
            if (e.key === "ArrowUp" || e.key === "ArrowDown") {
                e.preventDefault();
                if (!history.items.length) return;
                history.pos = Math.max(0, Math.min(history.items.length, history.pos + (e.key === "ArrowUp" ? -1 : 1)));
                ui.serialInput.value = history.items[history.pos] || "";
            } else if (e.key === "c" && e.ctrlKey && ui.serialInput.selectionStart === ui.serialInput.selectionEnd) {
                e.preventDefault();               // 没选中文字时，Ctrl+C 发给虚拟机（中断命令），而不是复制
                sendSerial("\x03");
            }
        });
        ui.serialCtrlC.addEventListener("click", () => sendSerial("\x03"));
        ui.serialClear.addEventListener("click", () => term.clear());

        document.addEventListener("fullscreenchange", onFullscreenChange);
        document.addEventListener("webkitfullscreenchange", onFullscreenChange);
        if (!(document.fullscreenEnabled || document.webkitFullscreenEnabled)) {
            ui.fullscreen.disabled = true;
            ui.fullscreen.title = "这个浏览器不支持网页元素全屏";
        }

        if ("ResizeObserver" in window) {
            const ro = new ResizeObserver(() => {
                layoutScreen();
                drawRings();
                if (S.phase === "loading") drawProgress(renderLoadingCard());
            });
            ro.observe(ui.screen);
            ro.observe(ui.viewport);
            ro.observe(ui.notebook);
        }
        window.addEventListener("resize", () => { ui.sizer._k = 0; layoutScreen(); });

        window.addEventListener("error", (e) => onGlobalError(e.error ? (e.error.stack || e.error.message) : e.message, e.filename));
        window.addEventListener("unhandledrejection", (e) => onGlobalError(e.reason && (e.reason.stack || e.reason.message || e.reason), ""));
        setInterval(tick, 1000);
    }

    function init() {
        drawLogo();
        drawLamp();
        drawStepDots();
        drawIcons();
        Sketch.decorate(document);
        drawRings();
        SketchDiagram.mount(ui.diagram);
        if (document.fonts && document.fonts.ready) {
            // 字体换掉以后按钮宽度会变，重画一遍边框
            document.fonts.ready.then(() => document.querySelectorAll("[data-sketch]").forEach((n) => Sketch.redraw(n)));
        }
        wire();
        setScaleMode(store("scale") === "1x" ? "1x" : "fit");
        if (store("serial") === "1") toggleSerial(true);
        updateKbdState();
        setPhase("idle");
        if (OPTS.autoboot) {
            startMachine();
        } else {
            setStatus("已准备好，点「开机」启动 SketchOS", "idle");
            showOverlay([
                h("h2", null, "SketchOS 已准备好"),
                h("p", null, "点下面的按钮开机。第一次启动需要下载几兆字节的文件。"),
                h("div", { class: "actions" }, actionButton("开机", "boot", "var(--green)", true)),
            ]);
        }
    }

    // 给自动化测试留一个只读的观察口
    window.SketchOSWeb = {
        get phase() { return S.phase; },
        get emulator() { return S.emu; },
        get captured() { return S.captured; },
        get generation() { return S.gen; },
        terminalText: () => term.text(),
        sendSerial: sendSerial,
    };

    init();
})();
