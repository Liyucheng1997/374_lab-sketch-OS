/*
 * svc — 查看 sinit 管理的服务
 *
 * 1 号进程 sinit 按 /etc/sketch/services.conf 启动并看护各个服务（串口 shell、图形桌面……），
 * 服务退出了就按策略重启。sinit 把每个服务的当前状态写进 /run/sinit/status（一个文本文件，
 * 每行用制表符分隔：名称 状态 进程号 重启次数 命令），svc 负责把它排版得好看一些。
 * 这是进程间通信最朴素的方式之一：一方写文件，另一方读文件。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(svc, "查看 sinit 管理的服务的状态")

static const char usage[] =
    "用法: svc [list]                 列出所有服务\n"
    "  或: svc status 名称            显示一个服务的详细信息\n"
    "  或: svc 名称                   同上\n"
    "  或: svc log 名称 [-n 行数]     显示服务日志的最后几行（默认 20 行）\n"
    "服务的状态由 1 号进程 sinit 写在 /run/sinit/status 里：\n"
    "  运行中 running   已停止 stopped   失败 failed（崩溃太频繁，已放弃重启）\n"
    "  已完成 done（once/wait 类型的服务正常结束）   未启用 disabled（条件不满足，如 cond=gui）\n";

typedef struct {
    char *name, *state, *pid, *restarts, *cmd;
} svc_t;

static const char *status_path(void)
{
    /* 环境变量 SKBOX_SVC_STATUS 只是为了在没有 sinit 的环境里测试 svc */
    const char *p = getenv("SKBOX_SVC_STATUS");
    return p && *p ? p : "/run/sinit/status";
}

static int load(svc_t **out, char **buf)
{
    *buf = sk_read_file(status_path(), NULL);
    if (!*buf) {
        sb_err("找不到 %s：sinit 没有在运行（或者这里不是 SketchOS）", status_path());
        return -1;
    }
    int n = 0, cap = 16;
    svc_t *v = malloc(sizeof *v * (size_t)cap);
    if (!v)
        return -1;
    for (char *line = *buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (line[0] && line[0] != '#') {
            char *f[5] = { 0 };
            char *p = line;
            for (int k = 0; k < 5 && p; k++) {
                f[k] = p;
                char *tab = k < 4 ? strchr(p, '\t') : NULL;
                if (tab) {
                    *tab = 0;
                    p = tab + 1;
                } else {
                    p = NULL;
                }
            }
            if (f[1]) {
                if (n == cap) {
                    cap *= 2;
                    svc_t *nv = realloc(v, sizeof *v * (size_t)cap);
                    if (!nv)
                        break;
                    v = nv;
                }
                v[n].name = f[0];
                v[n].state = f[1];
                v[n].pid = f[2] ? f[2] : "-";
                v[n].restarts = f[3] ? f[3] : "0";
                v[n].cmd = f[4] ? f[4] : "";
                n++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    *out = v;
    return n;
}

static void state_style(const char *state, const char **zh, const char **mark, const char **color)
{
    static const struct { const char *en, *zh, *mark, *color; } t[] = {
        { "running", "运行中", "●", SB_GREEN },
        { "stopped", "已停止", "■", SB_YELLOW },
        { "failed", "失败", "✗", SB_RED SB_BOLD },
        { "done", "已完成", "✓", SB_CYAN },
        { "disabled", "未启用", "○", SB_GRAY },
    };
    for (size_t i = 0; i < SK_ARRAY_LEN(t); i++) {
        if (strcmp(state, t[i].en) == 0) {
            *zh = t[i].zh;
            *mark = t[i].mark;
            *color = t[i].color;
            return;
        }
    }
    *zh = state;
    *mark = "?";
    *color = "";
}

static int list(svc_t *v, int n)
{
    int color = sb_color_enabled(STDOUT_FILENO);
    int cols = isatty(STDOUT_FILENO) ? sk_term_width(STDOUT_FILENO) : 0;
    int wn = sk_utf8_width("服务");
    for (int i = 0; i < n; i++)
        wn = SK_MAX(wn, sk_utf8_width(v[i].name));
    if (color)
        fputs(SB_BOLD, stdout);
    sb_put_padded(stdout, "服务", wn, 1);
    fputs("  ", stdout);
    sb_put_padded(stdout, "状态", 10, 1);
    fputs("  ", stdout);
    sb_put_padded(stdout, "进程号", 6, 0);   /* 汉字按显示宽度对齐，不能用 printf 的 %6s */
    fputs("  ", stdout);
    sb_put_padded(stdout, "重启", 4, 0);
    printf("  命令%s\n", color ? SB_RESET : "");
    int counts[6] = { 0 };
    for (int i = 0; i < n; i++) {
        const char *zh, *mark, *col;
        state_style(v[i].state, &zh, &mark, &col);
        char st[64];
        snprintf(st, sizeof st, "%s %s", mark, zh);
        sb_put_padded(stdout, v[i].name, wn, 1);
        fputs("  ", stdout);
        if (color)
            fputs(col, stdout);
        sb_put_padded(stdout, st, 10, 1);
        if (color)
            fputs(SB_RESET, stdout);
        printf("  %6s  %4s  ", v[i].pid, v[i].restarts);
        int used = wn + 2 + 10 + 2 + 6 + 2 + 4 + 2;
        if (cols > used + 4) {
            size_t k = sk_utf8_fit(v[i].cmd, cols - used, NULL);
            fwrite(v[i].cmd, 1, k, stdout);
            putchar('\n');
        } else {
            puts(v[i].cmd);
        }
        if (!strcmp(v[i].state, "running")) counts[0]++;
        else if (!strcmp(v[i].state, "failed")) counts[1]++;
        else if (!strcmp(v[i].state, "stopped")) counts[2]++;
        else counts[3]++;
    }
    printf("\n共 %d 个服务：%d 个运行中", n, counts[0]);
    if (counts[1])
        printf("，%d 个失败", counts[1]);
    if (counts[2])
        printf("，%d 个已停止", counts[2]);
    if (counts[3])
        printf("，%d 个其它", counts[3]);
    putchar('\n');
    return 0;
}

/* 在 services.conf 里找这个服务的 log= 选项；找不到时按约定用 /var/log/<名称>.log */
static void log_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, "/var/log/%s.log", name);
    char *conf = sk_read_file("/etc/sketch/services.conf", NULL);
    if (!conf)
        return;
    for (char *line = conf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *argv[64];
        char tmp[1024];
        sk_strlcpy(tmp, line, sizeof tmp);
        char *hash = tmp[0] == '#' ? tmp : NULL;
        if (!hash) {
            int ac = sk_split_args(tmp, argv, 64);
            if (ac > 0 && strcmp(argv[0], name) == 0) {
                for (int k = 3; k < ac; k++)
                    if (strncmp(argv[k], "log=", 4) == 0)
                        sk_strlcpy(out, argv[k] + 4, n);
                break;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(conf);
}

static int show_status(svc_t *v, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(v[i].name, name) != 0)
            continue;
        const char *zh, *mark, *col;
        state_style(v[i].state, &zh, &mark, &col);
        int color = sb_color_enabled(STDOUT_FILENO);
        char lp[512];
        log_path(name, lp, sizeof lp);
        printf("服务:     %s\n", v[i].name);
        printf("状态:     %s%s %s%s（%s）\n", color ? col : "", mark, zh, color ? SB_RESET : "", v[i].state);
        printf("进程号:   %s\n", v[i].pid);
        printf("重启次数: %s\n", v[i].restarts);
        printf("命令:     %s\n", v[i].cmd);
        printf("日志:     %s%s\n", lp, access(lp, R_OK) == 0 ? "" : "（不存在）");
        if (strcmp(v[i].pid, "-") != 0 && strcmp(v[i].pid, "0") != 0) {
            sk_procinfo pi;
            if (sk_read_proc((pid_t)atoi(v[i].pid), &pi) == 0) {
                double up = 0;
                sk_read_uptime(&up, NULL);
                char dur[64], mem[32];
                double age = up - (double)pi.start_time / (double)sk_clk_tck();
                sk_fmt_duration(dur, sizeof dur, (uint64_t)(age > 0 ? age : 0));
                sk_fmt_size_short(mem, sizeof mem, pi.rss_kb * 1024);
                printf("已运行:   %s\n", dur);
                printf("内存:     %s（常驻）\n", mem);
            }
        }
        return 0;
    }
    sb_err("没有叫 '%s' 的服务（用 svc 查看全部）", name);
    return 1;
}

static int show_log(const char *name, long long lines)
{
    char lp[512];
    log_path(name, lp, sizeof lp);
    size_t len;
    char *s = sk_read_file(lp, &len);
    if (!s) {
        sb_perr("无法读取服务 '%s' 的日志 '%s'", name, lp);
        return 1;
    }
    size_t pos = len;
    if (pos && s[pos - 1] == '\n')
        pos--;
    long long seen = 0;
    while (pos > 0) {
        if (s[pos - 1] == '\n' && ++seen == lines)
            break;
        pos--;
    }
    fwrite(s + pos, 1, len - pos, stdout);
    free(s);
    return 0;
}

int applet_svc(int argc, char **argv)
{
    if (sb_help(argc, argv, usage))
        return 0;
    const char *cmd = argc > 1 ? argv[1] : "list";
    svc_t *v = NULL;
    char *buf = NULL;
    int rc;
    if (strcmp(cmd, "log") == 0) {
        if (argc < 3) {
            sb_err("用法: svc log 名称 [-n 行数]");
            return 1;
        }
        long long n = 20;
        if (argc >= 5 && strcmp(argv[3], "-n") == 0 && (sb_parse_ll(argv[4], &n) != 0 || n < 0)) {
            sb_err("无效的行数 '%s'", argv[4]);
            return 1;
        }
        return show_log(argv[2], n);
    }
    int n = load(&v, &buf);
    if (n < 0)
        return 1;
    if (strcmp(cmd, "list") == 0)
        rc = list(v, n);
    else if (strcmp(cmd, "status") == 0) {
        if (argc < 3) {
            sb_err("用法: svc status 名称");
            rc = 1;
        } else {
            rc = show_status(v, n, argv[2]);
        }
    } else
        rc = show_status(v, n, cmd);
    free(v);
    free(buf);
    return rc;
}
