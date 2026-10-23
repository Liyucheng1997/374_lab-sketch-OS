/*
 * webtest.c — 网页版（v86）自动化测试用的“迷你 init”
 *
 * tests/web/run_tests.sh 把它单独打包成一个 initramfs（里面只有 /init = 本程序），
 * 用 tools/v86_harness.mjs 在 v86 模拟器里启动，逐项检查浏览器版依赖的虚拟硬件：
 *   - 串口：harness 的 wait / serial / run 命令（本程序在 /dev/console 上跑一个小命令解释器）
 *   - 键盘：PS/2 扫描码 → 内核 atkbd 驱动 → 虚拟终端 tty1 → 本程序读出来报告到串口
 *   - 鼠标：PS/2 中断(IRQ12) + VMware 后门端口给出的绝对坐标（sketchd 用的是同一套机制）
 *   - 显卡：bochs-drm + fbcon 在 tty1 上画字，harness 的截图读的就是这块显存
 *
 * 它是 1 号进程，所以也要做 1 号进程该做的事：挂载 /proc /sys /dev，回收僵尸进程。
 * 不是 1 号进程时（比如直接在 WSL 里运行）只启动命令解释器，方便单独测试命令解析：
 *     echo "echo a'b' \"c d\"; uname" | ./webtest
 *
 * 串口上的输出格式（测试脚本按这些前缀匹配）：
 *     webtest: start pid=1
 *     webtest: tty1 ready / webtest: tty1 line: <内容>
 *     webtest: input ready mice=<个数> vmmouse=<on|off>
 *     webtest: mouse button left down / webtest: mouse wheel 1
 *     webtest: vmmouse x=<屏幕x> y=<屏幕y> raw=<0..65535>,<0..65535> buttons=<L/M/R或-> wheel=<n>
 *     webtest: shell ready，提示符是 "webtest:~# "
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

/* 32 位 x86 上内核写出的 input_event 是 16 字节（秒、微秒各 32 位）。musl 1.2 的 time_t
 * 是 64 位，如果头文件仍按 struct timeval 定义，结构体会变成 24 字节，读出来全是错位的。
 * musl 定义了 __USE_TIME_BITS64，内核头文件就会改用两个 32 位字段；这里编译期确认一下。 */
_Static_assert(sizeof(struct input_event) == 16, "input_event 必须和内核的 32 位布局一致");

#define MAX_MICE 8

/* ---------- 输出 ---------- */

/* 每条消息先拼好再用一次 write() 写出：本程序有三个进程同时往串口写，
 * 一次写完一整行，行与行之间就不会被别的进程插进来的字打断。
 * 不用 stdio 的缓冲还有一个原因：fork 会把缓冲区里还没写出去的内容复制一份，导致重复输出。 */
static void say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof buf)
        n = sizeof buf - 1;
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += w;
        n -= w;
    }
}

static void write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

/* 把不可打印的字节写成 <xx>，让串口日志里能看清楚键盘到底送来了什么 */
static void escape_bytes(const unsigned char *in, size_t n, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < outsz; i++) {
        unsigned char c = in[i];
        if (c >= 0x20 && c < 0x7f && c != '<')
            out[o++] = (char)c;
        else
            o += (size_t)snprintf(out + o, outsz - o, "<%02x>", c);
    }
    out[o] = '\0';
}

/* ---------- 1 号进程的准备工作 ---------- */

static void mount_one(const char *src, const char *dir, const char *type)
{
    mkdir(dir, 0755);
    /* EBUSY：已经挂载过了（比如内核自动挂了 devtmpfs），不算错误 */
    if (mount(src, dir, type, 0, NULL) < 0 && errno != EBUSY)
        say("webtest: mount %s on %s failed: %s\n", type, dir, strerror(errno));
}

/* 内核在运行 /init 之前会打开 /dev/console 作为 0、1、2 号文件描述符。
 * 如果 initramfs 里没有 /dev/console 这个设备节点，三个描述符就都是关着的，
 * 那就等 devtmpfs 挂好以后自己打开一次。 */
static void ensure_console(void)
{
    if (fcntl(STDOUT_FILENO, F_GETFD) >= 0)
        return;
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return;
    for (int i = 0; i < 3; i++)
        if (fd != i)
            dup2(fd, i);
    if (fd > 2)
        close(fd);
}

/* 子进程退出后会变成“僵尸”：进程已经结束，但退出状态还留在内核里等父进程来取。
 * 1 号进程是所有孤儿进程的“继父”，必须不停地 wait() 把它们收走，否则进程表会被占满。
 * 信号处理函数里只能调用异步信号安全的函数，waitpid 是其中之一。 */
static void on_sigchld(int sig)
{
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved;
}

/* ---------- tty1：键盘 → 虚拟终端 ---------- */

/* 键盘按键的旅程：v86 把按键变成 PS/2 扫描码放进 i8042 控制器并触发 1 号中断，
 * 内核 atkbd 驱动把扫描码翻译成“键码”，再由虚拟终端(VT)按键盘布局翻译成字符，
 * 送到当前前台终端 tty1。我们把 tty1 设成原始模式，自己回显、自己按行报告。 */
static void run_tty1_reader(void)
{
    int fd = open("/dev/tty1", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        say("webtest: tty1 open failed: %s\n", strerror(errno));
        _exit(1);
    }
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        /* 关掉“规范模式”(按行缓冲、处理退格)和内核回显，保留输出处理(\n → \r\n) */
        t.c_lflag &= ~(ICANON | ECHO | ECHONL | ISIG | IEXTEN);
        t.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSANOW, &t) < 0)
            say("webtest: tty1 tcsetattr failed: %s\n", strerror(errno));
    }
    static const char banner[] =
        "\r\n  SketchOS web test system (tests/web/guest/webtest.c)\r\n"
        "  Type here: every line is reported to the serial console.\r\n\r\n> ";
    write_all(fd, banner, sizeof banner - 1);
    say("webtest: tty1 ready\n");

    unsigned char line[256];
    size_t len = 0;
    for (;;) {
        unsigned char buf[64];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            say("webtest: tty1 read failed: %s\n", strerror(errno));
            _exit(1);
        }
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = buf[i];
            if (c == '\r' || c == '\n') {
                char esc[1100];
                escape_bytes(line, len, esc, sizeof esc);
                say("webtest: tty1 line: %s\n", esc);
                write_all(fd, "\r\n> ", 4);
                len = 0;
            } else if (c == 0x7f || c == '\b') {
                if (len > 0) {
                    len--;
                    write_all(fd, "\b \b", 3);
                }
            } else {
                if (len < sizeof line)
                    line[len++] = c;
                if (c >= 0x20 && c < 0x7f)
                    write_all(fd, (const char *)&c, 1);
                else
                    write_all(fd, "?", 1);
            }
        }
    }
}

/* ---------- 鼠标：PS/2 + VMware 后门 ---------- */

/* VMware“后门”：客户机把魔数放进 EAX、命令号放进 ECX，然后对 I/O 端口 0x5658 执行一条 in 指令，
 * 模拟器拦截这条指令，把结果写回 EAX/EBX/ECX/EDX。真机上这个端口什么都没有。
 * 绝对坐标鼠标就是这样实现的：主机知道鼠标在画面上的确切位置(0..65535)，
 * 客户机每收到一次 PS/2 鼠标中断，就来这个端口取一次队列里的坐标包。
 * 普通进程不能执行 in/out 指令（会触发保护错误）；root 用 ioperm() 让内核在
 * 该进程的“I/O 权限位图”里打开这几个端口，这就是“用户态驱动”。 */
#define VMW_MAGIC          0x564D5868u
#define VMW_PORT           0x5658
#define VMW_GETVERSION     10
#define VMW_ABS_DATA       39
#define VMW_ABS_STATUS     40
#define VMW_ABS_COMMAND    41
#define VMW_ABS_ENABLE     0x45414552u
#define VMW_ABS_DISABLE    0x000000F5u
#define VMW_ABS_ABSOLUTE   0x53424152u
#define VMW_VERSION_ID     0x3442554Au
#define VMW_ERROR          0xFFFF0000u
#define VMW_BTN_LEFT       0x20
#define VMW_BTN_RIGHT      0x10
#define VMW_BTN_MIDDLE     0x08
#define VMW_RELATIVE       0x00010000u

struct vmw_regs { uint32_t eax, ebx, ecx, edx; };

static void vmw_call(uint32_t cmd, uint32_t arg, struct vmw_regs *r)
{
    uint32_t a, b, c, d;
    __asm__ __volatile__("inl %%dx, %%eax"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "0"(VMW_MAGIC), "1"(arg), "2"(cmd), "3"(VMW_PORT)
                         : "memory");
    r->eax = a;
    r->ebx = b;
    r->ecx = c;
    r->edx = d;
}

static int vmw_enable(void)
{
    if (ioperm(VMW_PORT, 4, 1) < 0) {
        say("webtest: ioperm(0x5658) failed: %s\n", strerror(errno));
        return 0;
    }
    struct vmw_regs r;
    vmw_call(VMW_GETVERSION, 0, &r);
    if (r.ebx != VMW_MAGIC) {
        say("webtest: no VMware backdoor (ebx=%08x)\n", r.ebx);
        return 0;
    }
    vmw_call(VMW_ABS_COMMAND, VMW_ABS_ENABLE, &r);
    vmw_call(VMW_ABS_STATUS, 0, &r);
    if ((r.eax & VMW_ERROR) == VMW_ERROR || (r.eax & 0xFFFF) == 0) {
        say("webtest: vmmouse enable failed (status=%08x)\n", r.eax);
        return 0;
    }
    vmw_call(VMW_ABS_DATA, 1, &r);
    if (r.eax != VMW_VERSION_ID) {
        say("webtest: vmmouse bad version id %08x\n", r.eax);
        return 0;
    }
    vmw_call(VMW_ABS_COMMAND, VMW_ABS_ABSOLUTE, &r);
    return 1;
}

/* 读取帧缓冲的分辨率，把 0..65535 的绝对坐标换算成屏幕像素 */
static void fb_size(int *w, int *h)
{
    *w = 1024;
    *h = 768;
    FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (!f)
        return;
    int a, b;
    if (fscanf(f, "%d,%d", &a, &b) == 2 && a > 0 && b > 0) {
        *w = a;
        *h = b;
    }
    fclose(f);
}

static void vmw_drain(void)
{
    int w, h;
    fb_size(&w, &h);
    for (int guard = 0; guard < 256; guard++) {
        struct vmw_regs r;
        vmw_call(VMW_ABS_STATUS, 0, &r);
        if ((r.eax & VMW_ERROR) == VMW_ERROR) {
            say("webtest: vmmouse error status, re-enabling\n");
            vmw_enable();
            return;
        }
        uint32_t queued = r.eax & 0xFFFF;
        if (queued < 4)
            return;
        vmw_call(VMW_ABS_DATA, 4, &r);
        if (r.eax & VMW_RELATIVE) {
            say("webtest: vmmouse relative dx=%d dy=%d\n", (int32_t)r.ebx, (int32_t)r.ecx);
            continue;
        }
        /* 线性映射：0 → 第 0 列，65535 → 最后一列 (w-1)，向下取整 */
        int x = (int)((uint64_t)r.ebx * (uint64_t)(w - 1) / 65535u);
        int y = (int)((uint64_t)r.ecx * (uint64_t)(h - 1) / 65535u);
        char btn[4] = "---";
        if (r.eax & VMW_BTN_LEFT)   btn[0] = 'L';
        if (r.eax & VMW_BTN_MIDDLE) btn[1] = 'M';
        if (r.eax & VMW_BTN_RIGHT)  btn[2] = 'R';
        say("webtest: vmmouse x=%d y=%d raw=%u,%u buttons=%s wheel=%d\n",
            x, y, r.ebx, r.ecx, btn, (int)(int8_t)(r.edx & 0xFF));
    }
}

#define BITS_PER_LONG_ (8 * sizeof(unsigned long))
#define TEST_BIT(bit, arr) ((arr[(bit) / BITS_PER_LONG_] >> ((bit) % BITS_PER_LONG_)) & 1UL)

static void run_input_monitor(void)
{
    struct pollfd pfd[MAX_MICE];
    int nmice = 0;
    for (int i = 0; i < 32 && nmice < MAX_MICE; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned long types[(EV_MAX + BITS_PER_LONG_) / BITS_PER_LONG_] = {0};
        char name[128] = "?";
        if (ioctl(fd, EVIOCGBIT(0, sizeof types), types) < 0 || !TEST_BIT(EV_REL, types)) {
            close(fd);   /* 只关心有“相对位移”能力的设备，也就是鼠标 */
            continue;
        }
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) < 0)
            strcpy(name, "?");
        say("webtest: input mouse %s \"%s\"\n", path, name);
        pfd[nmice].fd = fd;
        pfd[nmice].events = POLLIN;
        nmice++;
    }
    int vmw = vmw_enable();
    say("webtest: input ready mice=%d vmmouse=%s\n", nmice, vmw ? "on" : "off");
    if (nmice == 0)
        _exit(0);

    for (;;) {
        /* 没有事件就一直睡在 poll() 里：不占 CPU（在 v86 里尤其重要） */
        int n = poll(pfd, nmice, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            say("webtest: poll failed: %s\n", strerror(errno));
            _exit(1);
        }
        for (int i = 0; i < nmice; i++) {
            if (!(pfd[i].revents & POLLIN))
                continue;
            struct input_event ev[32];
            ssize_t r = read(pfd[i].fd, ev, sizeof ev);
            if (r < (ssize_t)sizeof ev[0])
                continue;
            for (size_t k = 0; k < (size_t)r / sizeof ev[0]; k++) {
                const struct input_event *e = &ev[k];
                if (e->type == EV_KEY && e->code >= BTN_LEFT && e->code <= BTN_MIDDLE) {
                    static const char *names[] = {"left", "right", "middle"};
                    say("webtest: mouse button %s %s\n", names[e->code - BTN_LEFT],
                        e->value ? "down" : "up");
                } else if (e->type == EV_REL && e->code == REL_WHEEL) {
                    say("webtest: mouse wheel %d\n", e->value);
                } else if (e->type == EV_SYN && e->code == SYN_REPORT && vmw) {
                    /* 一个 PS/2 数据包处理完了：去后门端口取这一刻的绝对坐标 */
                    vmw_drain();
                }
            }
        }
    }
}

/* ---------- 串口上的小命令解释器 ---------- */

/* 把一条命令切成参数：空白分隔，支持 '单引号'、"双引号" 和反斜杠转义。
 * 就地改写字符串（输出位置永远不会超过读取位置），返回参数个数。 */
static int split_args(char *s, char **argv, int max)
{
    int argc = 0;
    char *in = s, *out = s;
    while (*in && argc < max - 1) {
        while (*in == ' ' || *in == '\t')
            in++;
        if (!*in)
            break;
        argv[argc++] = out;
        char quote = 0;
        while (*in) {
            char c = *in;
            if (quote) {
                if (c == quote) {
                    quote = 0;
                    in++;
                    continue;
                }
                if (quote == '"' && c == '\\' && (in[1] == '"' || in[1] == '\\'))
                    c = *++in;
            } else {
                if (c == ' ' || c == '\t') {
                    in++;
                    break;
                }
                if (c == '\'' || c == '"') {
                    quote = c;
                    in++;
                    continue;
                }
                if (c == '\\' && in[1])
                    c = *++in;
            }
            *out++ = c;
            in++;
        }
        *out++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

/* 按不在引号里的 ';' 把一行切成多条命令 */
static int split_commands(char *line, char **cmds, int max)
{
    int n = 0;
    char quote = 0;
    if (max < 1)
        return 0;
    cmds[n++] = line;
    for (char *p = line; *p; p++) {
        if (quote) {
            if (*p == quote)
                quote = 0;
            else if (*p == '\\' && quote == '"' && p[1])
                p++;
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '\\' && p[1]) {
            p++;
        } else if (*p == ';' && n < max) {
            *p = '\0';
            cmds[n++] = p + 1;
        }
    }
    return n;
}

static void cmd_cat(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        say("cat: %s: %s\n", path, strerror(errno));
        return;
    }
    char buf[4096];
    ssize_t n;
    size_t total = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0 && total < (1 << 20)) {
        write_all(STDOUT_FILENO, buf, (size_t)n);
        total += (size_t)n;
    }
    close(fd);
}

static void cmd_ls(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        say("ls: %s: %s\n", path, strerror(errno));
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            say("%s\n", e->d_name);
    closedir(d);
}

static void cmd_tty1(int argc, char **argv)
{
    int fd = open("/dev/tty1", O_WRONLY | O_NOCTTY);
    if (fd < 0) {
        say("tty1: %s\n", strerror(errno));
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            write_all(fd, " ", 1);
        write_all(fd, argv[i], strlen(argv[i]));
    }
    write_all(fd, "\r\n", 2);
    close(fd);
}

static void run_command(int argc, char **argv)
{
    const char *c = argv[0];
    if (!strcmp(c, "echo")) {
        char out[2048];
        size_t o = 0;
        for (int i = 1; i < argc && o < sizeof out - 2; i++)
            o += (size_t)snprintf(out + o, sizeof out - o, "%s%s", i > 1 ? " " : "", argv[i]);
        if (o > sizeof out - 2)
            o = sizeof out - 2;
        out[o++] = '\n';
        write_all(STDOUT_FILENO, out, o);
    } else if (!strcmp(c, "uname")) {
        struct utsname u;
        if (uname(&u) == 0)
            say("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
    } else if (!strcmp(c, "cat") && argc > 1) {
        for (int i = 1; i < argc; i++)
            cmd_cat(argv[i]);
    } else if (!strcmp(c, "ls")) {
        cmd_ls(argc > 1 ? argv[1] : ".");
    } else if (!strcmp(c, "tty1")) {
        cmd_tty1(argc, argv);
    } else if (!strcmp(c, "fb")) {
        int w, h;
        fb_size(&w, &h);
        say("fb0 %dx%d\n", w, h);
    } else if (!strcmp(c, "sleep") && argc > 1) {
        struct timespec ts = { atoi(argv[1]), 0 };
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
            ;
    } else if (!strcmp(c, "true")) {
        /* 什么也不做 */
    } else if (!strcmp(c, "poweroff") || !strcmp(c, "reboot")) {
        if (getpid() != 1) {
            say("%s: 只有 1 号进程才这样做\n", c);
            return;
        }
        say("webtest: %s\n", c);
        sync();
        /* 没有 ACPI 时内核不会真正断电，而是打印 "reboot: System halted" 后停机；
         * 重启时打印 "reboot: Restarting system" 并通过键盘控制器复位 CPU。
         * 网页版靠这两句话判断虚拟机关机/重启了。 */
        reboot(!strcmp(c, "poweroff") ? RB_POWER_OFF : RB_AUTOBOOT);
        say("webtest: reboot() failed: %s\n", strerror(errno));
    } else if (!strcmp(c, "help")) {
        say("命令: echo uname cat ls tty1 fb sleep true poweroff reboot help\n");
    } else {
        say("webtest: 未知命令: %s\n", c);
    }
}

static void run_shell(void)
{
    char line[4096];
    int interactive = isatty(STDIN_FILENO);
    say("webtest: shell ready\n");
    for (;;) {
        if (interactive)
            say("webtest:~# ");
        if (!fgets(line, sizeof line, stdin)) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            if (getpid() != 1)
                return;
            /* 1 号进程不能退出（退出内核就会 panic），控制台读不了就睡着 */
            for (;;)
                pause();
        }
        line[strcspn(line, "\r\n")] = '\0';
        char *cmds[32];
        int nc = split_commands(line, cmds, 32);
        for (int i = 0; i < nc; i++) {
            char *argv[64];
            int argc = split_args(cmds[i], argv, 64);
            if (argc > 0)
                run_command(argc, argv);
        }
    }
}

/* fork 一个子进程去执行 fn；fn 不返回 */
static void spawn(void (*fn)(void), const char *what)
{
    pid_t pid = fork();
    if (pid < 0) {
        say("webtest: fork %s failed: %s\n", what, strerror(errno));
        return;
    }
    if (pid == 0) {
        /* 子进程恢复默认的 SIGCHLD 处理，免得继承父进程的“回收所有子进程” */
        signal(SIGCHLD, SIG_DFL);
        fn();
        _exit(0);
    }
}

int main(void)
{
    if (getpid() == 1) {
        mount_one("devtmpfs", "/dev", "devtmpfs");
        ensure_console();
        mount_one("proc", "/proc", "proc");
        mount_one("sysfs", "/sys", "sysfs");
        say("\nwebtest: start pid=1\n");

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sigchld;
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);

        spawn(run_tty1_reader, "tty1");
        spawn(run_input_monitor, "input");
    }
    run_shell();
    return 0;
}
