/*
 * launch.c — 应用列表 (/etc/sketch/apps.conf) 与启动应用进程
 *
 * Unix 启动新程序分两步，这是操作系统课上最经典的一对系统调用：
 *   fork()  把当前进程“复制”一份（子进程和父进程几乎一模一样，内存是写时复制的，
 *           所以很快）；fork 在父进程里返回子进程的 pid，在子进程里返回 0。
 *   execve() 在子进程里把自己“替换”成另一个程序：代码、数据、堆栈全部换掉，
 *           但 pid、打开的文件（没设 CLOEXEC 的）、环境变量由我们决定。
 * 分成两步的好处是：fork 之后、exec 之前，子进程可以先布置好“新程序的环境”——
 * 新建会话、重置信号、关掉不该继承的文件、重定向标准输入……而不需要一个参数巨多的
 * “创建进程”函数。
 *
 * 子进程结束后会变成“僵尸”(zombie)：进程已经没了，但内核还保留着它的退出状态，
 * 等父进程用 waitpid() 来取。父进程不取，僵尸就一直占着进程表。所以 sketchd 收到
 * SIGCHLD（“有子进程状态变化”）时要调用 launch_reap_children() 回收它们。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static skd_app g_apps[SKD_MAX_APPS];
static int     g_napps;
static char    g_socket_path[108] = SK_SOCKET_PATH;

#define MAX_CHILDREN 64
typedef struct {
    pid_t    pid;
    char     id[SK_APPNAME_MAX];
    uint64_t start_ms;
} child_rec;
static child_rec g_children[MAX_CHILDREN];

/* ---------- apps.conf ---------- */

static int valid_id(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= SK_APPNAME_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static int parse_color(const char *s, sk_color *out)
{
    if (s[0] != '#' || strlen(s) != 7)
        return 0;
    for (int i = 1; i < 7; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    *out = 0xFF000000u | (uint32_t)strtoul(s + 1, NULL, 16);
    return 1;
}

/* 取出下一个用空白分隔的字段，返回指向字段的指针并把 *p 移到字段之后 */
static char *next_field(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return NULL;
    char *start = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s)
        *s++ = '\0';
    *p = s;
    return start;
}

int apps_load(const char *path)
{
    g_napps = 0;
    char *text = sk_read_file(path, NULL);
    if (!text) {
        sk_log("apps: 读取 %s 失败: %s（桌面上不会有应用图标）", path, strerror(errno));
        return 0;
    }
    int lineno = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        lineno++;
        char *s = sk_trim(line);
        if (!*s || *s == '#')
            continue;
        char *p = s;
        char *id = next_field(&p);
        char *icon = next_field(&p);
        char *name = next_field(&p);
        char *color = next_field(&p);
        char *cmd = p ? sk_trim(p) : NULL;
        sk_color col = 0;
        if (!id || !icon || !name || !color || !cmd || !*cmd) {
            sk_log("apps: %s 第 %d 行字段不够（需要: id 图标 名称 颜色 命令），忽略", path, lineno);
            continue;
        }
        if (!valid_id(id) || strlen(icon) >= 32 || strlen(name) >= 64 || strlen(cmd) >= 256) {
            sk_log("apps: %s 第 %d 行 id/图标/名称/命令 太长或含非法字符，忽略", path, lineno);
            continue;
        }
        if (!parse_color(color, &col)) {
            sk_log("apps: %s 第 %d 行颜色 \"%s\" 不是 #RRGGBB，忽略", path, lineno, color);
            continue;
        }
        if (cmd[0] != '/') {
            sk_log("apps: %s 第 %d 行命令必须是绝对路径: %s", path, lineno, cmd);
            continue;
        }
        if (apps_find(id)) {
            sk_log("apps: %s 第 %d 行 id \"%s\" 重复，忽略", path, lineno, id);
            continue;
        }
        if (g_napps >= SKD_MAX_APPS) {
            sk_log("apps: 应用太多，最多 %d 个", SKD_MAX_APPS);
            break;
        }
        skd_app *a = &g_apps[g_napps++];
        memset(a, 0, sizeof *a);
        sk_strlcpy(a->id, id, sizeof a->id);
        sk_strlcpy(a->icon_name, icon, sizeof a->icon_name);
        a->icon = sk_icon_from_name(icon);
        sk_strlcpy(a->name, name, sizeof a->name);
        a->color = col;
        sk_strlcpy(a->command, cmd, sizeof a->command);
    }
    free(text);
    sk_log("apps: 从 %s 读到 %d 个应用", path, g_napps);
    return g_napps;
}

int apps_count(void) { return g_napps; }

const skd_app *apps_get(int i) { return (i >= 0 && i < g_napps) ? &g_apps[i] : NULL; }

const skd_app *apps_find(const char *id)
{
    if (!id)
        return NULL;
    for (int i = 0; i < g_napps; i++)
        if (strcmp(g_apps[i].id, id) == 0)
            return &g_apps[i];
    return NULL;
}

void launch_set_socket_path(const char *path) { sk_strlcpy(g_socket_path, path, sizeof g_socket_path); }

/* ---------- 启动 ---------- */

/* 新程序的环境变量 = sketchd 自己的环境 + SKETCH_SOCKET（已有的同名变量被替换） */
static char **build_env(char *sockvar, size_t sockvar_len)
{
    size_t n = 0;
    while (environ && environ[n])
        n++;
    char **env = calloc(n + 2, sizeof(char *));
    if (!env)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!sk_startswith(environ[i], SK_SOCKET_ENV "="))
            env[k++] = environ[i];
    snprintf(sockvar, sockvar_len, "%s=%s", SK_SOCKET_ENV, g_socket_path);
    env[k++] = sockvar;
    env[k] = NULL;
    return env;
}

static void remember_child(pid_t pid, const char *id)
{
    for (int i = 0; i < MAX_CHILDREN; i++)
        if (g_children[i].pid == 0) {
            g_children[i].pid = pid;
            sk_strlcpy(g_children[i].id, id, sizeof g_children[i].id);
            g_children[i].start_ms = skd_now_ms();
            return;
        }
}

pid_t launch_app(const char *id, const char *arg)
{
    const skd_app *a = apps_find(id);
    if (!a) {
        sk_log("launch: apps.conf 里没有应用 \"%s\"", id ? id : "(null)");
        char msg[96];
        snprintf(msg, sizeof msg, "没有找到应用：%s", id ? id : "?");
        menu_toast(msg, 2500);
        return -1;
    }

    /* 命令行切分成 argv；%f 换成参数，没有参数时整个字段去掉 */
    char cmd[256];
    sk_strlcpy(cmd, a->command, sizeof cmd);
    char *tok[32];
    int nt = sk_split_args(cmd, tok, (int)SK_ARRAY_LEN(tok) - 1);
    static char expanded[32][512];
    char *argv[33];
    int argc = 0;
    for (int i = 0; i < nt; i++) {
        char *pf = strstr(tok[i], "%f");
        if (!pf) {
            argv[argc++] = tok[i];
            continue;
        }
        if (!arg || !*arg)
            continue;
        snprintf(expanded[argc], sizeof expanded[0], "%.*s%s%s",
                 (int)(pf - tok[i]), tok[i], arg, pf + 2);
        argv[argc] = expanded[argc];
        argc++;
    }
    argv[argc] = NULL;
    if (argc == 0)
        return -1;
    if (access(argv[0], X_OK) != 0) {
        sk_log("launch: %s 无法执行: %s", argv[0], strerror(errno));
        char msg[128];
        snprintf(msg, sizeof msg, "无法启动「%s」：找不到程序", a->name);
        menu_toast(msg, 3000);
        return -1;
    }

    char sockvar[160];
    char **env = build_env(sockvar, sizeof sockvar);
    if (!env)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        sk_log("launch: fork 失败: %s", strerror(errno));
        free(env);
        menu_toast("系统资源不足，无法启动应用", 3000);
        return -1;
    }
    if (pid == 0) {
        /* ---- 子进程 ---- */
        /* 新建会话：脱离 sketchd 的控制终端 tty1，应用不会收到终端的 SIGHUP/SIGINT */
        setsid();
        /* 信号：sketchd 屏蔽了 SIGCHLD/SIGTERM 等（交给 signalfd），还忽略了 SIGPIPE。
         * 信号屏蔽字和“忽略”设置都会被 exec 继承！必须恢复成默认，否则应用行为异常
         * （例如 shell 里的程序收不到 SIGINT，写断开的管道不会退出）。 */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int s = 1; s < NSIG; s++)
            signal(s, SIG_DFL);
        /* 标准输入换成 /dev/null（不能让应用去读 tty1：键盘已经由 sketchd 接管） */
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0 && nul != 0) {
            dup2(nul, 0);
            close(nul);
        }
        /* 关掉 0/1/2 以外所有继承来的文件描述符（套接字、显卡、输入设备……）。
         * 我们的 fd 都设置了 O_CLOEXEC，这里是“双保险”。 */
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
            for (int fd = 3; fd < 1024; fd++)
                close(fd);
        execve(argv[0], argv, env);
        /* 只有 exec 失败才会走到这里 */
        static const char err[] = "sketchd: execve 失败\n";
        ssize_t w = write(2, err, sizeof err - 1);
        (void)w;
        _exit(127);
    }
    free(env);
    remember_child(pid, a->id);
    sk_log_console("launch %s pid=%d", a->id, (int)pid);
    return pid;
}

void launch_reap_children(void)
{
    int status;
    pid_t pid;
    /* WNOHANG：没有已结束的子进程就立即返回。多个子进程同时退出时只会合并成一个
     * SIGCHLD，所以要循环把能收的都收掉。 */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        const skd_app *a = NULL;
        uint64_t age = 0;
        for (int i = 0; i < MAX_CHILDREN; i++)
            if (g_children[i].pid == pid) {
                a = apps_find(g_children[i].id);
                age = skd_now_ms() - g_children[i].start_ms;
                g_children[i].pid = 0;
                break;
            }
        if (WIFEXITED(status)) {
            sk_log("launch: 进程 %d (%s) 退出，状态 %d", (int)pid, a ? a->id : "?", WEXITSTATUS(status));
            if (a && WEXITSTATUS(status) == 127 && age < 5000) {
                char msg[128];
                snprintf(msg, sizeof msg, "无法启动「%s」", a->name);
                menu_toast(msg, 3000);
            }
        } else if (WIFSIGNALED(status)) {
            sk_log("launch: 进程 %d (%s) 被信号 %d 终止", (int)pid, a ? a->id : "?", WTERMSIG(status));
        }
    }
}

const char *launch_app_of_pid(pid_t pid)
{
    if (pid <= 0)
        return NULL;
    for (int i = 0; i < MAX_CHILDREN; i++)
        if (g_children[i].pid == pid)
            return g_children[i].id;
    return NULL;
}

void launch_autostart(void)
{
    char buf[256];
    if (!sk_cmdline_get("sketch.autostart", buf, sizeof buf) || !buf[0])
        return;
    char *save = NULL;
    for (char *id = strtok_r(buf, ",", &save); id; id = strtok_r(NULL, ",", &save)) {
        id = sk_trim(id);
        if (*id)
            launch_app(id, NULL);
    }
}
