/*
 * cli.c — 命令行里的 AI：ai
 *
 *   ai 帮我看看内存还剩多少          问一个问题，AI 可能会运行命令、读写文件来完成它
 *   ps | ai 哪个进程占内存最多？      标准输入是管道时，把管道里的内容附在问题后面
 *   ai                               进入对话模式（多轮对话；/new 新对话，/exit 或 Ctrl+D 退出）
 *   ai --status                      看看 AI 服务的状态
 * 选项：-q 不显示工具调用过程；-v 显示模型的思考摘要。
 *
 * ai 自己什么都不“懂”：它只是连上 AI 服务 skaid（Unix 域套接字），把问题发过去，
 * 再把一段段流回来的文字打印出来——和图形应用 skai 用的是同一套协议（common/skai.h）。
 * 回答时按 Ctrl+C 会让 skaid 取消这次请求。
 */
#include "skaid.h"

#include <skutil.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PIPE_MAX (64 * 1024)

static volatile sig_atomic_t g_interrupted;
static int g_quiet, g_verbose, g_color;
static int g_at_line_start = 1;     /* 标准输出上次写到了行首吗（工具提示要另起一行） */

static void on_sigint(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

static const char *c(const char *code) { return g_color ? code : ""; }

static void out_text(const char *s)
{
    size_t n = strlen(s);
    if (!n)
        return;
    fwrite(s, 1, n, stdout);
    fflush(stdout);
    g_at_line_start = s[n - 1] == '\n';
}

static void newline_if_needed(void)
{
    if (!g_at_line_start) {
        fputc('\n', stdout);
        fflush(stdout);
        g_at_line_start = 1;
    }
}

/* 处理一条来自 skaid 的消息。返回 1 = 这个问题结束了 */
static int handle(const char *cmd, const char *arg, int *failed, int *thinking)
{
    if (!strcmp(cmd, "TEXT")) {
        if (*thinking) {
            newline_if_needed();
            *thinking = 0;
        }
        out_text(arg);
    } else if (!strcmp(cmd, "THINK")) {
        if (g_quiet)
            return 0;
        if (!*arg && !*thinking) {
            newline_if_needed();
            fprintf(stderr, "%s（思考中……）%s\n", c("\033[2m"), c("\033[0m"));
        } else if (*arg && g_verbose) {
            fprintf(stderr, "%s%s%s", c("\033[2;3m"), arg, c("\033[0m"));
        }
        *thinking = 1;
    } else if (!strcmp(cmd, "TOOL")) {
        if (!g_quiet) {
            newline_if_needed();
            fprintf(stderr, "%s▶ %s%s\n", c("\033[36m"), arg, c("\033[0m"));
        }
        *thinking = 0;
    } else if (!strcmp(cmd, "RESULT")) {
        if (!g_quiet) {
            int ok = !strncmp(arg, "ok", 2);
            const char *sum = strchr(arg, ' ');
            sum = sum ? sum + 1 : "";
            fprintf(stderr, "%s  → %s%s\n", c(ok ? "\033[2m" : "\033[31m"), *sum ? sum : (ok ? "完成" : "失败"),
                    c("\033[0m"));
        }
    } else if (!strcmp(cmd, "ERROR")) {
        newline_if_needed();
        fprintf(stderr, "%sai: %s%s\n", c("\033[31m"), arg, c("\033[0m"));
        *failed = 1;
    } else if (!strcmp(cmd, "DONE")) {
        newline_if_needed();
        if (strcmp(arg, "end_turn") != 0 && strcmp(arg, "stop_sequence") != 0)
            *failed = 1;
        return 1;
    }
    /* STATUS、BUSY、USAGE：命令行里不显示 */
    return 0;
}

/* 等 skaid 的消息直到这个问题结束。返回 0 成功、1 失败、130 被 Ctrl+C 打断、-1 连接断了 */
static int wait_answer(int fd, skai_reader *rd)
{
    int failed = 0, thinking = 0, cancelled = 0;
    for (;;) {
        if (g_interrupted && !cancelled) {
            skai_send(fd, "CANCEL", NULL);
            cancelled = 1;
        }
        char *line;
        while ((line = skai_reader_next(rd))) {
            char *arg;
            const char *cmd = skai_split(line, &arg);
            int done = handle(cmd, arg, &failed, &thinking);
            free(line);
            if (done)
                return cancelled ? 130 : failed;
        }
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, -1);
        if (r < 0 && errno == EINTR)
            continue;           /* Ctrl+C 打断了 poll：回到循环顶上发 CANCEL */
        long n = skai_reader_fill(rd, fd);
        if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
            newline_if_needed();
            fprintf(stderr, "ai: 和 AI 服务的连接断了\n");
            return -1;
        }
    }
}

/* 读第一条 STATUS（skaid 在连接建立时主动发）。返回状态名 */
static void read_status(int fd, skai_reader *rd, char *state, size_t sn, char *model, size_t mn,
                        char *note, size_t nn)
{
    state[0] = model[0] = note[0] = 0;
    for (int tries = 0; tries < 50; tries++) {
        char *line;
        while ((line = skai_reader_next(rd))) {
            char *arg;
            const char *cmd = skai_split(line, &arg);
            if (!strcmp(cmd, "STATUS")) {
                char *argv[3];
                char *dup = strdup(arg);
                int n = 0;
                /* 前两个字段是单词，剩下的都是说明 */
                char *p = dup, *sp;
                while (n < 2 && (sp = strchr(p, ' '))) {
                    *sp = 0;
                    argv[n++] = p;
                    p = sp + 1;
                }
                argv[n++] = p;
                sk_strlcpy(state, argv[0], sn);
                if (n > 1)
                    sk_strlcpy(model, argv[1], mn);
                if (n > 2)
                    sk_strlcpy(note, argv[2], nn);
                free(dup);
                free(line);
                return;
            }
            free(line);
        }
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 200) > 0 && skai_reader_fill(rd, fd) <= 0)
            return;
    }
}

static char *join_args(int argc, char **argv)
{
    size_t len = 1;
    for (int i = 0; i < argc; i++)
        len += strlen(argv[i]) + 1;
    char *s = calloc(1, len);
    if (!s)
        abort();
    for (int i = 0; i < argc; i++) {
        if (i)
            strcat(s, " ");
        strcat(s, argv[i]);
    }
    return s;
}

static char *read_stdin(void)
{
    size_t cap = 4096, len = 0;
    char *d = malloc(cap);
    if (!d)
        abort();
    ssize_t n;
    while (len < PIPE_MAX && (n = read(0, d + len, cap - len - 1)) > 0) {
        len += (size_t)n;
        if (cap - len < 1024 && cap < PIPE_MAX + 4096) {
            cap *= 2;
            d = realloc(d, cap);
            if (!d)
                abort();
        }
    }
    d[len] = 0;
    return d;
}

static void usage(void)
{
    fprintf(stderr,
            "用法: ai [-q] [-v] [问题...]\n"
            "  ai 帮我看看系统里有哪些进程     问一个问题（AI 可以运行命令、读写文件、打开应用）\n"
            "  ls -l | ai 哪个文件最大          把管道里的内容附在问题后面\n"
            "  ai                              对话模式：/new 新对话，/exit 或 Ctrl+D 退出\n"
            "  ai --status                     查看 AI 服务的状态\n"
            "  -q  不显示工具调用过程    -v  显示模型的思考摘要\n"
            "API 密钥和模型在网页上的「AI 设置」里设置（默认模型 claude-opus-5-5）。\n");
}

int cli_main(int argc, char **argv)
{
    int status_only = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-q")) g_quiet = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--status")) status_only = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { usage(); return 2; }
    }
    g_color = isatty(2) && getenv("TERM") && strcmp(getenv("TERM"), "dumb") != 0;
    signal(SIGPIPE, SIG_IGN);

    int fd = skai_connect();
    if (fd < 0) {
        fprintf(stderr, "ai: 连不上 AI 服务（%s）：skaid 没有运行？可以用 svc 命令看看服务状态\n", sk_strerror(errno));
        return 1;
    }
    skai_reader rd = { 0 };
    char state[16], model[64], note[256];
    read_status(fd, &rd, state, sizeof state, model, sizeof model, note, sizeof note);
    if (!strcmp(state, "waiting"))          /* skaid 刚启动、还在等网页回话：它一有结果就会再发一条 STATUS */
        read_status(fd, &rd, state, sizeof state, model, sizeof model, note, sizeof note);
    if (status_only) {
        printf("状态：%s\n模型：%s\n%s\n", state[0] ? state : "未知", model[0] ? model : "-", note);
        return strcmp(state, "ready") == 0 ? 0 : 1;
    }
    if (strcmp(state, "ready") != 0) {
        fprintf(stderr, "ai: %s\n", note[0] ? note : "AI 服务还没有准备好");
        return 1;
    }

    /* 用 sigaction 而不是 signal：不设 SA_RESTART，Ctrl+C 才能把我们从 poll()/fgets() 里叫醒 */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    if (i < argc || !isatty(0)) {
        /* 一次性提问 */
        char *q = i < argc ? join_args(argc - i, argv + i) : strdup("");
        if (!isatty(0)) {
            char *in = read_stdin();
            if (*in) {
                char *both = sk_strdup_printf("%s%s以下是通过管道传给我的内容：\n%s", q, *q ? "\n\n" : "", in);
                free(q);
                q = both;
            }
            free(in);
        }
        if (!*q) {
            usage();
            return 2;
        }
        skai_send(fd, "ASK", q);
        free(q);
        int rc = wait_answer(fd, &rd);
        close(fd);
        return rc < 0 ? 1 : rc;
    }

    /* 对话模式 */
    printf("%sSketchOS AI 助手%s（模型 %s）。直接输入问题；/new 开始新对话，/exit 退出。\n",
           c("\033[1m"), c("\033[0m"), model[0] ? model : "-");
    char line[4096];
    for (;;) {
        g_interrupted = 0;
        printf("%s你>%s ", c("\033[1;32m"), c("\033[0m"));
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
            if (g_interrupted && !feof(stdin)) {
                clearerr(stdin);
                putchar('\n');
                continue;
            }
            putchar('\n');
            break;
        }
        char *q = sk_trim(line);
        if (!*q)
            continue;
        if (!strcmp(q, "/exit") || !strcmp(q, "/quit") || !strcmp(q, "exit"))
            break;
        if (!strcmp(q, "/new")) {
            skai_send(fd, "RESET", NULL);
            printf("（已开始新的对话）\n");
            continue;
        }
        skai_send(fd, "ASK", q);
        g_at_line_start = 1;
        if (wait_answer(fd, &rd) < 0)
            return 1;
        if (g_interrupted)
            printf("（已取消）\n");
    }
    close(fd);
    return 0;
}
