/*
 * stat — 显示文件（inode）的全部属性，或者文件系统的信息
 *
 * stat() 系统调用把 inode 里的信息填进 struct stat：设备号、inode 号、类型和权限、
 * 硬链接数、属主、大小、占用的块数、三个时间戳。ls -l 显示的就是其中一部分。
 */
#include "skbox.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

SKBOX_APPLET(stat, "显示文件或文件系统的详细信息")

static const char usage[] =
    "用法: stat [选项] 文件...\n"
    "显示文件的详细信息（inode 的内容）。\n\n"
    "  -L          跟随符号链接，显示它指向的文件\n"
    "  -f          显示文件所在文件系统的信息\n"
    "  -c 格式     按格式输出（每个文件后加换行）\n"
    "  --printf=格式  同 -c，但不自动换行，并解释 \\n 等转义\n\n"
    "文件格式符：%n 文件名  %N 带引号的名字（链接显示指向）  %s 大小  %b 块数  %B 块大小\n"
    "  %a 八进制权限  %A 权限字符串  %F 文件类型  %f 原始模式（十六进制）\n"
    "  %u %U 属主 id/名字  %g %G 组 id/名字  %h 硬链接数  %i inode 号  %o IO 块大小\n"
    "  %d %D 设备号（十进制/十六进制）  %t %T 设备文件的主/次设备号（十六进制）\n"
    "  %x %y %z 访问/修改/状态改变时间  %X %Y %Z 同上（Unix 秒数）  %% 百分号\n"
    "文件系统格式符（-f）：%n 文件名  %t 类型（十六进制）  %T 类型名  %s 块大小  %S 基本块大小\n"
    "  %b 总块数  %f 空闲块  %a 普通用户可用块  %c 总 inode 数  %d 空闲 inode  %l 最长文件名  %i 文件系统 id\n";

static const char *fs_type_name(unsigned long t)
{
    switch (t) {
    case 0x01021994: return "tmpfs";
    case 0x858458f6: return "ramfs";
    case 0x9fa0:     return "proc";
    case 0x62656572: return "sysfs";
    case 0x1cd1:     return "devpts";
    case 0xef53:     return "ext2/ext3/ext4";
    case 0x64626720: return "debugfs";
    case 0x27e0eb:   return "cgroup";
    case 0x63677270: return "cgroup2";
    case 0x6e736673: return "nsfs";
    case 0x73636673: return "securityfs";
    case 0x01021997: return "v9fs";
    case 0x65735546: return "fuse";
    case 0x9123683e: return "btrfs";
    case 0x58465342: return "xfs";
    case 0x4d44:     return "msdos/vfat";
    case 0x6969:     return "nfs";
    case 0x794c7630: return "overlayfs";
    case 0x42494e4d: return "binfmt_misc";
    case 0xcafe4a11: return "bpf";
    case 0x74726163: return "tracefs";
    case 0x73717368: return "squashfs";
    case 0x9660:     return "iso9660";
    case 0x19800202: return "mqueue";
    case 0x6165676c: return "pstore";
    case 0x1373:     return "devfs";
    case 0x5346544e: return "ntfs";
    case 0x4244:     return "hfs";
    case 0x43415d53: return "smackfs";
    case 0x1badface: return "bfs";
    case 0x00c36400: return "ceph";
    case 0xde5e81e4: return "efivarfs";
    case 0x2011bab0: return "exfat";
    case 0x53464846: return "wslfs";
    }
    return "未知";
}

/* 按格式符输出一个值：把用户写的 %-10 之类的修饰原样用到 printf 上 */
static void emit_str(const char *mods, const char *s)
{
    char f[32];
    snprintf(f, sizeof f, "%%%ss", mods);
    printf(f, s);
}

static void emit_num(const char *mods, unsigned long long v)
{
    char f[32];
    snprintf(f, sizeof f, "%%%sllu", mods);
    printf(f, v);
}

static void emit_hex(const char *mods, unsigned long long v)
{
    char f[32];
    snprintf(f, sizeof f, "%%%sllx", mods);
    printf(f, v);
}

static void time_str(char *buf, size_t n, const struct timespec *ts)
{
    sb_fmt_time_full(buf, n, ts);
}

static void quoted_name(char *buf, size_t n, const char *path, const struct stat *st)
{
    if (S_ISLNK(st->st_mode)) {
        char target[PATH_MAX];
        ssize_t k = readlink(path, target, sizeof target - 1);
        if (k >= 0) {
            target[k] = 0;
            snprintf(buf, n, "'%s' -> '%s'", path, target);
            return;
        }
    }
    snprintf(buf, n, "'%s'", path);
}

/* 处理一个格式串；escapes：--printf 模式下解释反斜杠转义 */
static void run_format(const char *fmt, const char *path, const struct stat *st,
                       const struct statfs *sf, int escapes)
{
    char buf[PATH_MAX * 2 + 16];
    for (const char *p = fmt; *p; p++) {
        if (escapes && *p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break;
            case 'a': putchar('\a'); break;
            case 'e': putchar('\033'); break;
            case '\\': putchar('\\'); break;
            case '"': putchar('"'); break;
            default:
                if (*p >= '0' && *p <= '7') {
                    int v = 0;
                    for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++, p++)
                        v = v * 8 + (*p - '0');
                    p--;
                    putchar(v);
                } else {
                    putchar('\\');
                    putchar(*p);
                }
            }
            continue;
        }
        if (*p != '%') {
            putchar(*p);
            continue;
        }
        char mods[24];
        size_t ml = 0;
        p++;
        while ((*p && strchr("-+ #0'", *p)) || isdigit((unsigned char)*p) || *p == '.') {
            if (ml + 1 < sizeof mods)
                mods[ml++] = *p;
            p++;
        }
        mods[ml] = 0;
        char conv = *p;
        if (!conv) {
            putchar('%');
            break;
        }
        if (conv == '%') {
            putchar('%');
            continue;
        }
        if (sf) {
            switch (conv) {
            case 'n': emit_str(mods, path); break;
            case 'i': snprintf(buf, sizeof buf, "%x%08x", (unsigned)sf->f_fsid.__val[0],
                               (unsigned)sf->f_fsid.__val[1]);
                      emit_str(mods, buf); break;
            case 'l': emit_num(mods, (unsigned long long)sf->f_namelen); break;
            case 't': emit_hex(mods, (unsigned long long)(unsigned long)sf->f_type); break;
            case 'T': emit_str(mods, fs_type_name((unsigned long)sf->f_type)); break;
            case 'b': emit_num(mods, (unsigned long long)sf->f_blocks); break;
            case 'f': emit_num(mods, (unsigned long long)sf->f_bfree); break;
            case 'a': emit_num(mods, (unsigned long long)sf->f_bavail); break;
            case 's': emit_num(mods, (unsigned long long)sf->f_bsize); break;
            case 'S': emit_num(mods, (unsigned long long)(sf->f_frsize ? sf->f_frsize : sf->f_bsize)); break;
            case 'c': emit_num(mods, (unsigned long long)sf->f_files); break;
            case 'd': emit_num(mods, (unsigned long long)sf->f_ffree); break;
            default:  putchar('?'); break;
            }
            continue;
        }
        switch (conv) {
        case 'n': emit_str(mods, path); break;
        case 'N': quoted_name(buf, sizeof buf, path, st); emit_str(mods, buf); break;
        case 's': emit_num(mods, (unsigned long long)st->st_size); break;
        case 'b': emit_num(mods, (unsigned long long)st->st_blocks); break;
        case 'B': emit_num(mods, 512); break;
        case 'o': emit_num(mods, (unsigned long long)st->st_blksize); break;
        case 'a': {
            char f[32];
            snprintf(f, sizeof f, "%%%so", mods);
            printf(f, (unsigned)(st->st_mode & 07777));
            break;
        }
        case 'A': sb_mode_string(st->st_mode, buf); emit_str(mods, buf); break;
        case 'F': emit_str(mods, sb_file_type_name(st->st_mode, st->st_size)); break;
        case 'f': emit_hex(mods, (unsigned long long)st->st_mode); break;
        case 'u': emit_num(mods, (unsigned long long)st->st_uid); break;
        case 'U': emit_str(mods, sk_user_name(st->st_uid, buf, sizeof buf)); break;
        case 'g': emit_num(mods, (unsigned long long)st->st_gid); break;
        case 'G': emit_str(mods, sk_group_name(st->st_gid, buf, sizeof buf)); break;
        case 'h': emit_num(mods, (unsigned long long)st->st_nlink); break;
        case 'i': emit_num(mods, (unsigned long long)st->st_ino); break;
        case 'd': emit_num(mods, (unsigned long long)st->st_dev); break;
        case 'D': emit_hex(mods, (unsigned long long)st->st_dev); break;
        case 't': emit_hex(mods, (unsigned long long)major(st->st_rdev)); break;
        case 'T': emit_hex(mods, (unsigned long long)minor(st->st_rdev)); break;
        case 'x': time_str(buf, sizeof buf, &st->st_atim); emit_str(mods, buf); break;
        case 'y': time_str(buf, sizeof buf, &st->st_mtim); emit_str(mods, buf); break;
        case 'z': time_str(buf, sizeof buf, &st->st_ctim); emit_str(mods, buf); break;
        case 'X': emit_num(mods, (unsigned long long)st->st_atim.tv_sec); break;
        case 'Y': emit_num(mods, (unsigned long long)st->st_mtim.tv_sec); break;
        case 'Z': emit_num(mods, (unsigned long long)st->st_ctim.tv_sec); break;
        case 'w': emit_str(mods, "-"); break;
        case 'W': emit_num(mods, 0); break;
        default:  putchar('?'); break;
        }
    }
}

static void print_default(const char *path, const struct stat *st)
{
    char name[PATH_MAX * 2 + 16], mode[12], ub[64], gb[64], t[96];
    quoted_name(name, sizeof name, path, st);
    printf("  文件: %s\n", name);
    printf("  大小: %-12lld 块: %-10lld IO 块: %-7ld %s\n", (long long)st->st_size,
           (long long)st->st_blocks, (long)st->st_blksize, sb_file_type_name(st->st_mode, st->st_size));
    char dev[32];
    snprintf(dev, sizeof dev, "%llxh/%llud", (unsigned long long)st->st_dev, (unsigned long long)st->st_dev);
    printf("  设备: %-12s Inode: %-10llu 硬链接: %lu", dev, (unsigned long long)st->st_ino,
           (unsigned long)st->st_nlink);
    if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
        printf("   设备类型: %u,%u", major(st->st_rdev), minor(st->st_rdev));
    putchar('\n');
    sb_mode_string(st->st_mode, mode);
    printf("  权限: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n", (unsigned)(st->st_mode & 07777), mode,
           (unsigned)st->st_uid, sk_user_name(st->st_uid, ub, sizeof ub),
           (unsigned)st->st_gid, sk_group_name(st->st_gid, gb, sizeof gb));
    /* atime：最后读取；mtime：最后修改内容；ctime：最后修改 inode（权限、属主、链接数……） */
    time_str(t, sizeof t, &st->st_atim);
    printf("最近访问: %s\n", t);
    time_str(t, sizeof t, &st->st_mtim);
    printf("最近修改: %s\n", t);
    time_str(t, sizeof t, &st->st_ctim);
    printf("状态改变: %s\n", t);
}

static void print_fs_default(const char *path, const struct statfs *sf)
{
    char id[40];
    snprintf(id, sizeof id, "%x%08x", (unsigned)sf->f_fsid.__val[0], (unsigned)sf->f_fsid.__val[1]);
    printf("  文件: \"%s\"\n", path);
    printf("    ID: %-16s 文件名长度上限: %-7ld 类型: %s\n", id, (long)sf->f_namelen,
           fs_type_name((unsigned long)sf->f_type));
    printf("块大小: %-10ld 基本块大小: %ld\n", (long)sf->f_bsize,
           (long)(sf->f_frsize ? sf->f_frsize : sf->f_bsize));
    printf("    块: 总计: %-10llu 空闲: %-10llu 可用: %llu\n", (unsigned long long)sf->f_blocks,
           (unsigned long long)sf->f_bfree, (unsigned long long)sf->f_bavail);
    printf("Inode: 总计: %-10llu 空闲: %llu\n", (unsigned long long)sf->f_files,
           (unsigned long long)sf->f_ffree);
}

int applet_stat(int argc, char **argv)
{
    static const struct option lo[] = {
        { "printf", required_argument, NULL, 'P' },
        { "format", required_argument, NULL, 'c' },
        { "dereference", no_argument, NULL, 'L' },
        { "file-system", no_argument, NULL, 'f' },
        { 0, 0, 0, 0 },
    };
    int deref = 0, fs = 0, escapes = 0, c;
    const char *fmt = NULL;
    while ((c = sb_getopt_long(argc, argv, "Lfc:", lo, usage)) != -1) {
        switch (c) {
        case 'L': deref = 1; break;
        case 'f': fs = 1; break;
        case 'c': fmt = optarg; escapes = 0; break;
        case 'P': fmt = optarg; escapes = 1; break;
        default:  return 1;
        }
    }
    if (optind >= argc) {
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        const char *path = argv[i];
        if (fs) {
            struct statfs sf;
            if (statfs(path, &sf) != 0) {
                sb_perr("无法读取 '%s' 所在文件系统的信息", path);
                rc = 1;
                continue;
            }
            if (fmt) {
                run_format(fmt, path, NULL, &sf, escapes);
                if (!escapes)
                    putchar('\n');
            } else {
                print_fs_default(path, &sf);
            }
            continue;
        }
        struct stat st;
        if ((deref ? stat(path, &st) : lstat(path, &st)) != 0) {
            sb_perr("无法获取 '%s' 的文件信息", path);
            rc = 1;
            continue;
        }
        if (fmt) {
            run_format(fmt, path, &st, NULL, escapes);
            if (!escapes)
                putchar('\n');
        } else {
            print_default(path, &st);
        }
    }
    return rc;
}
