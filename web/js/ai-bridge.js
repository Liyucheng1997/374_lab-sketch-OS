/*
 * ai-bridge.js — “AI 桥”：把虚拟机第二个串口（COM2 / ttyS1）接到 Claude API 上
 *
 * 浏览器里的这台虚拟电脑没有网卡。SketchOS 里的 AI 服务 skaid（userspace/ai/）把要发给 Claude
 * 的请求写进 /dev/ttyS1，v86 模拟的 16550 串口芯片把一个个字节交给这里；我们加上 API 密钥和用户
 * 选的模型，用 fetch 发给 Claude，再把流式回应（SSE）的每个事件写回串口。所以这块“桥”就像一块
 * 只会和 Claude 说话的网卡：对话历史、工具调用这些“智能”的部分全在虚拟机里，密钥却从不进入虚拟机。
 *
 * 串口上的协议（每条消息一行，见 userspace/ai/bridge.c 开头）：
 *   虚拟机 → 网页   HELLO 1 | REQ <编号> <字节数>\n<请求体 JSON>\n | CANCEL <编号>
 *   网页 → 虚拟机   READY <有密钥 0|1> <模型> | EV <编号> <事件 JSON> | END <编号> | ERR <编号> <消息>
 *
 * 浏览器和 Node.js 都能用（tools/v86_harness.mjs 用它做端到端测试，可以换一个假的 fetch）。
 */
(function (root) {
    "use strict";

    const DEFAULTS = {
        key: "",
        baseUrl: "https://api.anthropic.com",
        model: "claude-opus-5-5",
        effort: "medium",           // Claude Opus 5.5 的默认努力程度；low 更快更省，high/xhigh/max 更仔细
        auth: "x-api-key",          // 第三方兼容网关有的要 "bearer"（Authorization: Bearer <密钥>）
        thinkingSummary: true,      // 让模型返回思考过程的摘要（虚拟机里 ai -v 能看到）
        fallback: true,             // 被安全策略拒答时，服务器自动换一个模型重试
    };
    const FALLBACK_BETA = "server-side-fallback-2026-07-01";
    const MAX_LINE = 4096;

    /** 官方 API 地址才加实验性的字段；第三方代理多半不认识它们，会直接报 400 */
    function isOfficial(base) {
        return /^https:\/\/api\.anthropic\.com\/?$/.test(base || "");
    }

    /** 新一代模型（Claude 4.6 以后）才支持 adaptive thinking 和 output_config.effort */
    function isModern(model) {
        return /claude-(opus|sonnet|haiku|fable|mythos)-(5|4-[6-9])/.test(model || "");
    }

    /** 支持服务器端拒答回退（fallbacks: "default"）的模型 */
    function supportsFallback(model) {
        return /^claude-(opus-5|fable-5-1|sonnet-5-5|mythos-5-1)/.test(model || "");
    }

    const esc = (s) => String(s).replace(/\\/g, "\\\\").replace(/\r/g, "\\r").replace(/\n/g, "\\n");

    class SketchAIBridge {
        /**
         * @param {object} o
         *   send(bytes: Uint8Array)   把字节写进虚拟机的串口
         *   config(): object          当前设置（字段见 DEFAULTS）
         *   fetch                     可选：替换 fetch（测试用）
         *   onActivity(ev)            可选：状态变化（给网页显示用），ev.type 是 hello/request/done/error/cancel
         */
        constructor(o) {
            this.o = o;
            this.fetch = o.fetch || (typeof fetch === "function" ? fetch.bind(root) : null);
            this.enc = new TextEncoder();
            this.reset();
        }

        static get DEFAULTS() { return Object.assign({}, DEFAULTS); }

        /** 新的一次开机：清掉半截的数据和正在进行的请求 */
        reset() {
            for (const r of (this.active || new Map()).values()) {
                r.cancelled = true;     // 旧请求的回应不能写进新开机的虚拟机
                r.ctrl.abort();
            }
            this.active = new Map();
            this.line = [];
            this.body = null;           // 正在接收的请求体 { id, need, bytes: [], got }
            this.hello = false;
        }

        cfg() {
            return Object.assign({}, DEFAULTS, (this.o.config && this.o.config()) || {});
        }

        emit(ev) {
            try {
                if (this.o.onActivity) this.o.onActivity(ev);
            } catch (e) {
                // 显示出错不影响桥本身
            }
        }

        write(text) {
            this.o.send(this.enc.encode(text));
        }

        /** 告诉虚拟机现在的设置（有没有密钥、用哪个模型）。设置改了以后调用 */
        configChanged() {
            if (this.hello) this.sendReady();
        }

        sendReady() {
            const c = this.cfg();
            this.write(`READY ${c.key ? 1 : 0} ${String(c.model || "-").replace(/\s+/g, "")}\n`);
        }

        /** 虚拟机写出了一个字节（serial1-output-byte 事件） */
        feed(b) {
            if (this.body) {
                const r = this.body;
                if (r.got < r.need) {
                    r.bytes[r.got++] = b;
                    if (r.got === r.need) {
                        this.body = null;
                        this.startRequest(r.id, r.bytes);
                    }
                }
                return;
            }
            if (b === 0x0A) {
                const text = new TextDecoder().decode(Uint8Array.from(this.line));
                this.line = [];
                this.onLine(text.replace(/\r$/, ""));
            } else if (this.line.length < MAX_LINE) {
                this.line.push(b);
            }
        }

        onLine(line) {
            let m;
            if (!line) return;                       // 请求体后面的那个换行
            if (/^HELLO\b/.test(line)) {
                this.hello = true;
                this.sendReady();
                this.emit({ type: "hello" });
            } else if ((m = /^REQ (\d+) (\d+)$/.exec(line))) {
                const need = Number(m[2]);
                if (need > 64 * 1024 * 1024) return;
                this.body = { id: Number(m[1]), need, got: 0, bytes: new Uint8Array(need) };
                if (need === 0) {
                    this.body = null;
                    this.startRequest(Number(m[1]), new Uint8Array(0));
                }
            } else if ((m = /^CANCEL (\d+)$/.exec(line))) {
                const r = this.active.get(Number(m[1]));
                if (r) {
                    r.cancelled = true;
                    r.ctrl.abort();
                    this.active.delete(Number(m[1]));
                    this.emit({ type: "cancel", id: Number(m[1]) });
                }
            }
        }

        fail(id, msg) {
            this.write(`ERR ${id} ${esc(msg)}\n`);
        }

        /** 按用户的设置补全请求体：模型、思考方式、努力程度、拒答回退 */
        buildRequest(body, c) {
            const official = isOfficial(c.baseUrl);
            const headers = {
                "content-type": "application/json",
                "anthropic-version": "2023-06-01",
                // 浏览器直接调用 API（没有自己的后端服务器）要明确声明：密钥会出现在这个网页里
                "anthropic-dangerous-direct-browser-access": "true",
            };
            if (c.auth === "bearer") headers.authorization = "Bearer " + c.key;
            else headers["x-api-key"] = c.key;
            body.model = c.model;
            body.stream = true;
            if (isModern(c.model)) {
                body.thinking = c.thinkingSummary ? { type: "adaptive", display: "summarized" } : { type: "adaptive" };
                if (c.effort) body.output_config = Object.assign({}, body.output_config, { effort: c.effort });
            }
            if (official && c.fallback && supportsFallback(c.model)) {
                body.fallbacks = "default";
                headers["anthropic-beta"] = FALLBACK_BETA;
            }
            if (!official && Array.isArray(body.tools)) {
                for (const t of body.tools) delete t.eager_input_streaming;
            }
            const base = String(c.baseUrl || DEFAULTS.baseUrl).replace(/\/+$/, "");
            return { url: base + "/v1/messages", headers, body };
        }

        async startRequest(id, bytes) {
            const c = this.cfg();
            let body;
            try {
                body = JSON.parse(new TextDecoder().decode(bytes));
            } catch (e) {
                this.fail(id, "请求体不是合法的 JSON：" + e.message);
                return;
            }
            if (!c.key) {
                this.fail(id, "网页上还没有设置 API 密钥（点「AI 设置」）");
                return;
            }
            if (!this.fetch) {
                this.fail(id, "这个环境里没有 fetch，无法联网");
                return;
            }
            const req = this.buildRequest(body, c);
            const ctrl = typeof AbortController === "function" ? new AbortController() : { abort() {}, signal: undefined };
            const rec = { id, ctrl, cancelled: false, started: Date.now(), usage: { in: 0, out: 0 }, model: c.model };
            this.active.set(id, rec);
            this.emit({ type: "request", id, model: c.model, bytes: bytes.length, messages: (body.messages || []).length });
            let res;
            try {
                res = await this.fetch(req.url, {
                    method: "POST", headers: req.headers, body: JSON.stringify(req.body), signal: ctrl.signal,
                });
            } catch (e) {
                if (rec.cancelled) return;
                this.active.delete(id);
                const msg = `连不上 ${req.url}（${e && e.message || e}）。检查网络、API 地址，` +
                    `或者这个地址不允许浏览器直接访问（跨域 CORS）`;
                this.fail(id, msg);
                this.emit({ type: "error", id, message: msg });
                return;
            }
            if (!res.ok) {
                let detail = "";
                try {
                    const t = await res.text();
                    try {
                        const j = JSON.parse(t);
                        detail = (j.error && (j.error.message || j.error.type)) || t;
                    } catch (e) {
                        detail = t;
                    }
                } catch (e) {
                    // 读不到错误正文
                }
                this.active.delete(id);
                const hint = res.status === 401 ? "（API 密钥不对？）" : res.status === 404 ? "（模型名或 API 地址不对？）" :
                    res.status === 429 ? "（请求太频繁或额度用完了，稍后再试）" : res.status === 529 ? "（服务器繁忙，稍后再试）" : "";
                const msg = `API 返回 ${res.status}${hint}：${String(detail).slice(0, 400)}`;
                this.fail(id, msg);
                this.emit({ type: "error", id, status: res.status, message: msg });
                return;
            }
            try {
                await this.pump(id, rec, res);
            } catch (e) {
                if (rec.cancelled) return;
                this.active.delete(id);
                const msg = "读取流式回应时出错：" + (e && e.message || e);
                this.fail(id, msg);
                this.emit({ type: "error", id, message: msg });
            }
        }

        /** 读 SSE 流：每个 "data: {...}" 行就是一个事件，原样转发给虚拟机 */
        async pump(id, rec, res) {
            const reader = res.body.getReader();
            const dec = new TextDecoder();
            let buf = "", stop = "";
            const handle = (line) => {
                if (!line.startsWith("data:")) return;          // event: 行、注释、空行都不需要
                const data = line.slice(5).trim();
                if (!data || data === "[DONE]") return;
                let ev = null;
                try {
                    ev = JSON.parse(data);
                } catch (e) {
                    return;
                }
                if (ev.type === "ping") return;                 // 心跳：不用占串口
                if (ev.type === "message_start" && ev.message) {
                    const u = ev.message.usage || {};
                    rec.usage.in += (u.input_tokens || 0) + (u.cache_read_input_tokens || 0) + (u.cache_creation_input_tokens || 0);
                    rec.cached = (rec.cached || 0) + (u.cache_read_input_tokens || 0);
                    if (ev.message.model) rec.model = ev.message.model;
                } else if (ev.type === "message_delta") {
                    if (ev.usage && ev.usage.output_tokens) rec.usage.out = ev.usage.output_tokens;
                    if (ev.delta && ev.delta.stop_reason) stop = ev.delta.stop_reason;
                }
                // JSON.stringify 保证是一行（字符串里的换行都是 \n 转义）
                this.write(`EV ${id} ${JSON.stringify(ev)}\n`);
            };
            for (;;) {
                const { value, done } = await reader.read();
                if (rec.cancelled) return;
                if (done) break;
                buf += dec.decode(value, { stream: true });
                let nl;
                while ((nl = buf.indexOf("\n")) >= 0) {
                    handle(buf.slice(0, nl).replace(/\r$/, ""));
                    buf = buf.slice(nl + 1);
                }
            }
            if (buf) handle(buf);
            this.active.delete(id);
            this.write(`END ${id}\n`);
            this.emit({ type: "done", id, stop, model: rec.model, usage: rec.usage, cached: rec.cached || 0,
                        ms: Date.now() - rec.started });
        }

        /** “测试连接”：查一下模型信息（GET /v1/models/<模型>），不花 token */
        async test() {
            const c = this.cfg();
            if (!c.key) return { ok: false, message: "还没有填 API 密钥" };
            const { headers } = this.buildRequest({}, c);
            delete headers["content-type"];
            delete headers["anthropic-beta"];
            const base = String(c.baseUrl || DEFAULTS.baseUrl).replace(/\/+$/, "");
            let res;
            try {
                res = await this.fetch(base + "/v1/models/" + encodeURIComponent(c.model), { headers });
            } catch (e) {
                return { ok: false, message: `连不上 ${base}（${e && e.message || e}）` };
            }
            let j = null;
            try {
                j = await res.json();
            } catch (e) {
                // 不是 JSON
            }
            if (res.ok) return { ok: true, message: `连接成功：${(j && j.display_name) || c.model}` };
            const detail = j && j.error ? j.error.message : "";
            return { ok: false, message: `API 返回 ${res.status}${res.status === 401 ? "：密钥无效" : res.status === 404 ? "：找不到这个模型" : ""}${detail ? "（" + detail + "）" : ""}` };
        }
    }

    root.SketchAIBridge = SketchAIBridge;
    if (typeof module === "object" && module.exports) module.exports = SketchAIBridge;
})(typeof globalThis !== "undefined" ? globalThis : this);
