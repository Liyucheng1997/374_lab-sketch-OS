/*
 * skutil.h — SketchOS 用户态公共工具函数（链接进所有程序）
 *
 * 包括：日志、字符串、文件读写、内核命令行参数、/proc 信息解析。
 * /proc 是 Linux 内核把内部状态“伪装成文件”提供给用户态的接口：
 * 读 /proc/meminfo 就是在问内核“内存用了多少”，读 /proc/<pid>/stat 就是问“这个进程的状态”。
 * ps、free、top、系统监视器等程序全靠这些函数。
 */
#ifndef SKUTIL_H
#define SKUTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef SKETCHOS_VERSION
#define SKETCHOS_VERSION "0.1"
#endif
#define SKETCHOS_NAME "SketchOS"

#define SK_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define SK_MIN(a, b) ((a) < (b) ? (a) : (b))
#define SK_MAX(a, b) ((a) > (b) ? (a) : (b))
#define SK_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* ---------- 日志 ----------
 * sk_log: 写到 stderr（形如 "[tag] 消息"）并写进内核日志 /dev/kmsg（用 dmesg 可以看到）。
 * sk_log_console: 额外写到 /dev/console（串口），用于“系统已就绪”这类里程碑消息，
 *                 自动化测试脚本就是等待这些消息的。 */
void sk_log_init(const char *tag);   /* 设置标签（默认程序名） */
void sk_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sk_log_console(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------- 字符串 ---------- */
size_t sk_strlcpy(char *dst, const char *src, size_t size);       /* 总是 NUL 结尾 */
size_t sk_strlcat(char *dst, const char *src, size_t size);
char  *sk_trim(char *s);                                           /* 原地去掉首尾空白，返回 s 内的指针 */
int    sk_startswith(const char *s, const char *prefix);
int    sk_endswith(const char *s, const char *suffix);
/* 按空白切分（支持 "..." 和 '...' 引号），原地修改 s，返回个数（最多 max 个） */
int    sk_split_args(char *s, char **argv, int max);
/* 人类可读的大小："512 B"、"1.5 KB"、"23.0 MB" */
void   sk_fmt_size(char *buf, size_t n, uint64_t bytes);
/* 时长："3 分 05 秒"、"2 小时 7 分"、"1 天 3 小时" */
void   sk_fmt_duration(char *buf, size_t n, uint64_t seconds);
char  *sk_strdup_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------- 文件 ---------- */
/* 读整个文件（末尾补 NUL），返回 malloc 的缓冲区，*len 可为 NULL。失败 NULL。
 * /proc 文件的 st_size 是 0，这个函数会循环读到 EOF，适用于 /proc。 */
char *sk_read_file(const char *path, size_t *len);
/* 读文件第一行到 buf（去掉换行），成功返回 0 */
int   sk_read_line(const char *path, char *buf, size_t n);
int   sk_write_file(const char *path, const void *data, size_t len);   /* 0 成功 */
int   sk_mkdir_p(const char *path, mode_t mode);                       /* 0 成功 */
/* 拼接路径到 buf：sk_path_join(buf, n, "/root", "a.txt") → "/root/a.txt" */
void  sk_path_join(char *buf, size_t n, const char *dir, const char *name);
/* 规范化绝对路径（处理 . 和 ..，不解析符号链接），结果写回 path */
void  sk_path_normalize(char *path);

/* ---------- 内核命令行 (/proc/cmdline) ----------
 * 例如命令行里有 "sketch.mode=800x600"：sk_cmdline_get("sketch.mode", buf, n) 返回 1 并填入 "800x600"；
 * 只有 "sketch.nogui"（无等号）时返回 1，buf 为空串；不存在返回 0。 */
int sk_cmdline_get(const char *key, char *buf, size_t n);

/* ---------- 系统信息 ---------- */
typedef struct {
    uint64_t total_kb, free_kb, available_kb, buffers_kb, cached_kb, shmem_kb;
} sk_meminfo;
int sk_read_meminfo(sk_meminfo *m);                  /* 0 成功 */

/* CPU 时间（单位：时钟滴答 jiffies），来自 /proc/stat 第一行 */
typedef struct {
    uint64_t user, nice, system, idle, iowait, irq, softirq;
    uint64_t total;   /* 以上之和 */
    uint64_t busy;    /* total - idle - iowait */
} sk_cpustat;
int sk_read_cpustat(sk_cpustat *c);
/* 两次采样之间的 CPU 占用率（千分比 0..1000） */
int sk_cpu_usage_permille(const sk_cpustat *prev, const sk_cpustat *now);

int sk_read_uptime(double *uptime, double *idle);    /* /proc/uptime */
int sk_read_loadavg(double load[3], int *running, int *total);

/* 进程信息（/proc/<pid>/stat、status、cmdline） */
typedef struct {
    pid_t    pid, ppid;
    char     state;          /* R 运行, S 睡眠, D 不可中断, Z 僵尸, T 停止, I 空闲内核线程 */
    char     comm[32];       /* 进程名 */
    char     cmdline[256];   /* 完整命令行（参数用空格连接）；内核线程为空 */
    uint64_t utime, stime;   /* 用户态/内核态 CPU 时间（jiffies） */
    uint64_t start_time;     /* 启动时刻（开机后的 jiffies） */
    uint64_t vsize_kb;       /* 虚拟内存大小 */
    uint64_t rss_kb;         /* 常驻内存 */
    int      threads;
    int      priority, nice;
    int      is_kernel_thread;
} sk_procinfo;
/* 读取一个进程，成功 0 */
int sk_read_proc(pid_t pid, sk_procinfo *p);
/* 列出所有进程（按 pid 升序），返回 malloc 的数组，*count 为个数。失败 NULL */
sk_procinfo *sk_list_procs(int *count);
/* 每秒时钟滴答数（sysconf(_SC_CLK_TCK)，Linux 上一般为 100） */
long sk_clk_tck(void);

/* 读取 /proc/version、主机名等的快捷函数 */
void sk_kernel_release(char *buf, size_t n);        /* uname -r */

/* ======================================================================
 * 以下是 skbox 组追加的通用工具（只增加，不改动上面已有的函数）
 * ====================================================================== */

/* ---------- 终端 ----------
 * 终端（TTY）的窗口大小保存在内核的 tty 结构里，用 ioctl(TIOCGWINSZ) 查询。
 * 终端模拟器（skterm）改变窗口大小时用 TIOCSWINSZ 告诉内核，内核再给前台进程组发 SIGWINCH。
 * 串口终端没有人设置窗口大小（查到的是 0），这时退回到环境变量 COLUMNS/LINES，再退回 80x24。 */
int sk_term_size(int fd, int *cols, int *rows);   /* 从内核查到返回 0；用的是后备值返回 -1 */
int sk_term_width(int fd);                         /* 只要列数 */

/* ---------- UTF-8 与显示宽度 ----------
 * 汉字在终端里占 2 列，字母占 1 列，组合用的附加符号占 0 列。按列对齐（ls 分栏、表格）时
 * 不能用 strlen（那是字节数，一个汉字 3 字节），要用“显示宽度”。 */
/* 解码一个字符，返回消耗的字节数（>=1）；非法序列返回 1 并令 *cp = 0xFFFD */
int    sk_utf8_decode(const char *s, size_t len, uint32_t *cp);
int    sk_wcwidth(uint32_t cp);                 /* 一个字符的显示宽度：0、1 或 2（控制字符算 0） */
int    sk_utf8_width(const char *s);            /* 整个字符串的显示宽度 */
int    sk_utf8_nwidth(const char *s, size_t len);
/* 不超过 max_cols 列的最长前缀有多少字节（不会切断一个字符）；*cols 返回该前缀的宽度（可为 NULL） */
size_t sk_utf8_fit(const char *s, int max_cols, int *cols);

/* ---------- 中文错误信息 ----------
 * 系统调用失败时内核返回一个错误号（errno，例如 ENOENT=2），这里翻译成中文；不认识的退回 strerror()。 */
const char *sk_strerror(int err);

/* ---------- 用户和组 ----------
 * 内核只认识数字 uid/gid；“root”这样的名字是用户态的约定，记在 /etc/passwd、/etc/group 的文本行里。
 * 查不到时：0 → "root"，其它 → 数字字符串。结果写进 buf，并返回 buf。 */
const char *sk_user_name(uid_t uid, char *buf, size_t n);
const char *sk_group_name(gid_t gid, char *buf, size_t n);

/* ---------- 信号 ----------
 * 信号是内核发给进程的“软件中断”，只是一个编号；名字（TERM、KILL…）是约定俗成的。 */
int         sk_signal_from_name(const char *name); /* "TERM"、"SIGTERM"、"term"、"15" → 15；不认识返回 -1 */
const char *sk_signal_name(int sig);               /* 15 → "TERM"；不认识返回 NULL */
const char *sk_signal_desc(int sig);               /* 15 → 中文说明；不认识返回 NULL */

/* ---------- 紧凑的人类可读大小 ----------
 * 与 GNU 工具 -h 选项相同的格式（1024 进制、向上取整、不带空格）："512"、"4.0K"、"1.5M"、"23G"。
 * 适合放进表格的窄列（ls -lh、df -h、du -h）。 */
void sk_fmt_size_short(char *buf, size_t n, uint64_t bytes);

#endif /* SKUTIL_H */
