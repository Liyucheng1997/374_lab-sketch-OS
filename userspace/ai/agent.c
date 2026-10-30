/*
 * agent.c — 对话与“智能体循环”
 *
 * ┌──────────────── 智能体循环 ────────────────┐
 *
 *   用户提问 ──▶ 把整个对话历史发给模型 ──▶ 模型流式回答
 *                     ▲                          │
 *                     │                  stop_reason 是什么？
 *                     │            ┌─────────────┴─────────────┐
 *                     │        tool_use                    end_turn
 *                     │     （模型想用工具）                 （说完了）
 *                     │            │                           │
 *        把工具结果作为新的        执行每个工具                 回答结束，
 *        user 消息追加进历史 ◀──（tools.c）                    等下一个问题
 *
 *  模型是“无状态”的：它不记得上一次请求。所以每次请求都要把完整的对话历史
 *  （messages 数组）重新发过去。历史只能往后追加、不能修改——模型每次思考留下的
 *  thinking 块带着签名，原样交回去，模型才能接着上一步的思路往下做。
 *
 * └────────────────────────────────────────────┘
 *
 * 流式回应（SSE）的事件顺序：
 *   message_start → (content_block_start → content_block_delta × N → content_block_stop) × 块数
 *   → message_delta（带 stop_reason 和输出 token 数）→ message_stop
 * 内容块有几种：text（文字）、thinking（思考，带 signature）、tool_use（工具调用，参数是一段段
 * 拼起来的 JSON 文本 input_json_delta）。我们把它们一块块重新拼成完整的助手消息，追加进历史。
 */
#include "skaid.h"

#include <skutil.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static session *g_sess[SKAID_MAX_SESSIONS];

void agent_init(void)
{
    memset(g_sess, 0, sizeof g_sess);
}

/* ================================================================ 给客户端发消息 */

void client_send(session *s, const char *cmd, const char *arg)
{
    if (s->dead)
        return;
    if (skai_send(s->fd, cmd, arg) < 0)
        s->dead = 1;          /* 对方关了连接（写入得到 EPIPE）：主循环会回收这个会话 */
}

void client_sendf(session *s, const char *cmd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *a = NULL;
    if (vasprintf(&a, fmt, ap) < 0)
        a = NULL;
    va_end(ap);
    client_send(s, cmd, a ? a : "");
    free(a);
}

void agent_send_status(session *s)
{
    static const char *const names[] = { "waiting", "nobridge", "nokey", "ready" };
    static const char *const notes[] = {
        "正在联系网页上的 AI 桥……",
        "没有找到 AI 桥：AI 功能需要在网页版 SketchOS 里使用（浏览器负责连接 Claude）",
        "还没有设置 API 密钥：请在网页上点「AI 设置」，填入 Anthropic API 密钥",
        "AI 已就绪",
    };
    bridge_state st = bridge_get_state();
    client_sendf(s, "STATUS", "%s %s %s", names[st], bridge_model(), notes[st]);
}

void agent_broadcast_status(void)
{
    for (int i = 0; i < SKAID_MAX_SESSIONS; i++)
        if (g_sess[i])
            agent_send_status(g_sess[i]);
}

/* ================================================================ 会话 */

session *agent_new_session(int fd)
{
    for (int i = 0; i < SKAID_MAX_SESSIONS; i++) {
        if (!g_sess[i]) {
            session *s = calloc(1, sizeof *s);
            if (!s)
                return NULL;
            s->fd = fd;
            s->messages = json_arr();
            g_sess[i] = s;
            agent_send_status(s);
            return s;
        }
    }
    return NULL;
}

static void drop_current(session *s)
{
    json_free(s->cur_content);
    json_free(s->cur_json);
    s->cur_content = s->cur_json = NULL;
}

void agent_close_session(session *s)
{
    for (int i = 0; i < SKAID_MAX_SESSIONS; i++)
        if (g_sess[i] == s)
            g_sess[i] = NULL;
    if (s->req_id)
        bridge_cancel(s->req_id);
    drop_current(s);
    json_free(s->messages);
    skai_reader_free(&s->rd);
    close(s->fd);
    free(s);
}

static session *by_request(int req_id)
{
    if (req_id <= 0)
        return NULL;
    for (int i = 0; i < SKAID_MAX_SESSIONS; i++)
        if (g_sess[i] && g_sess[i]->req_id == req_id)
            return g_sess[i];
    return NULL;
}

static void finish(session *s, const char *reason)
{
    s->state = SESS_IDLE;
    s->req_id = 0;
    drop_current(s);
    client_send(s, "DONE", reason);
    client_send(s, "BUSY", "0");
}

static void fail(session *s, const char *msg)
{
    sk_log("会话出错: %s", msg);
    client_send(s, "ERROR", msg);
    finish(s, "error");
}

/* ================================================================ 对话历史 */

/* 往历史里追加一个 user 内容块。如果最后一条已经是 user 消息（比如上次回答出错或被截断，
 * 留下了一条工具结果），就合并进去——API 要求 user 和 assistant 轮流说话。 */
static void append_user_block(session *s, jnode *block)
{
    jnode *last = s->messages->last;
    const char *role = last ? json_get_str(last, "role") : NULL;
    jnode *content = last ? json_get(last, "content") : NULL;
    if (role && !strcmp(role, "user") && content && content->t == J_ARR) {
        json_add(content, NULL, block);
        return;
    }
    jnode *m = json_add(s->messages, NULL, json_obj());
    json_add(m, "role", json_str("user"));
    json_add(json_add(m, "content", json_arr()), NULL, block);
}

static void start_request(session *s)
{
    if (++s->steps > SKAID_MAX_STEPS) {
        fail(s, "这个任务来回调用工具的次数太多了，先停一下。可以告诉 AI 接下来怎么做。");
        return;
    }
    if (bridge_get_state() != BRIDGE_READY) {
        fail(s, bridge_get_state() == BRIDGE_NOKEY ? "网页上还没有设置 API 密钥（点网页上的「AI 设置」）"
                                                   : "AI 桥没有连接：AI 功能需要在网页版 SketchOS 里使用");
        return;
    }
    /* 请求体：模型（model）、思考方式和努力程度由网页按用户的设置补上，这里不写 */
    jnode *body = json_obj();
    json_add(body, "max_tokens", json_num(SKAID_MAX_TOKENS));
    json_add(body, "system", json_str(prompt_system()));
    json_add(body, "tools", tools_definitions());
    json_add(body, "messages", json_clone(s->messages));
    json_add(body, "stream", json_bool(1));
    /* 自动提示缓存：工具定义、系统提示和之前的对话都和上一次请求一模一样，
     * 服务器缓存住这一段，下一轮只需要处理新增的部分——更快，也更便宜 */
    jnode *cc = json_add(body, "cache_control", json_obj());
    json_add(cc, "type", json_str("ephemeral"));
    size_t len;
    char *text = json_dump(body, &len);
    json_free(body);

    drop_current(s);
    s->cur_content = json_arr();
    s->cur_json = json_arr();
    s->stop_reason[0] = 0;
    s->state = SESS_WAITING;
    s->req_id = bridge_request(text, len);
    free(text);
    if (s->req_id < 0) {
        s->req_id = 0;
        fail(s, "向 AI 桥发送请求失败（串口写不进去）");
    }
}

/* ================================================================ 客户端命令 */

void agent_on_client(session *s, const char *cmd, const char *arg)
{
    if (!strcmp(cmd, "ASK")) {
        if (s->state != SESS_IDLE) {
            client_send(s, "ERROR", "还在回答上一个问题，请稍等（或者先取消）");
            return;
        }
        if (!*arg) {
            client_send(s, "ERROR", "问题是空的");
            return;
        }
        jnode *b = json_obj();
        json_add(b, "type", json_str("text"));
        json_add(b, "text", json_str(arg));
        append_user_block(s, b);
        s->steps = 0;
        s->in_tokens = s->out_tokens = 0;
        client_send(s, "BUSY", "1");
        start_request(s);
    } else if (!strcmp(cmd, "CANCEL")) {
        if (s->state == SESS_WAITING) {
            bridge_cancel(s->req_id);
            /* 收到一半的回答直接丢掉：它从来没进入历史，下次提问会和没发生过一样 */
            finish(s, "cancelled");
        }
    } else if (!strcmp(cmd, "RESET")) {
        if (s->state == SESS_WAITING) {
            bridge_cancel(s->req_id);
            finish(s, "cancelled");
        }
        json_free(s->messages);
        s->messages = json_arr();
        agent_send_status(s);
    } else if (!strcmp(cmd, "STATUS")) {
        agent_send_status(s);
    } else {
        client_sendf(s, "ERROR", "不认识的命令 %s", cmd);
    }
}

/* ================================================================ 流式事件 */

static void on_block_start(session *s, const jnode *ev)
{
    jnode *cb = json_get(ev, "content_block");
    if (!cb)
        return;
    jnode *block = json_add(s->cur_content, NULL, json_clone(cb));
    const char *type = json_get_str(block, "type");
    json_add(s->cur_json, NULL, json_str(""));       /* 和 content 一一对应的 JSON 碎片缓冲 */
    if (type && !strcmp(type, "thinking"))
        client_send(s, "THINK", "");
    else if (type && !strcmp(type, "tool_use"))
        json_set(block, "input", json_obj());
}

static void append_field(jnode *block, const char *field, const char *s, size_t len)
{
    jnode *f = json_get(block, field);
    if (!f || f->t != J_STR) {
        f = json_strn("", 0);
        json_set(block, field, f);
    }
    json_str_append(f, s, len);
}

static void on_block_delta(session *s, const jnode *ev)
{
    int idx = (int)json_get_num(ev, "index", -1);
    jnode *block = json_at(s->cur_content, idx);
    jnode *delta = json_get(ev, "delta");
    const char *dt = json_get_str(delta, "type");
    if (!block || !dt)
        return;
    jnode *v;
    if (!strcmp(dt, "text_delta") && (v = json_get(delta, "text")) && v->t == J_STR) {
        append_field(block, "text", v->s, v->slen);
        client_send(s, "TEXT", v->s);
    } else if (!strcmp(dt, "thinking_delta") && (v = json_get(delta, "thinking")) && v->t == J_STR) {
        append_field(block, "thinking", v->s, v->slen);
        if (v->slen)
            client_send(s, "THINK", v->s);
    } else if (!strcmp(dt, "signature_delta") && (v = json_get(delta, "signature")) && v->t == J_STR) {
        append_field(block, "signature", v->s, v->slen);
    } else if (!strcmp(dt, "input_json_delta") && (v = json_get(delta, "partial_json")) && v->t == J_STR) {
        json_str_append(json_at(s->cur_json, idx), v->s, v->slen);
    }
    /* 其它（citations_delta……）我们用不到，忽略 */
}

static void on_block_stop(session *s, const jnode *ev)
{
    int idx = (int)json_get_num(ev, "index", -1);
    jnode *block = json_at(s->cur_content, idx);
    jnode *buf = json_at(s->cur_json, idx);
    const char *type = json_get_str(block, "type");
    if (!block || !buf || !type || strcmp(type, "tool_use") != 0)
        return;
    /* 工具参数拼完了：解析成 JSON 对象。参数是边生成边流过来的（eager_input_streaming），
     * 服务器不替我们检查格式——解析失败时把缓冲换成 null，执行时回一个错误让模型重来 */
    if (!buf->slen)
        return;                                    /* 没有参数 = 空对象 */
    jnode *input = json_parse(buf->s, buf->slen, NULL);
    if (input && input->t == J_OBJ) {
        json_set(block, "input", input);
    } else {
        json_free(input);
        buf->t = J_NULL;
    }
}

void agent_on_event(int req_id, const char *text, size_t len)
{
    session *s = by_request(req_id);
    if (!s)
        return;                     /* 已经取消的请求，迟到的事件 */
    const char *err;
    jnode *ev = json_parse(text, len, &err);
    if (!ev) {
        dbg("事件不是合法的 JSON（%s）: %s", err, clip(text, 80));
        return;
    }
    const char *type = json_get_str(ev, "type");
    if (!type) {
    } else if (!strcmp(type, "content_block_delta")) {
        on_block_delta(s, ev);
    } else if (!strcmp(type, "content_block_start")) {
        on_block_start(s, ev);
    } else if (!strcmp(type, "content_block_stop")) {
        on_block_stop(s, ev);
    } else if (!strcmp(type, "message_start")) {
        jnode *u = json_get(json_get(ev, "message"), "usage");
        s->in_tokens += (long)(json_get_num(u, "input_tokens", 0) + json_get_num(u, "cache_read_input_tokens", 0) +
                               json_get_num(u, "cache_creation_input_tokens", 0));
    } else if (!strcmp(type, "message_delta")) {
        const char *sr = json_get_str(json_get(ev, "delta"), "stop_reason");
        if (sr)
            sk_strlcpy(s->stop_reason, sr, sizeof s->stop_reason);
        s->out_tokens += (long)json_get_num(json_get(ev, "usage"), "output_tokens", 0);
    } else if (!strcmp(type, "error")) {
        const char *m = json_get_str(json_get(ev, "error"), "message");
        sk_strlcpy(s->stop_reason, "error", sizeof s->stop_reason);
        client_sendf(s, "ERROR", "API 错误：%s", m ? m : "未知错误");
    }
    json_free(ev);
}

/* 一次回答收完了：存进历史，再看要不要调用工具、继续下一轮 */
void agent_on_end(int req_id)
{
    session *s = by_request(req_id);
    if (!s)
        return;
    s->req_id = 0;
    if (!strcmp(s->stop_reason, "error")) {
        finish(s, "error");         /* 错误事件里的消息已经发给客户端了 */
        return;
    }
    if (!s->stop_reason[0]) {
        fail(s, "回应在中途断开了（没有收到结束标记）");
        return;
    }
    jnode *content = s->cur_content, *jsons = s->cur_json;
    s->cur_content = s->cur_json = NULL;
    if (json_count(content) > 0) {
        jnode *m = json_add(s->messages, NULL, json_obj());
        json_add(m, "role", json_str("assistant"));
        json_add(m, "content", content);
    } else {
        json_free(content);
        content = NULL;
    }
    client_sendf(s, "USAGE", "%ld %ld", s->in_tokens, s->out_tokens);

    /* 助手消息里的每个 tool_use 都必须有对应的 tool_result（哪怕没执行），否则下一次请求会被拒绝 */
    int ntools = 0, idx = 0;
    for (jnode *b = content ? content->child : NULL; b; b = b->next, idx++) {
        const char *type = json_get_str(b, "type");
        if (!type || strcmp(type, "tool_use") != 0)
            continue;
        ntools++;
        const char *name = json_get_str(b, "name");
        const char *id = json_get_str(b, "id");
        jnode *input = json_get(b, "input");
        char *res, *summary = NULL;
        int is_error = 1;
        char *desc = tools_describe(name ? name : "?", input);
        client_send(s, "TOOL", desc);
        sk_log("工具 %s", desc);
        free(desc);
        jnode *buf = json_at(jsons, idx);
        if (strcmp(s->stop_reason, "tool_use") != 0) {
            res = strdup("这个工具调用没有执行：回答在参数写完之前就被截断了");
            summary = strdup("没有执行（回答被截断）");
        } else if (buf && buf->t == J_NULL) {
            res = strdup("工具参数不是合法的 JSON（可能被截断了），请重新调用");
            summary = strdup("参数无效");
        } else {
            res = tools_run(name ? name : "", input, &is_error, &summary);
        }
        client_sendf(s, "RESULT", "%s %s", is_error ? "err" : "ok", summary ? summary : "");
        free(summary);
        jnode *tr = json_obj();
        json_add(tr, "type", json_str("tool_result"));
        json_add(tr, "tool_use_id", json_str(id ? id : ""));
        json_add(tr, "content", json_str(res));
        if (is_error)
            json_add(tr, "is_error", json_bool(1));
        free(res);
        append_user_block(s, tr);
    }
    json_free(jsons);

    if (s->dead)
        return;
    if (!strcmp(s->stop_reason, "tool_use") && ntools) {
        start_request(s);           /* 把工具结果交回模型，继续 */
    } else if (!strcmp(s->stop_reason, "pause_turn")) {
        start_request(s);
    } else if (!strcmp(s->stop_reason, "max_tokens")) {
        client_send(s, "ERROR", "回答太长，被截断了。可以让 AI “继续”。");
        finish(s, "max_tokens");
    } else if (!strcmp(s->stop_reason, "refusal")) {
        client_send(s, "ERROR", "模型拒绝回答这个请求。");
        finish(s, "refusal");
    } else {
        finish(s, s->stop_reason);
    }
}

void agent_on_error(int req_id, const char *msg)
{
    session *s = by_request(req_id);
    if (!s)
        return;
    s->req_id = 0;
    fail(s, msg);
}
