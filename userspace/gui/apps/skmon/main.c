/*
 * skmon — 系统监视器（“任务管理器”）
 *
 * 两个标签页：
 *   概览   CPU 占用率和最近 60 秒的曲线、内存、负载均值、开机时长、进程/线程数
 *   进程   所有进程的表格（点表头排序），可以给选中的进程发信号结束它
 *
 * ┌─────────────── 这些数字是从哪里来的？ ───────────────┐
 *
 *  系统监视器没有任何特权，也没有调用什么“神秘接口”：它只是每秒读一遍 /proc 下的几个文件。
 *  /proc 不在磁盘上，是内核“现场编出来”的文本——读 /proc/stat 就是问内核“CPU 忙了多久”，
 *  读 /proc/<pid>/stat 就是问“这个进程的状态”。解析这些文件的代码在 common/skutil.c，
 *  命令行的 ps / top / free（skbox）用的也是同一套函数，所以这里显示的数字和它们一致。
 *
 *  1. CPU 占用率：为什么必须“采样两次再相减”？
 *     内核用一个周期性的时钟中断（每秒 HZ 次，用户态看到的单位是 sysconf(_SC_CLK_TCK)，
 *     一般是 100）来记账：每次中断时 CPU 正在干什么（用户态代码、内核代码、空闲……），
 *     对应的计数器就加一。这个单位叫“时钟滴答”（jiffies / clock ticks）。
 *     /proc/stat 第一行给出的是**开机以来**的累计值，只能算出“开机以来平均有多忙”。
 *     想知道“现在”有多忙，就要隔一段时间读两次，看这段时间里增加的滴答中有多少不是空闲：
 *         占用率 = (忙碌滴答的增量) / (全部滴答的增量)
 *     每个进程的 CPU% 也一样：/proc/<pid>/stat 里的 utime+stime 是它一共用掉的滴答数，
 *     两次采样相减，再除以这段时间流逝的滴答数（与 top 的算法相同）。
 *
 *  2. 内存：为什么“空闲”很少不用担心？
 *     Linux 会把暂时没人用的内存拿来缓存文件内容（页缓存，page cache）：读过的文件留在内存里，
 *     下次再读就不用碰磁盘。所以开机久了 MemFree（真正一个字节都没用的内存）总是很少。
 *     但缓存随时可以丢掉、还给程序用。内核在 /proc/meminfo 里给了一个估计值 MemAvailable：
 *     “不用换出到磁盘就能分配给新程序的内存”。所以这里的“已用 = 总量 − 可用”，和 free、top 一致。
 *
 *  3. 负载均值（load average）
 *     /proc/loadavg 的前三个数，是“正在运行或排队等 CPU 的任务数”（再加上处于不可中断睡眠、
 *     通常是在等磁盘的任务）在最近 1、5、15 分钟里的指数衰减平均值。
 *     单核机器上 1.0 表示 CPU 刚好一直有活干；超过核数说明有任务在排队等 CPU。
 *
 * └──────────────────────────────────────────────────────┘
 *
 * 性能（网页版跑在 v86 模拟器里，CPU 比真机慢几十倍）：
 *   - 一个每秒一次的定时器；窗口没有焦点时改成两秒一次。其余时间进程睡在 poll() 里。
 *   - 只有“进程”页可见时才扫描所有进程（每个进程要读好几个 /proc 文件，这是最贵的一步）；
 *     概览页只读 /proc/stat、meminfo、loadavg、uptime 这几个小文件。
 *   - 进程表：行的顺序没变时只用 sk_list_set_cell 更新内容变了的单元格；
 *     顺序变了（排序、有进程出生或退出）才 clear + add，列表会逐行比较、只重画变了的行。
 *     选中的进程和滚动位置在刷新时保持不变。
 *   - 标签、占用率条只在文字或数值真的变化时才重画；曲线图每秒只重画绘图区。
 *
 * 命令行与调试：
 *   skmon [--tab 0|1]       启动时显示第几页（0=概览，1=进程）
 *   环境变量 SKMON_DEBUG=1  每次刷新后在标准输出打印一行 “SKMON {JSON}”：当前页、CPU 千分比、
 *                           内存、进程数、表格里各行的进程号（按显示顺序）、选中的进程、滚动位置、
 *                           排序列，以及主要控件的位置（自动化测试 tests/apps/test_skmon.py 用它
 *                           找到要点击的地方并核对结果）。结束进程、排序等操作也会各打印一行。
 */
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include <sketch/rough.h>
#include "skutil.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MARGIN       16
#define TABS_Y       8
#define TABS_H       40
#define CONTENT_Y    (TABS_Y + TABS_H + 10)
#define CARD_GAP     12
#define CARD_HEAD    46          /* 卡片标题栏高度（图标 + 标题） */
#define KV_H         24          /* 卡片里“名称 数值”一行的高度 */
#define HIST_SECS    60          /* 曲线图显示最近多少秒 */
#define REFRESH_MS   1000
#define REFRESH_BG_MS 2000       /* 窗口没有焦点时的刷新间隔 */
#define ROW_H        28          /* 进程表行高 */
#define PAGE_MAX     24
#define NPAGES       2
#define STATUS_H     30
#define NSYS         5           /* 系统卡片的行数（地方不够时最后几行不显示） */

enum { PAGE_OVERVIEW = 0, PAGE_PROCS = 1 };
enum { COL_NAME, COL_PID, COL_USER, COL_STATE, COL_CPU, COL_MEM, COL_CMD, NCOLS };

/* 上一次采样时每个进程用掉的 CPU 滴答（按 pid 排序，二分查找） */
typedef struct {
    pid_t    pid;
    uint64_t start;     /* 进程启动时刻：pid 被新进程复用时，start 不同，不能拿来相减 */
    uint64_t ticks;     /* utime + stime */
} prev_t;

/* 这一次采样到的一个进程 */
typedef struct {
    sk_procinfo info;
    uid_t       uid;
    double      cpu;    /* 百分比（一个核占满 = 100） */
    int         icon;
} proc_t;

/* 卡片（概览页的三块“便签纸”）：外框、图标、标题由卡片自己画，数值是放在上面的标签控件 */
typedef struct {
    const char *title;
    const char *sub;     /* 标题后面淡淡的一行说明 */
    int         icon;
    sk_color    color;
} card_info;

typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    int        debug;
    int        page;
    int        focused;
    int        timer, timer_ms;
    int        soon_timer;               /* “马上再刷新一次”（结束进程之后）的一次性定时器 */
    unsigned long ticks_done;            /* 刷新了几次 */

    /* ---- 系统整体 ---- */
    long       hz;                       /* 每秒时钟滴答数 */
    int        ncpu;
    sk_cpustat cpu_prev;
    int        have_cpu;
    int        cpu_permille, cpu_user, cpu_sys, cpu_io;    /* 千分比 */
    short      hist[HIST_SECS + 1];      /* 环形缓冲：每秒一个 CPU 千分比，最新的在 hist_head-1 */
    int        hist_head, hist_n;
    uint64_t   hist_t0;                  /* 第一个样本的时刻（毫秒） */
    uint64_t   hist_pushed;              /* 一共放进去多少个样本 */
    sk_meminfo mem;
    uint64_t   reclaimable_kb;           /* SReclaimable：可回收的内核缓存（算进“缓存”） */
    double     load[3];
    double     uptime;
    int        nproc_all, nthread_all;

    /* ---- 进程 ---- */
    proc_t    *procs;
    int        nprocs;
    prev_t    *prev;
    int        nprev;
    uint64_t   prev_total;               /* 上一次进程采样时 /proc/stat 的总滴答 */
    int        have_prev;
    int       *order;                    /* 显示顺序：procs 的下标 */
    int        norder;
    pid_t     *shown;                    /* 表格里第 i 行是哪个进程 */
    int       *shown_icon;
    int        nshown, cap_shown;
    int        nkernel;
    const int *widths;                   /* 表格当前用的列宽（col_widths 或 col_widths_narrow） */
    sk_rect    status_row;               /* 底部状态栏那一行 */
    pid_t      sel_pid;                  /* 选中的进程（按 pid 记，排序、刷新后依然跟着它） */
    int        sort_col, sort_desc;      /* sort_col = -1：默认按 PID 升序，表头不画小三角 */
    int        hide_kernel;
    char       msg[256];
    sk_color   msg_color;

    /* ---- 控件 ---- */
    sk_widget *tabs;
    sk_widget *page_w[NPAGES][PAGE_MAX];
    int        npage_w[NPAGES];
    /* 概览 */
    sk_widget *cpu_card, *mem_card, *sys_card, *graph;
    sk_widget *cpu_value, *cpu_detail;
    sk_widget *mem_value, *mem_meter, *mem_keys[3], *mem_vals[3], *mem_hint;
    sk_widget *sys_keys[NSYS], *sys_vals[NSYS];
    /* 进程 */
    sk_widget *chk_kernel, *btn_term, *btn_kill, *list, *status, *msg_label;
} monitor;

static const char *const tab_labels[NPAGES] = { "概览", "进程" };
static const char *const col_titles[NCOLS] = { "名称", "PID", "用户", "状态", "CPU%", "内存", "命令行" };
/* 列宽：正数左对齐，负数右对齐（数字列），0 = 占满剩下的宽度（见 ui_list.c 开头的说明）。
 * 窗口窄的时候换一套窄一点的，给“命令行”多留点地方 */
static const int col_widths[NCOLS] = { 140, -64, 72, 64, -64, -72, 0 };
static const int col_widths_narrow[NCOLS] = { 112, -60, 60, 64, -60, -60, 0 };

static card_info cpu_info_card = { "处理器", NULL, SK_ICON_CHIP, SK_PURPLE };
static card_info mem_info_card = { "内存", "/proc/meminfo", SK_ICON_MEMORY, SK_GREEN };
static card_info sys_info_card = { "系统", "/proc/loadavg 等", SK_ICON_KERNEL, SK_ORANGE };
static char cpu_sub[64];

static void refresh(monitor *m, int full);
static void debug_dump(monitor *m, const char *why);
static sk_rect graph_plot(const sk_widget *w);

/* ================================================================ 小工具 */

/* 把控件登记到某一页（切换标签页时整页显示/隐藏） */
static sk_widget *add(monitor *m, int page, sk_widget *w)
{
    if (w && m->npage_w[page] < PAGE_MAX)
        m->page_w[page][m->npage_w[page]++] = w;
    if (w && page != m->page)
        sk_widget_set_visible(w, 0);
    return w;
}

static sk_widget *add_label(monitor *m, int page, const char *text, int font, int align, sk_color color)
{
    sk_widget *w = add(m, page, sk_label_new(m->ui, sk_rect_make(0, 0, 10, 10), text, font, align));
    sk_label_set_color(w, color);
    return w;
}

/* 内存大小（KB）→ "1.8 GB" 这样的文字 */
static void fmt_kb(char *buf, size_t n, uint64_t kb)
{
    sk_fmt_size(buf, n, kb * 1024);
}

/* 百分比（千分比输入），一位小数 */
static void fmt_permille(char *buf, size_t n, int pm)
{
    snprintf(buf, n, "%d.%d%%", pm / 10, pm % 10);
}

/* 读 /proc/meminfo 里的某一项（sk_meminfo 没有的字段） */
static uint64_t meminfo_key(const char *key)
{
    char *s = sk_read_file("/proc/meminfo", NULL);
    if (!s)
        return 0;
    uint64_t v = 0;
    size_t kl = strlen(key);
    for (char *p = s; p && *p;) {
        if (strncmp(p, key, kl) == 0) {
            v = strtoull(p + kl, NULL, 10);
            break;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    free(s);
    return v;
}

/* /proc 下有多少个数字目录 = 有多少个进程（只 readdir，不读文件，很便宜） */
static int count_processes(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (isdigit((unsigned char)e->d_name[0]))
            n++;
    closedir(d);
    return n;
}

/* 进程状态字母 → 简短的中文（字母来自 /proc/<pid>/stat 第 3 个字段） */
static const char *state_name(char st)
{
    switch (st) {
    case 'R': return "R 运行";     /* 正在 CPU 上跑，或者在就绪队列里排队 */
    case 'S': return "S 睡眠";     /* 等待某个事件（按键、网络、定时器……），可以被信号唤醒 */
    case 'D': return "D 等待";     /* 不可中断的睡眠：通常在等磁盘 I/O，连信号都叫不醒 */
    case 'Z': return "Z 僵尸";     /* 已经退出，但父进程还没有 wait() 它 */
    case 'T': return "T 暂停";     /* 被 SIGSTOP / Ctrl+Z 暂停了 */
    case 't': return "t 跟踪";     /* 被调试器停住 */
    case 'I': return "I 空闲";     /* 空闲的内核线程 */
    case 'X': return "X 退出";
    default: {
        static char buf[8];
        snprintf(buf, sizeof buf, "%c", st ? st : '?');
        return buf;
    }
    }
}

/* 给进程挑一个图标：我们自己的图形程序用它们在桌面上的图标，内核线程用“内核”图标 */
static int proc_icon(const sk_procinfo *p)
{
    static const struct { const char *comm; int icon; } known[] = {
        { "skterm", SK_ICON_TERMINAL }, { "skfiles", SK_ICON_FOLDER }, { "sknote", SK_ICON_NOTEPAD },
        { "skpaint", SK_ICON_PAINT },   { "skmon", SK_ICON_MONITOR },  { "skclock", SK_ICON_CLOCK },
        { "skabout", SK_ICON_ABOUT },   { "sketchd", SK_ICON_SKETCH_LOGO }, { "sksh", SK_ICON_TERMINAL },
        { "sinit", SK_ICON_SETTINGS },
    };
    if (p->is_kernel_thread)
        return SK_ICON_KERNEL;
    for (size_t i = 0; i < SK_ARRAY_LEN(known); i++)
        if (strcmp(p->comm, known[i].comm) == 0)
            return known[i].icon;
    return SK_ICON_PROCESS;
}

/* 显示用的命令行：内核线程没有命令行，用 [名字]；僵尸进程加 <defunct>（与 ps 一致） */
static void proc_cmdline(const sk_procinfo *p, char *buf, size_t n)
{
    if (p->state == 'Z')
        snprintf(buf, n, "[%s] <defunct>", p->comm);
    else if (p->is_kernel_thread || !p->cmdline[0])
        snprintf(buf, n, "[%s]", p->comm);
    else
        sk_strlcpy(buf, p->cmdline, n);
}

static proc_t *find_proc(monitor *m, pid_t pid)
{
    for (int i = 0; i < m->nprocs; i++)
        if (m->procs[i].info.pid == pid)
            return &m->procs[i];
    return NULL;
}

/* ================================================================ 采样：系统整体 */

static void hist_push(monitor *m, int v)
{
    m->hist[m->hist_head] = (short)v;
    m->hist_head = (m->hist_head + 1) % (HIST_SECS + 1);
    if (m->hist_n < HIST_SECS + 1)
        m->hist_n++;
    m->hist_pushed++;
}

/* 第 k 新的样本（k=0 是最新的） */
static int hist_get(const monitor *m, int k)
{
    int i = (m->hist_head - 1 - k + 2 * (HIST_SECS + 1)) % (HIST_SECS + 1);
    return m->hist[i];
}

static void sample_system(monitor *m)
{
    sk_cpustat now;
    if (sk_read_cpustat(&now) == 0) {
        if (m->have_cpu && now.total > m->cpu_prev.total) {
            /* 两次采样之间流逝的滴答里，有多少是在干活（见文件开头的说明） */
            uint64_t dt = now.total - m->cpu_prev.total;
            m->cpu_permille = sk_cpu_usage_permille(&m->cpu_prev, &now);
#define DELTA(f) (now.f >= m->cpu_prev.f ? now.f - m->cpu_prev.f : 0)
            m->cpu_user = (int)((DELTA(user) + DELTA(nice)) * 1000 / dt);
            m->cpu_sys = (int)((DELTA(system) + DELTA(irq) + DELTA(softirq)) * 1000 / dt);
            m->cpu_io = (int)(DELTA(iowait) * 1000 / dt);
#undef DELTA
            /* 定时器不会精确地每 1000 毫秒触发一次（窗口没焦点时还是两秒一次）：
             * 按真实流逝的秒数往历史里补样本，曲线的时间轴才不会被压缩 */
            uint64_t elapsed = sk_now_ms() - m->hist_t0;
            uint64_t want = (elapsed + 500) / 1000;
            int pushes = 0;
            while (m->hist_pushed < want && pushes < HIST_SECS + 1) {
                hist_push(m, m->cpu_permille);
                pushes++;
            }
            if (pushes == 0 && m->hist_n == 0)
                hist_push(m, m->cpu_permille);
            m->hist_pushed = want > m->hist_pushed ? want : m->hist_pushed;
        } else if (!m->have_cpu) {
            m->hist_t0 = sk_now_ms();
            m->hist_pushed = 0;
        }
        m->cpu_prev = now;
        m->have_cpu = 1;
    }
    if (sk_read_meminfo(&m->mem) == 0)
        m->reclaimable_kb = meminfo_key("SReclaimable:");
    int running = 0, total = 0;
    if (sk_read_loadavg(m->load, &running, &total) == 0)
        m->nthread_all = total;    /* 第 4 个字段的分母：内核里现存的调度实体（线程）总数 */
    sk_read_uptime(&m->uptime, NULL);
    m->nproc_all = count_processes();
}

/* ================================================================ 采样：进程 */

static int cmp_prev(const void *a, const void *b)
{
    const prev_t *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static void sample_procs(monitor *m)
{
    int n = 0;
    sk_procinfo *list = sk_list_procs(&n);
    if (!list) {
        snprintf(m->msg, sizeof m->msg, "无法读取 /proc：%s", sk_strerror(errno));
        m->msg_color = SK_RED;
        return;
    }
    proc_t *procs = calloc((size_t)(n ? n : 1), sizeof *procs);
    if (!procs) {
        free(list);
        return;
    }
    /* 这次进程采样对应的“全部 CPU 一共流逝了多少滴答” */
    uint64_t total = m->cpu_prev.total;
    uint64_t dt = m->have_prev && total > m->prev_total ? total - m->prev_total : 0;
    int nk = 0;
    for (int i = 0; i < n; i++) {
        proc_t *p = &procs[i];
        p->info = list[i];
        p->icon = proc_icon(&p->info);
        if (p->info.is_kernel_thread)
            nk++;
        /* /proc/<pid> 目录的属主就是进程的（有效）用户 */
        char path[32];
        struct stat st;
        snprintf(path, sizeof path, "/proc/%d", (int)p->info.pid);
        p->uid = stat(path, &st) == 0 ? st.st_uid : 0;

        uint64_t ticks = p->info.utime + p->info.stime;
        prev_t key = { p->info.pid, 0, 0 };
        prev_t *old = m->nprev ? bsearch(&key, m->prev, (size_t)m->nprev, sizeof key, cmp_prev) : NULL;
        double pct;
        if (dt && old && old->start == p->info.start_time) {
            /* 和 top 一样：一个进程最多占满一个核，所以乘以核数（dt 是所有核的滴答之和） */
            pct = (double)(ticks >= old->ticks ? ticks - old->ticks : 0) * 100.0 * m->ncpu / (double)dt;
        } else {
            /* 第一次看到这个进程：用“从启动到现在的平均值”代替 */
            double age = m->uptime - (double)p->info.start_time / (double)m->hz;
            pct = age > 0 ? (double)ticks / (double)m->hz / age * 100.0 : 0;
        }
        if (pct > 100.0 * m->ncpu)
            pct = 100.0 * m->ncpu;
        p->cpu = pct;
    }
    free(list);

    /* 保存这次的数据，留给下一次做差（sk_list_procs 已经按 pid 升序排好） */
    prev_t *np = malloc(sizeof *np * (size_t)(n ? n : 1));
    if (np) {
        for (int i = 0; i < n; i++) {
            np[i].pid = procs[i].info.pid;
            np[i].start = procs[i].info.start_time;
            np[i].ticks = procs[i].info.utime + procs[i].info.stime;
        }
        free(m->prev);
        m->prev = np;
        m->nprev = n;
        m->prev_total = total;
        m->have_prev = 1;
    }
    free(m->procs);
    m->procs = procs;
    m->nprocs = n;
    m->nkernel = nk;
}

/* ================================================================ 进程表 */

static monitor *g_cmp;   /* qsort 的比较函数拿不到 user 参数（单线程，安全） */

static int cmp_rows(const void *a, const void *b)
{
    const proc_t *x = &g_cmp->procs[*(const int *)a], *y = &g_cmp->procs[*(const int *)b];
    int r = 0;
    char ux[64], uy[64], cx[256], cy[256];
    switch (g_cmp->sort_col) {
    case COL_NAME:
        r = strcasecmp(x->info.comm, y->info.comm);
        break;
    case COL_USER:
        r = strcmp(sk_user_name(x->uid, ux, sizeof ux), sk_user_name(y->uid, uy, sizeof uy));
        break;
    case COL_STATE:
        r = (x->info.state > y->info.state) - (x->info.state < y->info.state);
        break;
    case COL_CPU:
        r = (x->cpu > y->cpu) - (x->cpu < y->cpu);
        break;
    case COL_MEM:
        r = (x->info.rss_kb > y->info.rss_kb) - (x->info.rss_kb < y->info.rss_kb);
        break;
    case COL_CMD:
        proc_cmdline(&x->info, cx, sizeof cx);
        proc_cmdline(&y->info, cy, sizeof cy);
        r = strcmp(cx, cy);
        break;
    default:
        break;
    }
    if (g_cmp->sort_col < 0)
        r = 0;
    if (r == 0)
        r = (x->info.pid > y->info.pid) - (x->info.pid < y->info.pid);
    return g_cmp->sort_desc && g_cmp->sort_col >= 0 ? -r : r;
}

/* 一行的各个单元格文字 */
static void row_cells(const proc_t *p, char cells[NCOLS][256])
{
    char user[64];
    snprintf(cells[COL_NAME], 256, "%s", p->info.comm);
    snprintf(cells[COL_PID], 256, "%d", (int)p->info.pid);
    snprintf(cells[COL_USER], 256, "%s", sk_user_name(p->uid, user, sizeof user));
    snprintf(cells[COL_STATE], 256, "%s", state_name(p->info.state));
    snprintf(cells[COL_CPU], 256, "%.1f", p->cpu);
    if (p->info.rss_kb)
        sk_fmt_size_short(cells[COL_MEM], 256, p->info.rss_kb * 1024);
    else
        snprintf(cells[COL_MEM], 256, "-");   /* 内核线程没有自己的用户态内存 */
    proc_cmdline(&p->info, cells[COL_CMD], 256);
}

static int ensure_shown_cap(monitor *m, int n)
{
    if (n <= m->cap_shown)
        return 0;
    int cap = n + 64;
    pid_t *s = realloc(m->shown, sizeof *s * (size_t)cap);
    if (!s)
        return -1;
    m->shown = s;
    int *ic = realloc(m->shown_icon, sizeof *ic * (size_t)cap);
    if (!ic)
        return -1;
    m->shown_icon = ic;
    m->cap_shown = cap;
    return 0;
}

static void update_buttons(monitor *m)
{
    proc_t *p = m->sel_pid ? find_proc(m, m->sel_pid) : NULL;
    /* 内核线程不接受普通信号（内核会直接忽略），所以不让结束它们 */
    int can = p && !p->info.is_kernel_thread;
    sk_widget_set_enabled(m->btn_term, can);
    sk_widget_set_enabled(m->btn_kill, can);
}

static void update_status(monitor *m)
{
    char buf[200];
    int shown = m->nshown;
    if (m->hide_kernel && m->nkernel)
        snprintf(buf, sizeof buf, "共 %d 个进程，显示 %d 个（隐藏了 %d 个内核线程）", m->nprocs, shown, m->nkernel);
    else
        snprintf(buf, sizeof buf, "共 %d 个进程（其中内核线程 %d 个）", m->nprocs, m->nkernel);
    sk_widget_set_text(m->status, buf);
    sk_widget_set_text(m->msg_label, m->msg);
    sk_label_set_color(m->msg_label, m->msg_color);
    /* 左边的统计按文字实际宽度占位，剩下的给右边的消息（窄窗口里两段都尽量完整） */
    sk_rect r = m->status_row;
    const sk_font *f = sk_font_get(SK_FONT_UI);
    int sw = SK_MIN(r.w, sk_text_width(f, buf) + 8);
    if (m->msg[0])
        sw = SK_MIN(sw, SK_MAX(r.w / 2, r.w - sk_text_width(f, m->msg) - 24));
    sk_widget_set_rect(m->status, sk_rect_make(r.x, r.y, sw, r.h));
    sk_widget_set_rect(m->msg_label, sk_rect_make(r.x + sw + 16, r.y, SK_MAX(1, r.w - sw - 16), r.h));
}

/* 把采样结果填进表格。顺序没变时逐个单元格更新，变了才重建（见文件开头“性能”） */
static void update_list(monitor *m)
{
    int *order = realloc(m->order, sizeof *order * (size_t)(m->nprocs ? m->nprocs : 1));
    if (!order)
        return;
    m->order = order;
    int n = 0;
    for (int i = 0; i < m->nprocs; i++)
        if (!(m->hide_kernel && m->procs[i].info.is_kernel_thread))
            order[n++] = i;
    m->norder = n;
    g_cmp = m;
    qsort(order, (size_t)n, sizeof *order, cmp_rows);
    if (ensure_shown_cap(m, n) < 0)
        return;

    int same = n == m->nshown && n == sk_list_count(m->list);
    for (int r = 0; same && r < n; r++)
        same = m->shown[r] == m->procs[order[r]].info.pid && m->shown_icon[r] == m->procs[order[r]].icon;

    static char cells[NCOLS][256];
    if (same) {
        /* 行没变：只改变了的单元格（sk_list_set_cell 内容一样时什么也不做） */
        for (int r = 0; r < n; r++) {
            row_cells(&m->procs[order[r]], cells);
            for (int c = 0; c < NCOLS; c++)
                sk_list_set_cell(m->list, r, c, cells[c]);
        }
    } else {
        int scroll = sk_list_scroll(m->list);
        int sel_row = -1;
        sk_list_clear(m->list);
        for (int r = 0; r < n; r++) {
            const proc_t *p = &m->procs[order[r]];
            row_cells(p, cells);
            const char *cp[NCOLS];
            for (int c = 0; c < NCOLS; c++)
                cp[c] = cells[c];
            sk_list_add(m->list, p->icon, cp, (intptr_t)p->info.pid);
            m->shown[r] = p->info.pid;
            m->shown_icon[r] = p->icon;
            if (p->info.pid == m->sel_pid)
                sel_row = r;
        }
        m->nshown = n;
        /* 选中行跟着进程走。sk_list_select 会把选中行滚动到可见处——但用户可能是故意滚走的，
         * 所以选完再把滚动位置恢复回来（只有选中行在屏幕外时才会多重画一次） */
        if (sel_row >= 0)
            sk_list_select(m->list, sel_row);
        else
            m->sel_pid = 0;
        if (sk_list_scroll(m->list) != scroll)
            sk_list_set_scroll(m->list, scroll);
    }
    m->nshown = n;
    update_buttons(m);
    update_status(m);
}

/* ================================================================ 概览页 */

static void set_kv(sk_widget *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_kv(sk_widget *w, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sk_widget_set_text(w, buf);
}

/* 把这一批改动立刻画出来并发给服务器。
 * 客户端库每个窗口一次最多攒 4 块 damage，再多就把新的一块并进“最划算”的旧块——
 * 内存卡片和系统卡片里各有几个数字在变，并在一起就成了横跨两张卡片的一大块。
 * 所以概览页按卡片分批：每批不超过 4 块，各自发出去，服务器只合成真正变了的地方。 */
static void flush_batch(monitor *m)
{
    if (m->page == PAGE_OVERVIEW)
        sk_ui_redraw_now(m->ui);
}

static void update_overview(monitor *m)
{
    char a[32], b[32], c[32];
    set_kv(m->cpu_value, "%d%%", (m->cpu_permille + 5) / 10);
    fmt_permille(a, sizeof a, m->cpu_user);
    fmt_permille(b, sizeof b, m->cpu_sys);
    fmt_permille(c, sizeof c, m->cpu_io);
    set_kv(m->cpu_detail, "用户态 %s · 内核态 %s · 等待 I/O %s", a, b, c);
    /* 曲线每秒都在动：只重画绘图区（坐标轴文字不变） */
    if (m->page == PAGE_OVERVIEW)
        sk_ui_invalidate(m->ui, sk_rect_inset(graph_plot(m->graph), -6, -6));
    flush_batch(m);

    /* 已用 = 总量 − 可用（MemAvailable），见文件开头的说明 */
    uint64_t total = m->mem.total_kb, avail = SK_MIN(m->mem.available_kb, total);
    uint64_t used = total - avail;
    int pm = total ? (int)(used * 1000 / total) : 0;
    set_kv(m->mem_value, "%d%%", (pm + 5) / 10);
    fmt_kb(a, sizeof a, used);
    fmt_kb(b, sizeof b, total);
    char cap[80];
    snprintf(cap, sizeof cap, "已用 %s / 共 %s", a, b);
    sk_meter_set(m->mem_meter, pm, cap);
    fmt_kb(a, sizeof a, avail);
    sk_widget_set_text(m->mem_vals[0], a);
    fmt_kb(a, sizeof a, m->mem.buffers_kb + m->mem.cached_kb + m->reclaimable_kb);
    sk_widget_set_text(m->mem_vals[1], a);
    fmt_kb(a, sizeof a, m->mem.free_kb);
    sk_widget_set_text(m->mem_vals[2], a);
    flush_batch(m);

    set_kv(m->sys_vals[0], "%.2f   %.2f   %.2f", m->load[0], m->load[1], m->load[2]);
    char dur[64];
    sk_fmt_duration(dur, sizeof dur, (uint64_t)m->uptime);
    sk_widget_set_text(m->sys_vals[1], dur);
    set_kv(m->sys_vals[2], "%d 个", m->nproc_all);
    set_kv(m->sys_vals[3], "%d 个", m->nthread_all);
    /* 用户态看到的时钟滴答频率（/proc 里的 CPU 时间都以它为单位，见文件开头） */
    set_kv(m->sys_vals[4], "每秒 %ld 次", m->hz);
}

/* 卡片：手绘外框 + 图标 + 标题（标题下面用主题色的荧光笔涂一道） */
static void card_draw(sk_widget *w, sk_canvas *c, void *user)
{
    const card_info *ci = user;
    sk_rect r = w->r;
    sk_rough_style s;
    sk_style_pencil(&s, w->seed);
    s.stroke = SK_INK_SOFT;
    s.stroke_width = 1.4f;
    s.roughness = 0.6f;
    sk_rough_rect(c, r.x + 1.5f, r.y + 1.5f, r.w - 3.0f, r.h - 3.0f, &s);

    const sk_font *tf = sk_font_get(SK_FONT_TITLE), *f = sk_font_get(SK_FONT_UI);
    int tx = r.x + 52;
    int tw = sk_text_width(tf, ci->title);
    sk_marker_highlight(c, sk_rect_make(tx - 4, r.y + 24, tw + 8, 12), SK_WITH_ALPHA(ci->color, 0x70),
                        sk_hash_u32(w->seed, 0x4D4Bu));
    sk_draw_icon(c, ci->icon, r.x + 14, r.y + 10, 28, sk_hash_str(ci->title));
    sk_draw_text_in_rect(c, tf, sk_rect_make(tx, r.y + 8, tw + 4, 32), ci->title, SK_INK, SK_ALIGN_LEFT);
    if (ci->sub && *ci->sub) {
        int sx = tx + tw + 12;
        int sw = r.x + r.w - 110 - sx;
        /* 放不下就不画（截成“/proc/…”反而让人看不懂） */
        if (sk_text_width(f, ci->sub) <= sw)
            sk_draw_text_in_rect(c, f, sk_rect_make(sx, r.y + 12, sw, 26), ci->sub, SK_INK_FAINT, SK_ALIGN_LEFT);
    }
}

/* 淡色的多行说明文字（user 是文字） */
static void hint_draw(sk_widget *w, sk_canvas *c, void *user)
{
    sk_draw_text_wrapped(c, sk_font_get(SK_FONT_UI), w->r, user, SK_INK_FAINT, 4);
}

/* 曲线图的绘图区（控件矩形去掉坐标轴文字的位置） */
static sk_rect graph_plot(const sk_widget *w)
{
    return sk_rect_make(w->r.x + 52, w->r.y + 8, w->r.w - 52 - 12, w->r.h - 8 - 24);
}

/* 最近 60 秒的 CPU 曲线：方格纸 + 排线涂色的面积 + 一条彩色铅笔线 */
static void graph_draw(sk_widget *w, sk_canvas *c, void *user)
{
    monitor *m = user;
    sk_rect p = graph_plot(w);
    if (p.w < 30 || p.h < 30)
        return;
    const sk_font *f = sk_font_get(SK_FONT_UI);
    int asc = sk_font_ascent(f);

    /* 方格纸：横线每 25%，竖线每 10 秒 */
    for (int i = 0; i <= 4; i++) {
        int y = p.y + p.h - p.h * i / 4;
        sk_hline(c, p.x, p.x + p.w, y, i == 2 ? sk_darken(SK_DESK_GRID, 20) : SK_DESK_GRID);
    }
    for (int i = 0; i <= 6; i++)
        sk_vline(c, p.x + p.w * i / 6, p.y, p.y + p.h, SK_DESK_GRID);
    /* 坐标轴文字 */
    static const char *const ylab[3] = { "0%", "50%", "100%" };
    for (int i = 0; i < 3; i++) {
        int y = p.y + p.h - p.h * i / 2;
        sk_draw_text_in_rect(c, f, sk_rect_make(w->r.x, y - 11, 46, 22), ylab[i], SK_INK_SOFT, SK_ALIGN_RIGHT);
    }
    int ly = p.y + p.h + 4 + asc;
    sk_draw_text(c, f, p.x, ly, "60 秒前", SK_INK_FAINT);
    int tw = sk_text_width(f, "30 秒前");
    sk_draw_text(c, f, p.x + p.w / 2 - tw / 2, ly, "30 秒前", SK_INK_FAINT);
    tw = sk_text_width(f, "现在");
    sk_draw_text(c, f, p.x + p.w - tw, ly, "现在", SK_INK_FAINT);

    /* 数据：最新的样本在最右边，每秒向左挪一格 */
    int n = SK_MIN(m->hist_n, HIST_SECS + 1);
    if (n >= 1) {
        sk_pointf pts[HIST_SECS + 3];
        for (int k = 0; k < n; k++) {
            int v = hist_get(m, n - 1 - k);   /* 从最旧到最新 */
            int age = n - 1 - k;
            pts[k].x = (float)(p.x + p.w) - (float)age * (float)p.w / HIST_SECS;
            pts[k].y = (float)(p.y + p.h) - (float)v * (float)p.h / 1000.0f;
        }
        if (n >= 2) {
            /* 曲线下面的面积：先垫一层很淡的颜色，再用蜡笔排线（和占用率条同一种画法） */
            pts[n] = (sk_pointf){ pts[n - 1].x, (float)(p.y + p.h) };
            pts[n + 1] = (sk_pointf){ pts[0].x, (float)(p.y + p.h) };
            sk_fill_polygon(c, pts, n + 2, SK_WITH_ALPHA(SK_PURPLE, 0x30));
            sk_rough_style h;
            sk_style_hatched(&h, SK_WITH_ALPHA(SK_PURPLE, 0xB0), sk_hash_str("skmon-graph-fill"));
            h.stroke_width = 0;
            h.roughness = 0.5f;
            h.hachure_gap = 6.0f;
            h.fill_weight = 1.3f;
            sk_rough_fill_polygon(c, pts, n + 2, &h);
            sk_rough_style s;
            sk_style_pencil(&s, sk_hash_str("skmon-graph-line"));
            s.stroke = sk_darken(SK_PURPLE, 50);
            s.stroke_width = 2.0f;
            s.roughness = 0.3f;
            s.bowing = 0.4f;
            s.double_stroke = 0;
            sk_rough_polyline(c, pts, n, &s);
        }
        /* 最新的一个点：一个小圆点 */
        sk_fill_circle_aa(c, pts[n - 1].x, pts[n - 1].y, 3.5f, sk_darken(SK_PURPLE, 50));
    }
    /* 坐标轴：铅笔画的两条线 */
    sk_rough_style a;
    sk_style_pencil(&a, sk_hash_str("skmon-axis"));
    a.stroke = SK_INK_SOFT;
    a.stroke_width = 1.3f;
    a.roughness = 0.5f;
    a.double_stroke = 0;
    sk_rough_line(c, (float)p.x, (float)p.y - 4, (float)p.x, (float)(p.y + p.h), &a);
    sk_rough_line(c, (float)p.x, (float)(p.y + p.h), (float)(p.x + p.w) + 4, (float)(p.y + p.h), &a);
}

/* ================================================================ 刷新 */

/* full=1：同时扫描所有进程（只在“进程”页可见时需要） */
static void refresh(monitor *m, int full)
{
    sample_system(m);
    update_overview(m);
    if (full) {
        sample_procs(m);
        update_list(m);
    }
    m->ticks_done++;
    debug_dump(m, "refresh");
}

static void on_tick(sk_app *app, void *user)
{
    (void)app;
    monitor *m = user;
    refresh(m, m->page == PAGE_PROCS);
}

static void on_soon(sk_app *app, void *user)
{
    (void)app;
    monitor *m = user;
    m->soon_timer = 0;
    refresh(m, m->page == PAGE_PROCS);
}

/* 每秒一次；窗口没有焦点（用户在用别的程序）时两秒一次，给慢速的网页版省点 CPU */
static void set_rate(monitor *m, int ms)
{
    if (m->timer && m->timer_ms == ms)
        return;
    if (m->timer)
        sk_app_remove_timer(m->app, m->timer);
    m->timer = sk_app_add_timer(m->app, ms, 1, on_tick, m);
    m->timer_ms = ms;
}

/* ================================================================ 回调 */

static void show_page(monitor *m, int page)
{
    for (int i = 0; i < NPAGES; i++)
        for (int k = 0; k < m->npage_w[i]; k++)
            sk_widget_set_visible(m->page_w[i][k], i == page);
    m->page = page;
}

static void on_tab(sk_widget *w, void *user)
{
    monitor *m = user;
    show_page(m, sk_tabs_active(w));
    /* 切到“进程”页时立刻扫描一次，不用等下一秒 */
    refresh(m, m->page == PAGE_PROCS);
}

static void on_sort(sk_widget *w, int col, void *user)
{
    (void)w;
    monitor *m = user;
    /* 和表头小三角的规则一致：第一次点某列升序，再点同一列切换成降序 */
    if (m->sort_col == col)
        m->sort_desc = !m->sort_desc;
    else {
        m->sort_col = col;
        m->sort_desc = 0;
    }
    update_list(m);
    if (m->debug) {
        printf("SKMON {\"event\":\"sort\",\"col\":%d,\"desc\":%d}\n", m->sort_col, m->sort_desc);
        fflush(stdout);
    }
    debug_dump(m, "sort");
}

static void on_select(sk_widget *w, void *user)
{
    monitor *m = user;
    int row = sk_list_selected(w);
    m->sel_pid = row >= 0 ? (pid_t)sk_list_data(w, row) : 0;
    update_buttons(m);
    debug_dump(m, "select");
}

static void on_hide_kernel(sk_widget *w, void *user)
{
    monitor *m = user;
    m->hide_kernel = sk_widget_checked(w);
    update_list(m);
    debug_dump(m, "filter");
}

/* 给选中的进程发信号。kill() 这个系统调用名字很吓人，其实它只是“发信号”：
 *   SIGTERM（15）是礼貌的请求，进程可以捕获它，先保存数据、清理现场再退出（也可以不理）；
 *   SIGKILL（9）由内核直接执行，进程根本收不到通知，来不及做任何事情。
 * 权限检查在内核里：普通用户只能给自己的进程发信号（否则 EPERM）；进程已经不在了是 ESRCH。 */
static void do_kill(monitor *m, int sig)
{
    pid_t pid = m->sel_pid;
    proc_t *p = pid ? find_proc(m, pid) : NULL;
    if (!p)
        return;
    char name[40], text[640], extra[200] = "";
    sk_strlcpy(name, p->info.comm, sizeof name);
    if (pid == 1)
        snprintf(extra, sizeof extra, sig == SIGKILL
                     ? "\n\n这是 1 号进程（sinit）。内核会保护它：发给 1 号进程的 SIGKILL 会被忽略。"
                     : "\n\n注意：这是 1 号进程（sinit）。它收到 SIGTERM 会重新启动整个系统！");
    else if (pid == getpid())
        snprintf(extra, sizeof extra, "\n\n这就是系统监视器自己。");
    int r;
    if (sig == SIGKILL) {
        snprintf(text, sizeof text,
                 "注意：要强制结束“%s”（PID %d）吗？\n\n将发送 SIGKILL：内核直接终止它，"
                 "进程没有机会保存数据或清理现场。%s", name, (int)pid, extra);
        r = sk_dialog_message(m->app, "强制结束进程", text, "强制结束|取消");
    } else {
        snprintf(text, sizeof text,
                 "确定要结束“%s”（PID %d）吗？\n\n将发送 SIGTERM：请它自己退出，"
                 "程序可以先保存数据、清理现场。%s", name, (int)pid, extra);
        r = sk_dialog_message(m->app, "结束进程", text, "结束进程|取消");
    }
    const char *signame = sig == SIGKILL ? "SIGKILL" : "SIGTERM";
    if (r != 0) {
        snprintf(m->msg, sizeof m->msg, "已取消");
        m->msg_color = SK_INK_SOFT;
        update_status(m);
        return;
    }
    int ok = kill(pid, sig) == 0;
    int err = ok ? 0 : errno;
    if (m->debug) {
        printf("SKMON {\"event\":\"kill\",\"pid\":%d,\"sig\":%d,\"ok\":%d,\"errno\":%d}\n", (int)pid, sig, ok, err);
        fflush(stdout);
    }
    if (!ok) {
        snprintf(m->msg, sizeof m->msg, "无法结束“%s”（PID %d）：%s", name, (int)pid, sk_strerror(err));
        m->msg_color = SK_RED;
        update_status(m);
        char etext[300];
        snprintf(etext, sizeof etext, "无法给“%s”（PID %d）发送 %s：%s。", name, (int)pid, signame, sk_strerror(err));
        sk_dialog_message(m->app, "无法结束进程", etext, "确定");
        return;
    }
    snprintf(m->msg, sizeof m->msg, "已向“%s”（PID %d）发送 %s", name, (int)pid, signame);
    m->msg_color = sk_darken(SK_GREEN, 40);
    update_status(m);
    /* 信号是异步的：kill() 返回时进程多半还没退出。稍等一下再刷新，表格里它就消失了 */
    if (!m->soon_timer)
        m->soon_timer = sk_app_add_timer(m->app, 250, 0, on_soon, m);
}

static void on_term(sk_widget *w, void *user)
{
    (void)w;
    do_kill(user, SIGTERM);
}

static void on_kill(sk_widget *w, void *user)
{
    (void)w;
    do_kill(user, SIGKILL);
}

/* 窗口级事件：焦点变化调整刷新频率；Delete 键结束进程、F5 立即刷新 */
static int on_event(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    monitor *m = user;
    if (e->type == SK_EVENT_FOCUS) {
        m->focused = e->focused;
        set_rate(m, e->focused ? REFRESH_MS : REFRESH_BG_MS);
        return 0;
    }
    if (e->type == SK_EVENT_KEY && e->key.state != 0) {
        if (e->key.keycode == KEY_DELETE && m->page == PAGE_PROCS && m->sel_pid) {
            do_kill(m, (e->key.modifiers & SK_MOD_SHIFT) ? SIGKILL : SIGTERM);
            return 1;
        }
        if (e->key.keycode == KEY_F5) {
            refresh(m, m->page == PAGE_PROCS);
            return 1;
        }
    }
    return 0;
}

/* ================================================================ 布局 */

/* 卡片里的一行“名称 数值”。数值标签每秒都可能变：只给它放得下文字的宽度（val_w），
 * 每次重画的面积小；不同卡片里的标签也不会被合并成一大块横跨窗口的重画区域 */
static void place_kv(sk_widget *k, sk_widget *v, sk_rect card, int y, int key_w, int val_w, int bottom)
{
    int visible = y + KV_H <= bottom;
    sk_widget_set_rect(k, sk_rect_make(card.x + 18, y, key_w, KV_H));
    sk_widget_set_rect(v, sk_rect_make(card.x + 18 + key_w, y, SK_MIN(val_w, card.w - 36 - key_w), KV_H));
    sk_widget_set_visible(k, visible);
    sk_widget_set_visible(v, visible);
}

static void layout_overview(monitor *m, int W, int H)
{
    int x = MARGIN, y = CONTENT_Y, w = W - 2 * MARGIN;
    int avail = H - MARGIN - y - CARD_GAP;
    int ch = avail * 52 / 100;
    sk_rect cpu = sk_rect_make(x, y, w, ch);
    int by = y + ch + CARD_GAP, bh = H - MARGIN - by;
    int hw = (w - CARD_GAP) / 2;
    sk_rect mem = sk_rect_make(x, by, hw, bh);
    sk_rect sys = sk_rect_make(x + hw + CARD_GAP, by, w - hw - CARD_GAP, bh);

    sk_widget_set_rect(m->cpu_card, cpu);
    sk_widget_set_rect(m->cpu_value, sk_rect_make(cpu.x + cpu.w - 150, cpu.y + 6, 134, 40));
    /* 这一行每秒都在变：宽度只要放得下文字；左边和绘图区对齐，这样它和曲线图的重画区域
     * 被合并成一块时（客户端库每个窗口最多攒 4 块 damage）几乎不会多画 */
    sk_widget_set_rect(m->cpu_detail, sk_rect_make(cpu.x + 52, cpu.y + CARD_HEAD - 4, SK_MIN(cpu.w - 70, 440), 24));
    sk_widget_set_rect(m->graph, sk_rect_make(cpu.x + 6, cpu.y + CARD_HEAD + 28, cpu.w - 16,
                                              cpu.h - CARD_HEAD - 28 - 8));

    sk_widget_set_rect(m->mem_card, mem);
    sk_widget_set_rect(m->mem_value, sk_rect_make(mem.x + mem.w - 110, mem.y + 6, 94, 40));
    sk_widget_set_rect(m->mem_meter, sk_rect_make(mem.x + 18, mem.y + CARD_HEAD + 4, mem.w - 36, 28));
    int ky = mem.y + CARD_HEAD + 42, bottom = mem.y + mem.h - 8;
    for (int i = 0; i < 3; i++)
        place_kv(m->mem_keys[i], m->mem_vals[i], mem, ky + i * KV_H, 52, 110, bottom);
    int hy = ky + 3 * KV_H + 6;
    int hint_h = bottom - hy;
    sk_widget_set_rect(m->mem_hint, sk_rect_make(mem.x + 18, hy, mem.w - 36, hint_h > 0 ? hint_h : 1));
    /* 提示文字放得下（至少两行）才显示 */
    sk_widget_set_visible(m->mem_hint, m->page == PAGE_OVERVIEW && hint_h >= 44);

    sk_widget_set_rect(m->sys_card, sys);
    ky = sys.y + CARD_HEAD + 4;
    bottom = sys.y + sys.h - 8;
    int pitch = SK_CLAMP((bottom - ky) / NSYS, KV_H, KV_H + 6);   /* 地方小就排紧一点 */
    for (int i = 0; i < NSYS; i++)
        place_kv(m->sys_keys[i], m->sys_vals[i], sys, ky + i * pitch, 84, 170, bottom);
    /* place_kv 可能显示了不属于当前页的控件：按页面修正 */
    if (m->page != PAGE_OVERVIEW)
        for (int k = 0; k < m->npage_w[PAGE_OVERVIEW]; k++)
            sk_widget_set_visible(m->page_w[PAGE_OVERVIEW][k], 0);
}

static void layout_procs(monitor *m, int W, int H)
{
    int y = CONTENT_Y;
    sk_widget_set_rect(m->chk_kernel, sk_rect_make(MARGIN, y + 3, 170, 28));
    sk_widget_set_rect(m->btn_kill, sk_rect_make(W - MARGIN - 104, y, 104, 34));
    sk_widget_set_rect(m->btn_term, sk_rect_make(W - MARGIN - 104 - 10 - 104, y, 104, 34));
    int ly = y + 44;
    sk_widget_set_rect(m->list, sk_rect_make(MARGIN, ly, W - 2 * MARGIN, H - ly - STATUS_H - 8));
    const int *widths = W - 2 * MARGIN < 720 ? col_widths_narrow : col_widths;
    if (widths != m->widths) {
        m->widths = widths;
        sk_list_set_columns(m->list, col_titles, widths, NCOLS);
    }
    m->status_row = sk_rect_make(MARGIN, H - STATUS_H - 2, W - 2 * MARGIN, STATUS_H - 4);
    update_status(m);
}

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    monitor *m = user;
    sk_widget_set_rect(m->tabs, sk_rect_make(MARGIN, TABS_Y, W - 2 * MARGIN, TABS_H));
    layout_overview(m, W, H);
    layout_procs(m, W, H);
    debug_dump(m, "layout");
}

/* ================================================================ 调试输出 */

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch < 0x20)
            fprintf(f, "\\u%04x", ch);
        else
            fputc(ch, f);
    }
    fputc('"', f);
}

static void json_rect(FILE *f, const char *name, sk_rect r)
{
    fprintf(f, "\"%s\":[%d,%d,%d,%d]", name, r.x, r.y, r.w, r.h);
}

static void debug_dump(monitor *m, const char *why)
{
    if (!m->debug || !m->list)
        return;
    FILE *f = stdout;
    fprintf(f, "SKMON {\"why\":\"%s\",\"tick\":%lu,\"page\":%d,\"focused\":%d,\"rate\":%d,", why, m->ticks_done,
            m->page, m->focused, m->timer_ms);
    fprintf(f, "\"cpu\":%d,\"hist\":%d,\"mem_total_kb\":%llu,\"mem_avail_kb\":%llu,\"mem_free_kb\":%llu,", m->cpu_permille,
            m->hist_n, (unsigned long long)m->mem.total_kb, (unsigned long long)m->mem.available_kb,
            (unsigned long long)m->mem.free_kb);
    fprintf(f, "\"load\":[%.2f,%.2f,%.2f],\"uptime\":%.0f,\"nproc\":%d,\"nthread\":%d,", m->load[0], m->load[1],
            m->load[2], m->uptime, m->nproc_all, m->nthread_all);
    fprintf(f, "\"procs\":%d,\"kernel\":%d,\"hide_kernel\":%d,\"sort\":%d,\"desc\":%d,\"sel\":%d,\"sel_row\":%d,"
               "\"scroll\":%d,\"count\":%d,",
            m->nprocs, m->nkernel, m->hide_kernel, m->sort_col, m->sort_desc, (int)m->sel_pid,
            sk_list_selected(m->list), sk_list_scroll(m->list), sk_list_count(m->list));
    fprintf(f, "\"term_enabled\":%d,", m->btn_term && !(m->btn_term->state & SK_STATE_DISABLED));
    fprintf(f, "\"rows\":[");
    for (int i = 0; i < m->nshown; i++)
        fprintf(f, "%s%d", i ? "," : "", (int)m->shown[i]);
    fprintf(f, "],\"names\":[");
    for (int i = 0; i < m->nshown; i++) {
        proc_t *p = find_proc(m, m->shown[i]);
        if (i)
            fputc(',', f);
        json_str(f, p ? p->info.comm : "");
    }
    fprintf(f, "],\"self\":");
    proc_t *self = find_proc(m, getpid());
    if (self) {
        static char cells[NCOLS][256];
        row_cells(self, cells);
        fputc('[', f);
        for (int c = 0; c < NCOLS; c++) {
            if (c)
                fputc(',', f);
            json_str(f, cells[c]);
        }
        fputc(']', f);
    } else {
        fprintf(f, "null");
    }
    fprintf(f, ",\"msg\":");
    json_str(f, m->msg);
    fprintf(f, ",\"ui\":{");
    for (int i = 0; i < NPAGES; i++) {
        char name[16];
        snprintf(name, sizeof name, "tab%d", i);
        json_rect(f, name, sk_tabs_item_rect(m->tabs->r, tab_labels, NPAGES, i));
        fputc(',', f);
    }
    json_rect(f, "list", m->list->r);
    fputc(',', f);
    json_rect(f, "term", m->btn_term->r);
    fputc(',', f);
    json_rect(f, "kill", m->btn_kill->r);
    fputc(',', f);
    json_rect(f, "hide", m->chk_kernel->r);
    fputc(',', f);
    json_rect(f, "graph", m->graph->r);
    /* 表格的行高和表头高度（与 ui_list.c 的 li_layout 同一个公式），测试用来算点击位置 */
    int lh = sk_font_line_height(sk_font_get(SK_FONT_UI));
    fprintf(f, ",\"row_h\":%d,\"header_h\":%d,\"widths\":[", SK_MAX(ROW_H, lh + 2), SK_MAX(26, lh + 10));
    for (int c = 0; c < NCOLS; c++)
        fprintf(f, "%s%d", c ? "," : "", m->widths[c] < 0 ? -m->widths[c] : m->widths[c]);
    fprintf(f, "]}}\n");
    fflush(f);
}

/* ================================================================ 创建 */

static void build_overview(monitor *m)
{
    sk_ui *ui = m->ui;
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    /* 卡片先创建：控件按创建顺序从下往上画，卡片的外框垫在数字标签下面 */
    snprintf(cpu_sub, sizeof cpu_sub, "%d 个核心 · /proc/stat", m->ncpu);
    cpu_info_card.sub = cpu_sub;
    m->cpu_card = add(m, PAGE_OVERVIEW, sk_custom_new(ui, z, card_draw, NULL, 0, &cpu_info_card));
    m->mem_card = add(m, PAGE_OVERVIEW, sk_custom_new(ui, z, card_draw, NULL, 0, &mem_info_card));
    m->sys_card = add(m, PAGE_OVERVIEW, sk_custom_new(ui, z, card_draw, NULL, 0, &sys_info_card));

    m->cpu_value = add_label(m, PAGE_OVERVIEW, "", SK_FONT_LARGE, SK_ALIGN_RIGHT, sk_darken(SK_PURPLE, 40));
    m->cpu_detail = add_label(m, PAGE_OVERVIEW, "", SK_FONT_UI, SK_ALIGN_LEFT, SK_INK_SOFT);
    m->graph = add(m, PAGE_OVERVIEW, sk_custom_new(ui, z, graph_draw, NULL, 0, m));

    m->mem_value = add_label(m, PAGE_OVERVIEW, "", SK_FONT_LARGE, SK_ALIGN_RIGHT, sk_darken(SK_GREEN, 40));
    m->mem_meter = add(m, PAGE_OVERVIEW, sk_meter_new(ui, z, SK_GREEN));
    static const char *const mem_keys[3] = { "可用", "缓存", "空闲" };
    for (int i = 0; i < 3; i++) {
        m->mem_keys[i] = add_label(m, PAGE_OVERVIEW, mem_keys[i], SK_FONT_UI, SK_ALIGN_LEFT, SK_INK_SOFT);
        m->mem_vals[i] = add_label(m, PAGE_OVERVIEW, "", SK_FONT_UI, SK_ALIGN_LEFT, SK_INK);
    }
    /* 段落控件不能改颜色，提示文字用自定义控件画成淡色 */
    m->mem_hint = add(m, PAGE_OVERVIEW, sk_custom_new(ui, z, hint_draw, NULL, 0,
        (void *)"“空闲”少不要紧：内核把闲置的内存拿来缓存文件，需要时随时让出来。已用 = 总量 − 可用。"));

    static const char *const sys_keys[NSYS] = { "负载均值", "已运行", "进程", "线程", "时钟滴答" };
    for (int i = 0; i < NSYS; i++) {
        m->sys_keys[i] = add_label(m, PAGE_OVERVIEW, sys_keys[i], SK_FONT_UI, SK_ALIGN_LEFT, SK_INK_SOFT);
        m->sys_vals[i] = add_label(m, PAGE_OVERVIEW, "", SK_FONT_UI, SK_ALIGN_LEFT, SK_INK);
    }
}

static void build_procs(monitor *m)
{
    sk_ui *ui = m->ui;
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    m->chk_kernel = add(m, PAGE_PROCS, sk_checkbox_new(ui, z, "隐藏内核线程", m->hide_kernel, on_hide_kernel, m));
    m->btn_term = add(m, PAGE_PROCS, sk_button_new(ui, z, "结束进程", on_term, m));
    sk_widget_set_accent(m->btn_term, SK_ORANGE);
    m->btn_kill = add(m, PAGE_PROCS, sk_button_new(ui, z, "强制结束", on_kill, m));
    sk_widget_set_accent(m->btn_kill, SK_RED);
    m->list = add(m, PAGE_PROCS, sk_list_new(ui, z, on_select, NULL, m));
    m->widths = col_widths;
    sk_list_set_columns(m->list, col_titles, col_widths, NCOLS);
    sk_list_set_row_height(m->list, ROW_H);
    sk_list_set_sort_handler(m->list, on_sort);
    sk_widget_set_text(m->list, "（没有进程）");
    m->status = add_label(m, PAGE_PROCS, "", SK_FONT_UI, SK_ALIGN_LEFT, SK_INK_SOFT);
    m->msg_label = add_label(m, PAGE_PROCS, "", SK_FONT_UI, SK_ALIGN_RIGHT, SK_INK_SOFT);
    sk_widget_set_enabled(m->btn_term, 0);
    sk_widget_set_enabled(m->btn_kill, 0);
}

int main(int argc, char **argv)
{
    monitor m;
    memset(&m, 0, sizeof m);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tab") == 0 && i + 1 < argc) {
            m.page = atoi(argv[++i]) == 1 ? PAGE_PROCS : PAGE_OVERVIEW;
        } else {
            fprintf(stderr, "用法: skmon [--tab 0|1]\n");
            return 2;
        }
    }
    const char *dbg = getenv("SKMON_DEBUG");
    m.debug = dbg && *dbg && strcmp(dbg, "0") != 0;
    m.hz = sk_clk_tck();
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    m.ncpu = ncpu > 0 ? (int)ncpu : 1;
    m.sort_col = -1;
    m.hide_kernel = 1;
    m.focused = 1;
    m.msg_color = SK_INK_SOFT;

    m.app = sk_app_connect("skmon");
    if (!m.app)
        return 1;
    m.win = sk_window_create(m.app, "系统监视器", 800, 560, SK_WIN_CENTER | SK_WIN_RESIZABLE, SK_PURPLE);
    if (!m.win) {
        sk_app_disconnect(m.app);
        return 1;
    }
    sk_window_set_min_size(m.win, 600, 440);
    m.ui = sk_ui_new(m.win);
    if (!m.ui) {
        sk_app_disconnect(m.app);
        return 1;
    }
    m.tabs = sk_tabs_new(m.ui, sk_rect_make(0, 0, 10, 10), tab_labels, NPAGES, on_tab, &m);
    build_overview(&m);
    build_procs(&m);
    sk_tabs_set_active(m.tabs, m.page);
    sk_ui_set_handler(m.ui, on_event, &m);

    /* 先采样一次 CPU 作为“起点”（占用率要两次采样才算得出来），稍等一下再采第二次，
     * 这样窗口一出来就有有意义的数字，而不是 0% */
    sample_system(&m);
    struct timespec ts = { 0, 250 * 1000000L };
    nanosleep(&ts, NULL);
    sk_ui_set_layout(m.ui, layout, &m);
    refresh(&m, m.page == PAGE_PROCS);
    set_rate(&m, REFRESH_MS);
    if (m.page == PAGE_PROCS)
        sk_ui_focus(m.ui, m.list);

    int rc = sk_app_run(m.app);
    sk_ui_free(m.ui);
    sk_app_disconnect(m.app);
    free(m.procs);
    free(m.prev);
    free(m.order);
    free(m.shown);
    free(m.shown_icon);
    return rc;
}
