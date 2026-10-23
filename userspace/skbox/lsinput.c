/*
 * lsinput — 列出输入设备（键盘、鼠标、触摸板……）
 *
 * Linux 的输入子系统把所有输入设备统一成“事件设备” /dev/input/eventN：
 * 程序 read() 它就能拿到一个个 struct input_event（按键按下/松开、鼠标移动了多少……）。
 * /proc/bus/input/devices 列出了每个设备的名字、总线、能产生哪些类型的事件（EV= 位图）。
 * SketchOS 的显示服务器 sketchd 就是读这些 event 设备来获得键盘和鼠标输入的。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(lsinput, "列出键盘、鼠标等输入设备")

static const char usage[] =
    "用法: lsinput [-v]\n"
    "列出内核识别到的输入设备（来自 /proc/bus/input/devices）。\n\n"
    "  -v    显示详细信息：物理路径、sysfs 路径、事件类型位图\n";

/* 事件类型（linux/input-event-codes.h） */
enum { EV_KEY = 1, EV_REL = 2, EV_ABS = 3, EV_MSC = 4, EV_SW = 5, EV_LED = 0x11, EV_REP = 0x14 };

typedef struct {
    char name[128], phys[128], sysfs[256], handlers[128], ev[32];
    unsigned bus, vendor, product;
} dev_t_info;

static const char *bus_name(unsigned bus)
{
    switch (bus) {
    case 0x01: return "PCI";
    case 0x03: return "USB";
    case 0x05: return "蓝牙";
    case 0x06: return "虚拟";
    case 0x10: return "ISA";
    case 0x11: return "i8042 (PS/2)";
    case 0x18: return "I2C";
    case 0x19: return "主机";
    case 0x1c: return "SPI";
    }
    return "其他";
}

static const char *kind(const dev_t_info *d)
{
    unsigned long ev = strtoul(d->ev, NULL, 16);
    int has_event = strstr(d->handlers, "event") != NULL;
    if (ev & (1UL << EV_REL))
        return "鼠标（相对移动）";
    if (ev & (1UL << EV_ABS))
        return (ev & (1UL << EV_KEY)) ? "绝对定位（触摸板/平板/vmmouse）" : "绝对坐标设备";
    if ((ev & (1UL << EV_KEY)) && ((ev & (1UL << EV_LED)) || (ev & (1UL << EV_REP)) || strstr(d->handlers, "kbd")))
        return "键盘";
    if (ev & (1UL << EV_SW))
        return "开关";
    if (ev & (1UL << EV_KEY))
        return "按键（如电源键）";
    return has_event ? "其他" : "未知";
}

static void print_dev(const dev_t_info *d, int verbose)
{
    /* 找出 handlers 里的 eventN，那就是 /dev/input/ 下对应的设备文件 */
    char evname[32] = "-";
    const char *e = strstr(d->handlers, "event");
    if (e) {
        size_t k = 0;
        while (e[k] && e[k] != ' ' && k + 1 < sizeof evname)
            k++;
        memcpy(evname, e, k);
        evname[k] = 0;
    }
    sb_put_padded(stdout, evname, 9, 1);
    sb_put_padded(stdout, d->name, 36, 1);
    putchar(' ');
    sb_put_padded(stdout, kind(d), 22, 1);
    printf(" %s\n", bus_name(d->bus));
    if (verbose) {
        printf("         厂商:产品 %04x:%04x   处理程序: %s\n", d->vendor, d->product, d->handlers);
        if (d->phys[0])
            printf("         物理路径: %s\n", d->phys);
        if (d->sysfs[0])
            printf("         sysfs:    /sys%s\n", d->sysfs);
        printf("         事件位图 EV=%s\n", d->ev);
    }
}

int applet_lsinput(int argc, char **argv)
{
    int verbose = 0, c;
    while ((c = sb_getopt(argc, argv, "v", usage)) != -1) {
        if (c != 'v')
            return 1;
        verbose = 1;
    }
    char *s = sk_read_file("/proc/bus/input/devices", NULL);
    if (!s) {
        sb_perr("无法读取 /proc/bus/input/devices（内核没有输入子系统，或 /proc 没挂载）");
        return 1;
    }
    sb_put_padded(stdout, "设备", 9, 1);
    sb_put_padded(stdout, "名称", 36, 1);
    putchar(' ');
    sb_put_padded(stdout, "类型", 22, 1);
    puts(" 总线");
    dev_t_info d;
    memset(&d, 0, sizeof d);
    int have = 0, count = 0;
    /* 每个设备是一段，段之间用空行分隔；每行以 "X: " 开头，X 表示这一行的含义 */
    for (char *line = s; line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (!*line) {
            if (have) {
                print_dev(&d, verbose);
                count++;
            }
            memset(&d, 0, sizeof d);
            have = 0;
        } else if (line[1] == ':' && line[2] == ' ') {
            const char *v = line + 3;
            have = 1;
            switch (line[0]) {
            case 'I':
                sscanf(v, "Bus=%x Vendor=%x Product=%x", &d.bus, &d.vendor, &d.product);
                break;
            case 'N':
                if (strncmp(v, "Name=", 5) == 0) {
                    v += 5;
                    if (*v == '"')
                        v++;
                    sk_strlcpy(d.name, v, sizeof d.name);
                    size_t l = strlen(d.name);
                    if (l && d.name[l - 1] == '"')
                        d.name[l - 1] = 0;
                }
                break;
            case 'P':
                if (strncmp(v, "Phys=", 5) == 0)
                    sk_strlcpy(d.phys, v + 5, sizeof d.phys);
                break;
            case 'S':
                if (strncmp(v, "Sysfs=", 6) == 0)
                    sk_strlcpy(d.sysfs, v + 6, sizeof d.sysfs);
                break;
            case 'H':
                if (strncmp(v, "Handlers=", 9) == 0)
                    sk_strlcpy(d.handlers, v + 9, sizeof d.handlers);
                break;
            case 'B':
                if (strncmp(v, "EV=", 3) == 0)
                    sk_strlcpy(d.ev, v + 3, sizeof d.ev);
                break;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (have) {
        print_dev(&d, verbose);
        count++;
    }
    free(s);
    if (!count)
        puts("（没有输入设备）");
    return 0;
}
