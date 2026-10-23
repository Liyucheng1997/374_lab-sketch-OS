/*
 * terminal.js — 串口控制台的显示部分：一个很小的“终端模拟器”
 *
 * 虚拟机的串口 ttyS0 送出来的是一串字节：普通文字之外还夹着控制字符和 ANSI 转义序列——
 *   \r 回到行首，\n 换行，\b 光标左移；
 *   ESC [ ... m 改颜色（sksh 的彩色提示符），ESC [ K 清除到行尾，ESC [ n D 光标左移 n 格……
 * shell 的行编辑（退格、左右方向键、历史命令）全靠这些序列重画当前行。
 * 这里把它们解释成“一行行带颜色的文字”，画进页面里的一个 <div>。
 * 只支持日志/命令行用得到的那部分；全屏程序（像 top）会尽量按最后 24 行当作“屏幕”来处理。
 */
(function (global) {
    "use strict";

    const MAX_LINES = 3000;     // 最多保留的行数，再多就丢掉最早的
    const SCREEN_ROWS = 24;     // 光标定位序列 ESC[行;列H 以最后这么多行作为“屏幕”

    // 默认样式：前景、背景（-1 = 默认）、粗体、下划线、反显
    const DEFAULT_STYLE = { fg: -1, bg: -1, bold: false, underline: false, inverse: false, rgbFg: null, rgbBg: null };

    function sameStyle(a, b) {
        return a.fg === b.fg && a.bg === b.bg && a.bold === b.bold && a.underline === b.underline &&
            a.inverse === b.inverse && a.rgbFg === b.rgbFg && a.rgbBg === b.rgbBg;
    }

    /** 256 色中 16 以上的颜色换算成 CSS 颜色 */
    function color256(n) {
        if (n < 16) return null;
        if (n >= 232) {
            const v = 8 + (n - 232) * 10;
            return `rgb(${v},${v},${v})`;
        }
        const i = n - 16, steps = [0, 95, 135, 175, 215, 255];
        return `rgb(${steps[Math.floor(i / 36)]},${steps[Math.floor(i / 6) % 6]},${steps[i % 6]})`;
    }

    class SerialTerminal {
        constructor(container) {
            this.el = container;
            this.lines = [[]];          // 每行是 [{ch, st}] 数组
            this.row = 0;
            this.col = 0;
            this.style = Object.assign({}, DEFAULT_STYLE);
            this.saved = null;
            this.state = 0;             // 0 普通，1 收到 ESC，2 CSI 参数中，3 OSC 字符串中
            this.params = "";
            this.dirty = new Set();
            this.lineEls = [];
            this.removedLines = 0;
            this.scheduled = false;
            this.cursorVisible = true;
        }

        clear() {
            this.lines = [[]];
            this.row = this.col = 0;
            this.lineEls.forEach((e) => e.remove());
            this.lineEls = [];
            this.removedLines = 0;
            this._cursorRow = undefined;
            this.dirty.clear();
            this.dirty.add(0);
            this._schedule();
        }

        /** 写入一段已经按 UTF-8 解码好的文字 */
        write(text) {
            for (const ch of text) this._put(ch);
            this._schedule();
        }

        _line() {
            while (this.lines.length <= this.row) this.lines.push([]);
            return this.lines[this.row];
        }

        _put(ch) {
            const c = ch.codePointAt(0);
            if (this.state === 1) {                  // ESC 之后
                if (ch === "[") { this.state = 2; this.params = ""; }
                else if (ch === "]") { this.state = 3; }
                else if (ch === "7") { this.saved = [this.row, this.col]; this.state = 0; }
                else if (ch === "8") { this._restore(); this.state = 0; }
                else if (ch === "c") { this.clear(); this.state = 0; }
                else this.state = 0;                 // 其它两字节序列：忽略
                return;
            }
            if (this.state === 2) {                  // CSI：参数字节 0x30-0x3F，中间字节 0x20-0x2F，结束字节 0x40-0x7E
                if (c >= 0x40 && c <= 0x7E) {
                    this._csi(ch, this.params);
                    this.state = 0;
                } else if (this.params.length < 64) {
                    this.params += ch;
                } else {
                    this.state = 0;                  // 太长的乱序列，丢掉
                }
                return;
            }
            if (this.state === 3) {                  // OSC（比如设置窗口标题）：到 BEL 或 ESC 为止，忽略内容
                if (c === 7) this.state = 0;
                else if (c === 27) this.state = 1;
                return;
            }
            switch (c) {
                case 27: this.state = 1; return;
                case 13: this.col = 0; return;
                case 10: this._newline(); return;
                case 8: if (this.col > 0) this.col--; return;
                case 9: this.col = (Math.floor(this.col / 8) + 1) * 8; return;
                case 7: case 0: case 14: case 15: return;   // 响铃、空字符、字符集切换：忽略
            }
            if (c < 32 || c === 127) return;
            const line = this._line();
            while (line.length < this.col) line.push({ ch: " ", st: DEFAULT_STYLE });
            line[this.col] = { ch: ch, st: this.style };
            this.col++;
            this.dirty.add(this.row);
        }

        _newline() {
            this.dirty.add(this.row);
            this.row++;
            this.col = 0;
            this._line();
            this.dirty.add(this.row);
            if (this.lines.length > MAX_LINES) {
                const drop = this.lines.length - MAX_LINES;
                this.lines.splice(0, drop);
                this.row -= drop;
                this.removedLines += drop;
                const shifted = new Set();
                this.dirty.forEach((r) => { if (r - drop >= 0) shifted.add(r - drop); });
                this.dirty = shifted;
            }
        }

        _restore() {
            if (!this.saved) return;
            [this.row, this.col] = this.saved;
            this._line();
        }

        _screenTop() {
            return Math.max(0, this.lines.length - SCREEN_ROWS);
        }

        _csi(cmd, raw) {
            const priv = raw.startsWith("?");
            const nums = (priv ? raw.slice(1) : raw).split(";").map((s) => (s === "" ? NaN : parseInt(s, 10)));
            const n = (i, def) => (Number.isFinite(nums[i]) ? nums[i] : def);
            const line = this._line();
            switch (cmd) {
                case "m": this._sgr(nums); break;
                case "K": {
                    const mode = n(0, 0);
                    if (mode === 0) line.length = Math.min(line.length, this.col);
                    else if (mode === 1) for (let i = 0; i <= this.col && i < line.length; i++) line[i] = { ch: " ", st: DEFAULT_STYLE };
                    else line.length = 0;
                    this.dirty.add(this.row);
                    break;
                }
                case "C": this.col += Math.max(1, n(0, 1)); break;
                case "D": this.col = Math.max(0, this.col - Math.max(1, n(0, 1))); break;
                case "G": this.col = Math.max(0, n(0, 1) - 1); break;
                case "A": this.row = Math.max(this._screenTop(), this.row - Math.max(1, n(0, 1))); break;
                case "B": this.row += Math.max(1, n(0, 1)); this._line(); break;
                case "H": case "f":
                    this.row = this._screenTop() + Math.max(0, n(0, 1) - 1);
                    this.col = Math.max(0, n(1, 1) - 1);
                    this._line();
                    break;
                case "J": {
                    const mode = n(0, 0);
                    if (mode === 2 || mode === 3) {
                        // 清屏：在日志里表现为“另起一屏”，旧内容留在上面，可以往回翻
                        for (let i = 0; i < SCREEN_ROWS; i++) this.lines.push([]);
                        this.row = this.lines.length - SCREEN_ROWS;
                        this.col = 0;
                        for (let i = this.row; i < this.lines.length; i++) this.dirty.add(i);
                    } else if (mode === 0) {
                        line.length = Math.min(line.length, this.col);
                        for (let r = this.row + 1; r < this.lines.length; r++) { this.lines[r] = []; this.dirty.add(r); }
                        this.dirty.add(this.row);
                    }
                    break;
                }
                case "s": this.saved = [this.row, this.col]; break;
                case "u": this._restore(); break;
                case "h": case "l":
                    if (priv && n(0, 0) === 25) this.cursorVisible = cmd === "h";   // ESC[?25l 隐藏光标
                    break;
                default: break;                       // 其它序列（滚动区域、设备查询……）不处理
            }
            this.dirty.add(this.row);
        }

        /** SGR：选择图形样式（颜色、粗体……） */
        _sgr(nums) {
            const st = Object.assign({}, this.style);
            if (!nums.length || (nums.length === 1 && !Number.isFinite(nums[0]))) nums = [0];
            for (let i = 0; i < nums.length; i++) {
                const v = Number.isFinite(nums[i]) ? nums[i] : 0;
                if (v === 0) Object.assign(st, DEFAULT_STYLE);
                else if (v === 1) st.bold = true;
                else if (v === 4) st.underline = true;
                else if (v === 7) st.inverse = true;
                else if (v === 22) st.bold = false;
                else if (v === 24) st.underline = false;
                else if (v === 27) st.inverse = false;
                else if (v >= 30 && v <= 37) { st.fg = v - 30; st.rgbFg = null; }
                else if (v >= 90 && v <= 97) { st.fg = v - 90 + 8; st.rgbFg = null; }
                else if (v === 39) { st.fg = -1; st.rgbFg = null; }
                else if (v >= 40 && v <= 47) { st.bg = v - 40; st.rgbBg = null; }
                else if (v >= 100 && v <= 107) { st.bg = v - 100 + 8; st.rgbBg = null; }
                else if (v === 49) { st.bg = -1; st.rgbBg = null; }
                else if (v === 38 || v === 48) {
                    // 38;5;n（256 色）或 38;2;r;g;b（真彩色）
                    let css = null, idx = -1;
                    if (nums[i + 1] === 5) {
                        const k = nums[i + 2] | 0;
                        if (k < 16) idx = k; else css = color256(k);
                        i += 2;
                    } else if (nums[i + 1] === 2) {
                        css = `rgb(${nums[i + 2] | 0},${nums[i + 3] | 0},${nums[i + 4] | 0})`;
                        i += 4;
                    }
                    if (v === 38) { st.fg = idx; st.rgbFg = css; } else { st.bg = idx; st.rgbBg = css; }
                }
            }
            this.style = sameStyle(st, DEFAULT_STYLE) ? DEFAULT_STYLE : st;
        }

        /* ----- 渲染：只重画改动过的行，并且合并到下一帧一起做 ----- */
        _schedule() {
            if (this.scheduled) return;
            this.scheduled = true;
            requestAnimationFrame(() => {
                this.scheduled = false;
                this._render();
            });
        }

        _render() {
            const el = this.el;
            const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
            // 行数被裁掉后，DOM 里也删掉最前面的那些行
            if (this.removedLines) {
                const k = Math.min(this.removedLines, this.lineEls.length);
                for (let i = 0; i < k; i++) this.lineEls[i].remove();
                this.lineEls.splice(0, k);
                this.removedLines = 0;
            }
            while (this.lineEls.length < this.lines.length) {
                const d = document.createElement("div");
                d.className = "tl";
                el.appendChild(d);
                this.lineEls.push(d);
                this.dirty.add(this.lineEls.length - 1);
            }
            while (this.lineEls.length > this.lines.length) this.lineEls.pop().remove();
            if (this._cursorRow !== undefined && this._cursorRow !== this.row) this.dirty.add(this._cursorRow);
            this.dirty.add(this.row);
            this._cursorRow = this.row;
            this.dirty.forEach((r) => {
                if (r >= 0 && r < this.lines.length) this._renderLine(r);
            });
            this.dirty.clear();
            if (atBottom) el.scrollTop = el.scrollHeight;
        }

        _renderLine(r) {
            const d = this.lineEls[r];
            const line = this.lines[r];
            const frag = document.createDocumentFragment();
            const withCursor = r === this.row && this.cursorVisible;
            const n = Math.max(line.length, withCursor ? this.col + 1 : 0);
            let i = 0;
            while (i < n) {
                const cell = line[i] || { ch: " ", st: DEFAULT_STYLE };
                if (withCursor && i === this.col) {
                    const s = document.createElement("span");
                    s.className = "cursor";
                    s.textContent = cell.ch;
                    frag.appendChild(s);
                    i++;
                    continue;
                }
                // 相同样式的连续字符放进同一个 <span>
                let j = i, text = "";
                while (j < n && !(withCursor && j === this.col)) {
                    const c2 = line[j] || { ch: " ", st: DEFAULT_STYLE };
                    if (!sameStyle(c2.st, cell.st)) break;
                    text += c2.ch;
                    j++;
                }
                frag.appendChild(this._span(text, cell.st));
                i = j;
            }
            if (!n) frag.appendChild(document.createTextNode("​"));   // 空行也要占一行高度
            d.replaceChildren(frag);
        }

        _span(text, st) {
            if (st === DEFAULT_STYLE) return document.createTextNode(text);
            const s = document.createElement("span");
            let fg = st.fg, bg = st.bg, rgbFg = st.rgbFg, rgbBg = st.rgbBg;
            if (st.inverse) {
                [fg, bg] = [bg < 0 ? 16 : bg, fg < 0 ? 17 : fg];   // 16/17 = 反显时的默认前景/背景
                [rgbFg, rgbBg] = [rgbBg, rgbFg];
            }
            const cls = [];
            if (fg >= 0) cls.push("f" + fg);
            if (bg >= 0) cls.push("b" + bg);
            if (st.bold) cls.push("bold");
            if (st.underline) cls.push("ul");
            if (cls.length) s.className = cls.join(" ");
            if (rgbFg) s.style.color = rgbFg;
            if (rgbBg) s.style.backgroundColor = rgbBg;
            s.textContent = text;
            return s;
        }

        /** 纯文本内容（测试用） */
        text() {
            return this.lines.map((l) => l.map((c) => c.ch).join("").replace(/\s+$/, "")).join("\n");
        }
    }

    global.SerialTerminal = SerialTerminal;
})(window);
