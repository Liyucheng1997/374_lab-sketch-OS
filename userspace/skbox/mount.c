/*
 * mount — 挂载文件系统，或列出已挂载的文件系统
 *
 * Linux 只有一棵目录树。“挂载”就是把一个文件系统（磁盘分区、内存里的 tmpfs、
 * 内核提供的 proc……）接到这棵树的某个目录上，之后访问这个目录其实是在访问那个文件系统。
 * SketchOS 启动时 sinit 会挂载 /proc（进程信息）、/sys（设备信息）、/dev（设备文件）、
 * /tmp 和 /run（内存文件系统 tmpfs）。
 *
 * mount() 系统调用有两类选项：
 *   通用的标志位（MS_RDONLY 只读、MS_NOSUID、MS_NOEXEC……），所有文件系统都认识；
 *   文件系统自己的选项（tmpfs 的 size=、mode=……），作为一个字符串原样交给文件系统驱动。
 */
#include "skbox.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

SKBOX_APPLET(mount, "挂载文件系统或列出已挂载的文件系统")

static const char usage[] =
    "用法: mount [-t 类型]                       列出已挂载的文件系统（可按类型过滤）\n"
    "  或: mount [-t 类型] [-o 选项] 设备 目录    把设备上的文件系统挂载到目录\n"
    "  或: mount --bind 源目录 目录              把一个目录“镜像”到另一个位置\n"
    "  或: mount -o remount,选项 目录            修改已挂载文件系统的选项\n\n"
    "  -t 类型    文件系统类型：tmpfs、proc、sysfs、devtmpfs、ext2……（省略时依次尝试）\n"
    "  -o 选项    用逗号分隔：ro 只读、rw 读写、nosuid、nodev、noexec、noatime、relatime、\n"
    "             remount 重新挂载、bind 绑定挂载；其它选项（如 tmpfs 的 size=16m,mode=755）\n"
    "             原样交给文件系统\n"
    "  -r         只读挂载（同 -o ro）\n"
    "  --bind     绑定挂载\n"
    "例如: mount -t tmpfs -o size=16m tmpfs /mnt\n";

static const struct {
    const char *name;
    unsigned long set, clear;
} g_flags[] = {
    { "ro", MS_RDONLY, 0 },          { "rw", 0, MS_RDONLY },
    { "nosuid", MS_NOSUID, 0 },      { "suid", 0, MS_NOSUID },
    { "nodev", MS_NODEV, 0 },        { "dev", 0, MS_NODEV },
    { "noexec", MS_NOEXEC, 0 },      { "exec", 0, MS_NOEXEC },
    { "sync", MS_SYNCHRONOUS, 0 },   { "async", 0, MS_SYNCHRONOUS },
    { "noatime", MS_NOATIME, 0 },    { "atime", 0, MS_NOATIME },
    { "nodiratime", MS_NODIRATIME, 0 }, { "diratime", 0, MS_NODIRATIME },
    { "relatime", MS_RELATIME, 0 },  { "norelatime", 0, MS_RELATIME },
    { "strictatime", MS_STRICTATIME, 0 },
    { "dirsync", MS_DIRSYNC, 0 },    { "silent", MS_SILENT, 0 },
    { "remount", MS_REMOUNT, 0 },    { "bind", MS_BIND, 0 },
    { "rbind", MS_BIND | MS_REC, 0 }, { "move", MS_MOVE, 0 },
    { "private", MS_PRIVATE, 0 },    { "rprivate", MS_PRIVATE | MS_REC, 0 },
    { "shared", MS_SHARED, 0 },      { "rshared", MS_SHARED | MS_REC, 0 },
    { "slave", MS_SLAVE, 0 },        { "rslave", MS_SLAVE | MS_REC, 0 },
    { "unbindable", MS_UNBINDABLE, 0 }, { "defaults", 0, 0 },
};

/* 把 "ro,size=16m,noexec" 拆成标志位和留给文件系统的字符串 */
static void parse_options(const char *opts, unsigned long *flags, char *data, size_t dn)
{
    char *copy = strdup(opts), *save = NULL;
    if (!copy)
        return;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int known = 0;
        for (size_t i = 0; i < SK_ARRAY_LEN(g_flags); i++) {
            if (strcmp(tok, g_flags[i].name) == 0) {
                *flags |= g_flags[i].set;
                *flags &= ~g_flags[i].clear;
                known = 1;
                break;
            }
        }
        if (!known) {
            if (data[0])
                sk_strlcat(data, ",", dn);
            sk_strlcat(data, tok, dn);
        }
    }
    free(copy);
}

static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r;) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static int list_mounts(const char *type_filter)
{
    char *s = sk_read_file("/proc/self/mounts", NULL);
    if (!s)
        s = sk_read_file("/proc/mounts", NULL);
    if (!s) {
        sb_perr("无法读取 /proc/mounts（/proc 挂载了吗？）");
        return 1;
    }
    /* 先收集再按列宽对齐输出 */
    char *fields[512][4];
    int n = 0, w[4] = { 0 };
    const char *hdr[4] = { "设备", "挂载点", "类型", "选项" };
    for (int k = 0; k < 4; k++)
        w[k] = sk_utf8_width(hdr[k]);
    for (char *line = s; line && *line && n < 512;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *save = NULL;
        char *f[4];
        int k;
        for (k = 0; k < 4; k++) {
            f[k] = strtok_r(k ? NULL : line, " ", &save);
            if (!f[k])
                break;
        }
        if (k == 4 && (!type_filter || strcmp(f[2], type_filter) == 0)) {
            for (k = 0; k < 4; k++) {
                if (k < 2)
                    unescape(f[k]);
                fields[n][k] = f[k];
                if (k < 3)
                    w[k] = SK_MAX(w[k], sk_utf8_width(f[k]));
            }
            n++;
        }
        line = nl ? nl + 1 : NULL;
    }
    int color = sb_color_enabled(STDOUT_FILENO);
    if (color)
        fputs(SB_BOLD, stdout);
    for (int k = 0; k < 4; k++) {
        if (k < 3) {
            sb_put_padded(stdout, hdr[k], w[k], 1);
            fputs("  ", stdout);
        } else {
            fputs(hdr[k], stdout);
        }
    }
    puts(color ? SB_RESET : "");
    for (int i = 0; i < n; i++) {
        sb_put_padded(stdout, fields[i][0], w[0], 1);
        fputs("  ", stdout);
        if (color)
            fputs(SB_BLUE, stdout);
        sb_put_padded(stdout, fields[i][1], w[1], 1);
        if (color)
            fputs(SB_RESET, stdout);
        fputs("  ", stdout);
        if (color)
            fputs(SB_GREEN, stdout);
        sb_put_padded(stdout, fields[i][2], w[2], 1);
        if (color)
            fputs(SB_RESET, stdout);
        fputs("  ", stdout);
        puts(fields[i][3]);
    }
    free(s);
    return 0;
}

/* 没给 -t 时：按 /proc/filesystems 里列出的、需要真实设备的类型逐个尝试 */
static int try_all_types(const char *src, const char *dir, unsigned long flags, const char *data)
{
    char *s = sk_read_file("/proc/filesystems", NULL);
    if (!s) {
        errno = ENODEV;
        return -1;
    }
    int r = -1, err = ENODEV;
    for (char *line = s; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        /* 以 "nodev" 开头的是不需要块设备的虚拟文件系统，不能靠“猜”来挂载 */
        if (strncmp(line, "nodev", 5) != 0) {
            char *t = sk_trim(line);
            if (*t && mount(src, dir, t, flags, data) == 0) {
                r = 0;
                break;
            }
            if (errno != EINVAL && errno != ENODEV)
                err = errno;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(s);
    if (r != 0)
        errno = err;
    return r;
}

int applet_mount(int argc, char **argv)
{
    static const struct option lo[] = {
        { "bind", no_argument, NULL, 'B' },
        { "rbind", no_argument, NULL, 'R' },
        { "move", no_argument, NULL, 'M' },
        { 0, 0, 0, 0 },
    };
    const char *type = NULL;
    unsigned long flags = 0;
    char data[1024] = "";
    int c;
    while ((c = sb_getopt_long(argc, argv, "t:o:rwvn", lo, usage)) != -1) {
        switch (c) {
        case 't': type = optarg; break;
        case 'o': parse_options(optarg, &flags, data, sizeof data); break;
        case 'r': flags |= MS_RDONLY; break;
        case 'w': flags &= ~(unsigned long)MS_RDONLY; break;
        case 'v':
        case 'n': break;
        case 'B': flags |= MS_BIND; break;
        case 'R': flags |= MS_BIND | MS_REC; break;
        case 'M': flags |= MS_MOVE; break;
        default:  return 1;
        }
    }
    int n = argc - optind;
    if (n == 0)
        return list_mounts(type);

    const char *src, *dir;
    if (n == 1) {
        /* 只给一个参数：只能是 remount（修改已挂载的目录） */
        if (!(flags & MS_REMOUNT)) {
            sb_err("只给出一个参数时必须使用 -o remount（SketchOS 没有 /etc/fstab）");
            sb_hint();
            return 1;
        }
        src = "none";
        dir = argv[optind];
    } else if (n == 2) {
        src = argv[optind];
        dir = argv[optind + 1];
    } else {
        sb_err("多余的操作数 '%s'", argv[optind + 2]);
        sb_hint();
        return 1;
    }

    int r;
    if (flags & (MS_BIND | MS_MOVE | MS_REMOUNT))
        r = mount(src, dir, type, flags, data[0] ? data : NULL);   /* 这几种不需要类型 */
    else if (type)
        r = mount(src, dir, type, flags, data[0] ? data : NULL);
    else
        r = try_all_types(src, dir, flags, data[0] ? data : NULL);
    if (r != 0) {
        int err = errno;
        const char *hint = "";
        if (err == ENODEV)
            hint = "（内核不支持这种文件系统类型）";
        else if (err == ENOTBLK)
            hint = "（需要一个块设备）";
        else if (err == EPERM)
            hint = "（需要 root 权限）";
        sb_err("无法把 '%s' 挂载到 '%s': %s%s", src, dir, sk_strerror(err), hint);
        return 1;
    }
    /* 绑定挂载时 ro 等选项要再 remount 一次才生效（内核的规定） */
    if ((flags & MS_BIND) && (flags & MS_RDONLY) && !(flags & MS_REMOUNT)) {
        if (mount("none", dir, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0)
            sb_perr("无法把绑定挂载 '%s' 设为只读", dir);
    }
    return 0;
}
