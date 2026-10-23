/*
 * sinit.h — SketchOS 的 1 号进程 sinit 的内部接口
 *
 * ┌──────────────────────── 1 号进程要做的事 ────────────────────────┐
 * │ 内核启动的最后一步，是在根文件系统里找到 /init 并以 PID 1 运行它。 │
 * │ 从这一刻起，“用户态世界”里的一切都由 1 号进程负责创造：           │
 * │   1. 挂载虚拟文件系统（/proc、/sys、/dev……），让别的程序能工作    │
 * │   2. 设置主机名、时区、环境变量、回环网卡                         │
 * │   3. 按服务表启动服务（shell、图形桌面），并在它们退出时重启        │
 * │   4. 收养并回收所有“孤儿进程”，防止僵尸进程堆积                  │
 * │   5. 收到关机/重启信号时，有序地结束所有进程，再请求内核断电        │
 * │ 1 号进程绝对不能退出：它一退出，内核就会 panic（整个系统崩溃）。     │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * 源文件分工：
 *   main.c      入口、开机流程、事件循环、作为普通命令运行时的命令行
 *   console.c   往控制台输出（横幅、关机提示）
 *   mounts.c    挂载/卸载文件系统
 *   system.c    主机名、时区、环境变量、回环网卡、Ctrl+Alt+Del
 *   signals.c   信号处理（signalfd，或者“自管道”后备方案）
 *   services.c  服务表解析、启动服务、重启策略、状态文件
 *   shutdown.c  关机流程
 */
#ifndef SINIT_H
#define SINIT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SINIT_CONF        "/etc/sketch/services.conf"
#define SINIT_STATUS_DIR  "/run/sinit"
#define SINIT_STATUS      "/run/sinit/status"
#define SINIT_HOME        "/root"

#define SVC_NAME_MAX   32      /* 服务名最长 31 个字符 */
#define SVC_PATH_MAX   128
#define SVC_ARGV_MAX   32      /* 命令最多 32 个参数 */
#define SVC_MAX        64      /* 服务表最多 64 项 */
#define SVC_DELAY_MAX  600000  /* delay= 最大 10 分钟 */

/* 重启频率限制：10 秒内退出超过 5 次（即第 6 次），就认为它“坏掉了”，不再重启 */
#define RESPAWN_WINDOW_MS  10000
#define RESPAWN_MAX_EXITS  5
/* fork 失败（内存不足等）时，隔多久再试 */
#define RETRY_FORK_MS      1000

/* ---------------- 服务 ---------------- */

typedef enum {
    POLICY_RESPAWN,   /* 退出后自动重启 */
    POLICY_ONCE,      /* 只启动一次 */
    POLICY_WAIT,      /* 启动后等它结束，再启动后面的服务 */
} svc_policy;

typedef enum {
    COND_ALWAYS,      /* 没有 cond= 选项 */
    COND_GUI,         /* 只在图形模式下启动（命令行里没有 sketch.nogui） */
    COND_NOGUI,       /* 只在文字模式下启动（命令行里有 sketch.nogui） */
} svc_cond;

typedef enum {
    SVC_STOPPED,      /* 没在运行：等待延时启动、等待重试，或者已在关机时停止 */
    SVC_RUNNING,
    SVC_FAILED,       /* 放弃了：崩溃太频繁 / 找不到程序 / once 服务返回非 0 */
    SVC_DONE,         /* once/wait 服务正常结束（退出码 0） */
    SVC_DISABLED,     /* cond= 条件不满足，本次开机不启动 */
} svc_state;

typedef struct service {
    /* ---- 来自配置文件 ---- */
    char        name[SVC_NAME_MAX];
    svc_policy  policy;
    svc_cond    cond;
    char        tty[SVC_PATH_MAX];     /* "-" 表示没有终端（/dev/null） */
    char        log[SVC_PATH_MAX];     /* 空串：不重定向 */
    int         delay_ms;              /* 每次启动前等待的毫秒数 */
    int         argc;
    char       *argv[SVC_ARGV_MAX + 1];/* 以 NULL 结尾，字符串都在 argbuf 里 */
    char       *argbuf;
    char        cmdline[512];          /* 命令行（状态文件、日志里显示用） */
    int         line;                  /* 在配置文件的第几行 */

    /* ---- 运行时状态 ---- */
    svc_state   state;
    pid_t       pid;                   /* 0 = 没在运行 */
    int         restarts;              /* 累计被重启了几次 */
    int         last_status;           /* 上次退出的状态码（被信号杀死时是 128+信号） */
    int64_t     start_at;              /* >0：计划在这个时刻（单调时钟，毫秒）启动 */
    int64_t     started_ms;            /* 本次启动的时刻 */
    int64_t     exit_ms[RESPAWN_MAX_EXITS + 1]; /* 最近几次退出的时刻（环形缓冲） */
    int         exit_pos;
} service;

/* 服务表解析：把 path 里的服务追加到 out[]（最多 max 个），返回解析到的个数，文件打不开返回 -1。
 * *errors 返回出错的行数。 */
int  svc_parse_file(const char *path, service **out, int max, int *errors);
void svc_free(service *s);

const char *svc_state_name(svc_state st);
const char *svc_policy_name(svc_policy p);

/* 1 号进程里的服务管理（services.c） */
void svc_init(int gui);                 /* gui=0 表示命令行里有 sketch.nogui */
void svc_load_and_boot(void);           /* 读服务表并开始按顺序启动 */
int  svc_child_exited(pid_t pid, int status);   /* 是某个服务的进程就返回 1 */
void svc_run_timers(int64_t now);       /* 启动到时间的服务 */
int  svc_next_timeout(int64_t now);     /* 距离下一个计划启动还有多少毫秒，-1 表示没有 */
void svc_reload(void);                  /* SIGHUP：重新读服务表，只启动新增的服务 */
void svc_shutdown_begin(void);          /* 关机：不再重启任何服务 */
int  svc_list_ttys(const char **out, int max);  /* 服务用到的终端（去重，不含 "-"），返回个数 */
void svc_write_status(void);            /* 写 /run/sinit/status */
int  svc_check_main(const char *path);  /* sinit --check：检查配置文件并打印 */

/* 错误报告方式：1 号进程里写控制台和内核日志；作为命令运行时写 stderr */
extern int g_is_pid1;
void sinit_report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------------- 控制台（console.c） ---------------- */

void con_init(void);                     /* 打开所有活动的控制台 */
/* 写到所有控制台：zh 给串口等能显示中文的控制台，en 给屏幕上的虚拟终端（它的字体没有汉字） */
void con_write_split(const char *zh, const char *en);
void con_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* 只用于纯 ASCII 内容 */
#define CON_SAY(zh, en, ...) do {                                   \
        char con_zh_[1024], con_en_[1024];                          \
        snprintf(con_zh_, sizeof con_zh_, zh, ##__VA_ARGS__);       \
        snprintf(con_en_, sizeof con_en_, en, ##__VA_ARGS__);       \
        con_write_split(con_zh_, con_en_);                          \
    } while (0)
void con_banner(const char *hostname, const char *tz, long boot_centis);   /* boot_centis<0 表示不知道 */
/* 画一个“手绘”提示框（中文版和英文版各一份，lines 以 NULL 结尾） */
void con_box(const char *title_zh, const char *const *zh, const char *title_en, const char *const *en);

/* ---------------- 挂载（mounts.c） ---------------- */

int  mount_early(void);                  /* 返回失败的挂载个数（错误已记录） */
void mount_early_report(void);           /* 日志系统就绪后，补报挂载阶段的问题 */
void mount_teardown(void);               /* 关机前尽力卸载 tmpfs */

/* ---------------- 系统设置（system.c） ---------------- */

void        sys_stdio_ensure(void);      /* 保证 0/1/2 号文件描述符有效 */
void        sys_stdio_detach(void);      /* 把 sinit 自己的 0/1/2 改指向 /dev/null */
void        sys_set_hostname(char *out, size_t n);
void        sys_setup_env(char *tz_out, size_t n);
char      **sys_service_env(void);       /* 给服务用的环境变量数组 */
const char *sys_env_get(const char *key);
void        sys_loopback_up(void);
void        sys_disable_cad(void);
int         sys_gui_enabled(void);
int64_t     now_ms(void);                /* 单调时钟（开机以来的毫秒数，不受改系统时间影响） */
long        sys_uptime_centis(void);     /* /proc/uptime，单位 1/100 秒；失败返回 -1 */

/* ---------------- 信号（signals.c） ---------------- */

typedef struct {
    int   signo;
    pid_t sender;    /* 发送者 pid（自管道模式下不知道，为 0） */
} sig_event;

int  sig_setup(void);                    /* 返回要 poll 的文件描述符 */
int  sig_fd(void);
int  sig_read(sig_event *ev, int max);   /* 取出已到达的信号，返回个数 */
void sig_reset_in_child(void);           /* fork 之后、exec 之前调用 */
const char *sig_name(int signo);

/* ---------------- 进程回收与关机（main.c / shutdown.c） ---------------- */

typedef enum { SD_NONE, SD_REBOOT, SD_HALT, SD_POWEROFF } sd_kind;

void reap_children(void);                /* 回收所有已退出的子进程（包括被收养的孤儿） */
void sinit_shutdown(sd_kind how) __attribute__((noreturn));
void sinit_idle_forever(void) __attribute__((noreturn));

#endif /* SINIT_H */
