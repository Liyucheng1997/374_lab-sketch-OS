/*
 * lspci — 列出 PCI 设备
 *
 * PCI 是电脑里连接显卡、网卡、磁盘控制器的总线。每个设备有一个“地址”（总线:插槽.功能）、
 * 厂商号和设备号（比如 8086 是 Intel，1234:1111 是 QEMU/Bochs 的标准显卡），以及一个类别码。
 * 内核启动时扫描 PCI 总线，把每个设备都放在 /sys/bus/pci/devices/ 下面，
 * 每个属性都是一个小文件（vendor、device、class……）。lspci 只是把它们读出来翻译成人话。
 */
#include "skbox.h"

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(lspci, "列出 PCI 设备")

static const char usage[] =
    "用法: lspci [-n] [-v] [-k]\n"
    "列出 PCI 总线上的设备（来自 /sys/bus/pci/devices）。\n\n"
    "  -n    只显示数字形式的厂商号和设备号\n"
    "  -v    详细信息：子系统、中断号、I/O 端口和内存区域、驱动\n"
    "  -k    显示负责这个设备的内核驱动\n";

static const struct { unsigned v; const char *name; } g_vendors[] = {
    { 0x8086, "Intel" }, { 0x1234, "QEMU/Bochs" }, { 0x1af4, "Red Hat (virtio)" },
    { 0x1b36, "Red Hat (QEMU)" }, { 0x10ec, "Realtek" }, { 0x1013, "Cirrus Logic" },
    { 0x15ad, "VMware" }, { 0x1022, "AMD" }, { 0x10de, "NVIDIA" }, { 0x1002, "AMD/ATI" },
    { 0x80ee, "VirtualBox" }, { 0x1274, "Ensoniq" }, { 0x106b, "Apple" }, { 0x1106, "VIA" },
};

static const struct { unsigned v, d; const char *name; } g_devices[] = {
    { 0x8086, 0x1237, "440FX - 82441FX PMC [Natoma] 主板芯片组（北桥）" },
    { 0x8086, 0x7000, "82371SB PIIX3 ISA [Natoma/Triton II]（南桥）" },
    { 0x8086, 0x7010, "82371SB PIIX3 IDE [Natoma/Triton II] 硬盘/光驱控制器" },
    { 0x8086, 0x7020, "82371SB PIIX3 USB [Natoma/Triton II]" },
    { 0x8086, 0x7113, "82371AB/EB/MB PIIX4 ACPI 电源管理" },
    { 0x8086, 0x100e, "82540EM 千兆网卡 (e1000)" },
    { 0x8086, 0x10d3, "82574L 千兆网卡 (e1000e)" },
    { 0x8086, 0x2415, "82801AA AC'97 声卡" },
    { 0x8086, 0x2668, "82801FB HD Audio 声卡" },
    { 0x8086, 0x29c0, "82G33/G31/P35/P31 Express DRAM 控制器 (Q35)" },
    { 0x8086, 0x2918, "82801IB (ICH9) LPC 接口控制器" },
    { 0x8086, 0x2922, "82801IR (ICH9R) SATA 控制器 [AHCI]" },
    { 0x8086, 0x2930, "82801I (ICH9) SMBus 控制器" },
    { 0x1234, 0x1111, "标准 VGA 显卡（Bochs VBE，bochs-drm 驱动）" },
    { 0x10ec, 0x8029, "RTL-8029(AS) NE2000 兼容网卡" },
    { 0x10ec, 0x8139, "RTL-8100/8101L/8139 快速以太网卡" },
    { 0x1013, 0x00b8, "GD 5446 显卡 (Cirrus VGA)" },
    { 0x15ad, 0x0405, "SVGA II 显卡" },
    { 0x1274, 0x5000, "ES1370 声卡" },
    { 0x1af4, 0x1000, "virtio 网卡（旧版）" }, { 0x1af4, 0x1001, "virtio 块设备（旧版）" },
    { 0x1af4, 0x1002, "virtio 内存气球（旧版）" }, { 0x1af4, 0x1003, "virtio 控制台（旧版）" },
    { 0x1af4, 0x1005, "virtio 随机数发生器（旧版）" }, { 0x1af4, 0x1009, "virtio 9P 文件系统（旧版）" },
    { 0x1af4, 0x1041, "virtio 1.0 网卡" }, { 0x1af4, 0x1042, "virtio 1.0 块设备" },
    { 0x1af4, 0x1043, "virtio 1.0 控制台" }, { 0x1af4, 0x1044, "virtio 1.0 随机数发生器" },
    { 0x1af4, 0x1045, "virtio 1.0 内存气球" }, { 0x1af4, 0x1049, "virtio 1.0 9P 文件系统" },
    { 0x1af4, 0x1050, "virtio 1.0 GPU" }, { 0x1af4, 0x1052, "virtio 1.0 输入设备" },
    { 0x1b36, 0x000d, "QEMU XHCI USB 控制器" }, { 0x1b36, 0x0100, "QXL 显卡" },
};

/* 类别码：高 8 位是大类，中间 8 位是子类 */
static const char *class_name(unsigned cls)
{
    unsigned base = cls >> 16, sub = (cls >> 8) & 0xFF;
    switch (base) {
    case 0x00: return sub == 0x01 ? "VGA 兼容设备" : "未分类设备";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI 存储控制器";
        case 0x01: return "IDE 接口";
        case 0x05: return "ATA 控制器";
        case 0x06: return "SATA 控制器";
        case 0x08: return "NVMe 控制器";
        default:   return "存储控制器";
        }
    case 0x02: return sub == 0x00 ? "以太网控制器" : "网络控制器";
    case 0x03: return sub == 0x00 ? "VGA 兼容显示控制器" : "显示控制器";
    case 0x04:
        switch (sub) {
        case 0x01: return "音频设备";
        case 0x03: return "音频设备（HD Audio）";
        default:   return "多媒体控制器";
        }
    case 0x05: return "内存控制器";
    case 0x06:
        switch (sub) {
        case 0x00: return "主桥";
        case 0x01: return "ISA 桥";
        case 0x04: return "PCI 桥";
        default:   return "桥接设备";
        }
    case 0x07: return sub == 0x00 ? "串口控制器" : "通信控制器";
    case 0x08: return "系统外设";
    case 0x09: return "输入设备控制器";
    case 0x0c:
        switch (sub) {
        case 0x03: return "USB 控制器";
        case 0x05: return "SMBus 控制器";
        default:   return "串行总线控制器";
        }
    case 0xff: return "未知类别";
    }
    return "其他设备";
}

static unsigned read_hex(const char *dir, const char *name)
{
    char path[PATH_MAX], line[64];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (sk_read_line(path, line, sizeof line) != 0)
        return 0;
    return (unsigned)strtoul(line, NULL, 0);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int applet_lspci(int argc, char **argv)
{
    int numeric = 0, verbose = 0, drivers = 0, c;
    while ((c = sb_getopt(argc, argv, "nvk", usage)) != -1) {
        switch (c) {
        case 'n': numeric = 1; break;
        case 'v': verbose = 1; drivers = 1; break;
        case 'k': drivers = 1; break;
        default:  return 1;
        }
    }
    const char *base = "/sys/bus/pci/devices";
    DIR *d = opendir(base);
    if (!d) {
        sb_perr("无法打开 %s（/sys 挂载了吗？没有 PCI 总线？）", base);
        return 1;
    }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **nn = realloc(names, cap * sizeof *nn);
            if (!nn)
                break;
            names = nn;
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (names)
        qsort(names, n, sizeof *names, cmp_str);

    for (size_t i = 0; i < n; i++) {
        if (!names[i])
            continue;
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/%s", base, names[i]);
        unsigned vendor = read_hex(dir, "vendor"), device = read_hex(dir, "device");
        unsigned cls = read_hex(dir, "class"), rev = read_hex(dir, "revision");
        /* 地址 "0000:00:01.0" 的开头 "0000:" 是 PCI 域，通常都是 0，显示时省略 */
        const char *addr = strncmp(names[i], "0000:", 5) == 0 ? names[i] + 5 : names[i];
        if (numeric) {
            printf("%s %04x: %04x:%04x", addr, cls >> 8, vendor, device);
        } else {
            const char *vname = NULL, *dname = NULL;
            for (size_t k = 0; k < SK_ARRAY_LEN(g_vendors); k++)
                if (g_vendors[k].v == vendor)
                    vname = g_vendors[k].name;
            for (size_t k = 0; k < SK_ARRAY_LEN(g_devices); k++)
                if (g_devices[k].v == vendor && g_devices[k].d == device)
                    dname = g_devices[k].name;
            printf("%s %s: ", addr, class_name(cls));
            if (vname)
                printf("%s ", vname);
            else
                printf("厂商 %04x ", vendor);
            if (dname)
                printf("%s ", dname);
            else
                printf("设备 %04x ", device);
            printf("[%04x:%04x]", vendor, device);
        }
        if (rev)
            printf(" (rev %02x)", rev);
        putchar('\n');
        if (verbose) {
            unsigned sv = read_hex(dir, "subsystem_vendor"), sd = read_hex(dir, "subsystem_device");
            if (sv || sd)
                printf("\t子系统: %04x:%04x\n", sv, sd);
            unsigned irq = read_hex(dir, "irq");
            if (irq)
                printf("\t中断号: %u\n", irq);
            /* resource 文件每行一个 BAR（基地址寄存器）：起始 结束 标志；标志 0x100 表示 I/O 端口，0x200 表示内存 */
            char path[PATH_MAX + 32];
            snprintf(path, sizeof path, "%s/resource", dir);
            char *res = sk_read_file(path, NULL);
            int bar = 0;
            for (char *line = res; line && *line && bar < 7; bar++) {
                char *nl = strchr(line, '\n');
                if (nl)
                    *nl = 0;
                unsigned long long start = 0, end = 0, flags = 0;
                if (sscanf(line, "%llx %llx %llx", &start, &end, &flags) == 3 && end > start) {
                    char sz[32];
                    sk_fmt_size_short(sz, sizeof sz, end - start + 1);
                    if (flags & 0x100)
                        printf("\tI/O 端口 %llx-%llx（%s）\n", start, end, sz);
                    else if (flags & 0x200)
                        printf("\t内存 %08llx-%08llx（%s）%s\n", start, end, sz,
                               (flags & 0x2000) ? "，可预取" : "");
                }
                line = nl ? nl + 1 : NULL;
            }
            free(res);
        }
        if (drivers) {
            char path[PATH_MAX + 32], target[PATH_MAX];
            snprintf(path, sizeof path, "%s/driver", dir);
            ssize_t k = readlink(path, target, sizeof target - 1);
            if (k > 0) {
                target[k] = 0;
                char drv[256];
                printf("\t内核驱动: %s\n", sb_basename(target, drv, sizeof drv));
            } else {
                printf("\t内核驱动: （无）\n");
            }
        }
        free(names[i]);
    }
    free(names);
    return 0;
}
