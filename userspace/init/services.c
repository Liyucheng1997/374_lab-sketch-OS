/*
 * services.c — 服务表：解析 /etc/sketch/services.conf，启动服务，看护它们
 *
 * “服务”就是 1 号进程负责启动并照看的长期运行的程序，例如串口上的 shell、图形桌面 sketchd。
 * 每一行配置的格式（见 docs/开发规范.md 4.3）：
 *
 *     名称   策略   终端   [选项...]   命令 参数...
 *     serial respawn /dev/ttyS0        /bin/sksh -l
 *
 * 启动一个服务的经典步骤是 fork + exec：
 *   fork()   把 sinit 自己复制一份，得到一个子进程（此时子进程运行的还是 sinit 的代码）
 *   在子进程里“布置舞台”：新会话、控制终端、标准输入输出、信号设置、工作目录……
 *   execve() 把子进程的程序整个换成服务程序（pid 不变，打开的文件和会话也保留）
 * 把“创建进程”和“换程序”拆成两步，正是为了让中间这段布置工作可以用普通代码完成。
 */
#define _GNU_SOURCE
#include "sinit.h"
#include "skutil.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

int g_is_pid1;

void sinit_report(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (g_is_pid1)
        sk_log_console("%s", msg);
    else
        fprintf(stderr, "sinit: %s\n", msg);
}

const char *svc_state_name(svc_state st)
{
    switch (st) {
    case SVC_RUNNING:  return "running";
    case SVC_FAILED:   return "failed";
    case SVC_DONE:     return "done";
    case SVC_DISABLED: return "disabled";
    case SVC_STOPPED:
    default:           return "stopped";
    }
}

const char *svc_policy_name(svc_policy p)
{
    switch (p) {
    case POLICY_ONCE: return "once";
    case POLICY_WAIT: return "wait";
    default:          return "respawn";
    }
}

static const char *cond_name(svc_cond c)
{
    return c == COND_GUI ? "gui" : c == COND_NOGUI ? "nogui" : "";
}

void svc_free(service *s)
{
    if (!s)
        return;
    free(s->argbuf);
    free(s);
}

/* ======================= 解析配置 ======================= */

static int valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= SVC_NAME_MAX)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.')
            return 0;
    return 1;
}

/* 形如 key=value 且 key 全是小写字母的词，是选项（命令本身不会长这样） */
static int is_option(const char *t)
{
    const char *eq = strchr(t, '=');
    if (!eq || eq == t)
        return 0;
    for (const char *p = t; p < eq; p++)
        if (!islower((unsigned char)*p) && *p != '_')
            return 0;
    return 1;
}

/* 把参数重新拼成一行给人看；含空格的参数加上引号。状态文件用 Tab 分列，所以不能出现 Tab。 */
static void build_cmdline(service *s)
{
    s->cmdline[0] = 0;
    for (int i = 0; i < s->argc; i++) {
        const char *a = s->argv[i];
        int quote = !*a || strpbrk(a, " \t") != NULL;
        if (i)
            sk_strlcat(s->cmdline, " ", sizeof s->cmdline);
        if (quote)
            sk_strlcat(s->cmdline, "\"", sizeof s->cmdline);
        sk_strlcat(s->cmdline, a, sizeof s->cmdline);
        if (quote)
            sk_strlcat(s->cmdline, "\"", sizeof s->cmdline);
    }
    for (char *p = s->cmdline; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r')
            *p = ' ';
}

/* 解析一行。返回 1 成功，-1 出错（已报告）。where 是 "文件名:行号"，报错时用。 */
static int parse_line(const char *where, const char *text, service *s)
{
    memset(s, 0, sizeof *s);
    char *buf = strdup(text);
    if (!buf) {
        sinit_report("%s: 内存不足", where);
        return -1;
    }
    char *tok[3 + 8 + SVC_ARGV_MAX + 1];
    int nt = sk_split_args(buf, tok, (int)SK_ARRAY_LEN(tok));

#define FAIL(...) do { char m_[256]; snprintf(m_, sizeof m_, __VA_ARGS__); \
                       sinit_report("%s: %s", where, m_); free(buf); return -1; } while (0)

    if (nt == (int)SK_ARRAY_LEN(tok))
        FAIL("这一行的参数太多了（命令最多 %d 个参数）", SVC_ARGV_MAX);
    if (nt < 4)
        FAIL("格式不对，应该是：名称 策略 终端 [选项...] 命令 [参数...]");

    if (!valid_name(tok[0]))
        FAIL("服务名 \"%s\" 不合法（只能用字母、数字、_ - .，最长 %d 个字符）", tok[0], SVC_NAME_MAX - 1);
    sk_strlcpy(s->name, tok[0], sizeof s->name);

    if (strcmp(tok[1], "respawn") == 0)
        s->policy = POLICY_RESPAWN;
    else if (strcmp(tok[1], "once") == 0)
        s->policy = POLICY_ONCE;
    else if (strcmp(tok[1], "wait") == 0)
        s->policy = POLICY_WAIT;
    else
        FAIL("未知的策略 \"%s\"（可选 respawn、once、wait）", tok[1]);

    if (strcmp(tok[2], "-") != 0 && (tok[2][0] != '/' || strlen(tok[2]) >= SVC_PATH_MAX))
        FAIL("终端 \"%s\" 不对：要写设备路径（如 /dev/ttyS0），没有终端写 -", tok[2]);
    sk_strlcpy(s->tty, tok[2], sizeof s->tty);

    int i = 3;
    for (; i < nt && is_option(tok[i]); i++) {
        char *key = tok[i], *val = strchr(key, '=');
        *val++ = 0;
        if (strcmp(key, "log") == 0) {
            if (val[0] != '/' || strlen(val) >= SVC_PATH_MAX)
                FAIL("log= 要写绝对路径，例如 log=/var/log/%s.log", s->name);
            sk_strlcpy(s->log, val, sizeof s->log);
        } else if (strcmp(key, "cond") == 0) {
            if (strcmp(val, "gui") == 0)
                s->cond = COND_GUI;
            else if (strcmp(val, "nogui") == 0)
                s->cond = COND_NOGUI;
            else
                FAIL("cond= 只能是 gui 或 nogui，不能是 \"%s\"", val);
        } else if (strcmp(key, "delay") == 0) {
            char *end;
            long ms = strtol(val, &end, 10);
            if (!*val || *end || ms < 0 || ms > SVC_DELAY_MAX)
                FAIL("delay= 要写 0 到 %d 之间的毫秒数，不能是 \"%s\"", SVC_DELAY_MAX, val);
            s->delay_ms = (int)ms;
        } else {
            sinit_report("%s: 不认识的选项 \"%s=%s\"，已忽略（可用 log= cond= delay=）", where, key, val);
        }
    }
    if (i >= nt)
        FAIL("缺少要运行的命令");

    s->argc = nt - i;
    for (int k = 0; k < s->argc; k++)
        s->argv[k] = tok[i + k];
    s->argv[s->argc] = NULL;
    s->argbuf = buf;
    build_cmdline(s);
    s->state = SVC_STOPPED;
    return 1;
#undef FAIL
}

int svc_parse_file(const char *path, service **out, int max, int *errors)
{
    *errors = 0;
    FILE *f = fopen(path, "re");
    if (!f)
        return -1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char *line = NULL;
    size_t cap = 0;
    int lineno = 0, n = 0;
    while (getline(&line, &cap, f) >= 0) {
        lineno++;
        line[strcspn(line, "\r\n")] = 0;          /* 顺便容忍 Windows 的 CRLF 换行 */
        char *p = sk_trim(line);
        if (!*p || *p == '#')                      /* 空行和注释（# 必须是一行的第一个字符） */
            continue;

        char where[96];
        snprintf(where, sizeof where, "%s:%d", base, lineno);
        service *s = malloc(sizeof *s);
        if (!s || parse_line(where, p, s) < 0) {
            free(s);
            (*errors)++;
            continue;
        }
        s->line = lineno;

        int dup = -1;
        for (int k = 0; k < n; k++)
            if (strcmp(out[k]->name, s->name) == 0)
                dup = k;
        if (dup >= 0) {
            sinit_report("%s: 服务名 %s 重复了（第 %d 行已经用过），这一行被忽略", where, s->name, out[dup]->line);
            svc_free(s);
            (*errors)++;
            continue;
        }
        if (n >= max) {
            sinit_report("%s: 服务太多了（最多 %d 个），这一行被忽略", where, max);
            svc_free(s);
            (*errors)++;
            continue;
        }
        out[n++] = s;
    }
    free(line);
    fclose(f);
    return n;
}

/* ======================= sinit --check ======================= */

int svc_check_main(const char *path)
{
    service *list[SVC_MAX];
    int errors = 0;
    int n = svc_parse_file(path, list, SVC_MAX, &errors);
    if (n < 0) {
        fprintf(stderr, "sinit: 打不开 %s: %s\n", path, sk_strerror(errno));
        return 2;
    }
    printf("%s：%d 个服务，%d 处错误\n\n", path, n, errors);
    if (n > 0)
        printf("  名称         策略     终端          选项                              命令\n");
    for (int i = 0; i < n; i++) {
        service *s = list[i];
        char opts[256] = "";
        char tmp[160];
        if (s->cond != COND_ALWAYS) {
            snprintf(tmp, sizeof tmp, "cond=%s ", cond_name(s->cond));
            sk_strlcat(opts, tmp, sizeof opts);
        }
        if (s->delay_ms) {
            snprintf(tmp, sizeof tmp, "delay=%d ", s->delay_ms);
            sk_strlcat(opts, tmp, sizeof opts);
        }
        if (s->log[0]) {
            snprintf(tmp, sizeof tmp, "log=%s ", s->log);
            sk_strlcat(opts, tmp, sizeof opts);
        }
        char *o = sk_trim(opts);
        printf("  %-12s %-8s %-13s %-33s %s\n", s->name, svc_policy_name(s->policy), s->tty,
               *o ? o : "-", s->cmdline);
        svc_free(s);
    }
    return errors ? 1 : 0;
}

/* ======================= 1 号进程里的服务管理 ======================= */

static service *g_svc[SVC_MAX];
static int      g_nsvc;
static int      g_gui = 1;
static int      g_cursor;            /* 开机启动顺序进行到第几个服务 */
static service *g_waiting;           /* 正在等它结束的 wait 服务 */
static int      g_announced;         /* 已经打印过 "services started" */
static int      g_shutting_down;
static int      g_fallback_used;     /* 已经启动过后备 shell */

static void boot_advance(void);
static void after_change(service *s);

void svc_init(int gui)
{
    g_gui = gui;
}

static service *find_pid(pid_t pid)
{
    for (int i = 0; i < g_nsvc; i++)
        if (g_svc[i]->pid == pid)
            return g_svc[i];
    return NULL;
}

static service *find_name(const char *name)
{
    for (int i = 0; i < g_nsvc; i++)
        if (strcmp(g_svc[i]->name, name) == 0)
            return g_svc[i];
    return NULL;
}

static int cond_ok(const service *s)
{
    return s->cond == COND_ALWAYS || (s->cond == COND_GUI) == (g_gui != 0);
}

/* 状态文件：给 svc 命令看的“服务表快照”。
 * 先写临时文件，再 rename() 成正式文件名：rename 是原子操作，
 * 读的人要么看到完整的旧文件，要么看到完整的新文件，绝不会读到写了一半的内容。 */
void svc_write_status(void)
{
    if (!g_is_pid1)
        return;
    const char *tmp = SINIT_STATUS ".tmp";
    FILE *f = fopen(tmp, "we");
    if (!f) {
        sk_mkdir_p(SINIT_STATUS_DIR, 0755);
        f = fopen(tmp, "we");
        if (!f) {
            sk_log("无法写状态文件 %s: %s", tmp, sk_strerror(errno));
            return;
        }
    }
    fprintf(f, "# name\tstate\tpid\trestarts\tcommand\n");
    for (int i = 0; i < g_nsvc; i++) {
        const service *s = g_svc[i];
        fprintf(f, "%s\t%s\t%d\t%d\t%s\n", s->name, svc_state_name(s->state), (int)s->pid,
                s->restarts, s->cmdline);
    }
    if (fclose(f) != 0 || rename(tmp, SINIT_STATUS) != 0)
        sk_log("写状态文件失败: %s", sk_strerror(errno));
}

/* 在 PATH 里找程序（命令里没有 / 时），找到返回 0 */
static int resolve_exec(const char *cmd, char *out, size_t n)
{
    if (strchr(cmd, '/')) {
        sk_strlcpy(out, cmd, n);
        return access(out, X_OK);
    }
    const char *path = sys_env_get("PATH");
    if (!path)
        path = "/bin:/sbin";
    while (*path) {
        size_t len = strcspn(path, ":");
        snprintf(out, n, "%.*s/%s", (int)len, path, cmd);
        if (len && access(out, X_OK) == 0)
            return 0;
        path += len;
        if (*path == ':')
            path++;
    }
    return -1;
}

/* 把终端恢复成“正常”的行编辑模式（相当于 stty sane），就像 getty 在登录前做的那样。
 * 上一个程序如果崩溃时终端还处在原始模式（不回显、不处理退格），新 shell 就会很难用。 */
static void tty_sane(int fd)
{
    struct termios t;
    if (tcgetattr(fd, &t) != 0)
        return;
    t.c_iflag &= ~(IGNBRK | INLCR | IGNCR | IXOFF | PARMRK | ISTRIP);
    t.c_iflag |= BRKINT | ICRNL | IXON | IMAXBEL | IUTF8;   /* IUTF8：退格时按 UTF-8 字符删，汉字不会删半个 */
    t.c_oflag &= ~(OCRNL | ONOCR | ONLRET);
    t.c_oflag |= OPOST | ONLCR;                                /* 输出时把 \n 变成 \r\n */
    t.c_lflag &= ~(ECHONL | NOFLSH | TOSTOP);
    t.c_lflag |= ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    /* CLOCAL：忽略调制解调器的“载波检测”线。不设的话，打开串口可能会一直等“电话接通” */
    t.c_cflag |= CREAD | CLOCAL;
    t.c_cc[VINTR] = 3;      /* Ctrl+C */
    t.c_cc[VQUIT] = 0x1c;   /* Ctrl+\ */
    t.c_cc[VERASE] = 0x7f;  /* 退格键 */
    t.c_cc[VKILL] = 0x15;   /* Ctrl+U */
    t.c_cc[VEOF] = 4;       /* Ctrl+D */
    t.c_cc[VSTART] = 0x11;  /* Ctrl+Q */
    t.c_cc[VSTOP] = 0x13;   /* Ctrl+S */
    t.c_cc[VSUSP] = 0x1a;   /* Ctrl+Z */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);

    /* 串口线并不知道对面的屏幕有多大（屏幕终端 tty1 的大小由内核根据分辨率设置），
     * 如果是 0x0 就给个常见的默认值，免得程序排版出错 */
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && (ws.ws_col == 0 || ws.ws_row == 0)) {
        ws.ws_col = 80;
        ws.ws_row = 24;
        ioctl(fd, TIOCSWINSZ, &ws);
    }
}

/* 子进程里：布置好运行环境，然后 execve 变成服务程序。这个函数不会返回。 */
static void __attribute__((noreturn)) child_exec(const service *s, const char *exe)
{
    /* 1. 信号：恢复默认处理方式、解除屏蔽（见 signals.c 的说明） */
    sig_reset_in_child();

    /* 2. 新会话。会话 (session) 是“一次登录”里所有进程的集合，它最多有一个“控制终端”。
     *    setsid() 让这个进程成为新会话的首领，同时脱离 sinit 的会话。
     *    只有会话首领才能获得控制终端，有了控制终端：
     *      - 在终端上按 Ctrl+C，内核才知道该给哪些进程发 SIGINT；
     *      - shell 才能做作业控制（前台/后台任务、Ctrl+Z）。 */
    if (setsid() < 0)
        sk_log("服务 %s：setsid 失败: %s", s->name, sk_strerror(errno));

    int fd = -1;
    if (strcmp(s->tty, "-") != 0) {
        /* O_NONBLOCK：打开串口时不要等“载波”；O_NOCTTY 后面用 TIOCSCTTY 显式设置控制终端 */
        fd = open(s->tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            sk_log("服务 %s：打开终端 %s 失败: %s，改用 /dev/null", s->name, s->tty, sk_strerror(errno));
        } else {
            int fl = fcntl(fd, F_GETFL);
            if (fl >= 0)
                fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            if (isatty(fd)) {
                /* 3. 把这个终端设为本会话的控制终端；内核同时把本进程组设为“前台进程组” */
                if (ioctl(fd, TIOCSCTTY, 0) != 0)
                    sk_log("服务 %s：无法把 %s 设为控制终端: %s", s->name, s->tty, sk_strerror(errno));
                tty_sane(fd);
            }
        }
    }
    if (fd < 0)
        fd = open("/dev/null", O_RDWR | O_NOCTTY);

    /* 4. 标准输入、输出、错误 (0/1/2) 都指向这个终端。
     *    程序并不知道自己连着的是串口、屏幕还是文件——它只管读写 0/1/2，这就是“一切皆文件”。 */
    if (fd >= 0) {
        for (int i = 0; i <= 2; i++)
            if (fd != i)
                dup2(fd, i);
        if (fd > 2)
            close(fd);
    }

    /* 5. log=文件：把输出（1 和 2）改为追加写到日志文件 */
    if (s->log[0]) {
        int lfd = open(s->log, O_WRONLY | O_CREAT | O_APPEND | O_NOCTTY | O_CLOEXEC, 0644);
        if (lfd < 0) {
            dprintf(2, "sinit: 无法打开日志文件 %s: %s\n", s->log, sk_strerror(errno));
        } else {
            dup2(lfd, 1);
            dup2(lfd, 2);
            close(lfd);
            long up = sys_uptime_centis();
            if (up < 0)
                up = 0;
            dprintf(1, "\n==== [%ld.%02ld] sinit: 启动服务 %s (pid %d): %s ====\n",
                    up / 100, up % 100, s->name, (int)getpid(), s->cmdline);
        }
    }

    /* 6. 其它进程属性：新建文件的默认权限掩码、工作目录（从家目录开始） */
    umask(022);
    if (chdir(SINIT_HOME) != 0 && chdir("/") != 0)
        sk_log("服务 %s：chdir 失败", s->name);

    /* 7. 换成服务程序。成功的话 execve 永远不返回。 */
    execve(exe, s->argv, sys_service_env());
    int e = errno;
    dprintf(2, "sinit: 无法运行 %s: %s\n", exe, sk_strerror(e));
    sk_log("服务 %s：execve(%s) 失败: %s", s->name, exe, sk_strerror(e));
    _exit(127);
}

static void svc_start(service *s)
{
    char exe[256];
    s->start_at = 0;
    if (g_shutting_down)
        return;
    if (resolve_exec(s->argv[0], exe, sizeof exe) != 0) {
        s->pid = 0;
        s->state = SVC_FAILED;
        sinit_report("服务 %s 无法启动：找不到可执行的程序 %s", s->name, s->argv[0]);
        svc_write_status();
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        sinit_report("服务 %s：fork 失败 (%s)，%d 毫秒后重试", s->name, sk_strerror(errno), RETRY_FORK_MS);
        s->state = SVC_STOPPED;
        s->start_at = now_ms() + RETRY_FORK_MS;
        return;
    }
    if (pid == 0)
        child_exec(s, exe);

    s->pid = pid;
    s->state = SVC_RUNNING;
    s->started_ms = now_ms();
    sk_log("启动服务 %s (pid %d): %s", s->name, (int)pid, s->cmdline);
    svc_write_status();
}

/* 安排一次启动：有 delay= 就定个闹钟，没有就立刻启动 */
static void schedule_start(service *s, int64_t now)
{
    if (s->delay_ms > 0) {
        s->state = SVC_STOPPED;
        s->start_at = now + s->delay_ms;
        svc_write_status();
    } else {
        svc_start(s);
    }
}

/* 后备方案：没有可用的服务表（或者所有服务都挂了）时，在控制台上启动一个 shell。
 * 成功加入服务表返回 1；找不到任何 shell 返回 0。 */
static int add_fallback(const char *why)
{
    static const char *const candidates[][2] = {
        { "/bin/sksh", "fallback respawn /dev/console /bin/sksh -l" },
        { "/bin/sh",   "fallback respawn /dev/console /bin/sh -l" },
    };
    if (find_name("fallback")) {
        g_fallback_used = 2;
        return 0;
    }
    for (size_t i = 0; i < SK_ARRAY_LEN(candidates); i++) {
        if (access(candidates[i][0], X_OK) != 0)
            continue;
        if (g_nsvc >= SVC_MAX)
            break;
        service *s = malloc(sizeof *s);
        if (!s || parse_line("后备服务", candidates[i][1], s) < 0) {
            free(s);
            break;
        }
        sinit_report("%s，在控制台上启动后备 shell：%s", why, s->cmdline);
        g_svc[g_nsvc++] = s;
        g_fallback_used = 1;
        return 1;
    }
    g_fallback_used = 2;      /* 不会再有后备方案了，之后也不再重复提示 */
    static const char *const lines[] = {
        "sinit 找不到任何可以运行的 shell（/bin/sksh、/bin/sh）。",
        "可能是根文件系统没有打包好：请重新运行 scripts/build.sh。",
        "sinit 会继续运行（1 号进程不能退出，否则内核会 panic），",
        "它仍然会回收进程、响应关机信号，但这里已经没有可以交互的程序了。",
        NULL,
    };
    static const char *const lines_en[] = {
        "sinit cannot find any shell to run (/bin/sksh, /bin/sh).",
        "The root file system is probably incomplete: run scripts/build.sh again.",
        "sinit keeps running (if PID 1 exited, the kernel would panic):",
        "it still reaps processes and obeys shutdown signals (Ctrl+Alt+Del).",
        NULL,
    };
    sinit_report("%s，而且找不到可以运行的 shell", why);
    con_box("糟糕：没有可以运行的程序", lines, "Oops: nothing to run", lines_en);
    return 0;
}

/* 开机启动顺序：按配置文件里的顺序一个个启动；遇到 wait 服务就停下来等它结束。
 * SIGHUP 重读配置时，新增的服务追加在表的末尾，也从这里接着启动。 */
static void boot_advance(void)
{
    int64_t now = now_ms();
    while (!g_shutting_down && !g_waiting && g_cursor < g_nsvc) {
        service *s = g_svc[g_cursor++];
        if (!cond_ok(s)) {
            s->state = SVC_DISABLED;
            sk_log("服务 %s 要求 cond=%s，本次开机不启动", s->name, cond_name(s->cond));
            continue;
        }
        schedule_start(s, now);
        if (s->policy == POLICY_WAIT && (s->pid > 0 || s->start_at > 0)) {
            g_waiting = s;
            sk_log("等待服务 %s 结束后再启动后面的服务", s->name);
        }
    }
    svc_write_status();
    if (!g_shutting_down && !g_waiting && g_cursor >= g_nsvc && !g_announced) {
        g_announced = 1;
        sk_log_console("services started");
    }
}

/* 所有服务都停了（失败/完成/禁用），系统就没法用了：启动一次后备 shell 作为“急救” */
static void check_alive(void)
{
    if (g_shutting_down || !g_announced)
        return;
    for (int i = 0; i < g_nsvc; i++)
        if (g_svc[i]->pid > 0 || g_svc[i]->start_at > 0)
            return;
    if (!g_fallback_used) {
        if (add_fallback("已经没有正在运行的服务了"))
            boot_advance();
    } else if (g_fallback_used == 1) {
        g_fallback_used = 2;
        sinit_report("所有服务（包括后备 shell）都停止了；sinit 继续运行，可以按 Ctrl+Alt+Del 重启");
    }
}

/* 某个服务的状态变了之后：如果开机流程正在等它，就继续往下走 */
static void after_change(service *s)
{
    if (s && g_waiting == s && s->pid == 0 && s->start_at == 0) {
        g_waiting = NULL;
        boot_advance();
    }
    check_alive();
}

void svc_load_and_boot(void)
{
    int errors = 0;
    int n = svc_parse_file(SINIT_CONF, g_svc, SVC_MAX, &errors);
    if (n < 0) {
        char why[160];
        snprintf(why, sizeof why, "打不开 %s (%s)", SINIT_CONF, sk_strerror(errno));
        add_fallback(why);
    } else {
        g_nsvc = n;
        sk_log("读取 %s：%d 个服务，%d 处错误", SINIT_CONF, n, errors);
        if (n == 0)
            add_fallback("服务表里没有可用的服务");
    }
    boot_advance();
    check_alive();
}

int svc_child_exited(pid_t pid, int status)
{
    service *s = find_pid(pid);
    if (!s)
        return 0;

    /* 退出状态：正常退出取退出码；被信号杀死按 shell 的习惯记成 128+信号编号（如 SIGTERM → 143） */
    int code = WIFEXITED(status) ? WEXITSTATUS(status)
             : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : status;
    int64_t now = now_ms();
    s->pid = 0;
    s->last_status = code;
    if (WIFSIGNALED(status))
        sk_log("服务 %s (pid %d) 被信号 %s 结束", s->name, (int)pid, sig_name(WTERMSIG(status)));

    if (g_shutting_down) {
        s->state = SVC_STOPPED;
        svc_write_status();
        return 1;
    }

    if (s->policy != POLICY_RESPAWN) {
        s->state = code == 0 ? SVC_DONE : SVC_FAILED;
        if (code == 0)
            sk_log("服务 %s 已完成", s->name);
        else
            sinit_report("服务 %s 结束了，退出状态 %d", s->name, code);
        svc_write_status();
        after_change(s);
        return 1;
    }

    /* respawn：记下这次退出的时刻，数一数最近 10 秒内退出了几次。
     * 如果一个程序一启动就崩溃，无限重启只会刷屏、白白占满 CPU，所以要限制频率。 */
    s->exit_ms[s->exit_pos] = now;
    s->exit_pos = (s->exit_pos + 1) % (RESPAWN_MAX_EXITS + 1);
    int recent = 0;
    for (int i = 0; i <= RESPAWN_MAX_EXITS; i++)
        if (s->exit_ms[i] > 0 && now - s->exit_ms[i] <= RESPAWN_WINDOW_MS)
            recent++;
    if (recent > RESPAWN_MAX_EXITS) {
        s->state = SVC_FAILED;
        sk_log_console("service %s exited (status %d), giving up（%d 秒内退出了 %d 次，不再重启）",
                       s->name, code, RESPAWN_WINDOW_MS / 1000, recent);
        svc_write_status();
        after_change(s);
        return 1;
    }
    s->restarts++;
    sk_log_console("service %s exited (status %d), respawning", s->name, code);
    schedule_start(s, now);
    after_change(s);
    return 1;
}

void svc_run_timers(int64_t now)
{
    for (int i = 0; i < g_nsvc; i++) {
        service *s = g_svc[i];
        if (s->start_at > 0 && s->start_at <= now && s->pid == 0) {
            svc_start(s);
            after_change(s);
        }
    }
}

int svc_next_timeout(int64_t now)
{
    int64_t best = -1;
    for (int i = 0; i < g_nsvc; i++) {
        int64_t t = g_svc[i]->start_at;
        if (t > 0 && (best < 0 || t < best))
            best = t;
    }
    if (best < 0)
        return -1;
    int64_t d = best - now;
    return d <= 0 ? 0 : d > INT_MAX ? INT_MAX : (int)d;
}

void svc_reload(void)
{
    service *fresh[SVC_MAX];
    int errors = 0;
    int n = svc_parse_file(SINIT_CONF, fresh, SVC_MAX, &errors);
    if (n < 0) {
        sinit_report("重新读取 %s 失败: %s（服务保持不变）", SINIT_CONF, sk_strerror(errno));
        return;
    }
    int added = 0;
    for (int i = 0; i < n; i++) {
        if (find_name(fresh[i]->name) || g_nsvc >= SVC_MAX) {
            /* 已有的服务保持原样（修改已有服务的配置需要重启系统才生效） */
            svc_free(fresh[i]);
            continue;
        }
        g_svc[g_nsvc++] = fresh[i];
        added++;
    }
    sinit_report("重新读取了服务表：新增 %d 个服务%s", added, errors ? "（有错误，见上面）" : "");
    boot_advance();
    check_alive();
}

int svc_list_ttys(const char **out, int max)
{
    int n = 0;
    for (int i = 0; i < g_nsvc && n < max; i++) {
        const char *t = g_svc[i]->tty;
        if (strcmp(t, "-") == 0)
            continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (strcmp(out[k], t) == 0)
                dup = 1;
        if (!dup)
            out[n++] = t;
    }
    return n;
}

void svc_shutdown_begin(void)
{
    g_shutting_down = 1;
    for (int i = 0; i < g_nsvc; i++) {
        g_svc[i]->start_at = 0;
        if (g_svc[i]->pid == 0 && g_svc[i]->state == SVC_RUNNING)
            g_svc[i]->state = SVC_STOPPED;
    }
    svc_write_status();
}
