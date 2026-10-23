/*
 * lscpu — 显示 CPU 信息（来自 /proc/cpuinfo）
 *
 * 开机时内核用 CPUID 指令询问处理器：厂商、型号、支持哪些指令集扩展（flags）……
 * 结果整理在 /proc/cpuinfo 里。在 QEMU/v86 里看到的是模拟器“假装”出来的 CPU。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

SKBOX_APPLET(lscpu, "显示 CPU 的信息")

static const char usage[] =
    "用法: lscpu [-f]\n"
    "显示处理器的架构、型号、频率、缓存和支持的特性（来自 /proc/cpuinfo）。\n\n"
    "  -f    显示完整的特性（flags）列表，不截断\n";

static const char *field(char *block, const char *key, char *out, size_t n)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (char *line = block; line && *line;) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, kl) == 0 && (line[kl] == ' ' || line[kl] == '\t' || line[kl] == ':')) {
            char *colon = memchr(line, ':', len);
            if (colon) {
                const char *v = colon + 1;
                while (*v == ' ' || *v == '\t')
                    v++;
                size_t vl = (size_t)(line + len - v);
                if (vl >= n)
                    vl = n - 1;
                memcpy(out, v, vl);
                out[vl] = 0;
            }
            return out;
        }
        line = nl ? nl + 1 : NULL;
    }
    return out;
}

static void row(const char *label, const char *value)
{
    if (!value || !*value)
        return;
    sb_put_padded(stdout, label, 16, 1);
    puts(value);
}

int applet_lscpu(int argc, char **argv)
{
    int full = 0, c;
    while ((c = sb_getopt(argc, argv, "f", usage)) != -1) {
        if (c != 'f')
            return 1;
        full = 1;
    }
    char *info = sk_read_file("/proc/cpuinfo", NULL);
    if (!info) {
        sb_perr("无法读取 /proc/cpuinfo（/proc 挂载了吗？）");
        return 1;
    }
    int ncpu = 0;
    for (char *p = info; (p = strstr(p, "processor")); p++)
        if (p == info || p[-1] == '\n')
            ncpu++;
    /* 只看第一个处理器的那一段（以空行分隔） */
    char *end = strstr(info, "\n\n");
    if (end)
        end[1] = 0;

    struct utsname u;
    uname(&u);
    char buf[4096], tmp[64];
    row("架构:", u.machine);
    row("运行模式:", strstr(u.machine, "64") ? "64 位" : "32 位");
    row("字节序:", "小端（Little Endian，低字节在前）");
    snprintf(tmp, sizeof tmp, "%d", ncpu ? ncpu : 1);
    row("CPU 数量:", tmp);
    snprintf(tmp, sizeof tmp, "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    row("在线 CPU:", tmp);
    row("厂商 ID:", field(info, "vendor_id", buf, sizeof buf));
    row("型号名称:", field(info, "model name", buf, sizeof buf));
    row("CPU 系列:", field(info, "cpu family", buf, sizeof buf));
    row("型号:", field(info, "model", buf, sizeof buf));
    row("步进:", field(info, "stepping", buf, sizeof buf));
    if (*field(info, "cpu MHz", buf, sizeof buf)) {
        snprintf(tmp, sizeof tmp, "%.0f MHz", atof(buf));
        row("频率:", tmp);
    }
    row("缓存:", field(info, "cache size", buf, sizeof buf));
    row("BogoMIPS:", field(info, "bogomips", buf, sizeof buf));
    row("地址宽度:", field(info, "address sizes", buf, sizeof buf));
    field(info, "flags", buf, sizeof buf);
    if (buf[0]) {
        if (strstr(buf, "hypervisor"))
            row("虚拟化:", "运行在虚拟机里（CPU 报告了 hypervisor 标志）");
        /* 解释几个有代表性的特性 */
        static const struct { const char *flag, *desc; } known[] = {
            { "fpu", "浮点运算单元" }, { "tsc", "时间戳计数器" }, { "pae", "物理地址扩展" },
            { "mmx", "MMX 多媒体指令" }, { "sse", "SSE" }, { "sse2", "SSE2" }, { "apic", "高级中断控制器" },
            { "cmov", "条件传送指令" }, { "nx", "不可执行页保护" }, { "lm", "64 位长模式" },
        };
        char feats[512] = "";
        for (size_t i = 0; i < SK_ARRAY_LEN(known); i++) {
            char pat[32];
            snprintf(pat, sizeof pat, " %s ", known[i].flag);
            char padded[4200];
            snprintf(padded, sizeof padded, " %s ", buf);
            if (strstr(padded, pat)) {
                if (feats[0])
                    sk_strlcat(feats, "、", sizeof feats);
                sk_strlcat(feats, known[i].desc, sizeof feats);
            }
        }
        row("主要特性:", feats);
        if (!full) {
            int cols = sk_term_width(STDOUT_FILENO) - 16;
            if (cols > 20 && (int)strlen(buf) > cols) {
                size_t cut = (size_t)cols - 4;
                while (cut > 0 && buf[cut] != ' ')
                    cut--;
                sk_strlcpy(buf + cut, " ...", sizeof buf - cut);
            }
        }
        row("标志:", buf);
    }
    free(info);
    return 0;
}
