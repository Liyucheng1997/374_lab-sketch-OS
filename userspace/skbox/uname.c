/*
 * uname — 显示内核和机器的信息
 *
 * 这些信息都来自 uname() 系统调用：内核把自己的名字、版本、编译时间、
 * 主机名和 CPU 架构填进 struct utsname 返回给我们。
 */
#include "skbox.h"

#include <sys/utsname.h>

SKBOX_APPLET(uname, "显示内核名称、版本和机器类型")

static const char usage[] =
    "用法: uname [选项]\n"
    "显示系统信息。不带选项时相当于 -s。\n\n"
    "  -a    全部信息（按下面的顺序）\n"
    "  -s    内核名称（Linux）\n"
    "  -n    主机名（网络节点名）\n"
    "  -r    内核版本号\n"
    "  -v    内核编译版本（编译次数和时间）\n"
    "  -m    机器硬件架构（i686 = 32 位 x86）\n"
    "  -p    处理器类型\n"
    "  -i    硬件平台\n"
    "  -o    操作系统名称\n";

enum { F_S = 1, F_N = 2, F_R = 4, F_V = 8, F_M = 16, F_P = 32, F_I = 64, F_O = 128 };

int applet_uname(int argc, char **argv)
{
    int flags = 0, all = 0, c;
    while ((c = sb_getopt(argc, argv, "asnrvmpio", usage)) != -1) {
        switch (c) {
        case 'a': all = 1; break;
        case 's': flags |= F_S; break;
        case 'n': flags |= F_N; break;
        case 'r': flags |= F_R; break;
        case 'v': flags |= F_V; break;
        case 'm': flags |= F_M; break;
        case 'p': flags |= F_P; break;
        case 'i': flags |= F_I; break;
        case 'o': flags |= F_O; break;
        default:  return 1;
        }
    }
    if (optind < argc) {
        sb_err("多余的操作数 '%s'", argv[optind]);
        sb_hint();
        return 1;
    }
    struct utsname u;
    if (uname(&u) != 0) {
        sb_perr("uname 失败");
        return 1;
    }
    if (all)
        flags = F_S | F_N | F_R | F_V | F_M | F_O;   /* 和 GNU 一样，-a 省略未知的 -p -i */
    if (!flags)
        flags = F_S;
    const char *parts[8] = {
        (flags & F_S) ? u.sysname : NULL, (flags & F_N) ? u.nodename : NULL,
        (flags & F_R) ? u.release : NULL, (flags & F_V) ? u.version : NULL,
        (flags & F_M) ? u.machine : NULL, (flags & F_P) ? u.machine : NULL,
        (flags & F_I) ? u.machine : NULL,
        /* 内核是 Linux，内核之上的用户态是我们自己写的 SketchOS */
        (flags & F_O) ? "SketchOS" : NULL,
    };
    int first = 1;
    for (int i = 0; i < 8; i++) {
        if (!parts[i])
            continue;
        if (!first)
            putchar(' ');
        fputs(parts[i], stdout);
        first = 0;
    }
    putchar('\n');
    return 0;
}
