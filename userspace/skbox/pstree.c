/*
 * pstree — 用树形图显示进程之间的父子关系
 *
 * 在 Linux 里，除了 1 号进程（init，SketchOS 里是 sinit）和 2 号进程（kthreadd，所有内核线程
 * 的父亲）之外，每个进程都是被别的进程 fork() 出来的，所以所有进程组成一棵（两棵）树。
 * 父进程先退出时，孩子会被“过继”给 1 号进程——这就是为什么 sinit 必须不断 wait() 回收僵尸。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(pstree, "以树形图显示进程")

static const char usage[] =
    "用法: pstree [选项] [进程号]\n"
    "以树形图显示进程。默认从 1 号进程（sinit）开始；给出进程号时只显示以它为根的子树。\n"
    "名字相同、又没有子进程的兄弟进程会合并显示成 N*[名字]。\n\n"
    "  -p    显示进程号（同时关闭合并）\n"
    "  -a    显示完整的命令行参数\n"
    "  -k    同时显示内核线程（2 号进程 kthreadd 下面的那棵树）\n"
    "  -A    用 ASCII 字符画线（终端不支持 UTF-8 时使用）\n"
    "  -c    不合并相同的子树\n";

static sb_proc *g_p;
static int g_n;
static int show_pid, show_args, ascii, no_compact;
static const char *L_ONE, *L_FIRST, *L_MID, *L_LAST, *L_BAR;

static int find(pid_t pid)
{
    int lo = 0, hi = g_n - 1;   /* sb_list_procs 按 pid 升序排列，可以二分查找 */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_p[mid].info.pid == pid)
            return mid;
        if (g_p[mid].info.pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int has_children(pid_t pid)
{
    for (int i = 0; i < g_n; i++)
        if (g_p[i].info.ppid == pid && g_p[i].info.pid != pid)
            return 1;
    return 0;
}

static void make_label(const sb_proc *p, char *buf, size_t n)
{
    const sk_procinfo *pi = &p->info;
    size_t k = (size_t)snprintf(buf, n, "%s", pi->comm);
    if (show_pid && k < n)
        k += (size_t)snprintf(buf + k, n - k, "(%d)", (int)pi->pid);
    if (show_args && pi->cmdline[0] && k < n) {
        const char *sp = strchr(pi->cmdline, ' ');
        if (sp)
            snprintf(buf + k, n - k, "%s", sp);
    }
}

static int cmp_child(const void *a, const void *b)
{
    const sb_proc *x = *(sb_proc *const *)a, *y = *(sb_proc *const *)b;
    int r = strcmp(x->info.comm, y->info.comm);
    if (r)
        return r;
    return (x->info.pid > y->info.pid) - (x->info.pid < y->info.pid);
}

/* 输出以 idx 为根的子树。调用时，这一行前面的部分已经输出好了；
 * prefix 是后续各行要先输出的前缀（竖线和空格）。 */
static void draw(int idx, const char *prefix, const char *label_override)
{
    char label[384];
    if (label_override)
        sk_strlcpy(label, label_override, sizeof label);
    else
        make_label(&g_p[idx], label, sizeof label);
    fputs(label, stdout);
    if (idx < 0) {
        putchar('\n');
        return;
    }
    pid_t me = g_p[idx].info.pid;

    /* 收集孩子，按名字排序 */
    int nk = 0;
    sb_proc **kids = malloc(sizeof *kids * (size_t)(g_n ? g_n : 1));
    if (!kids) {
        putchar('\n');
        return;
    }
    for (int i = 0; i < g_n; i++)
        if (g_p[i].info.ppid == me && g_p[i].info.pid != me)
            kids[nk++] = &g_p[i];
    qsort(kids, (size_t)nk, sizeof *kids, cmp_child);

    /* 合并：相邻的、同名、没有孩子的叶子进程 → "3*[名字]" */
    typedef struct {
        int idx;
        int count;
    } item;
    item *items = malloc(sizeof *items * (size_t)(nk ? nk : 1));
    int ni = 0;
    for (int i = 0; items && i < nk; i++) {
        int ki = (int)(kids[i] - g_p);
        int leaf = !has_children(kids[i]->info.pid);
        if (!no_compact && !show_pid && leaf && ni > 0 && items[ni - 1].count > 0 &&
            strcmp(g_p[items[ni - 1].idx].info.comm, kids[i]->info.comm) == 0 &&
            (!show_args || strcmp(g_p[items[ni - 1].idx].info.cmdline, kids[i]->info.cmdline) == 0)) {
            items[ni - 1].count++;
            continue;
        }
        items[ni].idx = ki;
        items[ni].count = leaf ? 1 : 0;   /* count=0：有孩子的节点，不参与合并 */
        ni++;
    }
    free(kids);
    if (ni == 0 || !items) {
        free(items);
        putchar('\n');
        return;
    }

    int w = sk_utf8_width(label);
    size_t plen = strlen(prefix);
    char *child_prefix = malloc(plen + (size_t)w + 16);
    if (!child_prefix) {
        free(items);
        putchar('\n');
        return;
    }
    for (int i = 0; i < ni; i++) {
        int last = i == ni - 1;
        if (i == 0)
            fputs(ni == 1 ? L_ONE : L_FIRST, stdout);
        else {
            fputs(prefix, stdout);
            for (int s = 0; s < w; s++)
                putchar(' ');
            fputs(last ? L_LAST : L_MID, stdout);
        }
        /* 这个孩子的后代各行的前缀：父亲的前缀 + 父亲标签宽度的空格 + 竖线（后面还有兄弟时） */
        memcpy(child_prefix, prefix, plen);
        memset(child_prefix + plen, ' ', (size_t)w);
        child_prefix[plen + (size_t)w] = 0;
        sk_strlcat(child_prefix, last ? "   " : L_BAR, plen + (size_t)w + 16);
        if (items[i].count > 1) {
            char merged[400];
            char one[384];
            make_label(&g_p[items[i].idx], one, sizeof one);
            snprintf(merged, sizeof merged, "%d*[%s]", items[i].count, one);
            draw(-1, child_prefix, merged);
        } else {
            draw(items[i].idx, child_prefix, NULL);
        }
    }
    free(child_prefix);
    free(items);
}

int applet_pstree(int argc, char **argv)
{
    int kernel = 0, c;
    while ((c = sb_getopt(argc, argv, "pakAcl", usage)) != -1) {
        switch (c) {
        case 'p': show_pid = 1; break;
        case 'a': show_args = 1; break;
        case 'k': kernel = 1; break;
        case 'A': ascii = 1; break;
        case 'c': no_compact = 1; break;
        case 'l': break;
        default:  return 1;
        }
    }
    if (ascii) {
        L_ONE = "---"; L_FIRST = "-+-"; L_MID = " |-"; L_LAST = " `-"; L_BAR = " | ";
    } else {
        /* 制表符（box drawing）字符：U+2500 一族 */
        L_ONE = "───"; L_FIRST = "─┬─"; L_MID = " ├─"; L_LAST = " └─"; L_BAR = " │ ";
    }
    g_p = sb_list_procs(&g_n);
    if (!g_p) {
        sb_perr("无法读取 /proc（/proc 挂载了吗？）");
        return 1;
    }
    if (optind < argc) {
        long long pid;
        if (sb_parse_ll(argv[optind], &pid) != 0) {
            sb_err("无效的进程号 '%s'", argv[optind]);
            return 1;
        }
        int idx = find((pid_t)pid);
        if (idx < 0) {
            sb_err("没有进程号为 %lld 的进程", pid);
            return 1;
        }
        draw(idx, "", NULL);
        return 0;
    }
    int root = find(1);
    if (root >= 0) {
        draw(root, "", NULL);
    } else {
        /* 没有 1 号进程（比如在容器里）：把所有“父进程不存在”的进程当作根 */
        for (int i = 0; i < g_n; i++)
            if (find(g_p[i].info.ppid) < 0 && !g_p[i].info.is_kernel_thread)
                draw(i, "", NULL);
    }
    if (kernel) {
        int kt = find(2);
        if (kt >= 0)
            draw(kt, "", NULL);
    }
    free(g_p);
    return 0;
}
