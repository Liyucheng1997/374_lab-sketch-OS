/*
 * rough.js — SketchOS 网页版自带的迷你“手绘线条”生成器（不依赖任何第三方库）
 *
 * 和系统里的 userspace/gui/include/sketch/rough.h 是同一个思路：
 *   - 一条“铅笔线”不是直线，而是一条略微弯曲的三次贝塞尔曲线，两端和中间各有一点随机偏移；
 *   - 每条线描两遍（第二遍抖得少一点），看起来就像用铅笔来回描过；
 *   - 上色不用平涂，而是画一组斜着的平行线（排线 hachure）；
 *   - 随机数由“种子”决定：同一个控件每次重画都用同一个种子，所以形状完全一样，不会闪烁。
 * 产出的是 SVG 路径字符串（d 属性），由调用者放进 <path> 里。
 */
(function (global) {
    "use strict";

    const SVG_NS = "http://www.w3.org/2000/svg";

    /* ---------- 可复现的随机数 ---------- */

    /** 字符串 → 32 位种子（FNV-1a 哈希） */
    function hash(str) {
        let h = 0x811C9DC5;
        const s = String(str);
        for (let i = 0; i < s.length; i++) {
            h ^= s.charCodeAt(i);
            h = Math.imul(h, 0x01000193);
        }
        return h >>> 0;
    }

    /** mulberry32：很小但质量不错的伪随机数发生器，返回 [0, 1) */
    function rng(seed) {
        let a = seed >>> 0;
        return function () {
            a = (a + 0x6D2B79F5) >>> 0;
            let t = a;
            t = Math.imul(t ^ (t >>> 15), t | 1);
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }

    const fmt = (n) => (Math.round(n * 100) / 100).toString();

    /* ---------- 画笔 ---------- */

    /**
     * Pen：带着自己的随机数序列和风格参数的“笔”。
     *   roughness 抖动程度（1 = 正常铅笔，0 = 尺子画的直线）
     *   bowing    线条中段弯曲的程度
     *   jitter    端点最大偏移（像素）
     */
    class Pen {
        constructor(seed, opts) {
            const o = opts || {};
            this.rand = rng(typeof seed === "number" ? seed : hash(seed));
            this.roughness = o.roughness !== undefined ? o.roughness : 1;
            this.bowing = o.bowing !== undefined ? o.bowing : 1;
            this.jitter = o.jitter !== undefined ? o.jitter : 2;
        }

        /** [-x, x] 之间的随机数，再乘上抖动程度 */
        off(x, gain) {
            return this.roughness * (gain || 1) * (this.rand() * 2 * x - x);
        }

        /** 一笔：从 (x1,y1) 到 (x2,y2) 的一条弯曲贝塞尔曲线。overlay=true 是描第二遍，抖得少一些 */
        stroke(x1, y1, x2, y2, overlay) {
            const len = Math.hypot(x2 - x1, y2 - y1);
            // 长线条相对抖得少：否则长边会像波浪
            const gain = len < 200 ? 1 : len > 500 ? 0.4 : -0.0016668 * len + 1.233334;
            let o = this.jitter;
            if (o * o * 100 > len * len) o = len / 10;       // 短线条：偏移不能超过长度的 1/10
            const r = overlay ? o / 2 : o;
            const diverge = 0.2 + this.rand() * 0.2;
            const midX = this.off(this.bowing * this.jitter * (y2 - y1) / 200, gain);
            const midY = this.off(this.bowing * this.jitter * (x1 - x2) / 200, gain);
            const p = () => this.off(r, gain);
            return "M" + fmt(x1 + p()) + " " + fmt(y1 + p()) +
                "C" + fmt(midX + x1 + (x2 - x1) * diverge + p()) + " " + fmt(midY + y1 + (y2 - y1) * diverge + p()) +
                " " + fmt(midX + x1 + 2 * (x2 - x1) * diverge + p()) + " " + fmt(midY + y1 + 2 * (y2 - y1) * diverge + p()) +
                " " + fmt(x2 + p()) + " " + fmt(y2 + p());
        }

        /** 铅笔线：描两遍 */
        line(x1, y1, x2, y2) {
            return this.stroke(x1, y1, x2, y2, false) + this.stroke(x1, y1, x2, y2, true);
        }

        /** 只描一遍的线（排线、细节用） */
        thinLine(x1, y1, x2, y2) {
            return this.stroke(x1, y1, x2, y2, false);
        }

        /** 折线 / 多边形 */
        polyline(points, close) {
            let d = "";
            const n = points.length;
            for (let i = 0; i < n - 1; i++) d += this.line(points[i][0], points[i][1], points[i + 1][0], points[i + 1][1]);
            if (close && n > 2) d += this.line(points[n - 1][0], points[n - 1][1], points[0][0], points[0][1]);
            return d;
        }

        /** 矩形：四条边各自是一笔，角上自然地出头或者没接上，像手画的框 */
        rect(x, y, w, h) {
            return this.polyline([[x, y], [x + w, y], [x + w, y + h], [x, y + h]], true);
        }

        /** 椭圆：沿着圆周取一圈抖动过的点，用平滑曲线连起来；画一圈多一点，首尾交叠 */
        ellipse(cx, cy, w, h) {
            const rx = Math.abs(w / 2), ry = Math.abs(h / 2);
            const perimeter = Math.sqrt(2 * Math.PI * Math.sqrt((rx * rx + ry * ry) / 2));
            const steps = Math.max(9, Math.ceil(9 / Math.sqrt(200) * perimeter * 1.5));
            const inc = Math.PI * 2 / steps;
            const pass = (jit, overlap) => {
                const ax = rx + this.off(rx * 0.05), ay = ry + this.off(ry * 0.05);
                const start = this.rand() * Math.PI * 2;
                const pts = [];
                for (let a = start; a < start + Math.PI * 2 + overlap; a += inc) {
                    pts.push([cx + ax * Math.cos(a) + this.off(jit), cy + ay * Math.sin(a) + this.off(jit)]);
                }
                return this.curve(pts);
            };
            return pass(1, inc * (0.3 + this.rand() * 0.3)) + pass(1.5, 0);
        }

        /** 经过一串点的平滑曲线（Catmull-Rom 样条换算成三次贝塞尔） */
        curve(pts) {
            if (pts.length < 2) return "";
            let d = "M" + fmt(pts[0][0]) + " " + fmt(pts[0][1]);
            for (let i = 0; i < pts.length - 1; i++) {
                const p0 = pts[Math.max(0, i - 1)], p1 = pts[i], p2 = pts[i + 1], p3 = pts[Math.min(pts.length - 1, i + 2)];
                d += "C" + fmt(p1[0] + (p2[0] - p0[0]) / 6) + " " + fmt(p1[1] + (p2[1] - p0[1]) / 6) +
                    " " + fmt(p2[0] - (p3[0] - p1[0]) / 6) + " " + fmt(p2[1] - (p3[1] - p1[1]) / 6) +
                    " " + fmt(p2[0]) + " " + fmt(p2[1]);
            }
            return d;
        }

        /** 手写的波浪线（下划线用）：沿水平方向走，上下轻轻起伏 */
        squiggle(x1, x2, y, amp) {
            const pts = [];
            const a = amp === undefined ? 2.5 : amp;
            const n = Math.max(3, Math.round((x2 - x1) / 18));
            for (let i = 0; i <= n; i++) {
                pts.push([x1 + (x2 - x1) * i / n + this.off(2), y + (i % 2 ? a : -a) * (0.6 + this.rand() * 0.6)]);
            }
            return this.curve(pts);
        }

        /** 排线填充一个凸多边形：一组倾斜的平行线，每条线也稍微抖一下 */
        hachure(points, gap, angleDeg) {
            const g = Math.max(2, gap || 6);
            const ang = (angleDeg === undefined ? -41 : angleDeg) * Math.PI / 180;
            const cos = Math.cos(ang), sin = Math.sin(ang);
            // 把多边形旋转到“排线是水平线”的坐标系，在那里用扫描线求交点，再旋转回来
            const rot = points.map(([x, y]) => [x * cos + y * sin, -x * sin + y * cos]);
            let minY = Infinity, maxY = -Infinity;
            for (const p of rot) {
                minY = Math.min(minY, p[1]);
                maxY = Math.max(maxY, p[1]);
            }
            let d = "";
            for (let sy = minY + g / 2 + this.rand() * g / 2; sy < maxY; sy += g) {
                const xs = [];
                for (let i = 0; i < rot.length; i++) {
                    const a = rot[i], b = rot[(i + 1) % rot.length];
                    if ((a[1] <= sy && b[1] > sy) || (b[1] <= sy && a[1] > sy)) {
                        xs.push(a[0] + (sy - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
                    }
                }
                xs.sort((p, q) => p - q);
                for (let i = 0; i + 1 < xs.length; i += 2) {
                    const xa = xs[i] + 1, xb = xs[i + 1] - 1;
                    if (xb - xa < 2) continue;
                    // 转回原坐标系
                    const x1 = xa * cos - sy * sin, y1 = xa * sin + sy * cos;
                    const x2 = xb * cos - sy * sin, y2 = xb * sin + sy * cos;
                    d += this.thinLine(x1, y1, x2, y2);
                }
            }
            return d;
        }

        hachureRect(x, y, w, h, gap, angle) {
            return this.hachure([[x, y], [x + w, y], [x + w, y + h], [x, y + h]], gap, angle);
        }

        /** 手绘箭头：一条线加两笔箭头 */
        arrow(x1, y1, x2, y2, head) {
            const s = head || 9;
            const a = Math.atan2(y2 - y1, x2 - x1);
            const l = [x2 - s * Math.cos(a - 0.45), y2 - s * Math.sin(a - 0.45)];
            const r = [x2 - s * Math.cos(a + 0.45), y2 - s * Math.sin(a + 0.45)];
            return this.line(x1, y1, x2, y2) + this.thinLine(x2, y2, l[0], l[1]) + this.thinLine(x2, y2, r[0], r[1]);
        }
    }

    /* ---------- SVG 小工具 ---------- */

    function el(tag, attrs, parent) {
        const e = document.createElementNS(SVG_NS, tag);
        if (attrs) for (const k in attrs) if (attrs[k] !== undefined && attrs[k] !== null) e.setAttribute(k, attrs[k]);
        if (parent) parent.appendChild(e);
        return e;
    }

    /** 一条铅笔路径 */
    function path(parent, d, cls, extra) {
        return el("path", Object.assign({ d: d, class: cls || "ink", fill: "none" }, extra || {}), parent);
    }

    /* ---------- 给 HTML 元素加手绘外框 ---------- */
    /*
     * 页面上写 <button data-sketch="button">，这里自动在元素里放一个铺满它的 SVG，画上手绘边框。
     *   data-sketch = box | button | underline | marker | circle
     *   data-accent = 悬停时排线的颜色（CSS 颜色）
     *   data-seed   = 随机种子（默认用 id 或文字），保证每次画出来都一样
     * 元素大小变化时（窗口缩放、文字变化）自动重画。
     */
    const PAD = 12;  // SVG 比元素四周大出这么多，让抖出去的线和阴影不被裁掉

    /** 四个角稍微歪一点的四边形（纸片的底色），返回闭合路径 */
    function wobblyQuad(pen, x, y, w, h, j) {
        const p = () => pen.off(j);
        return `M${fmt(x + p())} ${fmt(y + p())}L${fmt(x + w + p())} ${fmt(y + p())}` +
            `L${fmt(x + w + p())} ${fmt(y + h + p())}L${fmt(x + p())} ${fmt(y + h + p())}Z`;
    }

    function seedOf(elm) {
        return hash(elm.getAttribute("data-seed") || elm.id || elm.textContent.trim() || "sketch");
    }

    function drawDecoration(elm) {
        const kind = elm.getAttribute("data-sketch");
        const w = elm.offsetWidth, h = elm.offsetHeight;
        if (!w || !h) return;
        if (elm._skSize === w + "x" + h) return;          // 尺寸没变就不重画
        elm._skSize = w + "x" + h;
        let svg = elm._skSvg;
        if (!svg) {
            svg = el("svg", { class: "sk-deco sk-deco-" + kind, "aria-hidden": "true", focusable: "false" });
            elm.insertBefore(svg, elm.firstChild);
            elm._skSvg = svg;
        }
        svg.setAttribute("width", w + PAD * 2);
        svg.setAttribute("height", h + PAD * 2);
        svg.setAttribute("viewBox", `${-PAD} ${-PAD} ${w + PAD * 2} ${h + PAD * 2}`);
        while (svg.firstChild) svg.removeChild(svg.firstChild);
        const seed = seedOf(elm);
        const pen = new Pen(seed, { roughness: kind === "box" ? 0.9 : 1.1 });
        const accent = elm.getAttribute("data-accent");
        if (accent) svg.style.setProperty("--accent", accent);
        if (kind === "button" || kind === "box") {
            const shadow = kind === "box" && elm.getAttribute("data-shadow") !== "0";
            if (shadow) {
                // 和系统里的窗口一样：右下方一块铅笔排线当作阴影
                const sp = new Pen(seed ^ 0x2545F491, { roughness: 0.8 });
                const off = Math.min(7, Math.max(4, Math.round(Math.min(w, h) / 40)));
                path(svg, sp.hachureRect(w - 1, off * 2, off + 1, h - off * 2) +       // 右边一条
                    sp.hachureRect(off * 2, h - 1, w - off, off + 1), "shadow-hatch");  // 下边一条
            }
            if (elm.getAttribute("data-fill") !== "none") {
                path(svg, wobblyQuad(new Pen(seed ^ 0x1B873593), 1, 1, w - 2, h - 2, 0.8), "paper-fill");
            }
            if (kind === "button") {
                path(svg, new Pen(seed ^ 0x5bd1e995).hachureRect(3, 3, w - 6, h - 6, 5.5), "fill-hatch");
            }
            path(svg, pen.rect(1, 1, w - 2, h - 2), "ink");
        } else if (kind === "underline") {
            path(svg, pen.squiggle(0, w, h - 2, 2), "ink accent-stroke");
        } else if (kind === "marker") {
            // 荧光笔：一笔很粗、半透明、两头略斜的线，压在文字下面
            const y = h * 0.62;
            path(svg, `M${fmt(-3 + pen.off(2))} ${fmt(y + pen.off(2))} L${fmt(w + 3 + pen.off(2))} ${fmt(y - 2 + pen.off(2))}`, "marker");
        } else if (kind === "circle") {
            path(svg, pen.ellipse(w / 2, h / 2, w + 8, h + 6), "ink accent-stroke");
        }
    }

    let observer = null;
    let refreshTimer = 0;
    /** 把页面上所有装饰检查一遍（尺寸没变的会直接跳过，很便宜） */
    function refreshAll() {
        clearTimeout(refreshTimer);
        refreshTimer = setTimeout(() => {
            document.querySelectorAll("[data-sketch]").forEach((n) => drawDecoration(n));
        }, 60);
    }
    function decorate(root) {
        const nodes = (root || document).querySelectorAll("[data-sketch]");
        if (!observer && "ResizeObserver" in global) {
            observer = new ResizeObserver((entries) => {
                for (const e of entries) {
                    try {
                        drawDecoration(e.target);
                    } catch (err) {
                        console.warn("rough.js:", err);   // 一个元素画坏了也不影响其它元素
                    }
                }
            });
            // 后台标签页不跑 ResizeObserver：窗口变化、页面重新可见时再补查一遍
            global.addEventListener("resize", refreshAll);
            document.addEventListener("visibilitychange", () => {
                if (document.visibilityState === "visible") refreshAll();
            });
        }
        nodes.forEach((n) => {
            drawDecoration(n);
            if (observer && !n._skObserved) {
                observer.observe(n);
                n._skObserved = true;
            }
        });
    }

    /** 强制重画（比如字体加载完成后元素大小变了，或者手动改了文字） */
    function redraw(elm) {
        elm._skSize = "";
        drawDecoration(elm);
    }

    /* ---------- 小图标（工具栏用），全部是代码画出来的 ---------- */
    function icon(name, seed) {
        const s = 22;
        const svg = el("svg", { class: "sk-icon", width: s, height: s, viewBox: `0 0 ${s} ${s}`, "aria-hidden": "true", focusable: "false" });
        const pen = new Pen(hash(name + (seed || "")), { roughness: 0.8, jitter: 1.2 });
        let d = "";
        switch (name) {
            case "power": {
                // 开机符号：缺口的圆 + 竖线
                const pts = [];
                for (let a = -Math.PI / 2 + 0.75; a <= Math.PI * 1.5 - 0.75; a += 0.35) {
                    pts.push([11 + 7.5 * Math.cos(a) + pen.off(0.4), 12 + 7.5 * Math.sin(a) + pen.off(0.4)]);
                }
                d = pen.curve(pts) + pen.line(11, 2.5, 11, 11);
                break;
            }
            case "fullscreen":
                d = pen.polyline([[3, 8], [3, 3], [8, 3]]) + pen.polyline([[14, 3], [19, 3], [19, 8]]) +
                    pen.polyline([[19, 14], [19, 19], [14, 19]]) + pen.polyline([[8, 19], [3, 19], [3, 14]]);
                break;
            case "camera":
                d = pen.rect(2.5, 6.5, 17, 12) + pen.polyline([[7, 6.5], [8.5, 3.5], [13.5, 3.5], [15, 6.5]]) +
                    pen.ellipse(11, 12.5, 7, 7);
                break;
            case "terminal":
                d = pen.rect(2.5, 3.5, 17, 15) + pen.polyline([[6, 8.5], [9, 11], [6, 13.5]]) + pen.line(10.5, 14, 15, 14);
                break;
            case "scale":
                d = pen.ellipse(9.5, 9.5, 11, 11) + pen.line(13.5, 13.5, 19.5, 19.5) + pen.line(7, 9.5, 12, 9.5) +
                    pen.line(9.5, 7, 9.5, 12);
                break;
            case "keyboard":
                d = pen.rect(2, 6, 18, 11);
                for (let i = 0; i < 4; i++) d += pen.thinLine(5 + i * 4, 9.5, 6 + i * 4, 9.5);
                d += pen.line(7, 13.5, 15, 13.5);
                break;
            case "restart": {
                const pts = [];
                for (let a = -0.6; a <= Math.PI * 1.55; a += 0.35) {
                    pts.push([11 + 7.5 * Math.cos(a) + pen.off(0.4), 11.5 + 7.5 * Math.sin(a) + pen.off(0.4)]);
                }
                d = pen.curve(pts) + pen.polyline([[13.5, 2.5], [17.5, 5.5], [13.5, 8.5]]);
                break;
            }
            case "exit":
                d = pen.polyline([[8, 3], [3, 3], [3, 8]]) + pen.polyline([[19, 8], [19, 3], [14, 3]]) +
                    pen.line(7, 7, 15, 15) + pen.line(15, 7, 7, 15);
                break;
            default:
                d = pen.rect(3, 3, 16, 16);
        }
        path(svg, d, "ink icon-ink");
        return svg;
    }

    global.Sketch = { hash, rng, Pen, el, path, decorate, redraw, refreshAll, icon, SVG_NS };
})(window);
