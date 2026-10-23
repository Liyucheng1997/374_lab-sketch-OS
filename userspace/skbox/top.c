/*
 * top — 实时显示进程的 CPU 和内存占用
 *
 * /proc/<pid>/stat 只给出“这个进程从启动到现在一共用了多少 CPU 时间”（单位：时钟滴答）。
 * 要知道“现在”的 CPU 占用率，必须采样两次再相减：
 *     %CPU = (这两次之间进程多用的滴答数) / (这两次之间流逝的滴答数) × 100
 * 所以 top 每隔几秒醒来一次、重新读一遍 /proc，再和上一次的数据做差。
 *
 * 终端方面：用 ANSI 转义序列把光标移回左上角重画整屏（而不是清屏，避免闪烁）；
 * 把终端切到“原始模式”，这样按一个键 top 立刻就能收到，不必等回车。
 */
#include "skbox.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(top, "实时显示占用 CPU 和内存最多的进程")

static const char usage[] =
    "用法: top [-b] [-n 次数] [-d 秒数]\n"
    "全屏显示系统概况和进程列表，每隔几秒刷新一次。\n\n"
    "  -d 秒数   刷新间隔（默认 2 秒，可以是小数）\n"
    "  -n 次数   刷新这么多次后退出\n"
    "  -b        批处理模式：不使用全屏和按键，直接把每次的结果打印出来（适合重定向到文件）\n\n"
    "按键：\n"
    "  q    退出               P    按 CPU 占用排序（默认）\n"
    "  M    按内存占用排序     T    按累计 CPU 时间排序     N    按进程号排序\n"
    "  c    切换显示进程名/完整命令行\n"
    "  k    结束一个进程（会询问进程号和信号）\n"
    "  空格 立即刷新\n";

enum { SORT_CPU, SORT_MEM, SORT_TIME, SORT_PID };

typedef struct {
    pid_t pid;
    uint64_t start;   /* 进程启动时刻：用来识别“进程号被新进程复用”的情况 */
    uint64_t ticks;
} prev_t;

typedef struct {
    sb_proc *p;
    double cpu;
} row_t;

static int g_sort = SORT_CPU, g_show_cmd, g_batch;
static double g_delay = 2.0;
static long g_hz;
static volatile sig_atomic_t g_quit, g_resized;
static sb_term g_term;
static int g_term_on;

/* 上一次采样的结果 */
static prev_t *g_prev;
static int g_nprev;
static sk_cpustat g_prev_cpu;
static int g_have_prev;

/* 这一次采样的结果 */
static sb_proc *g_procs;
static int g_nprocs;
static row_t *g_rows;
static sk_cpustat g_cpu_delta_base, g_cpu_now;
static char g_msg[256];

/* ---------- 输出缓冲：整屏先拼好，一次 write 出去 ---------- */

static char *g_out;
static size_t g_outlen, g_outcap;

static void out_raw(const char *s, size_t n)
{
    if (g_outlen + n + 1 > g_outcap) {
        size_t nc = (g_outcap ? g_outcap * 2 : 16384) + n;
        char *nb = realloc(g_out, nc);
        if (!nb)
            return;
        g_out = nb;
        g_outcap = nc;
    }
    memcpy(g_out + g_outlen, s, n);
    g_outlen += n;
}

static void out_str(const char *s)
{
    out_raw(s, strlen(s));
}

static void out_flush(void)
{
    sb_write_all(STDOUT_FILENO, g_out, g_outlen);
    g_outlen = 0;
}

/* 输出一行：按终端宽度截断（汉字占 2 列），全屏模式下用 ESC[K 擦掉行尾的旧内容 */
static void out_line(int cols, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out_line(int cols, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = cols > 0 ? sk_utf8_fit(buf, cols, NULL) : strlen(buf);
    out_raw(buf, n);
    if (!g_batch)
        out_str("\033[K");
    out_str("\n");
}

/* ---------- 终端 ---------- */

static void term_leave(void)
{
    if (!g_term_on)
        return;
    /* 显示光标、离开备用屏幕、恢复终端设置 */
    const char *s = "\033[?25h\033[?1049l";
    sb_write_all(STDOUT_FILENO, s, strlen(s));
    sb_term_restore(&g_term);
    g_term_on = 0;
}

static void term_enter(void)
{
    if (sb_term_raw(&g_term, STDIN_FILENO) != 0)
        return;
    /* 进入备用屏幕（退出后原来的终端内容会恢复）、隐藏光标 */
    const char *s = "\033[?1049h\033[?25l\033[H\033[2J";
    sb_write_all(STDOUT_FILENO, s, strlen(s));
    g_term_on = 1;
}

static void on_quit(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void on_winch(int sig)
{
    (void)sig;
    g_resized = 1;
}

/* Ctrl+Z：先恢复终端再真正暂停；被 fg 继续后重新进入全屏模式。
 * 这几步都是“异步信号安全”的函数，可以在信号处理函数里调用。 */
static void on_tstp(int sig)
{
    (void)sig;
    int was_on = g_term_on;
    term_leave();
    /* 换回默认动作（暂停），并解除处理函数执行期间对 SIGTSTP 的屏蔽，再给自己发一次 */
    struct sigaction dfl, mine;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGTSTP, &dfl, &mine);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTSTP);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    raise(SIGTSTP);               /* 进程在这里停住，直到收到 SIGCONT */
    sigprocmask(SIG_BLOCK, &set, NULL);
    sigaction(SIGTSTP, &mine, NULL);
    if (was_on)
        term_enter();
    g_resized = 1;
}

/* ---------- 采样 ---------- */

static int cmp_prev(const void *a, const void *b)
{
    const prev_t *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static uint64_t prev_ticks(pid_t pid, uint64_t start, int *found)
{
    prev_t key = { pid, 0, 0 };
    prev_t *hit = g_nprev ? bsearch(&key, g_prev, (size_t)g_nprev, sizeof key, cmp_prev) : NULL;
    if (hit && hit->start == start) {
        *found = 1;
        return hit->ticks;
    }
    *found = 0;
    return 0;
}

static void sample(void)
{
    sk_cpustat cpu;
    sk_read_cpustat(&cpu);
    free(g_procs);
    g_procs = sb_list_procs(&g_nprocs);
    if (!g_procs)
        g_nprocs = 0;
    free(g_rows);
    g_rows = calloc((size_t)(g_nprocs ? g_nprocs : 1), sizeof *g_rows);

    /* 两次采样之间，所有 CPU 一共流逝了多少滴答（/proc/stat 第一行各项之和的差） */
    uint64_t dt = g_have_prev && cpu.total > g_prev_cpu.total ? cpu.total - g_prev_cpu.total : 0;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1)
        ncpu = 1;
    double up = 0;
    sk_read_uptime(&up, NULL);

    for (int i = 0; i < g_nprocs && g_rows; i++) {
        sk_procinfo *pi = &g_procs[i].info;
        uint64_t ticks = pi->utime + pi->stime;
        int found;
        uint64_t old = prev_ticks(pi->pid, pi->start_time, &found);
        double pct;
        if (dt && found) {
            /* 单个进程最多占满一个 CPU：除以“每个 CPU 流逝的滴答数” */
            pct = (double)(ticks >= old ? ticks - old : 0) * 100.0 * (double)ncpu / (double)dt;
        } else {
            /* 第一次采样或新进程：用“从启动到现在的平均值”代替 */
            double age = up - (double)pi->start_time / (double)g_hz;
            pct = age > 0 ? (double)ticks / (double)g_hz / age * 100.0 : 0;
        }
        g_rows[i].p = &g_procs[i];
        g_rows[i].cpu = pct > 100.0 * (double)ncpu ? 100.0 * (double)ncpu : pct;
    }

    /* 保存这次的数据，留给下一次做差 */
    free(g_prev);
    g_prev = malloc(sizeof *g_prev * (size_t)(g_nprocs ? g_nprocs : 1));
    g_nprev = 0;
    for (int i = 0; g_prev && i < g_nprocs; i++) {
        g_prev[g_nprev].pid = g_procs[i].info.pid;
        g_prev[g_nprev].start = g_procs[i].info.start_time;
        g_prev[g_nprev].ticks = g_procs[i].info.utime + g_procs[i].info.stime;
        g_nprev++;
    }
    g_cpu_delta_base = g_have_prev ? g_prev_cpu : cpu;
    g_cpu_now = cpu;
    g_prev_cpu = cpu;
    g_have_prev = 1;
}

static int cmp_rows(const void *a, const void *b)
{
    const row_t *x = a, *y = b;
    const sk_procinfo *p = &x->p->info, *q = &y->p->info;
    int r = 0;
    switch (g_sort) {
    case SORT_CPU:
        r = (x->cpu < y->cpu) - (x->cpu > y->cpu);
        break;
    case SORT_MEM:
        r = (p->rss_kb < q->rss_kb) - (p->rss_kb > q->rss_kb);
        break;
    case SORT_TIME: {
        uint64_t a1 = p->utime + p->stime, b1 = q->utime + q->stime;
        r = (a1 < b1) - (a1 > b1);
        break;
    }
    }
    if (r == 0)
        r = (p->pid > q->pid) - (p->pid < q->pid);
    return r;
}

/* ---------- 画一屏 ---------- */

static void fmt_mem(char *buf, size_t n, uint64_t kb)
{
    sk_fmt_size_short(buf, n, kb * 1024);
}

static void draw(int rows, int cols)
{
    if (!g_batch)
        out_str("\033[H");
    int line = 0;

    /* 第 1 行：时间、运行时长、负载 */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    double up = 0, load[3] = { 0, 0, 0 };
    sk_read_uptime(&up, NULL);
    sk_read_loadavg(load, NULL, NULL);
    char dur[64];
    sk_fmt_duration(dur, sizeof dur, (uint64_t)up);
    out_line(cols, "top - %02d:%02d:%02d  已运行 %s  负载均值: %.2f, %.2f, %.2f", tm.tm_hour, tm.tm_min,
             tm.tm_sec, dur, load[0], load[1], load[2]);
    line++;

    /* 第 2 行：任务统计 */
    int nr = 0, ns = 0, nt = 0, nz = 0;
    for (int i = 0; i < g_nprocs; i++) {
        char st = g_procs[i].info.state;
        if (st == 'R') nr++;
        else if (st == 'T' || st == 't') nt++;
        else if (st == 'Z') nz++;
        else ns++;
    }
    out_line(cols, "任务: 共 %d 个，%d 运行，%d 睡眠，%d 停止，%d 僵尸", g_nprocs, nr, ns, nt, nz);
    line++;

    /* 第 3 行：CPU 时间的分布 */
    static const sk_cpustat zero;
    const sk_cpustat *a = &g_cpu_delta_base, *b = &g_cpu_now;
    double tot = (double)(b->total - a->total);
    if (tot <= 0) {
        a = &zero;    /* 两次采样之间没有流逝（太快了）：退回“开机以来”的平均值 */
        tot = (double)b->total;
    }
    if (tot <= 0)
        tot = 1;
#define PCT(f) ((double)(b->f - a->f) * 100.0 / tot)
    out_line(cols, "CPU: %5.1f 用户  %5.1f 系统  %5.1f 低优先级  %5.1f 空闲  %5.1f 等待IO  %5.1f 中断",
             PCT(user), PCT(system), PCT(nice), PCT(idle), PCT(iowait), PCT(irq) + PCT(softirq));
#undef PCT
    line++;

    /* 第 4 行：内存 */
    sk_meminfo mi;
    sk_read_meminfo(&mi);
    char m1[16], m2[16], m3[16], m4[16], m5[16];
    uint64_t used = mi.total_kb > mi.available_kb ? mi.total_kb - mi.available_kb : 0;
    fmt_mem(m1, sizeof m1, mi.total_kb);
    fmt_mem(m2, sizeof m2, mi.free_kb);
    fmt_mem(m3, sizeof m3, used);
    fmt_mem(m4, sizeof m4, mi.buffers_kb + mi.cached_kb);
    fmt_mem(m5, sizeof m5, mi.available_kb);
    out_line(cols, "内存: 总计 %s  空闲 %s  已用 %s  缓存 %s  可用 %s", m1, m2, m3, m4, m5);
    line++;

    /* 第 5 行：提示或消息 */
    if (!g_batch) {
        if (g_msg[0])
            out_line(cols, "%s", g_msg);
        else
            out_line(cols, "q 退出  P 按CPU  M 按内存  T 按时间  N 按进程号  c 命令行  k 结束进程");
        line++;
    } else {
        out_line(cols, "%s", "");
    }

    /* 表头（反色） */
    char hdr[160];
    snprintf(hdr, sizeof hdr, "%5s %-8s %3s %3s %6s %6s %c %5s %5s %9s %-*s", "PID", "USER", "PR", "NI", "VIRT",
             "RES", 'S', "%CPU", "%MEM", "TIME+", g_batch ? 1 : SK_MAX(1, cols - 58), "COMMAND");
    if (!g_batch)
        out_str("\033[7m");
    size_t hn = cols > 0 ? sk_utf8_fit(hdr, cols, NULL) : strlen(hdr);
    out_raw(hdr, hn);
    out_str(g_batch ? "\n" : "\033[0m\033[K\n");
    line++;

    if (g_rows)
        qsort(g_rows, (size_t)g_nprocs, sizeof *g_rows, cmp_rows);
    for (int i = 0; i < g_nprocs && g_rows && (g_batch || line < rows); i++, line++) {
        sb_proc *p = g_rows[i].p;
        sk_procinfo *pi = &p->info;
        char user[64], virt[16], res[16], tbuf[32], pr[16], cmd[300];
        sk_user_name(p->uid, user, sizeof user);
        fmt_mem(virt, sizeof virt, pi->vsize_kb);
        fmt_mem(res, sizeof res, pi->rss_kb);
        uint64_t t = (pi->utime + pi->stime) * 100 / (uint64_t)g_hz;   /* 百分之一秒 */
        snprintf(tbuf, sizeof tbuf, "%llu:%02llu.%02llu", (unsigned long long)(t / 6000),
                 (unsigned long long)(t / 100 % 60), (unsigned long long)(t % 100));
        /* 内核里的优先级：普通进程 20 + nice；负数是实时进程 */
        if (pi->priority < 0)
            snprintf(pr, sizeof pr, "rt");
        else
            snprintf(pr, sizeof pr, "%d", pi->priority);
        double pmem = mi.total_kb ? (double)pi->rss_kb * 100.0 / (double)mi.total_kb : 0;
        const char *name = g_show_cmd ? sb_proc_cmd(pi, cmd, sizeof cmd) : pi->comm;
        int highlight = !g_batch && pi->state == 'R';
        if (highlight)
            out_str("\033[1m");
        out_line(cols, "%5d %-8.8s %3s %3d %6s %6s %c %5.1f %5.1f %9s %s", (int)pi->pid, user, pr, pi->nice, virt,
                 res, pi->state, g_rows[i].cpu, pmem, tbuf, name);
        if (highlight) {
            /* out_line 已经输出了换行；把加粗关掉 */
            out_str("\033[0m");
        }
    }
    if (!g_batch)
        out_str("\033[J");   /* 擦掉屏幕下面剩下的旧内容 */
    else
        out_str("\n");
    out_flush();
}

/* ---------- 交互 ---------- */

/* 在第 5 行显示提示并读入一行（原始模式下要自己处理回显和退格）。Esc 取消返回 -1 */
static int prompt(const char *text, char *buf, size_t n)
{
    size_t len = 0;
    buf[0] = 0;
    for (;;) {
        char line[256];
        snprintf(line, sizeof line, "\033[5;1H\033[K\033[1m%s\033[0m%s", text, buf);
        sb_write_all(STDOUT_FILENO, line, strlen(line));
        unsigned char ch;
        ssize_t r = read(STDIN_FILENO, &ch, 1);
        if (r < 0 && errno == EINTR) {
            if (g_quit)
                return -1;
            continue;
        }
        if (r <= 0)
            return -1;
        if (ch == '\n' || ch == '\r')
            return 0;
        if (ch == 27)
            return -1;
        if ((ch == 127 || ch == 8) && len > 0) {
            buf[--len] = 0;
            continue;
        }
        if (ch >= 0x20 && ch < 0x7F && len + 1 < n) {
            buf[len++] = (char)ch;
            buf[len] = 0;
        }
    }
}

static void do_kill(void)
{
    char pidbuf[32], sigbuf[32];
    sb_write_all(STDOUT_FILENO, "\033[?25h", 6);
    int ok = prompt("要结束的进程号: ", pidbuf, sizeof pidbuf);
    long long pid = 0;
    if (ok == 0 && sb_parse_ll(pidbuf, &pid) == 0 && pid > 0) {
        ok = prompt("发送的信号 [15=TERM，9=KILL]: ", sigbuf, sizeof sigbuf);
        int sig = SIGTERM;
        if (ok == 0 && sigbuf[0])
            sig = sk_signal_from_name(sigbuf);
        if (ok != 0)
            snprintf(g_msg, sizeof g_msg, "已取消");
        else if (sig < 0)
            snprintf(g_msg, sizeof g_msg, "无效的信号 '%s'", sigbuf);
        else if (kill((pid_t)pid, sig) != 0)
            snprintf(g_msg, sizeof g_msg, "无法给进程 %lld 发送信号: %s", pid, sk_strerror(errno));
        else
            snprintf(g_msg, sizeof g_msg, "已向进程 %lld 发送信号 %d (%s)", pid, sig,
                     sk_signal_name(sig) ? sk_signal_name(sig) : "?");
    } else if (ok == 0) {
        snprintf(g_msg, sizeof g_msg, "无效的进程号 '%s'", pidbuf);
    } else {
        snprintf(g_msg, sizeof g_msg, "已取消");
    }
    sb_write_all(STDOUT_FILENO, "\033[?25l", 6);
}

/* 处理一个按键；返回 1 表示需要重画 */
static int handle_key(unsigned char ch)
{
    g_msg[0] = 0;
    switch (ch) {
    case 'q':
    case 'Q':
        g_quit = 1;
        return 0;
    case 'P': g_sort = SORT_CPU; snprintf(g_msg, sizeof g_msg, "按 CPU 占用排序"); return 1;
    case 'M': g_sort = SORT_MEM; snprintf(g_msg, sizeof g_msg, "按内存占用排序"); return 1;
    case 'T': g_sort = SORT_TIME; snprintf(g_msg, sizeof g_msg, "按累计 CPU 时间排序"); return 1;
    case 'N': g_sort = SORT_PID; snprintf(g_msg, sizeof g_msg, "按进程号排序"); return 1;
    case 'c':
        g_show_cmd = !g_show_cmd;
        snprintf(g_msg, sizeof g_msg, g_show_cmd ? "显示完整命令行" : "显示进程名");
        return 1;
    case 'k':
        do_kill();
        return 1;
    case 'h':
    case '?':
        snprintf(g_msg, sizeof g_msg, "按键: q 退出 P/M/T/N 排序 c 命令行 k 结束进程 空格 刷新（详见 top --help）");
        return 1;
    case 27: {
        /* 方向键等会发来 ESC [ X 这样的序列：把剩下的字节读掉，不当作命令 */
        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        unsigned char junk[16];
        while (poll(&p, 1, 20) > 0 && read(STDIN_FILENO, junk, sizeof junk) > 0)
            ;
        return 0;
    }
    default:
        return 1;
    }
}

int applet_top(int argc, char **argv)
{
    long long iterations = -1;
    int c;
    while ((c = sb_getopt(argc, argv, "bn:d:", usage)) != -1) {
        switch (c) {
        case 'b': g_batch = 1; break;
        case 'n':
            if (sb_parse_ll(optarg, &iterations) != 0 || iterations < 1) {
                sb_err("无效的次数 '%s'", optarg);
                return 1;
            }
            break;
        case 'd': {
            char *end;
            g_delay = strtod(optarg, &end);
            if (*end || g_delay < 0.1) {
                sb_err("无效的刷新间隔 '%s'（至少 0.1 秒）", optarg);
                return 1;
            }
            break;
        }
        default:
            return 1;
        }
    }
    if (!g_batch && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        sb_err("需要在终端里运行（输出到文件时请用 top -b）");
        return 1;
    }
    g_hz = sk_clk_tck();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit;      /* 不设 SA_RESTART：让 poll() 被信号打断，主循环能马上看到标志 */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    if (!g_batch) {
        sa.sa_handler = on_winch;
        sigaction(SIGWINCH, &sa, NULL);
        sa.sa_handler = on_tstp;
        sigaction(SIGTSTP, &sa, NULL);
        term_enter();
        if (!g_term_on) {
            sb_perr("无法设置终端");
            return 1;
        }
    }

    /* 第一次：先采样一次，稍等片刻再采样，这样第一屏就有有意义的 %CPU */
    sample();
    struct timespec ts = { 0, 300000000L };
    nanosleep(&ts, NULL);
    sample();

    long long shown = 0;
    while (!g_quit) {
        int cols, rows;
        sk_term_size(STDOUT_FILENO, &cols, &rows);
        if (g_batch)
            cols = 0;   /* 批处理模式不截断 */
        draw(rows, cols);
        if (iterations > 0 && ++shown >= iterations)
            break;

        /* 等待：超时就刷新；有按键就处理；收到 SIGWINCH 就按新尺寸重画 */
        int timeout = (int)(g_delay * 1000);
        int need_sample = 1;
        while (!g_quit) {
            if (g_batch) {
                struct timespec d = { (time_t)g_delay, (long)((g_delay - (double)(time_t)g_delay) * 1e9) };
                nanosleep(&d, NULL);
                break;
            }
            struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
            int r = poll(&p, 1, timeout);
            if (r < 0 && errno == EINTR) {
                if (g_resized) {
                    g_resized = 0;
                    need_sample = 0;
                    break;
                }
                continue;
            }
            if (r == 0)
                break;
            unsigned char ch;
            if (read(STDIN_FILENO, &ch, 1) == 1 && handle_key(ch)) {
                need_sample = ch == ' ';
                break;
            }
            if (r < 0 || (p.revents & (POLLHUP | POLLERR)))
                g_quit = 1;
        }
        if (need_sample && !g_quit)
            sample();
    }
    term_leave();
    free(g_out);
    free(g_procs);
    free(g_rows);
    free(g_prev);
    return 0;
}
