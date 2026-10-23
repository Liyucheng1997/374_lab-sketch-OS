/*
 * free — 显示内存使用情况（数据来自 /proc/meminfo）
 *
 * Linux 会把暂时没用的内存拿来缓存文件内容（页缓存、buffers），所以“空闲”往往很少——
 * 这不是坏事：缓存随时可以丢掉还给程序用。真正该看的是“可用”(MemAvailable)：
 * 内核估计的、不必换出就能分配给新程序的内存。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(free, "显示内存使用情况")

static const char usage[] =
    "用法: free [选项]\n"
    "显示物理内存和交换空间的使用情况。默认单位是 KB（1024 字节）。\n"
    "  已用 = 总计 - 空闲 - 缓冲/缓存；可用 = 内核估计还能分配给程序的内存（含可回收的缓存）\n\n"
    "  -b    以字节为单位\n"
    "  -k    以 KB 为单位（默认）\n"
    "  -m    以 MB 为单位\n"
    "  -g    以 GB 为单位\n"
    "  -h    自动选择合适的单位（K、M、G）\n"
    "  -t    再显示一行总计（内存 + 交换）\n";

static unsigned long long key_kb(const char *buf, const char *key)
{
    const char *p = buf;
    size_t kl = strlen(key);
    while ((p = strstr(p, key))) {
        if (p == buf || p[-1] == '\n')
            return strtoull(p + kl, NULL, 10);
        p += kl;
    }
    return 0;
}

static int g_human;
static unsigned long long g_div = 1024;   /* 输出单位（字节） */

static void cell(unsigned long long kb)
{
    char buf[32];
    unsigned long long bytes = kb * 1024;
    if (g_human)
        sk_fmt_size_short(buf, sizeof buf, bytes);
    else
        snprintf(buf, sizeof buf, "%llu", bytes / g_div);
    printf(" %11s", buf);
}

static void label(const char *s)
{
    sb_put_padded(stdout, s, 6, 1);
}

int applet_free(int argc, char **argv)
{
    int total_line = 0, c;
    while ((c = sb_getopt(argc, argv, "bkmghtw", usage)) != -1) {
        switch (c) {
        case 'b': g_div = 1; g_human = 0; break;
        case 'k': g_div = 1024; g_human = 0; break;
        case 'm': g_div = 1024 * 1024; g_human = 0; break;
        case 'g': g_div = 1024ULL * 1024 * 1024; g_human = 0; break;
        case 'h': g_human = 1; break;
        case 't': total_line = 1; break;
        case 'w': break;
        default:  return 1;
        }
    }
    char *mi = sk_read_file("/proc/meminfo", NULL);
    if (!mi) {
        sb_perr("无法读取 /proc/meminfo（/proc 挂载了吗？）");
        return 1;
    }
    unsigned long long total = key_kb(mi, "MemTotal:"), freek = key_kb(mi, "MemFree:");
    unsigned long long buffers = key_kb(mi, "Buffers:"), cached = key_kb(mi, "Cached:");
    unsigned long long sreclaim = key_kb(mi, "SReclaimable:"), shmem = key_kb(mi, "Shmem:");
    unsigned long long avail = key_kb(mi, "MemAvailable:");
    unsigned long long swap_total = key_kb(mi, "SwapTotal:"), swap_free = key_kb(mi, "SwapFree:");
    free(mi);
    /* “缓存”包括页缓存和可回收的内核对象缓存（slab），和 procps 的 free 一致 */
    unsigned long long cache = cached + sreclaim;
    unsigned long long buff_cache = buffers + cache;
    unsigned long long used = total > freek + buff_cache ? total - freek - buff_cache : total - freek;
    if (!avail)
        avail = freek + buff_cache;
    if (avail > total)
        avail = total;

    const char *hdr[6] = { "总计", "已用", "空闲", "共享", "缓冲/缓存", "可用" };
    label("");
    for (int i = 0; i < 6; i++) {
        putchar(' ');
        sb_put_padded(stdout, hdr[i], 11, 0);
    }
    putchar('\n');
    label("内存:");
    cell(total);
    cell(used);
    cell(freek);
    cell(shmem);
    cell(buff_cache);
    cell(avail);
    putchar('\n');
    label("交换:");
    cell(swap_total);
    cell(swap_total - swap_free);
    cell(swap_free);
    putchar('\n');
    if (total_line) {
        label("总计:");
        cell(total + swap_total);
        cell(used + swap_total - swap_free);
        cell(freek + swap_free);
        putchar('\n');
    }
    return 0;
}
