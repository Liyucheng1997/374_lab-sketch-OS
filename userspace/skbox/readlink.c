/*
 * readlink — 显示符号链接指向哪里；-f 时求出完整的真实路径
 *
 * 内核解析路径时，遇到符号链接就把它的内容“替换进去”继续找，最多替换 40 次（防止链接成环）。
 * -f 模式就是在用户态把这个过程模拟一遍：逐段处理 .、..和符号链接，得到不含链接的绝对路径。
 */
#include "skbox.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(readlink, "显示符号链接的目标或文件的真实路径")

static const char usage[] =
    "用法: readlink [选项] 文件...\n"
    "显示符号链接的内容（它指向的路径）。\n\n"
    "  -f    求出规范的绝对路径，逐层展开所有符号链接；除最后一段外都必须存在\n"
    "  -e    同 -f，但每一段都必须存在\n"
    "  -m    同 -f，但各段都可以不存在\n"
    "  -n    不输出结尾的换行\n"
    "  -v    出错时显示错误信息\n";

enum { CANON_NONE, CANON_LAST_MAY_MISS, CANON_ALL_EXIST, CANON_MISSING_OK };

/* 把 path 规范化到 out。成功 0，失败 -1（errno 有效） */
static int canonicalize(const char *path, int mode, char *out, size_t outsz)
{
    /* rest：还没处理的路径；res：已经确定的前缀（绝对路径） */
    char rest[PATH_MAX * 2], res[PATH_MAX], tmp[PATH_MAX * 2];
    if (path[0] == '/') {
        sk_strlcpy(rest, path, sizeof rest);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd))
            return -1;
        snprintf(rest, sizeof rest, "%s/%s", cwd, path);
    }
    strcpy(res, "/");
    int links = 0, missing = 0;
    char *p = rest;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        int is_last = 1;
        for (char *q = p; *q; q++)
            if (*q != '/') {
                is_last = 0;
                break;
            }
        if (len == 1 && s[0] == '.')
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            char *slash = strrchr(res, '/');
            if (slash == res)
                res[1] = 0;
            else if (slash)
                *slash = 0;
            continue;
        }
        size_t rl = strlen(res);
        if (rl + len + 2 > sizeof res) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (rl > 1)
            res[rl++] = '/';
        memcpy(res + rl, s, len);
        res[rl + len] = 0;
        if (missing)
            continue;   /* 已经有一段不存在了，后面的只做文字上的处理 */

        struct stat st;
        if (lstat(res, &st) != 0) {
            if (errno == ENOENT && (mode == CANON_MISSING_OK || (mode == CANON_LAST_MAY_MISS && is_last))) {
                missing = 1;
                continue;
            }
            return -1;
        }
        if (S_ISLNK(st.st_mode)) {
            if (++links > 40) {
                errno = ELOOP;
                return -1;
            }
            char target[PATH_MAX];
            ssize_t n = readlink(res, target, sizeof target - 1);
            if (n < 0)
                return -1;
            target[n] = 0;
            /* 把链接内容接到剩余路径前面，重新开始处理 */
            size_t tl = (size_t)n, pl = strlen(p);
            if (tl + pl + 1 > sizeof tmp) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(tmp, target, tl);
            memcpy(tmp + tl, p, pl + 1);
            memcpy(rest, tmp, tl + pl + 1);
            p = rest;
            if (target[0] == '/') {
                strcpy(res, "/");
            } else {
                char *slash = strrchr(res, '/');
                if (slash == res)
                    res[1] = 0;
                else if (slash)
                    *slash = 0;
            }
            continue;
        }
        if (!S_ISDIR(st.st_mode) && !is_last) {
            if (mode == CANON_MISSING_OK) {
                missing = 1;
                continue;
            }
            errno = ENOTDIR;
            return -1;
        }
    }
    sk_strlcpy(out, res, outsz);
    return 0;
}

int applet_readlink(int argc, char **argv)
{
    int mode = CANON_NONE, no_newline = 0, verbose = 0, c;
    while ((c = sb_getopt(argc, argv, "femnvqs", usage)) != -1) {
        switch (c) {
        case 'f': mode = CANON_LAST_MAY_MISS; break;
        case 'e': mode = CANON_ALL_EXIST; break;
        case 'm': mode = CANON_MISSING_OK; break;
        case 'n': no_newline = 1; break;
        case 'v': verbose = 1; break;
        case 'q':
        case 's': verbose = 0; break;
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
        char out[PATH_MAX];
        if (mode == CANON_NONE) {
            ssize_t n = readlink(argv[i], out, sizeof out - 1);
            if (n < 0) {
                if (verbose)
                    sb_perr("%s", argv[i]);
                rc = 1;
                continue;
            }
            out[n] = 0;
        } else if (canonicalize(argv[i], mode, out, sizeof out) != 0) {
            if (verbose)
                sb_perr("%s", argv[i]);
            rc = 1;
            continue;
        }
        fputs(out, stdout);
        if (!no_newline || argc - optind > 1)
            putchar('\n');
    }
    return rc;
}
