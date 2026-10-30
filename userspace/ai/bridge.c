/*
 * bridge.c — 第二个串口上的“AI 桥”：SketchOS 怎么连上 Claude
 *
 * ┌──────────────── 为什么要绕道串口？ ────────────────┐
 *
 *  网页版的 SketchOS 跑在浏览器的 x86 模拟器里，这台虚拟电脑没有网卡，上不了网。
 *  但浏览器本身能上网。于是我们给虚拟机插了第二个串口（COM2，Linux 里叫 /dev/ttyS1），
 *  串口的另一头接在网页的 JavaScript 上（web/js/ai-bridge.js）——它就像一块“只能和
 *  Claude 说话的网卡”：
 *
 *      skaid ──REQ 请求体──▶ /dev/ttyS1 ══ 模拟的 16550 串口 ══▶ 网页 JS ──HTTPS──▶ Claude API
 *      skaid ◀──EV 事件──── /dev/ttyS1 ◀══════════════════════ 网页 JS ◀──流式回应──┘
 *
 *  API 密钥只保存在网页里（浏览器的 localStorage），由网页在发请求时加上；虚拟机里的程序
 *  永远看不到密钥。网页还负责填上用户选的模型（默认 claude-opus-5-5）。
 *  真正的“智能”——对话历史、调用哪个工具、把结果交回模型——全在虚拟机里的 skaid 做，
 *  网页只是转发字节的“网卡”。
 *
 * └────────────────────────────────────────────────────┘
 *
 * 串口上的协议（每条消息一行，以 \n 结尾）：
 *   skaid → 网页   HELLO 1                     打招呼（开机时、以及每隔一段时间没有回应时）
 *                  REQ <编号> <字节数>\n<请求体>\n   一个 Messages API 请求（JSON，不含 model）
 *                  CANCEL <编号>                 取消请求
 *   网页 → skaid   READY <有密钥 0|1> <模型>      回应 HELLO；用户改了设置时也会主动发
 *                  EV <编号> <JSON>              流式回应里的一个事件（SSE 的一个 data: 行）
 *                  END <编号>                    回应结束
 *                  ERR <编号> <消息>              请求失败（消息已按 skai.h 的规则转义）
 *
 * 串口默认是给人用的“终端”：内核的行规程（line discipline）会回显输入、把 \r 变成 \n、
 * 遇到 Ctrl+C 发信号……对传数据来说这些全是捣乱。cfmakeraw() 把它切成“原始模式”：
 * 字节原样进出，内核不做任何加工。
 */
#include "skaid.h"

#include <skutil.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define HELLO_TIMEOUT_MS  4000      /* 打招呼后多久没回应就认为“不在网页版里” */
#define HELLO_RETRY_MS    15000     /* 没有桥时，隔多久再试一次（网页可能是后来才接上的） */

static int g_fd = -1;
static skai_reader g_rd;
static bridge_state g_state = BRIDGE_UNKNOWN;
static char g_model[64] = "";
static uint64_t g_hello_at;
static int g_next_id = 1;

bridge_state bridge_get_state(void) { return g_state; }
const char *bridge_model(void) { return g_model[0] ? g_model : "-"; }
int bridge_fd(void) { return g_fd; }

static int write_all(const char *d, size_t n)
{
    while (n) {
        ssize_t w = write(g_fd, d, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                /* 串口的发送缓冲区满了：等它腾出地方（tcdrain 会睡到数据都发出去） */
                tcdrain(g_fd);
                continue;
            }
            sk_log("写串口失败: %s", strerror(errno));
            return -1;
        }
        d += w;
        n -= (size_t)w;
    }
    return 0;
}

static void send_hello(void)
{
    if (g_fd < 0)
        return;
    write_all("HELLO 1\n", 8);
    g_hello_at = now_ms();
}

int bridge_open(void)
{
    /* 测试时用环境变量 SKAID_BRIDGE 换成一个伪终端（tests/ai/ 里由 Python 扮演网页） */
    const char *dev = getenv("SKAID_BRIDGE");
    if (!dev || !*dev)
        dev = SKAID_BRIDGE_DEV;
    /* O_NOCTTY：别让这个串口成为我们的“控制终端”（守护进程不该有控制终端，否则串口挂断时会收到 SIGHUP） */
    g_fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (g_fd < 0) {
        sk_log("打不开 %s: %s（只有网页版插着 AI 桥）", dev, strerror(errno));
        g_state = BRIDGE_NONE;
        return -1;
    }
    struct termios t;
    if (tcgetattr(g_fd, &t) < 0) {
        /* 端口号在，但没有串口芯片（QEMU 默认只有一个串口）：内核会在这里报 EIO */
        sk_log("%s 不是可用的串口: %s", dev, strerror(errno));
        close(g_fd);
        g_fd = -1;
        g_state = BRIDGE_NONE;
        return -1;
    }
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;     /* CLOCAL：不管调制解调器的“载波”信号，没接线也照样收发 */
    t.c_cflag &= ~CRTSCTS;           /* 不用硬件流控 */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(g_fd, TCSANOW, &t) < 0)
        sk_log("设置串口参数失败: %s", strerror(errno));
    tcflush(g_fd, TCIOFLUSH);         /* 丢掉开机时可能残留的杂乱字节 */
    send_hello();
    return g_fd;
}

int bridge_request(const char *body, size_t len)
{
    if (g_fd < 0)
        return -1;
    int id = g_next_id++;
    char head[64];
    int n = snprintf(head, sizeof head, "REQ %d %zu\n", id, len);
    dbg("bridge: REQ %d (%zu 字节)", id, len);
    if (write_all(head, (size_t)n) < 0 || write_all(body, len) < 0 || write_all("\n", 1) < 0)
        return -1;
    return id;
}

void bridge_cancel(int req_id)
{
    if (g_fd < 0 || req_id <= 0)
        return;
    char line[32];
    int n = snprintf(line, sizeof line, "CANCEL %d\n", req_id);
    write_all(line, (size_t)n);
}

static void set_state(bridge_state st, const char *model)
{
    int changed = st != g_state;
    if (model && strcmp(model, g_model) != 0) {
        sk_strlcpy(g_model, model, sizeof g_model);
        changed = 1;
    }
    g_state = st;
    if (changed) {
        sk_log("AI 桥: %s，模型 %s", st == BRIDGE_READY ? "就绪" : st == BRIDGE_NOKEY ? "没有 API 密钥" :
               st == BRIDGE_NONE ? "没有连接" : "未知", bridge_model());
        agent_broadcast_status();
    }
}

static void handle_line(char *line)
{
    if (strncmp(line, "EV ", 3) == 0) {
        char *end;
        long id = strtol(line + 3, &end, 10);
        if (*end == ' ')
            agent_on_event((int)id, end + 1, strlen(end + 1));
    } else if (strncmp(line, "END ", 4) == 0) {
        agent_on_end(atoi(line + 4));
    } else if (strncmp(line, "ERR ", 4) == 0) {
        char *end;
        long id = strtol(line + 4, &end, 10);
        char *msg = *end == ' ' ? end + 1 : end;
        skai_unescape(msg);
        agent_on_error((int)id, msg);
    } else if (strncmp(line, "READY ", 6) == 0) {
        char *argv[4];
        int n = sk_split_args(line + 6, argv, 4);
        int has_key = n >= 1 && atoi(argv[0]) != 0;
        set_state(has_key ? BRIDGE_READY : BRIDGE_NOKEY, n >= 2 ? argv[1] : NULL);
    } else if (line[0]) {
        dbg("bridge: 不认识的消息 %s", clip(line, 60));
    }
}

void bridge_on_readable(void)
{
    for (;;) {
        long n = skai_reader_fill(&g_rd, g_fd);
        if (n <= 0)
            break;          /* 非阻塞：EAGAIN 表示暂时读完了 */
    }
    char *line;
    while ((line = skai_reader_next(&g_rd))) {
        handle_line(line);
        free(line);
    }
}

void bridge_tick(uint64_t now)
{
    if (g_fd < 0)
        return;
    if (g_state == BRIDGE_UNKNOWN && now - g_hello_at >= HELLO_TIMEOUT_MS)
        set_state(BRIDGE_NONE, NULL);
    else if (g_state == BRIDGE_NONE && now - g_hello_at >= HELLO_RETRY_MS)
        send_hello();
}

int bridge_next_timeout(uint64_t now)
{
    if (g_fd < 0 || g_state == BRIDGE_READY || g_state == BRIDGE_NOKEY)
        return -1;
    uint64_t due = g_hello_at + (g_state == BRIDGE_UNKNOWN ? HELLO_TIMEOUT_MS : HELLO_RETRY_MS);
    return due > now ? (int)(due - now) : 0;
}
