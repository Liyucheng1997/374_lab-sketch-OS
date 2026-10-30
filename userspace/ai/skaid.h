/*
 * skaid.h — SketchOS 的 AI 服务（守护进程 skaid + 命令行 ai）内部共用的声明
 *
 * 各文件分工：
 *   main.c    事件循环（poll）、Unix 套接字服务端；argv[0] 是 "ai" 时改当命令行客户端
 *   bridge.c  第二个串口 /dev/ttyS1 上的“AI 桥”协议：把请求交给网页，收回流式事件
 *   agent.c   对话（会话）和智能体循环：提问 → 模型回答 → 调用工具 → 把结果交回模型 → ……
 *   tools.c   AI 可以使用的工具：运行命令、读写文件、列目录、打开应用、系统信息
 *   prompt.c  系统提示词（告诉模型它运行在什么样的电脑里）
 *   cli.c     命令行客户端 ai
 *   json.c    小型 JSON 库
 */
#ifndef SKAID_H
#define SKAID_H

#include "json.h"

#include <skai.h>
#include <stddef.h>
#include <stdint.h>

#define SKAID_BRIDGE_DEV   "/dev/ttyS1"
#define SKAID_MAX_SESSIONS 8
#define SKAID_MAX_STEPS    40         /* 一个问题里最多来回多少轮（防止模型陷入死循环） */
#define SKAID_MAX_TOKENS   32000      /* 每次回答最多输出多少 token */

/* ---------- 会话（agent.c） ---------- */
typedef enum {
    SESS_IDLE,          /* 等用户提问 */
    SESS_WAITING,       /* 请求已发给网页，等模型的流式回应 */
} sess_state;

typedef struct session {
    int fd;                     /* 客户端连接 */
    skai_reader rd;
    sess_state state;
    jnode *messages;            /* 完整的对话历史（JSON 数组），每次请求都整个发过去 */
    jnode *cur_content;         /* 正在接收的这条助手消息的 content 数组 */
    jnode *cur_json;            /* 每个 content 块的 tool_use 参数是一段段拼起来的 JSON 文本 */
    char  stop_reason[32];
    int   req_id;               /* 正在进行的请求编号（0 = 没有） */
    int   steps;                /* 这个问题已经请求了几次模型 */
    long  in_tokens, out_tokens;
    int   dead;                 /* 客户端已经断开（写失败），等主循环关闭 */
} session;

void agent_init(void);
session *agent_new_session(int fd);
void agent_close_session(session *s);
/* 处理客户端发来的一行（已拆分好命令和参数） */
void agent_on_client(session *s, const char *cmd, const char *arg);
/* 网页桥送来的事件 */
void agent_on_event(int req_id, const char *json, size_t len);
void agent_on_end(int req_id);
void agent_on_error(int req_id, const char *msg);
/* 桥的状态变了（READY/超时）：通知所有客户端 */
void agent_broadcast_status(void);
void agent_send_status(session *s);
/* 给客户端发一条消息（失败时关闭该会话由主循环处理） */
void client_send(session *s, const char *cmd, const char *arg);
void client_sendf(session *s, const char *cmd, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ---------- 网页桥（bridge.c） ---------- */
typedef enum {
    BRIDGE_UNKNOWN,     /* 刚启动，还没收到网页的回应 */
    BRIDGE_NONE,        /* 没有回应：不在网页版里（比如 QEMU） */
    BRIDGE_NOKEY,       /* 网页在，但还没填 API 密钥 */
    BRIDGE_READY,
} bridge_state;

int  bridge_open(void);                 /* 打开串口，返回 fd（失败 -1） */
int  bridge_fd(void);
void bridge_on_readable(void);
void bridge_tick(uint64_t now_ms);      /* 定期调用：超时判断、重新打招呼 */
int  bridge_next_timeout(uint64_t now_ms);  /* 距离下一次需要 tick 的毫秒数 */
bridge_state bridge_get_state(void);
const char *bridge_model(void);
/* 发出一个请求（body 是 Messages API 的请求体 JSON），返回请求编号；失败返回 -1 */
int  bridge_request(const char *body, size_t len);
void bridge_cancel(int req_id);

/* ---------- 工具（tools.c） ---------- */
jnode *tools_definitions(void);         /* 给 API 的 tools 数组 */
/* 执行一个工具。返回 malloc 的结果文字；*is_error 表示失败；*summary（malloc）是给界面看的一行摘要 */
char  *tools_run(const char *name, const jnode *input, int *is_error, char **summary);
/* 一行摘要描述工具调用（给界面显示 “运行命令：ls -l”） */
char  *tools_describe(const char *name, const jnode *input);

/* ---------- 系统提示词（prompt.c） ---------- */
const char *prompt_system(void);

/* ---------- 命令行客户端（cli.c） ---------- */
int cli_main(int argc, char **argv);

/* ---------- 杂项（main.c） ---------- */
uint64_t now_ms(void);
extern int g_debug;
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *clip(const char *s, int cols);     /* 调试日志用：取开头至多 cols 列（静态缓冲区） */

#endif /* SKAID_H */
