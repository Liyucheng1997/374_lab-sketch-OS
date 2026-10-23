/*
 * ps — 列出进程
 *
 * 内核用 task_struct 结构记录每个进程，并通过 /proc/<pid>/ 把它们“展示”给用户态。
 * ps 没有任何特权，它只是把 /proc 下的数字目录挨个读一遍（实现见 lib_proc.c 和 skutil.c）。
 * 进程状态：R 运行/可运行  S 睡眠（等待事件）  D 不可中断的睡眠（通常在等磁盘）
 *           T 被暂停  Z 僵尸（已退出、等父进程 wait 收尸）  I 空闲的内核线程
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(ps, "列出系统中的进程")

static const char usage[] =
    "用法: ps [选项]\n"
    "列出所有进程。内核线程（没有命令行的进程）显示在方括号里，例如 [kthreadd]。\n\n"
    "  （不带选项）  PID TTY TIME CMD：进程号、控制终端、累计 CPU 时间、命令\n"
    "  aux 或 -a     详细格式：用户、%CPU、%MEM、虚拟内存 VSZ、常驻内存 RSS（KB）、状态、启动时间、完整命令行\n"
    "  -f            完整格式：UID PID PPID C STIME TTY TIME CMD\n"
    "  -e, -A        所有进程（默认就是所有进程）\n"
    "  -p 进程号     只显示这些进程（用逗号分隔，可以多次使用）\n"
    "  w             输出到终端时也不截断长命令行\n\n"
    "STAT 列：R 运行  S 睡眠  D 不可中断睡眠  T 暂停  Z 僵尸  I 空闲内核线程\n"
    "         < 高优先级  N 低优先级  s 会话首进程  l 多线程  + 在终端的前台进程组\n";

enum { FMT_DEFAULT, FMT_AUX, FMT_FULL };

static pid_t *g_pids;
static int g_npids;

static int add_pids(const char *list)
{
    char *copy = strdup(list), *save = NULL;
    if (!copy)
        return -1;
    for (char *t = strtok_r(copy, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        long long v;
        if (sb_parse_ll(t, &v) != 0 || v <= 0) {
            sb_err("无效的进程号 '%s'", t);
            free(copy);
            return -1;
        }
        pid_t *np = realloc(g_pids, sizeof *np * (size_t)(g_npids + 1));
        if (!np) {
            free(copy);
            return -1;
        }
        g_pids = np;
        g_pids[g_npids++] = (pid_t)v;
    }
    free(copy);
    return 0;
}

static int wanted(pid_t pid)
{
    if (!g_npids)
        return 1;
    for (int i = 0; i < g_npids; i++)
        if (g_pids[i] == pid)
            return 1;
    return 0;
}

/* 截断到 max 列（按显示宽度），max<=0 表示不截断 */
static void put_cut(const char *s, int max)
{
    if (max <= 0) {
        fputs(s, stdout);
        return;
    }
    size_t n = sk_utf8_fit(s, max, NULL);
    fwrite(s, 1, n, stdout);
}

int applet_ps(int argc, char **argv)
{
    int fmt = FMT_DEFAULT, wide = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            fputs(usage, stdout);
            return 0;
        }
        if (a[0] != '-') {
            /* BSD 风格：ps aux（没有减号） */
            for (const char *p = a; *p; p++) {
                if (*p == 'u' || *p == 'a')
                    fmt = FMT_AUX;
                else if (*p == 'w')
                    wide = 1;
                else if (*p != 'x' && *p != 'e') {
                    sb_err("不认识的选项 '%c'", *p);
                    sb_hint();
                    return 1;
                }
            }
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'a':
            case 'u': fmt = FMT_AUX; break;
            case 'f': if (fmt != FMT_AUX) fmt = FMT_FULL; break;
            case 'e':
            case 'A':
            case 'x': break;
            case 'w': wide = 1; break;
            case 'p': {
                const char *list = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!list) {
                    sb_err("选项需要一个参数 -- 'p'");
                    sb_hint();
                    return 1;
                }
                if (add_pids(list) != 0)
                    return 1;
                goto next_arg;   /* -p 后面的字符都属于进程号列表，这个参数处理完了 */
            }
            default:
                sb_err("无效的选项 -- '%c'", *p);
                sb_hint();
                return 1;
            }
        }
    next_arg:;
    }

    int n;
    sb_proc *procs = sb_list_procs(&n);
    if (!procs) {
        sb_perr("无法读取 /proc（/proc 挂载了吗？）");
        return 1;
    }
    long hz = sk_clk_tck();
    double uptime = 0;
    sk_read_uptime(&uptime, NULL);
    sk_meminfo mi;
    sk_read_meminfo(&mi);
    long long boot = sb_boot_time();
    time_t now = time(NULL);
    int cols = isatty(STDOUT_FILENO) && !wide ? sk_term_width(STDOUT_FILENO) : 0;

    if (fmt == FMT_AUX)
        printf("%-8s %7s %4s %4s %6s %5s %-8s %-4s %5s %6s %s\n", "USER", "PID", "%CPU", "%MEM", "VSZ",
               "RSS", "TTY", "STAT", "START", "TIME", "COMMAND");
    else if (fmt == FMT_FULL)
        printf("%-8s %7s %7s %2s %5s %-8s %8s %s\n", "UID", "PID", "PPID", "C", "STIME", "TTY", "TIME", "CMD");
    else
        printf("%5s %-8s %8s %s\n", "PID", "TTY", "TIME", "CMD");

    int found = 0;
    for (int i = 0; i < n; i++) {
        sb_proc *p = &procs[i];
        sk_procinfo *pi = &p->info;
        if (!wanted(pi->pid))
            continue;
        found++;
        char tty[32], cmd[300], user[64], line[512];
        sb_tty_name(p->tty_nr, tty, sizeof tty);
        sb_proc_cmd(pi, cmd, sizeof cmd);
        long long cpu_secs = (long long)((pi->utime + pi->stime) / (uint64_t)hz);
        /* 进程运行了多久 = 开机时长 - 进程启动时（开机后第几个 tick） */
        double elapsed = uptime - (double)pi->start_time / (double)hz;
        double pcpu = elapsed > 0 ? (double)(pi->utime + pi->stime) / (double)hz / elapsed * 100.0 : 0;
        if (pcpu > 999.9)
            pcpu = 999.9;
        time_t started = (time_t)(boot + (long long)(pi->start_time / (uint64_t)hz));
        struct tm tm;
        localtime_r(&started, &tm);
        char start[16];
        if (now - started < 24 * 3600)
            snprintf(start, sizeof start, "%02d:%02d", tm.tm_hour, tm.tm_min);
        else if (now - started < 365 * 24 * 3600L)
            snprintf(start, sizeof start, "%02d-%02d", tm.tm_mon + 1, tm.tm_mday);
        else
            snprintf(start, sizeof start, "%d", tm.tm_year + 1900);

        int len;
        if (fmt == FMT_AUX) {
            sk_user_name(p->uid, user, sizeof user);
            if (strlen(user) > 8) {   /* 太长的用户名截断，末尾加 + 提示 */
                user[7] = '+';
                user[8] = 0;
            }
            char stat[8];
            int k = 0;
            stat[k++] = pi->state;
            if (pi->nice < 0)
                stat[k++] = '<';
            else if (pi->nice > 0)
                stat[k++] = 'N';
            if (pi->pid == p->session)
                stat[k++] = 's';
            if (pi->threads > 1)
                stat[k++] = 'l';
            if (p->tty_nr && p->tpgid == p->pgrp)
                stat[k++] = '+';
            stat[k] = 0;
            double pmem = mi.total_kb ? (double)pi->rss_kb * 100.0 / (double)mi.total_kb : 0;
            char tbuf[32];
            snprintf(tbuf, sizeof tbuf, "%lld:%02lld", cpu_secs / 60, cpu_secs % 60);
            len = snprintf(line, sizeof line, "%-8s %7d %4.1f %4.1f %6llu %5llu %-8s %-4s %5s %6s ", user,
                           (int)pi->pid, pcpu, pmem, (unsigned long long)pi->vsize_kb,
                           (unsigned long long)pi->rss_kb, tty, stat, start, tbuf);
        } else {
            char tbuf[32];
            snprintf(tbuf, sizeof tbuf, "%02lld:%02lld:%02lld", cpu_secs / 3600, cpu_secs / 60 % 60, cpu_secs % 60);
            if (fmt == FMT_FULL) {
                sk_user_name(p->uid, user, sizeof user);
                len = snprintf(line, sizeof line, "%-8s %7d %7d %2d %5s %-8s %8s ", user, (int)pi->pid,
                               (int)pi->ppid, (int)(pcpu > 99 ? 99 : pcpu), start, tty, tbuf);
            } else {
                len = snprintf(line, sizeof line, "%5d %-8s %8s ", (int)pi->pid, tty, tbuf);
            }
        }
        fputs(line, stdout);
        /* 默认格式显示进程名，其它格式显示完整命令行 */
        const char *shown = fmt == FMT_DEFAULT ? (pi->is_kernel_thread || !pi->cmdline[0] ? cmd : pi->comm) : cmd;
        put_cut(shown, cols ? cols - len : 0);
        putchar('\n');
    }
    free(procs);
    return g_npids && !found ? 1 : 0;
}
