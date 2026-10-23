/*
 * main.c — skbox 的入口：根据程序名把请求分发给对应的工具（applet）
 *
 * 一个程序怎么变成几十个命令？
 *   /bin/ls → skbox 的符号链接。shell 执行 /bin/ls 时，内核顺着链接加载 skbox，
 *   但传给 main() 的 argv[0] 仍然是 "ls"（或 "/bin/ls"）。我们取它的最后一段，
 *   在分发表里找到 applet_ls() 并调用它。也可以直接写 "skbox ls -l"。
 *
 * 分发表来自 Makefile 生成的 skbox_applets.inc，每行形如
 *   SKBOX_ENTRY(ls, "列出目录内容")
 * 下面把这个文件 #include 两次、每次给 SKBOX_ENTRY 不同的定义：
 * 第一次生成函数声明，第二次生成表格。这种技巧叫做 “X 宏”。
 */
#include "skbox.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 第一遍：声明所有 applet 函数 */
#define SKBOX_ENTRY(name, help) int applet_##name(int argc, char **argv);
#include "skbox_applets.inc"
#undef SKBOX_ENTRY

struct applet {
    const char *name;
    const char *help;
    int (*fn)(int argc, char **argv);
};

/* 第二遍：生成分发表 */
static const struct applet g_applets[] = {
#define SKBOX_ENTRY(name, help) { #name, help, applet_##name },
#include "skbox_applets.inc"
#undef SKBOX_ENTRY
};

static const struct applet *find_applet(const char *name)
{
    for (size_t i = 0; i < SK_ARRAY_LEN(g_applets); i++)
        if (strcmp(g_applets[i].name, name) == 0)
            return &g_applets[i];
    return NULL;
}

static int cmp_applet(const void *a, const void *b)
{
    const struct applet *const *x = a, *const *y = b;
    return strcmp((*x)->name, (*y)->name);
}

static void list_applets(int with_help)
{
    const struct applet *sorted[SK_ARRAY_LEN(g_applets)];
    for (size_t i = 0; i < SK_ARRAY_LEN(g_applets); i++)
        sorted[i] = &g_applets[i];
    qsort(sorted, SK_ARRAY_LEN(g_applets), sizeof sorted[0], cmp_applet);

    if (!with_help) {
        for (size_t i = 0; i < SK_ARRAY_LEN(g_applets); i++)
            printf("%s\n", sorted[i]->name);
        return;
    }
    int w = 0;
    for (size_t i = 0; i < SK_ARRAY_LEN(g_applets); i++)
        w = SK_MAX(w, (int)strlen(sorted[i]->name));
    for (size_t i = 0; i < SK_ARRAY_LEN(g_applets); i++)
        printf("  %-*s  %s\n", w, sorted[i]->name, sorted[i]->help);
}

static void print_usage(void)
{
    printf("SketchOS skbox %s —— 多合一命令行工具箱（共 %zu 个工具）\n\n"
           "用法: skbox 工具名 [参数...]\n"
           "      或者建立一个名为“工具名”、指向 skbox 的符号链接，直接运行它\n"
           "      skbox --list            列出全部工具及说明\n"
           "      skbox --names           只列出工具名\n"
           "      skbox --install [目录]   在目录（默认 /bin）里为每个工具建立符号链接\n"
           "      每个工具都支持 --help，例如 skbox ls --help\n\n",
           SKETCHOS_VERSION, SK_ARRAY_LEN(g_applets));
    list_applets(1);
}

/* --install：为每个工具建立 目录/名字 → skbox 的符号链接（已经存在的文件不动） */
static int install_links(const char *dir)
{
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n < 0) {
        sb_perr("无法确定 skbox 自己的路径（/proc 挂载了吗？）");
        return 1;
    }
    self[n] = 0;

    /* 如果 skbox 就在目标目录里，用相对链接 "skbox"，这样整个目录搬家也不会坏 */
    char real_dir[PATH_MAX], self_dir[PATH_MAX];
    const char *target = self;
    if (realpath(dir, real_dir)) {
        sk_strlcpy(self_dir, self, sizeof self_dir);
        char *slash = strrchr(self_dir, '/');
        if (slash) {
            size_t off = (size_t)(slash - self_dir) + 1;   /* 文件名在路径里的偏移 */
            *slash = 0;
            if (strcmp(self_dir, real_dir) == 0)
                target = self + off;
        }
    } else {
        sb_perr("无法访问目录 '%s'", dir);
        return 1;
    }

    int made = 0, skipped = 0, failed = 0;
    for (size_t i = 0; i <= SK_ARRAY_LEN(g_applets); i++) {
        /* 最后一个额外建立 "["（它不是合法的 C 标识符，所以不在表里，对应 test） */
        const char *name = i < SK_ARRAY_LEN(g_applets) ? g_applets[i].name : "[";
        char path[PATH_MAX];
        sk_path_join(path, sizeof path, dir, name);
        if (symlink(target, path) == 0) {
            made++;
        } else if (errno == EEXIST) {
            skipped++;
        } else {
            sb_perr("无法创建链接 '%s'", path);
            failed++;
        }
    }
    printf("skbox: 在 %s 里新建了 %d 个链接，跳过已存在的 %d 个\n", dir, made, skipped);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    sb_applet = "skbox";
    if (argc < 1 || !argv[0]) {
        print_usage();
        return 1;
    }

    char namebuf[256];
    const char *name = sb_basename(argv[0], namebuf, sizeof namebuf);
    /* 登录 shell 习惯在 argv[0] 前加 '-'，顺手去掉 */
    if (name[0] == '-')
        name++;

    if (strcmp(name, "skbox") == 0) {
        if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(argv[1], "--list") == 0) {
            list_applets(1);
            return 0;
        }
        if (strcmp(argv[1], "--names") == 0) {
            list_applets(0);
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("SketchOS skbox %s\n", SKETCHOS_VERSION);
            return 0;
        }
        if (strcmp(argv[1], "--install") == 0)
            return install_links(argc > 2 ? argv[2] : "/bin");
        /* "skbox ls -l"：去掉第一个参数，剩下的就和直接运行 ls -l 一样 */
        argv++;
        argc--;
        name = sb_basename(argv[0], namebuf, sizeof namebuf);
    }

    const struct applet *a;
    if (strcmp(name, "[") == 0) {
        a = find_applet("test");   /* "[ 表达式 ]" 就是 test，只是要求最后一个参数是 "]" */
        sb_applet = "[";
    } else {
        a = find_applet(name);
        if (a)
            sb_applet = a->name;
    }
    if (!a) {
        sb_err("没有叫 '%s' 的工具（用 skbox --list 查看全部）", name);
        return 127;
    }

    /* 按环境变量 LANG/LC_ALL（SketchOS 是 C.UTF-8）选择字符编码。没有这一步，C 库处于
     * “C”环境，把每个字节都当成一个字符，正则表达式里的 . 就只能匹配半个汉字。 */
    setlocale(LC_CTYPE, "");

    int rc = a->fn(argc, argv);

    /* 标准输出是带缓冲的：数据可能还在内存里。这里统一刷出去并检查写入是否成功
     * （比如磁盘满了），这样每个工具不必各自处理。 */
    if (fflush(stdout) == EOF || ferror(stdout)) {
        if (errno != EPIPE)
            sb_perr("写入标准输出失败");
        if (rc == 0)
            rc = 1;
    }
    return rc;
}
