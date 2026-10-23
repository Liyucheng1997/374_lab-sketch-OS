/*
 * skabout — 关于本机：系统信息 + 手绘的“系统分层结构图”
 *
 * 左边是一张图：SketchOS 从上到下分成四层——图形应用、我们自己写的系统程序、Linux 内核、
 * （虚拟的）硬件；每两层之间的箭头上写着它们“怎么说话”。点图里的任何一个方块或箭头，
 * 下面的便签就会解释它是做什么的。右边是这台机器的信息。窗口太窄时改成两个标签页。
 *
 * ┌─────────────── “关于本机”的信息都是从哪儿来的？ ───────────────┐
 *
 *  系统名称和版本  /etc/os-release：几乎所有 Linux 发行版都有的文本文件，KEY="值" 的格式。
 *                  程序读它来知道“这是什么系统”，而不是去猜。
 *  内核版本、架构  uname() 系统调用：内核把自己的名字、版本号、机器类型（i686、x86_64……）
 *  主机名          填进一个结构体交给我们。主机名也保存在内核里，开机时由 sinit 从
 *                  /etc/hostname 读出来、用 sethostname() 设置。
 *  处理器          /proc/cpuinfo：内核开机时用 CPUID 指令问出来的型号、频率、特性。
 *  内存            /proc/meminfo 的 MemTotal：比“插了多少内存”略少，因为内核自己的代码、
 *                  页表等在开机时就占掉了一部分。
 *  开机时长        /proc/uptime：内核从开机起数了多少秒。
 *  屏幕大小        不是问内核，而是问显示服务器 sketchd（连接时它在 WELCOME 报文里告诉我们）。
 *
 * └────────────────────────────────────────────────────────────────┘
 *
 * 性能：画面基本是静止的。唯一的定时器每分钟触发一次，只重画“已运行”那一行；
 * 点击图里的方块时只重画新旧两个方块和下面的便签。
 *
 * 命令行与调试：
 *   环境变量 SKABOUT_OS_RELEASE=文件   用这个文件代替 /etc/os-release（在宿主机上测试时用）
 *   环境变量 SKABOUT_DEBUG=1           布局和点击之后在标准输出打印一行 “SKABOUT {JSON}”：
 *                                      各项系统信息、布局模式、图里每个方块的位置、选中了什么。
 *                                      自动化测试 tests/apps/test_skabout.py 用它核对内容、找点击位置。
 */
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include <sketch/rough.h>
#include "skutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define MARGIN      16
#define TABS_Y      8
#define TABS_H      40
#define INFO_W      310          /* 宽窗口时右边信息栏的宽度 */
#define WIDE_MIN    780          /* 窗口至少这么宽才把图和信息并排放 */
#define NLAYERS     4
#define MAX_CHIPS   8
#define NINFO       7
#define NOTE_H      120         /* 便签：标题一行 + 三行解释 */
#define NOTE_MIN_H  80          /* 比这还矮就不放便签，改用消息框显示解释 */

/* ---------------------------------------------------------------- 图里的内容 */

typedef struct {
    int         icon;        /* -1 = 没有图标 */
    const char *name;        /* 程序名 / 短名字 */
    const char *label;       /* 跟在名字后面的中文（紧凑排版时省略） */
    const char *about;       /* 点击后便签上的解释 */
} chip_def;

typedef struct {
    const char *title;
    const char *sub;
    sk_color    color;
    int         icon;
    const char *about;
    int         nchips;
    chip_def    chips[MAX_CHIPS];
} layer_def;

static const layer_def layers[NLAYERS] = {
    { "图形应用", "每个应用都是一个独立的进程", SK_YELLOW, SK_ICON_SKETCH_LOGO,
      "每个图形应用都是一个普通进程，自己不碰显卡：画好的像素放在和 sketchd 共享的内存里，"
      "再通过 Unix 域套接字告诉 sketchd“这一块画好了”。",
      8,
      { { SK_ICON_TERMINAL, "skterm", "终端", "终端模拟器：在窗口里画出字符，把键盘输入经过伪终端（pty）交给 shell，再把程序的输出画出来。" },
        { SK_ICON_FOLDER, "skfiles", "文件", "文件管理器：用 opendir / readdir / stat 浏览目录，用 rename / unlink 整理文件。" },
        { SK_ICON_NOTEPAD, "sknote", "记事本", "记事本：用 open / read / write 读写文本文件；多行编辑器控件来自 libsketch。" },
        { SK_ICON_PAINT, "skpaint", "画板", "画板：直接在一块像素缓冲区上作画，可以保存成 PPM 图片。" },
        { SK_ICON_MONITOR, "skmon", "监视器", "系统监视器：每秒读一遍 /proc，显示 CPU、内存和所有进程，还能给进程发信号。" },
        { SK_ICON_CLOCK, "skclock", "时钟", "时钟：每秒被定时器唤醒一次读当前时间，其余时间睡在 poll() 里，不占 CPU。" },
        { SK_ICON_MUSIC, "skmusic", "音乐", "音乐播放器：按简谱算出声音的采样（或者从 WAV 文件里读出来），write() 进 /dev/dsp，声卡就把它播出来。" },
        { SK_ICON_ABOUT, "skabout", "关于", "就是你正在看的这个窗口：它读 /etc/os-release、uname()、/proc/cpuinfo 来认识这台机器。" } } },
    { "系统程序", "用户态 · 全是我们自己写的", SK_BLUE, SK_ICON_PROCESS,
      "内核之上的用户态程序全部是自己写的（只用了 musl C 库）。它们碰不到硬件，"
      "想读文件、开进程、画屏幕，都要通过系统调用请内核帮忙。",
      5,
      { { -1, "sinit", "1 号进程", "内核启动后运行的第一个程序（PID 1）：挂载 /proc 等文件系统，启动并看护各项服务，回收孤儿进程，负责关机和重启。" },
        { -1, "sksh", "命令行", "命令行 shell：读入一行命令，fork() 出子进程，再用 execve() 运行程序；管道和重定向靠 pipe() 和 dup2()。" },
        { -1, "skbox", "工具箱", "多合一工具箱：ls、cat、ps、top 等几十个命令其实是同一个程序，靠 argv[0]（被叫做什么名字）区分。" },
        { -1, "sketchd", "显示服务器", "显示服务器 + 窗口管理器：独占显卡和键盘鼠标，把各个窗口的共享内存合成到屏幕上，再把输入事件发给对应的窗口。" },
        { -1, "libsketch", "图形库", "图形库：手绘图元、字体、控件和连接 sketchd 的协议都在这里，每个图形应用都静态链接了它。" } } },
    { "Linux 6.12 内核", "原版内核，只裁剪了配置", SK_GREEN, SK_ICON_KERNEL,
      "内核运行在 CPU 的特权级（内核态），是唯一能直接操作硬件的程序：调度进程、管理内存、"
      "组织文件系统、驱动设备。我们用的是没改过的 Linux 6.12。",
      4,
      { { -1, "进程调度", NULL, "调度器决定下一刻由谁使用 CPU：时钟中断定期打断正在运行的进程，让大家轮流运行。" },
        { -1, "内存管理", NULL, "每个进程有自己的虚拟地址空间，内核用页表把它映射到物理内存；malloc 和 mmap 最终都要找它要内存页。" },
        { -1, "文件系统", NULL, "VFS 把 tmpfs、proc、sysfs、devtmpfs 统一成一棵目录树；/proc 里的文件是内核在你读的时候现场生成的。" },
        { -1, "设备驱动", NULL, "bochs-drm 显卡、i8042 键盘鼠标、evdev 输入、8250 串口、ALSA 的 SB16 声卡……驱动把硬件包装成 /dev 下的文件。" } } },
    { "虚拟硬件", "QEMU 或 v86（浏览器里的模拟器）", SK_ORANGE, SK_ICON_CHIP,
      "这台“电脑”是模拟出来的：在 Windows 上由 QEMU、在网页里由 v86 扮演一台完整的 x86 电脑。"
      "内核以为自己在真机上，操作的是同样的端口和寄存器。",
      6,
      { { -1, "CPU", NULL, "x86 处理器：一条条执行指令；区分内核态和用户态，收到中断就跳进内核的处理程序。" },
        { -1, "内存", NULL, "物理内存（RAM）：内核把它切成 4 KB 的“页”，再分给各个进程和自己用。" },
        { -1, "显卡", NULL, "Bochs VBE 显卡：一块帧缓冲（显存），内核的 bochs-drm 驱动把它交给 sketchd 画桌面。" },
        { -1, "键盘鼠标", NULL, "PS/2 键盘和鼠标：每按一个键就产生一次中断，内核的 i8042 驱动把它变成输入事件。" },
        { -1, "串口", NULL, "16550 串口：最古老、最简单的“终端”，开机日志和串口 shell 都走这里。" },
        { -1, "声卡", NULL, "Sound Blaster 16：插在 ISA 总线上的老式声卡（端口 0x220、中断 5），用 DMA 自己从内存里取声音采样播放。" } } },
};

/* 相邻两层之间怎么“说话” */
static const struct { const char *label; const char *about; } links[NLAYERS - 1] = {
    { "Unix 域套接字 + 共享内存",
      "应用和 sketchd 之间：小消息（创建窗口、鼠标事件、“这块画好了”）走 Unix 域套接字；"
      "整窗的像素放在 memfd 共享内存里，两边映射同一批物理页，一个像素都不用拷贝。" },
    { "系统调用",
      "用户程序请内核办事的唯一入口：open、read、write、mmap、fork、execve、ioctl……"
      "执行时 CPU 从用户态切到内核态，内核办完再切回来。" },
    { "I/O 端口 · 中断",
      "内核用 in/out 指令和内存映射的寄存器操作硬件；硬件有事（按了键、收到数据、时钟到点）"
      "就发出中断，CPU 立刻放下手里的事去执行内核的中断处理程序。" },
};

/* ---------------------------------------------------------------- 应用状态 */

enum { SEL_NONE = 0, SEL_LAYER, SEL_CHIP, SEL_LINK };

typedef struct {
    const char *key;
    char        value[200];
    const char *source;
    sk_rect     r;           /* 这一行（含来源说明）在窗口里的位置，draw 时算好 */
} info_row;

enum { INFO_VERSION, INFO_KERNEL, INFO_CPU, INFO_MEM, INFO_UPTIME, INFO_SCREEN, INFO_HOST };

typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    int        debug;
    int        wide;             /* 1 = 图和信息并排；0 = 标签页 */
    int        tab;
    int        uptime_timer;

    /* 系统信息 */
    char       os_name[64], os_pretty[128];
    char       release[128], machine[64], hostname[128];
    info_row   info[NINFO];
    int        info_h;           /* 信息栏内容的总高度（调试用） */
    int        info_compact;     /* 1 = 地方不够，不写每一行的来源说明 */

    /* 图的排版结果（窗口坐标） */
    int        compact;          /* 0..3：图太挤时逐级收紧 */
    int        note_on;          /* 便签放得下吗（放不下时点击改为弹出消息框） */
    sk_rect    title_r, layer_r[NLAYERS], chip_r[NLAYERS][MAX_CHIPS], link_r[NLAYERS - 1], note_r;
    int        sel_kind, sel_layer, sel_chip;

    sk_widget *tabs, *diagram, *infopanel;
} about_app;

static const char *const tab_labels[2] = { "本机信息", "系统结构" };

static void debug_dump(about_app *a, const char *why);

/* ================================================================ 收集系统信息 */

/* 解析 os-release 的一行 KEY="值"（值可以带引号，也可以不带） */
static int osr_value(const char *line, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0 || line[kl] != '=')
        return 0;
    const char *v = line + kl + 1;
    char q = 0;
    if (*v == '"' || *v == '\'')
        q = *v++;
    size_t i = 0;
    for (; *v && i + 1 < n; v++) {
        if (q && *v == q)
            break;
        if (*v == '\\' && v[1])
            v++;
        out[i++] = *v;
    }
    out[i] = 0;
    return 1;
}

static void read_os_release(about_app *a)
{
    const char *path = getenv("SKABOUT_OS_RELEASE");
    if (!path || !*path)
        path = "/etc/os-release";
    char version[80] = "";
    snprintf(a->os_name, sizeof a->os_name, "%s", SKETCHOS_NAME);
    snprintf(a->os_pretty, sizeof a->os_pretty, "%s 手绘操作系统 %s", SKETCHOS_NAME, SKETCHOS_VERSION);
    char *s = sk_read_file(path, NULL);
    if (s) {
        for (char *line = s; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = 0;
            osr_value(line, "NAME", a->os_name, sizeof a->os_name);
            osr_value(line, "PRETTY_NAME", a->os_pretty, sizeof a->os_pretty);
            osr_value(line, "VERSION", version, sizeof version);
            line = nl ? nl + 1 : NULL;
        }
        free(s);
    }
    /* 大字已经写了系统名称，副标题里就不再重复（"SketchOS 手绘操作系统 0.1" → "手绘操作系统 0.1"） */
    size_t nl = strlen(a->os_name);
    if (nl && strncmp(a->os_pretty, a->os_name, nl) == 0 && a->os_pretty[nl] == ' ')
        memmove(a->os_pretty, a->os_pretty + nl + 1, strlen(a->os_pretty + nl + 1) + 1);
    /* 没有 os-release（或者里面没写版本）时，用编译时写进程序的版本号 */
    snprintf(a->info[INFO_VERSION].value, sizeof a->info[INFO_VERSION].value, "%s",
             version[0] ? version : SKETCHOS_VERSION);
}

/* /proc/cpuinfo：每个逻辑处理器一段，段里是“名字<TAB>: 值” */
static void read_cpu(about_app *a)
{
    char model[160] = "", vendor[64] = "", family[16] = "", modelno[16] = "";
    int count = 0;
    char *s = sk_read_file("/proc/cpuinfo", NULL);
    if (s) {
        for (char *line = s; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = 0;
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = 0;
                char *k = sk_trim(line), *v = sk_trim(colon + 1);
                if (strcmp(k, "processor") == 0)
                    count++;
                else if (strcmp(k, "model name") == 0 && !model[0])
                    sk_strlcpy(model, v, sizeof model);
                else if (strcmp(k, "vendor_id") == 0 && !vendor[0])
                    sk_strlcpy(vendor, v, sizeof vendor);
                else if (strcmp(k, "cpu family") == 0 && !family[0])
                    sk_strlcpy(family, v, sizeof family);
                else if (strcmp(k, "model") == 0 && !modelno[0])
                    sk_strlcpy(modelno, v, sizeof modelno);
            }
            line = nl ? nl + 1 : NULL;
        }
        free(s);
    }
    /* 有的模拟器不报型号名：退回到“厂商 + 家族/型号编号” */
    if (!model[0] && vendor[0])
        snprintf(model, sizeof model, "%s（家族 %s，型号 %s）", vendor, family[0] ? family : "?",
                 modelno[0] ? modelno : "?");
    if (!model[0])
        snprintf(model, sizeof model, "未知处理器");
    /* 型号名里常有一串多余的空格 */
    char *w = model;
    for (char *r = model; *r; r++)
        if (!(r[0] == ' ' && r[1] == ' '))
            *w++ = *r;
    *w = 0;
    if (count > 1)   /* 数量写在前面：型号很长要折行时，“× 4”不会孤零零地掉到下一行 */
        snprintf(a->info[INFO_CPU].value, sizeof a->info[INFO_CPU].value, "%d × %s", count, model);
    else
        snprintf(a->info[INFO_CPU].value, sizeof a->info[INFO_CPU].value, "%s", model);
}

/* “3 天 4 小时”“2 小时 7 分钟”“12 分钟”：精确到分钟就够了（这样每分钟才需要刷新一次） */
static void fmt_uptime(char *buf, size_t n, double up)
{
    unsigned long m = (unsigned long)(up / 60);
    unsigned long d = m / 1440, h = m / 60 % 24, mm = m % 60;
    if (m == 0)
        snprintf(buf, n, "不到 1 分钟");
    else if (d)
        snprintf(buf, n, "%lu 天 %lu 小时 %lu 分钟", d, h, mm);
    else if (h)
        snprintf(buf, n, "%lu 小时 %lu 分钟", h, mm);
    else
        snprintf(buf, n, "%lu 分钟", mm);
}

static void read_uptime(about_app *a)
{
    double up = 0;
    if (sk_read_uptime(&up, NULL) == 0)
        fmt_uptime(a->info[INFO_UPTIME].value, sizeof a->info[INFO_UPTIME].value, up);
    else
        snprintf(a->info[INFO_UPTIME].value, sizeof a->info[INFO_UPTIME].value, "未知");
}

static void collect_info(about_app *a)
{
    static const char *const keys[NINFO] = { "系统版本", "内核", "处理器", "内存", "已运行", "屏幕", "主机名" };
    static const char *const sources[NINFO] = {
        "/etc/os-release", "uname() 系统调用", "/proc/cpuinfo", "/proc/meminfo",
        "/proc/uptime", "显示服务器 sketchd", "uname() 系统调用",
    };
    for (int i = 0; i < NINFO; i++) {
        a->info[i].key = keys[i];
        a->info[i].source = sources[i];
    }
    read_os_release(a);

    struct utsname u;
    if (uname(&u) == 0) {
        sk_strlcpy(a->release, u.release, sizeof a->release);
        sk_strlcpy(a->machine, u.machine, sizeof a->machine);
        sk_strlcpy(a->hostname, u.nodename, sizeof a->hostname);
        snprintf(a->info[INFO_KERNEL].value, sizeof a->info[INFO_KERNEL].value, "%s %s · %s", u.sysname, u.release,
                 u.machine);
    } else {
        fprintf(stderr, "skabout: uname: %s\n", sk_strerror(errno));
        snprintf(a->info[INFO_KERNEL].value, sizeof a->info[INFO_KERNEL].value, "未知（%s）", sk_strerror(errno));
    }
    snprintf(a->info[INFO_HOST].value, sizeof a->info[INFO_HOST].value, "%s", a->hostname[0] ? a->hostname : "未知");

    read_cpu(a);
    sk_meminfo mi;
    if (sk_read_meminfo(&mi) == 0)
        sk_fmt_size(a->info[INFO_MEM].value, sizeof a->info[INFO_MEM].value, mi.total_kb * 1024);
    else
        snprintf(a->info[INFO_MEM].value, sizeof a->info[INFO_MEM].value, "未知");
    read_uptime(a);
    snprintf(a->info[INFO_SCREEN].value, sizeof a->info[INFO_SCREEN].value, "%d × %d 像素",
             sk_app_screen_w(a->app), sk_app_screen_h(a->app));
}

/* ================================================================ 手绘小工具 */

/* 在排线上写字：先用纸色描一圈“光晕”，笔画才不会和排线混在一起 */
static void text_halo(sk_canvas *c, const sk_font *f, int x, int base, const char *t, sk_color ink)
{
    static const signed char off[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 },
                                           { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    for (int i = 0; i < 8; i++)
        sk_draw_text(c, f, x + off[i][0], base + off[i][1], t, SK_WITH_ALPHA(SK_PAPER, 0xD0));
    sk_draw_text(c, f, x, base, t, ink);
}

/* 手绘的双向箭头（竖直） */
static void draw_arrow(sk_canvas *c, float x, float y0, float y1, uint32_t seed)
{
    sk_rough_style s;
    sk_style_pencil(&s, seed);
    s.stroke = SK_INK_SOFT;
    s.stroke_width = 1.6f;
    s.roughness = 0.7f;
    sk_rough_line(c, x, y0 + 1, x, y1 - 1, &s);
    float hw = 6.0f, hh = 8.0f;
    s.seed = seed ^ 0x51u;
    sk_rough_line(c, x, y0, x - hw, y0 + hh, &s);
    sk_rough_line(c, x, y0, x + hw, y0 + hh, &s);
    s.seed = seed ^ 0x77u;
    sk_rough_line(c, x, y1, x - hw, y1 - hh, &s);
    sk_rough_line(c, x, y1, x + hw, y1 - hh, &s);
}

/* ================================================================ 图的排版 */

/* 排版密度：先试最宽松的，放不下就逐级收紧。
 *   0：芯片写“程序名 中文名”，应用带小图标      1：只写程序名，应用带小图标
 *   2：只写程序名，不带图标                      3：再压缩各处的间距 */
typedef struct {
    int chip_h, chip_gap, pad, title_h, link_h, icons, labels;
} density;

static const density densities[4] = {
    { 30, 8, 10, 32, 40, 1, 1 },
    { 28, 8, 10, 30, 34, 1, 0 },
    { 28, 8, 10, 30, 32, 0, 0 },
    { 26, 6, 8, 28, 28, 0, 0 },
};

static void chip_text(const chip_def *cd, const density *dn, char *buf, size_t n)
{
    if (cd->label && dn->labels)
        snprintf(buf, n, "%s %s", cd->name, cd->label);
    else
        snprintf(buf, n, "%s", cd->name);
}

static int chip_width(const chip_def *cd, const density *dn)
{
    char t[96];
    chip_text(cd, dn, t, sizeof t);
    int w = sk_text_width(sk_font_get(SK_FONT_UI), t) + 22;
    if (dn->icons && cd->icon >= 0)
        w += 22;
    return w;
}

/* 在矩形 area 里按密度 level 排版，箭头再加长 extra 像素；返回需要的总高度。store=1 时把结果存进 a */
static int layout_layers(about_app *a, sk_rect area, int level, int extra, int store)
{
    const density *dn = &densities[level];
    int y = area.y;
    for (int li = 0; li < NLAYERS; li++) {
        const layer_def *L = &layers[li];
        int x0 = area.x + dn->pad + 2, xmax = area.x + area.w - dn->pad - 2;
        int cy = y + dn->pad + dn->title_h, cx = x0;
        for (int ci = 0; ci < L->nchips; ci++) {
            int cw = SK_MIN(chip_width(&L->chips[ci], dn), xmax - x0);
            if (cx + cw > xmax && cx > x0) {        /* 这一行放不下：换行 */
                cx = x0;
                cy += dn->chip_h + dn->chip_gap;
            }
            if (store)
                a->chip_r[li][ci] = sk_rect_make(cx, cy, cw, dn->chip_h);
            cx += cw + dn->chip_gap;
        }
        int h = cy + dn->chip_h + dn->pad + 2 - y;
        if (store)
            a->layer_r[li] = sk_rect_make(area.x, y, area.w, h);
        y += h;
        if (li < NLAYERS - 1) {
            if (store) {
                const sk_font *f = sk_font_get(SK_FONT_UI);
                int lw = sk_text_width(f, links[li].label);
                a->link_r[li] = sk_rect_make(area.x + 28, y, 32 + lw + 8, dn->link_h + extra);
            }
            y += dn->link_h + extra;
        }
    }
    return y - area.y;
}

/* 图控件的矩形变了：重新排版（标题、四层、便签）。
 * 先找“带便签也放得下”的最宽松密度；都放不下就去掉便签再找（点击时改用消息框解释）。 */
static void diagram_layout(about_app *a)
{
    sk_rect r = a->diagram->r;
    int title_h = a->wide ? 40 : 4;      /* 标签页模式下标签本身就是标题 */
    a->title_r = sk_rect_make(r.x, r.y, r.w, a->wide ? 34 : 0);
    int top = r.y + title_h;
    sk_rect area = sk_rect_make(r.x + 2, top, r.w - 4, r.y + r.h - top);
    int level = -1;
    a->note_on = 1;
    for (int pass = 0; pass < 2 && level < 0; pass++) {
        int room = area.h - (pass == 0 ? NOTE_H + 12 : 0);
        for (int lv = 0; lv < 4 && level < 0; lv++)
            if (layout_layers(a, area, lv, 0, 0) <= room)
                level = lv;
        if (level < 0)
            a->note_on = 0;
    }
    if (level < 0)
        level = 3;                       /* 实在太小：最紧凑的排法，下面被裁掉一点 */
    a->compact = level;
    /* 多出来的高度：先把箭头拉长一点（最多 12 像素），剩下的给便签 */
    int need = layout_layers(a, area, level, 0, 0);
    int spare = area.h - need - (a->note_on ? NOTE_H + 12 : 0);
    int extra = SK_CLAMP(spare / 6, 0, 12);
    int used = layout_layers(a, area, level, extra, 1);
    int ny = top + used + 12;
    int nh = SK_MIN(r.y + r.h - ny - 2, NOTE_H + 40);
    if (nh < NOTE_MIN_H)
        a->note_on = 0;
    a->note_r = a->note_on ? sk_rect_make(r.x + 2, ny, r.w - 4, nh) : sk_rect_make(0, 0, 0, 0);
}

/* ================================================================ 画图 */

static int is_selected(const about_app *a, int kind, int li, int ci)
{
    return a->sel_kind == kind && a->sel_layer == li && (kind != SEL_CHIP || a->sel_chip == ci);
}

static void draw_chip(about_app *a, sk_canvas *c, int li, int ci)
{
    const chip_def *cd = &layers[li].chips[ci];
    const density *dn = &densities[a->compact];
    sk_rect r = a->chip_r[li][ci];
    uint32_t seed = sk_hash_str(cd->name);
    int sel = is_selected(a, SEL_CHIP, li, ci);
    /* 纸片：先垫一块边缘不规则的纸，盖住下面的排线；选中的用荧光笔涂黄 */
    sk_rough_style s;
    sk_style_pencil(&s, seed);
    s.fill_style = SK_FILL_SOLID;
    s.fill = SK_PAPER;
    s.stroke = sel ? SK_INK : SK_INK_SOFT;
    s.stroke_width = sel ? 2.0f : 1.2f;
    s.roughness = 0.6f;
    s.double_stroke = sel;
    sk_rough_rect(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, &s);
    if (sel)
        sk_marker_highlight(c, sk_rect_inset(r, 3, 4), SK_HIGHLIGHT, seed ^ 0xFEEDu);
    int x = r.x + 10;
    if (dn->icons && cd->icon >= 0) {
        sk_draw_icon(c, cd->icon, x - 2, r.y + (r.h - 18) / 2, 18, sk_hash_u32(0x1C0Du, (uint32_t)cd->icon));
        x += 22;
    }
    char t[96];
    chip_text(cd, dn, t, sizeof t);
    sk_draw_text_in_rect(c, sk_font_get(SK_FONT_UI), sk_rect_make(x, r.y, r.x + r.w - x - 6, r.h), t, SK_INK,
                         SK_ALIGN_LEFT);
}

static void draw_layer(about_app *a, sk_canvas *c, int li)
{
    const layer_def *L = &layers[li];
    const density *dn = &densities[a->compact];
    sk_rect r = a->layer_r[li];
    uint32_t seed = sk_hash_str(L->title);
    int sel = is_selected(a, SEL_LAYER, li, -1);
    /* 蜡笔排线涂满整层，再用铅笔描边 */
    sk_rough_style s;
    sk_style_hatched(&s, SK_WITH_ALPHA(L->color, 0x9C), seed);
    s.stroke = sel ? SK_INK : SK_INK_SOFT;
    s.stroke_width = sel ? 2.4f : 1.5f;
    s.roughness = 0.8f;
    s.hachure_gap = 7.0f;
    s.fill_weight = 1.3f;
    sk_rough_rect(c, r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h - 4.0f, &s);

    /* 标题行：图标 + 标题（选中时标题下面涂一道荧光笔）+ 淡淡的说明 */
    const sk_font *tf = sk_font_get(SK_FONT_TITLE), *f = sk_font_get(SK_FONT_UI);
    int ix = r.x + dn->pad + 2, ty = r.y + dn->pad;
    int isz = dn->title_h - 6;
    int tx = ix + isz + 8;
    int tw = sk_text_width(tf, L->title);
    if (sel)
        sk_marker_highlight(c, sk_rect_make(tx - 4, ty + 2, tw + 8, dn->title_h - 6), SK_HIGHLIGHT, seed ^ 0xABCu);
    sk_fill_circle_aa(c, ix + isz / 2.0f, ty + isz / 2.0f + 1, isz / 2.0f + 3, SK_WITH_ALPHA(SK_PAPER, 0xE0));
    sk_draw_icon(c, L->icon, ix, ty + 1, isz, seed ^ 0x1C0u);
    int base = ty + (dn->title_h - 4 + sk_font_ascent(tf) - sk_font_descent(tf)) / 2;
    text_halo(c, tf, tx, base, L->title, SK_INK);
    int sx = tx + tw + 14, sw = sk_text_width(f, L->sub);
    if (sx + sw < r.x + r.w - dn->pad - 4)
        text_halo(c, f, sx, base, L->sub, SK_INK_SOFT);

    for (int ci = 0; ci < L->nchips; ci++)
        draw_chip(a, c, li, ci);
}

static void draw_link(about_app *a, sk_canvas *c, int i)
{
    sk_rect r = a->link_r[i];
    uint32_t seed = sk_hash_str(links[i].label);
    float ax = (float)r.x + 12;
    draw_arrow(c, ax, (float)r.y + 5, (float)(r.y + r.h) - 5, seed);
    const sk_font *f = sk_font_get(SK_FONT_UI);
    int lx = r.x + 32, lw = sk_text_width(f, links[i].label);
    if (is_selected(a, SEL_LINK, i, -1))
        sk_marker_highlight(c, sk_rect_make(lx - 4, r.y + r.h / 2 - 10, lw + 8, 20), SK_HIGHLIGHT, seed ^ 0xF00u);
    sk_draw_text_in_rect(c, f, sk_rect_make(lx, r.y, lw + 4, r.h), links[i].label, SK_INK_SOFT, SK_ALIGN_LEFT);
}

/* 便签上写什么：选中项的名字和解释 */
static void note_text(const about_app *a, char *title, size_t tn, const char **body)
{
    switch (a->sel_kind) {
    case SEL_LAYER:
        snprintf(title, tn, "%s", layers[a->sel_layer].title);
        *body = layers[a->sel_layer].about;
        break;
    case SEL_CHIP: {
        const chip_def *cd = &layers[a->sel_layer].chips[a->sel_chip];
        if (cd->label)
            snprintf(title, tn, "%s · %s", cd->name, cd->label);
        else
            snprintf(title, tn, "%s", cd->name);
        *body = cd->about;
        break;
    }
    case SEL_LINK:
        snprintf(title, tn, "%s", links[a->sel_layer].label);
        *body = links[a->sel_layer].about;
        break;
    default:
        snprintf(title, tn, "点一点上面的方块");
        *body = "图里的每一层、每个小方块和每个箭头都可以点，这里会告诉你它是做什么的。"
                "整个系统就是这样一层一层搭起来的：每一层只和相邻的一层打交道。";
        break;
    }
}

/* 便签：一张淡黄色的纸，左上角一枚图钉 */
static void draw_note(about_app *a, sk_canvas *c)
{
    sk_rect r = a->note_r;
    if (!a->note_on)
        return;
    sk_rough_style s;
    sk_style_pencil(&s, 0x4E07Eu);
    s.fill_style = SK_FILL_SOLID;
    s.fill = SK_WITH_ALPHA(SK_YELLOW, 0x30);
    s.stroke = SK_INK_SOFT;
    s.stroke_width = 1.3f;
    s.roughness = 0.6f;
    sk_rough_rect(c, r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h - 4.0f, &s);
    sk_rough_style pin;
    sk_style_hatched(&pin, SK_RED, 0x9149u);
    pin.stroke_width = 1.2f;
    pin.hachure_gap = 3.0f;
    sk_rough_circle(c, r.x + 16.0f, r.y + 14.0f, 12.0f, &pin);

    char title[96];
    const char *body;
    note_text(a, title, sizeof title, &body);
    const sk_font *tf = sk_font_get(SK_FONT_TITLE), *f = sk_font_get(SK_FONT_UI);
    sk_rect old = sk_push_clip(c, sk_rect_inset(r, 4, 4));
    sk_draw_text(c, tf, r.x + 30, r.y + 10 + sk_font_ascent(tf), title, SK_INK);
    sk_draw_text_wrapped(c, f, sk_rect_make(r.x + 14, r.y + 40, r.w - 28, r.h - 46), body, SK_INK_SOFT, 4);
    sk_pop_clip(c, old);
}

static void diagram_draw(sk_widget *w, sk_canvas *c, void *user)
{
    about_app *a = user;
    (void)w;
    if (a->wide) {
        /* 标题（标签页模式下标签本身就是标题，不画） */
        const sk_font *tf = sk_font_get(SK_FONT_TITLE), *f = sk_font_get(SK_FONT_UI);
        sk_rect t = a->title_r;
        int base = t.y + 6 + sk_font_ascent(tf);
        sk_draw_text(c, tf, t.x + 2, base, "系统结构", SK_INK);
        int tw = sk_text_width(tf, "系统结构");
        const char *hint = "从上到下，一层搭在一层上";
        if (t.x + 16 + tw + sk_text_width(f, hint) < t.x + t.w)
            sk_draw_text(c, f, t.x + 16 + tw, base, hint, SK_INK_FAINT);
        sk_rough_underline(c, (float)t.x + 2, (float)(t.x + 2 + tw), (float)base + 6, SK_YELLOW, 3.0f, 0x7171u);
    }
    for (int i = 0; i < NLAYERS; i++)
        draw_layer(a, c, i);
    for (int i = 0; i < NLAYERS - 1; i++)
        draw_link(a, c, i);
    draw_note(a, c);
}

/* 点击：找出点到的是哪个方块，只重画新旧两个方块和便签 */
static void invalidate_sel(about_app *a)
{
    sk_rect r;
    switch (a->sel_kind) {
    case SEL_LAYER: r = a->layer_r[a->sel_layer]; break;
    case SEL_CHIP:  r = a->chip_r[a->sel_layer][a->sel_chip]; break;
    case SEL_LINK:  r = a->link_r[a->sel_layer]; break;
    default: return;
    }
    sk_ui_invalidate(a->ui, sk_rect_inset(r, -4, -4));
}

static int diagram_event(sk_widget *w, const sk_event *e, void *user)
{
    (void)w;
    about_app *a = user;
    if (e->type != SK_EVENT_MOUSE || e->mouse.kind != SK_MOUSE_DOWN || e->mouse.button != SK_BUTTON_LEFT)
        return e->type == SK_EVENT_MOUSE;
    int x = e->mouse.x, y = e->mouse.y;
    int kind = SEL_NONE, li = -1, ci = -1;
    for (int i = 0; i < NLAYERS && !kind; i++) {
        for (int k = 0; k < layers[i].nchips && !kind; k++)
            if (sk_rect_contains(a->chip_r[i][k], x, y)) {
                kind = SEL_CHIP;
                li = i;
                ci = k;
            }
        if (!kind && sk_rect_contains(a->layer_r[i], x, y)) {
            kind = SEL_LAYER;
            li = i;
        }
    }
    for (int i = 0; i < NLAYERS - 1 && !kind; i++)
        if (sk_rect_contains(a->link_r[i], x, y)) {
            kind = SEL_LINK;
            li = i;
        }
    if (!kind || (kind == a->sel_kind && li == a->sel_layer && ci == a->sel_chip))
        return 1;
    invalidate_sel(a);
    /* 选中/取消选中一个芯片时，它所在那一层的外观不变；选中一层时整层（含芯片）要重画 */
    a->sel_kind = kind;
    a->sel_layer = li;
    a->sel_chip = ci;
    invalidate_sel(a);
    debug_dump(a, "select");
    if (a->note_on) {
        sk_ui_invalidate(a->ui, sk_rect_inset(a->note_r, -4, -4));
    } else {
        /* 窗口太小放不下便签：用消息框显示解释（对话框会阻塞到用户点“知道了”） */
        char title[96];
        const char *body;
        note_text(a, title, sizeof title, &body);
        char dtitle[128];
        snprintf(dtitle, sizeof dtitle, "说明：%s", title);   /* 标题里有“说明”，消息框会配上“i”图标 */
        sk_dialog_message(a->app, dtitle, body, "知道了");
    }
    return 1;
}

/* ================================================================ 信息栏 */

/* 排版信息栏（draw 和失效计算都要用，所以单独算）。地方不够时去掉每行下面的来源说明 */
static int info_layout_pass(about_app *a, int compact)
{
    sk_rect r = a->infopanel->r;
    const sk_font *f = sk_font_get(SK_FONT_UI);
    int keyw = 76;
    int vw = r.w - 32 - keyw;
    int y = r.y + 104;
    for (int i = 0; i < NINFO; i++) {
        int vh = sk_draw_text_wrapped(NULL, f, sk_rect_make(0, 0, vw, 1000), a->info[i].value, SK_INK, 2);
        int h = SK_MAX(vh, 20) + (compact ? 10 : 2 + 20 + 10);   /* 数值 + 来源说明 + 行距 */
        a->info[i].r = sk_rect_make(r.x + 16, y, r.w - 32, h);
        y += h;
    }
    return y - r.y;
}

static void info_layout(about_app *a)
{
    a->info_compact = 0;
    a->info_h = info_layout_pass(a, 0);
    if (a->info_h > a->infopanel->r.h - 8) {
        a->info_compact = 1;
        a->info_h = info_layout_pass(a, 1);
    }
}

/* 信息栏底部空着的时候写一段简介 */
static const char *const blurb =
    "这是一个用来学习操作系统的系统：内核之上的每个程序都是自己写的，代码里全是中文注释。";

static void info_draw(sk_widget *w, sk_canvas *c, void *user)
{
    about_app *a = user;
    sk_rect r = w->r;
    sk_rough_style s;
    sk_style_pencil(&s, w->seed);
    s.stroke = SK_INK_SOFT;
    s.stroke_width = 1.4f;
    s.roughness = 0.6f;
    sk_rough_rect(c, r.x + 1.5f, r.y + 1.5f, r.w - 3.0f, r.h - 3.0f, &s);
    sk_rect old = sk_push_clip(c, sk_rect_inset(r, 3, 3));

    /* 抬头：手绘的 SketchOS 标志 + 名字 */
    const sk_font *lf = sk_font_get(SK_FONT_LARGE), *f = sk_font_get(SK_FONT_UI);
    sk_draw_icon(c, SK_ICON_SKETCH_LOGO, r.x + 16, r.y + 18, 56, 0x5C0Fu);
    int nx = r.x + 84;
    sk_draw_text(c, lf, nx, r.y + 18 + sk_font_ascent(lf), a->os_name, SK_INK);
    int nw = sk_text_width(lf, a->os_name);
    sk_rough_underline(c, (float)nx, (float)(nx + nw), (float)(r.y + 18 + sk_font_ascent(lf) + 6), SK_YELLOW, 3.0f,
                       0x0A11u);
    sk_draw_text_in_rect(c, f, sk_rect_make(nx, r.y + 60, r.x + r.w - nx - 10, 24), a->os_pretty, SK_INK_SOFT,
                         SK_ALIGN_LEFT);
    sk_draw_separator(c, r.x + 14, r.x + r.w - 14, r.y + 94, 0x5E9Au);

    int keyw = 76;
    for (int i = 0; i < NINFO; i++) {
        const info_row *row = &a->info[i];
        sk_rect rr = row->r;
        if (sk_rect_empty(sk_rect_intersect(rr, c->clip)))
            continue;
        int base = rr.y + sk_font_ascent(f);
        sk_draw_text(c, f, rr.x, base, row->key, SK_INK_SOFT);
        sk_rect vr = sk_rect_make(rr.x + keyw, rr.y, rr.w - keyw, rr.h);
        int vh = sk_draw_text_wrapped(c, f, sk_rect_make(vr.x, vr.y, vr.w, 1000), row->value, SK_INK, 2);
        if (!a->info_compact)
            sk_draw_text_in_rect(c, f, sk_rect_make(vr.x, rr.y + SK_MAX(vh, 20) + 2, vr.w, 20), row->source,
                                 SK_INK_FAINT, SK_ALIGN_LEFT);
    }

    /* 底部有空的话：分隔线 + 一段简介 */
    int bw = r.w - 32;
    int bh = sk_draw_text_wrapped(NULL, f, sk_rect_make(0, 0, bw, 1000), blurb, SK_INK, 4);
    int by = r.y + r.h - 14 - bh;
    if (by - 16 >= r.y + a->info_h) {
        sk_draw_separator(c, r.x + 14, r.x + r.w - 14, by - 10, 0xB1A8u);
        sk_draw_text_wrapped(c, f, sk_rect_make(r.x + 16, by, bw, bh), blurb, SK_INK_SOFT, 4);
    }
    sk_pop_clip(c, old);
}

/* 每分钟更新一次“已运行” */
static void on_uptime(sk_app *app, void *user)
{
    (void)app;
    about_app *a = user;
    char old[200];
    sk_strlcpy(old, a->info[INFO_UPTIME].value, sizeof old);
    read_uptime(a);
    if (strcmp(old, a->info[INFO_UPTIME].value) != 0 && a->infopanel->visible)
        sk_ui_invalidate(a->ui, sk_rect_inset(a->info[INFO_UPTIME].r, -2, -2));
    /* 第一次是“到下一个整分钟”的一次性定时器（触发后自动注销），之后改成每 60 秒一次 */
    if (a->uptime_timer > 0) {
        a->uptime_timer = 0;
        sk_app_add_timer(a->app, 60000, 1, on_uptime, a);
    }
}

/* ================================================================ 布局 */

static void show_tab(about_app *a, int tab)
{
    a->tab = tab;
    sk_widget_set_visible(a->infopanel, a->wide || tab == 0);
    sk_widget_set_visible(a->diagram, a->wide || tab == 1);
}

static void on_tab(sk_widget *w, void *user)
{
    about_app *a = user;
    show_tab(a, sk_tabs_active(w));
    debug_dump(a, "tab");
}

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    about_app *a = user;
    a->wide = W >= WIDE_MIN;
    if (a->wide) {
        /* 宽窗口：图在左，信息在右 */
        int iw = SK_CLAMP(W * 31 / 100, 270, 340);
        sk_widget_set_visible(a->tabs, 0);
        sk_widget_set_rect(a->infopanel, sk_rect_make(W - MARGIN - iw, MARGIN, iw, H - 2 * MARGIN));
        sk_widget_set_rect(a->diagram, sk_rect_make(MARGIN, MARGIN - 4, W - 3 * MARGIN - iw, H - 2 * MARGIN + 4));
    } else {
        /* 窄窗口：两个标签页，各占满整个窗口 */
        int y = TABS_Y + TABS_H + 10;
        sk_widget_set_visible(a->tabs, 1);
        sk_widget_set_rect(a->tabs, sk_rect_make(MARGIN, TABS_Y, W - 2 * MARGIN, TABS_H));
        sk_widget_set_rect(a->infopanel, sk_rect_make(MARGIN, y, W - 2 * MARGIN, H - y - MARGIN));
        sk_widget_set_rect(a->diagram, sk_rect_make(MARGIN, y - 4, W - 2 * MARGIN, H - y - MARGIN + 4));
    }
    show_tab(a, a->tab);
    diagram_layout(a);
    info_layout(a);
    debug_dump(a, "layout");
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

static void json_rect(FILE *f, sk_rect r)
{
    fprintf(f, "[%d,%d,%d,%d]", r.x, r.y, r.w, r.h);
}

static void debug_dump(about_app *a, const char *why)
{
    if (!a->debug)
        return;
    FILE *f = stdout;
    fprintf(f, "SKABOUT {\"why\":\"%s\",\"wide\":%d,\"tab\":%d,\"compact\":%d,\"note_on\":%d,", why, a->wide, a->tab,
            a->compact, a->note_on);
    fprintf(f, "\"os_name\":");
    json_str(f, a->os_name);
    fprintf(f, ",\"os_pretty\":");
    json_str(f, a->os_pretty);
    fprintf(f, ",\"release\":");
    json_str(f, a->release);
    fprintf(f, ",\"machine\":");
    json_str(f, a->machine);
    fprintf(f, ",\"hostname\":");
    json_str(f, a->hostname);
    fprintf(f, ",\"info\":{");
    for (int i = 0; i < NINFO; i++) {
        if (i)
            fputc(',', f);
        json_str(f, a->info[i].key);
        fputc(':', f);
        json_str(f, a->info[i].value);
    }
    fprintf(f, "},\"info_h\":%d,\"info_compact\":%d,\"diagram\":", a->info_h, a->info_compact);
    json_rect(f, a->diagram->r);
    fprintf(f, ",\"diagram_visible\":%d,\"info_visible\":%d,\"infopanel\":", a->diagram->visible, a->infopanel->visible);
    json_rect(f, a->infopanel->r);
    fprintf(f, ",\"tabs\":[");
    for (int i = 0; i < 2; i++) {
        if (i)
            fputc(',', f);
        json_rect(f, sk_tabs_item_rect(a->tabs->r, tab_labels, 2, i));
    }
    fprintf(f, "],\"layers\":[");
    for (int i = 0; i < NLAYERS; i++) {
        if (i)
            fputc(',', f);
        json_rect(f, a->layer_r[i]);
    }
    fprintf(f, "],\"chips\":{");
    for (int i = 0, first = 1; i < NLAYERS; i++)
        for (int k = 0; k < layers[i].nchips; k++) {
            if (!first)
                fputc(',', f);
            first = 0;
            json_str(f, layers[i].chips[k].name);
            fputc(':', f);
            json_rect(f, a->chip_r[i][k]);
        }
    fprintf(f, "},\"links\":[");
    for (int i = 0; i < NLAYERS - 1; i++) {
        if (i)
            fputc(',', f);
        json_rect(f, a->link_r[i]);
    }
    fprintf(f, "],\"note\":");
    json_rect(f, a->note_r);
    char title[96];
    const char *body;
    note_text(a, title, sizeof title, &body);
    fprintf(f, ",\"selected\":");
    json_str(f, title);
    fprintf(f, ",\"uptime_row\":");
    json_rect(f, a->info[INFO_UPTIME].r);
    fprintf(f, "}\n");
    fflush(f);
}

/* ================================================================ main */

int main(void)
{
    about_app a;
    memset(&a, 0, sizeof a);
    const char *dbg = getenv("SKABOUT_DEBUG");
    a.debug = dbg && *dbg && strcmp(dbg, "0") != 0;

    a.app = sk_app_connect("skabout");
    if (!a.app)
        return 1;
    collect_info(&a);
    a.win = sk_window_create(a.app, "关于本机", 940, 660, SK_WIN_CENTER | SK_WIN_RESIZABLE, SK_YELLOW);
    if (!a.win) {
        sk_app_disconnect(a.app);
        return 1;
    }
    sk_window_set_min_size(a.win, 560, 480);
    a.ui = sk_ui_new(a.win);
    if (!a.ui) {
        sk_app_disconnect(a.app);
        return 1;
    }
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    a.tabs = sk_tabs_new(a.ui, z, tab_labels, 2, on_tab, &a);
    a.diagram = sk_custom_new(a.ui, z, diagram_draw, diagram_event, 0, &a);
    a.infopanel = sk_custom_new(a.ui, z, info_draw, NULL, 0, &a);
    sk_ui_set_layout(a.ui, layout, &a);

    /* “已运行”精确到分钟：先等到下一个整分钟，之后每 60 秒一次 */
    double up = 0;
    sk_read_uptime(&up, NULL);
    int ms = (int)((60.0 - (up - 60.0 * (long)(up / 60))) * 1000.0) + 50;
    a.uptime_timer = sk_app_add_timer(a.app, SK_CLAMP(ms, 50, 60050), 0, on_uptime, &a);

    int rc = sk_app_run(a.app);
    sk_ui_free(a.ui);
    sk_app_disconnect(a.app);
    return rc;
}
