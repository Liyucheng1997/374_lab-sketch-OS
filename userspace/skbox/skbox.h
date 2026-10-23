/*
 * skbox.h — SketchOS 多合一工具箱的公共头文件
 *
 * skbox 和 busybox 的思路一样：几十个小工具（ls、cat、ps……）编译进同一个程序。
 * 为什么这样做？我们的程序都是静态链接的，每个程序都会带一份 C 库；
 * 做成一个程序只需要一份 C 库，整个系统小很多。/bin/ls、/bin/cat 都只是指向 skbox 的
 * 符号链接，skbox 启动时看 argv[0]（内核执行程序时传进来的“我叫什么名字”）决定当哪个工具。
 *
 * 写一个新工具：新建 skbox/<名字>.c，里面写
 *     SKBOX_APPLET(名字, "一句话中文说明")          ← 必须从第 0 列开始，Makefile 用 grep 收集
 *     int applet_<名字>(int argc, char **argv) { ... }
 * Makefile 会自动生成 skbox_applets.inc，main.c 据此建立分发表。
 *
 * 注意：每个 .c 文件都要把本头文件放在第一个 #include（它定义 _GNU_SOURCE）。
 */
#ifndef SKBOX_H
#define SKBOX_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* 这些命令行工具的瓶颈在系统调用和 I/O，不在 CPU；而整个系统镜像要被网页版下载，
 * 所以 skbox 按“体积最小”来优化（-Os），比全局的 -O2 小三成左右。 */
#pragma GCC optimize("Os")

#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>

#include "skutil.h"

/* 空的标记宏：只给 Makefile 的 grep 看，编译时什么也不生成 */
#define SKBOX_APPLET(name, help)

/* 当前工具的名字（错误信息的前缀），由 main.c 设置 */
extern const char *sb_applet;

/* ---------- 错误信息 ----------
 * 统一格式 "名字: 消息"，写到标准错误。 */
void sb_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 同上，并在末尾附加 ": <errno 的中文说明>" */
void sb_perr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 打印 "请用 '名字 --help' 查看用法。" */
void sb_hint(void);

/* ---------- 选项解析 ----------
 * 包装 getopt_long：自动支持 --help（打印 usage 后退出 0）；遇到错误选项时打印中文提示并返回 '?'。
 * musl 的 getopt_long 会像 GNU 一样“重排”参数，所以 "ls dir -l" 也能识别 -l；
 * optstring 以 '+' 开头时遇到第一个非选项参数就停止（env、nice 这类命令需要）。 */
#define SB_OPT_HELP 0x7F01
int sb_getopt(int argc, char **argv, const char *optstring, const char *usage);
int sb_getopt_long(int argc, char **argv, const char *optstring,
                   const struct option *longopts, const char *usage);
/* 不用 getopt 的工具（echo、test……）：只有一个参数且是 --help 时打印用法并返回 1 */
int sb_help(int argc, char **argv, const char *usage);

/* ---------- 数字解析 ---------- */
int sb_parse_ll(const char *s, long long *out);      /* 完整的十进制整数，成功 0 */
/* 带单位的大小："10"、"4K"、"1M"、"2G"（1024 进制），"b"=512 字节，"kB"/"MB" 是 1000 进制 */
int sb_parse_size(const char *s, long long *out);

/* ---------- 输出 ---------- */
int sb_write_all(int fd, const void *buf, size_t n);  /* 写完全部数据，成功 0 */
int sb_copy_fd(int in, int out);                       /* 把 in 读到 EOF 写进 out，成功 0 */
/* 打开输入文件，"-" 表示标准输入；失败返回 -1（errno 有效） */
int sb_open_input(const char *path);

/* ---------- 终端颜色（ANSI 转义序列） ----------
 * 终端把 "ESC [ ... m" 这样的字节串解释成“换颜色”的命令，而不是显示出来。
 * 输出到文件或管道时不应该加颜色，所以先判断 isatty()。 */
int sb_color_enabled(int fd);
#define SB_RESET   "\033[0m"
#define SB_BOLD    "\033[1m"
#define SB_DIM     "\033[2m"
#define SB_REVERSE "\033[7m"
#define SB_RED     "\033[31m"
#define SB_GREEN   "\033[32m"
#define SB_YELLOW  "\033[33m"
#define SB_BLUE    "\033[34m"
#define SB_MAGENTA "\033[35m"
#define SB_CYAN    "\033[36m"
#define SB_GRAY    "\033[90m"

/* 按显示宽度补空格：输出 s，再补空格到 width 列（left=1 左对齐，0 右对齐） */
void sb_put_padded(FILE *f, const char *s, int width, int left);

/* ---------- 文件属性 ---------- */
/* 权限字符串 "drwxr-xr-x"（out 至少 11 字节） */
void sb_mode_string(mode_t mode, char *out);
/* 文件类型的中文名："普通文件"、"目录"…… */
const char *sb_file_type_name(mode_t mode, off_t size);
/* 解析 chmod/mkdir -m 的模式（八进制 "755" 或符号 "u+x,go-w"），基于 old 计算新模式。
 * is_dir 影响 'X'；umask_val 用于省略了 ugoa 的子句。成功 0，格式错误 -1。 */
int sb_mode_parse(const char *spec, mode_t old, int is_dir, mode_t umask_val, mode_t *out);
/* 路径的最后一部分（不修改参数，结尾的 / 会被忽略，结果写进 buf） */
const char *sb_basename(const char *path, char *buf, size_t n);

/* ---------- 递归复制 / 删除（cp、mv、rm 共用） ---------- */
typedef struct {
    int recursive;     /* -r：复制目录 */
    int no_deref;      /* -P/-d：遇到符号链接时复制链接本身 */
    int deref_args;    /* 命令行上给出的符号链接要跟随（cp -r 默认不跟随） */
    int preserve;      /* -p/-a：保留权限、属主、时间 */
    int force;         /* -f：目标打不开时先删除再试 */
    int no_clobber;    /* -n：目标存在就跳过 */
    int interactive;   /* -i：覆盖前询问 */
    int verbose;       /* -v：打印每个操作 */
} sb_copy_opts;
/* 把 src 复制为 dst（dst 就是最终路径，不会再自动追加文件名）。成功 0，出错打印信息并返回 -1 */
int sb_copy_path(const char *src, const char *dst, const sb_copy_opts *o);
/* 删除 path（recursive 时可以删除目录树）。force：不存在不报错。成功 0 */
int sb_remove_path(const char *path, int recursive, int force, int interactive, int verbose);
/* 询问 yes/no（从标准输入读一行，y/Y/是 开头为同意） */
int sb_ask_yes(void);

/* ---------- 时间 ---------- */
/* strftime 的扩展版：%a %A %b %B %p %c %x %X 输出中文；支持 GNU 的 %N（纳秒）、%:z、
 * 以及标志 - _ 0 ^ 和宽度（如 %-d、%_H、%3N）。返回写入的字节数。 */
size_t sb_strftime(char *buf, size_t n, const char *fmt, const struct tm *tm, long nsec);
/* 解析日期："@秒数"、"YYYY-MM-DD"、"YYYY-MM-DD HH:MM[:SS]"、"HH:MM[:SS]"（今天）、"now"。
 * utc=1 时按 UTC 解释，否则按本地时区（环境变量 TZ）。成功 0 */
int sb_parse_date(const char *s, int utc, struct timespec *out);
/* "2026-10-23 13:55:00.123456789 +0800"（stat、ls --full-time 用） */
void sb_fmt_time_full(char *buf, size_t n, const struct timespec *ts);

/* ---------- 进程（/proc） ---------- */
typedef struct {
    sk_procinfo info;      /* skutil 解析出的基本信息 */
    pid_t pgrp, session;   /* 进程组、会话 */
    pid_t tpgid;           /* 控制终端的前台进程组 */
    int tty_nr;            /* 控制终端的设备号（0 = 没有终端） */
    uid_t uid;             /* 有效用户 */
} sb_proc;
/* 列出所有进程（按 pid 升序），malloc 的数组，失败 NULL */
sb_proc *sb_list_procs(int *count);
/* 补充读取 pgrp/session/tty/uid（sb_list_procs 已经调用过） */
int sb_proc_extra(sb_proc *p);
/* 终端设备号 → 名字："ttyS0"、"tty1"、"pts/0"，没有终端为 "?" */
void sb_tty_name(int tty_nr, char *buf, size_t n);
/* 显示用的命令行：内核线程显示成 [名字]，僵尸进程加 <defunct> */
const char *sb_proc_cmd(const sk_procinfo *p, char *buf, size_t n);
/* 系统启动时刻（/proc/stat 的 btime，Unix 时间戳），失败返回 0 */
long long sb_boot_time(void);

/* ---------- 终端原始模式 ----------
 * 终端默认工作在“规范模式”：内核的行规程（line discipline）会攒够一整行、处理退格，
 * 用户按回车程序才能读到。top、more 需要“按一个键马上响应”，所以要关掉 ICANON 和 ECHO。
 * 退出前必须恢复原来的设置，否则 shell 会变得不回显。 */
typedef struct {
    int fd;
    int active;
    struct termios saved;
} sb_term;
int  sb_term_raw(sb_term *t, int fd);      /* 成功 0 */
void sb_term_restore(sb_term *t);

/* ---------- 关机（reboot/poweroff/halt 共用） ---------- */
int sb_power_main(int argc, char **argv, int sig, int reboot_cmd, const char *msg, const char *usage);

#endif /* SKBOX_H */
