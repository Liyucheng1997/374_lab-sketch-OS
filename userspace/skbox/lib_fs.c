/*
 * lib_fs.c — 递归复制与递归删除（cp、mv、rm 共用）
 *
 * 文件系统的几个关键概念：
 *   - 目录只是“名字 → inode 编号”的表。文件的内容和属性（权限、大小、时间）存在 inode 里。
 *   - 硬链接 = 同一个 inode 的多个名字；符号链接 = 一个内容是“另一个路径”的小文件。
 *   - 所以“复制”要新建 inode 并把内容读出来再写进去；而同一文件系统内的“移动”
 *     只需要改目录表（rename 系统调用），一瞬间完成，不管文件多大。
 */
#include "skbox.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static mode_t current_umask(void)
{
    /* umask 只能“设置并返回旧值”，所以设一下再改回去 */
    mode_t m = umask(0);
    umask(m);
    return m;
}

static void preserve_attrs(const char *dst, const struct stat *st, int is_link)
{
    /* 保留属主：普通用户无权把文件送给别人，失败就算了（GNU cp 也是这样） */
    if (lchown(dst, st->st_uid, st->st_gid) != 0 && errno != EPERM)
        sb_perr("无法保留 '%s' 的属主", dst);
    if (!is_link && chmod(dst, st->st_mode & 07777) != 0)
        sb_perr("无法保留 '%s' 的权限", dst);
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    if (utimensat(AT_FDCWD, dst, ts, AT_SYMLINK_NOFOLLOW) != 0 && !is_link)
        sb_perr("无法保留 '%s' 的时间", dst);
}

static int copy_data(const char *src, const char *dst, const struct stat *st, const sb_copy_opts *o)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        sb_perr("无法打开 '%s'", src);
        return -1;
    }
    /* 新文件的权限 = 源文件权限再被 umask 过滤（内核在 open 时自动做） */
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st->st_mode & 0777);
    if (out < 0 && o->force && errno != ENOENT) {
        unlink(dst);
        out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st->st_mode & 0777);
    }
    if (out < 0) {
        sb_perr("无法创建 '%s'", dst);
        close(in);
        return -1;
    }
    int rc = 0;
    if (sb_copy_fd(in, out) != 0) {
        sb_perr("复制 '%s' 到 '%s' 时出错", src, dst);
        rc = -1;
    }
    close(in);
    if (close(out) != 0 && rc == 0) {
        sb_perr("写入 '%s' 失败", dst);
        rc = -1;
    }
    return rc;
}

/* dst 是不是在 src 里面（防止 cp -r a a/b 无限复制下去） */
static int is_inside(const char *src, const char *dst)
{
    char rs[PATH_MAX], rd[PATH_MAX], parent[PATH_MAX];
    if (!realpath(src, rs))
        return 0;
    /* dst 可能还不存在，取它的父目录 */
    sk_strlcpy(parent, dst, sizeof parent);
    char *slash = strrchr(parent, '/');
    const char *last = dst;
    if (slash) {
        last = dst + (slash - parent) + 1;
        if (slash == parent)
            parent[1] = 0;
        else
            *slash = 0;
    } else {
        sk_strlcpy(parent, ".", sizeof parent);
    }
    if (!realpath(parent, rd))
        return 0;
    sk_strlcat(rd, "/", sizeof rd);
    sk_strlcat(rd, last, sizeof rd);
    sk_path_normalize(rd);
    size_t n = strlen(rs);
    return strncmp(rs, rd, n) == 0 && (rd[n] == '/' || rd[n] == 0);
}

static int copy_rec(const char *src, const char *dst, const sb_copy_opts *o, int top)
{
    struct stat st, dst_st;
    int follow = top ? (o->deref_args || !o->no_deref) : !o->no_deref;
    if ((follow ? stat(src, &st) : lstat(src, &st)) != 0) {
        sb_perr("无法获取 '%s' 的文件信息", src);
        return -1;
    }
    int dst_exists = lstat(dst, &dst_st) == 0;
    if (dst_exists && st.st_dev == dst_st.st_dev && st.st_ino == dst_st.st_ino) {
        sb_err("'%s' 和 '%s' 是同一个文件", src, dst);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!o->recursive) {
            sb_err("未指定 -r，略过目录 '%s'", src);
            return -1;
        }
        if (dst_exists && !S_ISDIR(dst_st.st_mode)) {
            sb_err("无法用目录 '%s' 覆盖非目录 '%s'", src, dst);
            return -1;
        }
        if (top && is_inside(src, dst)) {
            sb_err("无法把目录 '%s' 复制到它自己里面 '%s'", src, dst);
            return -1;
        }
        int created = 0;
        if (!dst_exists) {
            /* 先给自己留出写权限，好往里面放文件；最后再改成正确的权限 */
            if (mkdir(dst, (st.st_mode & 07777) | S_IRWXU) != 0) {
                sb_perr("无法创建目录 '%s'", dst);
                return -1;
            }
            created = 1;
        }
        if (o->verbose)
            printf("'%s' -> '%s'\n", src, dst);
        DIR *d = opendir(src);
        if (!d) {
            sb_perr("无法打开目录 '%s'", src);
            return -1;
        }
        int rc = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char *cs = sk_strdup_printf("%s/%s", src, e->d_name);
            char *cd = sk_strdup_printf("%s/%s", dst, e->d_name);
            if (!cs || !cd) {
                sb_err("内存不足");
                rc = -1;
            } else if (copy_rec(cs, cd, o, 0) != 0) {
                rc = -1;
            }
            free(cs);
            free(cd);
        }
        closedir(d);
        if (o->preserve)
            preserve_attrs(dst, &st, 0);
        else if (created && chmod(dst, st.st_mode & 07777 & ~current_umask()) != 0)
            sb_perr("无法设置 '%s' 的权限", dst);
        return rc;
    }

    if (dst_exists && o->no_clobber)
        return 0;
    if (dst_exists && S_ISDIR(dst_st.st_mode)) {
        sb_err("无法用非目录 '%s' 覆盖目录 '%s'", src, dst);
        return -1;
    }
    if (dst_exists && o->interactive) {
        fprintf(stderr, "%s: 是否覆盖 '%s'？", sb_applet, dst);
        if (!sb_ask_yes())
            return 0;
    }

    int rc = 0;
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) {
            sb_perr("无法读取符号链接 '%s'", src);
            return -1;
        }
        target[n] = 0;
        if (dst_exists)
            unlink(dst);
        if (symlink(target, dst) != 0) {
            sb_perr("无法创建符号链接 '%s'", dst);
            return -1;
        }
        if (o->preserve)
            preserve_attrs(dst, &st, 1);
    } else if (S_ISREG(st.st_mode) || !o->recursive) {
        /* 不递归时，设备文件也按“读出内容”处理：cp /dev/null x 得到一个空文件 */
        rc = copy_data(src, dst, &st, o);
        if (rc == 0 && o->preserve)
            preserve_attrs(dst, &st, 0);
    } else {
        /* 递归复制时，管道和设备文件复制“文件本身”（新建同样的设备节点），而不是读它的内容 */
        if (dst_exists)
            unlink(dst);
        int r = S_ISFIFO(st.st_mode) ? mkfifo(dst, st.st_mode & 07777)
                                      : mknod(dst, st.st_mode, st.st_rdev);
        if (r != 0) {
            sb_perr("无法创建特殊文件 '%s'", dst);
            return -1;
        }
        if (o->preserve)
            preserve_attrs(dst, &st, 0);
    }
    if (rc == 0 && o->verbose)
        printf("'%s' -> '%s'\n", src, dst);
    return rc;
}

int sb_copy_path(const char *src, const char *dst, const sb_copy_opts *o)
{
    return copy_rec(src, dst, o, 1);
}

/* ---------- 递归删除 ----------
 * 用 openat/unlinkat 这类 “*at” 系统调用：相对于一个已打开的目录来操作。
 * 好处：不需要拼出很长的完整路径；而且在删除过程中，就算有人把某个目录换成了符号链接，
 * 我们也不会顺着链接跑到别处去乱删（O_NOFOLLOW 保证）。 */

typedef struct {
    int force, interactive, verbose;
} rm_opts;

static int ask(const char *what, const char *path)
{
    fprintf(stderr, "%s: 是否%s '%s'？", sb_applet, what, path);
    return sb_ask_yes();
}

static int rm_at(int dirfd, const char *name, const char *path, int recursive, const rm_opts *o)
{
    struct stat st;
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (o->force && errno == ENOENT)
            return 0;
        sb_perr("无法删除 '%s'", path);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (o->interactive) {
            char what[64];
            snprintf(what, sizeof what, "删除%s", sb_file_type_name(st.st_mode, st.st_size));
            if (!ask(what, path))
                return 0;
        }
        if (unlinkat(dirfd, name, 0) != 0) {
            sb_perr("无法删除 '%s'", path);
            return -1;
        }
        if (o->verbose)
            printf("已删除 '%s'\n", path);
        return 0;
    }

    if (!recursive) {
        sb_err("无法删除 '%s': 是一个目录", path);
        return -1;
    }
    if (o->interactive && !ask("进入目录", path))
        return 0;

    int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        sb_perr("无法打开目录 '%s'", path);
        return -1;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        sb_perr("无法读取目录 '%s'", path);
        close(fd);
        return -1;
    }
    /* 先把名字都读出来再删：一边遍历一边删除，readdir 的位置可能会乱 */
    char **names = NULL;
    size_t cnt = 0, cap = 0;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 32;
            char **nn = realloc(names, cap * sizeof *nn);
            if (!nn) {
                rc = -1;
                break;
            }
            names = nn;
        }
        names[cnt] = strdup(e->d_name);
        if (names[cnt])
            cnt++;
    }
    for (size_t i = 0; i < cnt; i++) {
        char *child = sk_strdup_printf("%s%s%s", path,
                                       path[strlen(path) - 1] == '/' ? "" : "/", names[i]);
        if (rm_at(fd, names[i], child ? child : names[i], 1, o) != 0)
            rc = -1;
        free(child);
        free(names[i]);
    }
    free(names);
    closedir(d);   /* 同时关闭了 fd */

    if (rc != 0)
        return rc;   /* 里面有东西没删掉，目录本身肯定也删不掉，不再重复报错 */
    if (o->interactive && !ask("删除目录", path))
        return 0;
    if (unlinkat(dirfd, name, AT_REMOVEDIR) != 0) {
        sb_perr("无法删除目录 '%s'", path);
        return -1;
    }
    if (o->verbose)
        printf("已删除目录 '%s'\n", path);
    return 0;
}

int sb_remove_path(const char *path, int recursive, int force, int interactive, int verbose)
{
    rm_opts o = { force, interactive, verbose };
    return rm_at(AT_FDCWD, path, path, recursive, &o);
}
