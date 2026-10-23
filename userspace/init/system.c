/*
 * system.c — 开机时的“系统级设置”：标准输入输出、主机名、时区和环境变量、回环网卡、Ctrl+Alt+Del
 */
#define _GNU_SOURCE
#include "sinit.h"
#include "skutil.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---------------- 标准输入/输出/错误 ----------------
 * 内核启动 1 号进程之前，会打开 /dev/console 并把它复制成 0、1、2 号文件描述符。
 * 但如果根文件系统里没有 /dev/console 这个设备文件，这三个描述符就是“空”的。
 * 那样很危险：之后第一个 open() 会拿到 0 号，被当成标准输入；
 * 往“标准错误”写的报错可能写进了某个配置文件里。所以先把它们补上。 */
void sys_stdio_ensure(void)
{
    for (int fd = 0; fd <= 2; fd++) {
        if (fcntl(fd, F_GETFD) >= 0)
            continue;
        int nfd = open("/dev/console", O_RDWR | O_NOCTTY);
        if (nfd < 0)
            nfd = open("/dev/null", O_RDWR);
        if (nfd < 0)
            continue;            /* 连 /dev/null 都没有，只能先这样，挂载 /dev 之后会再补一次 */
        if (nfd != fd) {
            dup2(nfd, fd);
            close(nfd);
        }
    }
}

/* sinit 自己的普通日志用 sk_log() 写进内核日志（dmesg 可以看），
 * 重要的消息用 sk_log_console() 同时写到控制台。sk_log 还会顺便写一份到 stderr——
 * 如果 stderr 仍是控制台，每条重要消息就会在串口上出现两遍，还会和串口 shell 的输出搅在一起。
 * 所以挂载好 /dev 之后，把 1 号进程自己的 0/1/2 指向 /dev/null（它们依然“有效”）；
 * 横幅、关机提示这些要给人看的内容，由 console.c 单独打开控制台来写。
 * 服务进程的 0/1/2 是启动时单独设置的，不受影响。 */
void sys_stdio_detach(void)
{
    int fd = open("/dev/null", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        sys_stdio_ensure();      /* 连 /dev/null 都没有（/dev 没挂上）：至少保证三个描述符有效 */
        return;
    }
    for (int i = 0; i <= 2; i++)
        if (fd != i)
            dup2(fd, i);
    if (fd > 2)
        close(fd);
}

/* ---------------- 主机名 ----------------
 * 主机名保存在内核里（uname -n 读的就是它）。每次开机都要由 1 号进程从 /etc/hostname 设置。 */
static int valid_hostname(const char *s)
{
    if (!*s || strlen(s) > 63)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '.')
            return 0;
    return 1;
}

void sys_set_hostname(char *out, size_t n)
{
    char buf[128];
    if (sk_read_line("/etc/hostname", buf, sizeof buf) == 0) {
        char *name = sk_trim(buf);
        if (!valid_hostname(name))
            sk_log_console("/etc/hostname 里的主机名 \"%s\" 不合法（只能用字母、数字、- 和 .）", name);
        else if (sethostname(name, strlen(name)) != 0)
            sk_log_console("设置主机名失败: %s", sk_strerror(errno));
    } else {
        sk_log("没有 /etc/hostname，使用内核默认主机名");
    }
    if (gethostname(out, n) != 0)
        sk_strlcpy(out, "sketchos", n);
    out[n - 1] = 0;
}

/* ---------------- 环境变量与时区 ----------------
 * 环境变量是进程从父进程那里“继承”来的一组 KEY=VALUE 字符串。
 * 1 号进程是所有进程的祖先，所以它给服务设置的环境，最终会传给系统里的每一个程序。
 *
 * TZ 时区：内核时钟只认 UTC（世界标准时间），“北京时间”是 C 库换算出来的。
 * POSIX 格式 "CST-8" 的意思是：时区名叫 CST，本地时间 = UTC + 8 小时（注意符号是反的）。 */
static char  g_env_store[8][160];
static char *g_env[9];

static int valid_tz(const char *s)
{
    if (!*s || strlen(s) > 60)
        return 0;
    for (; *s; s++)
        if (!isgraph((unsigned char)*s))
            return 0;
    return 1;
}

void sys_setup_env(char *tz_out, size_t n)
{
    char buf[128];
    const char *tz = NULL;
    char *t;

    if (sk_cmdline_get("sketch.tz", buf, sizeof buf) && valid_tz(t = sk_trim(buf)))
        tz = t;
    char filebuf[128];
    if (!tz && sk_read_line("/etc/sketch/timezone", filebuf, sizeof filebuf) == 0 &&
        valid_tz(t = sk_trim(filebuf)))
        tz = t;
    if (!tz)
        tz = "CST-8";
    sk_strlcpy(tz_out, tz, n);

    const char *vars[][2] = {
        { "PATH",  "/bin:/sbin" },
        { "HOME",  SINIT_HOME },
        { "USER",  "root" },
        { "LOGNAME", "root" },
        { "SHELL", "/bin/sksh" },
        { "TERM",  "linux" },
        { "TZ",    tz },
        { "LANG",  "C.UTF-8" },
    };
    for (size_t i = 0; i < SK_ARRAY_LEN(vars); i++) {
        snprintf(g_env_store[i], sizeof g_env_store[i], "%s=%s", vars[i][0], vars[i][1]);
        g_env[i] = g_env_store[i];
    }
    g_env[SK_ARRAY_LEN(vars)] = NULL;

    /* 内核会把命令行里它不认识的 xxx=yyy 参数塞进 1 号进程的环境变量，
     * 这里清空后换成我们自己的这一套，sinit 自己（比如显示开机时间）也用同样的时区。 */
    clearenv();
    for (int i = 0; g_env[i]; i++)
        putenv(g_env[i]);
    tzset();
}

char **sys_service_env(void)
{
    return g_env;
}

const char *sys_env_get(const char *key)
{
    size_t kl = strlen(key);
    for (int i = 0; g_env[i]; i++)
        if (strncmp(g_env[i], key, kl) == 0 && g_env[i][kl] == '=')
            return g_env[i] + kl + 1;
    return NULL;
}

/* ---------------- 回环网卡 lo ----------------
 * 即使电脑没有网卡，内核里也总有一个虚拟网卡 lo（loopback，回环）：
 * 发给 127.0.0.1 的数据包不会离开本机，直接“绕回来”交给本机的程序。
 * 很多程序默认用 TCP 连 localhost 做进程间通信，所以 lo 必须是“启用”(UP) 状态。
 * 内核创建它时是关着的，开机时由 1 号进程打开——相当于命令 ip link set lo up。
 * 方法是对任意一个套接字调用 ioctl(SIOCGIFFLAGS/SIOCSIFFLAGS) 读出、改写网卡标志位；
 * lo 一启用，内核会自动给它配上 127.0.0.1/8 地址。 */
void sys_loopback_up(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        sk_log("无法创建套接字来配置 lo: %s", sk_strerror(errno));
        return;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    sk_strlcpy(ifr.ifr_name, "lo", sizeof ifr.ifr_name);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        sk_log("读取 lo 状态失败: %s", sk_strerror(errno));
    } else if (!(ifr.ifr_flags & IFF_UP)) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0)
            sk_log("启用 lo 失败: %s", sk_strerror(errno));
        else
            sk_log("回环网卡 lo 已启用 (127.0.0.1)");
    }
    close(fd);
}

/* ---------------- Ctrl+Alt+Del ----------------
 * 默认情况下，在键盘上按 Ctrl+Alt+Del，内核会立刻重启——不通知任何程序，数据可能丢失。
 * reboot(RB_DISABLE_CAD) 告诉内核：别自己重启，改成给 1 号进程发 SIGINT 信号，
 * 由 sinit 走正常的关机流程（先通知所有进程退出，再重启）。 */
void sys_disable_cad(void)
{
    if (reboot(RB_DISABLE_CAD) != 0)
        sk_log("reboot(RB_DISABLE_CAD) 失败: %s", sk_strerror(errno));
}

int sys_gui_enabled(void)
{
    return !sk_cmdline_get("sketch.nogui", NULL, 0);
}

/* 开机到现在的时间，单位“百分之一秒”（/proc/uptime 第一个数，如 "12.34"）。
 * 这里故意只用整数解析：1 号进程越简单越可靠，没有必要为了显示一个时间动用浮点运算。 */
long sys_uptime_centis(void)
{
    char line[128];
    if (sk_read_line("/proc/uptime", line, sizeof line) != 0)
        return -1;
    const char *p = line;
    long sec = 0, cs = 0;
    if (!isdigit((unsigned char)*p))
        return -1;
    while (isdigit((unsigned char)*p))
        sec = sec * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        for (int i = 0; i < 2; i++) {
            cs *= 10;
            if (isdigit((unsigned char)*p))
                cs += *p++ - '0';
        }
    }
    return sec * 100 + cs;
}

int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
