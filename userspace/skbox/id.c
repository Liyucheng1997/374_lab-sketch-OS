/*
 * id — 显示用户和组的身份
 *
 * 每个进程有几组身份编号：
 *   实际用户 uid（是谁启动了我）、有效用户 euid（权限检查用哪个身份）、
 *   主组 gid，以及附加组列表（一个用户可以同时属于多个组）。
 * 内核做权限检查时只看这些数字。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(id, "显示用户和组的编号")

static const char usage[] =
    "用法: id [选项] [用户名]\n"
    "显示用户的 uid、主组 gid 和所属的全部组。不给用户名时显示当前进程的身份。\n\n"
    "  -u    只显示用户 id\n"
    "  -g    只显示主组 id\n"
    "  -G    显示所有组 id\n"
    "  -n    显示名字而不是数字（配合 -u -g -G）\n"
    "  -r    显示实际 id 而不是有效 id（配合 -u -g -G）\n";

/* 在 /etc/passwd 里按名字查找用户：name:x:uid:gid:... */
static int lookup_user(const char *name, uid_t *uid, gid_t *gid)
{
    char *s = sk_read_file("/etc/passwd", NULL);
    int found = 0;
    if (s) {
        size_t nl = strlen(name);
        for (char *line = s; line && *line;) {
            char *end = strchr(line, '\n');
            if (end)
                *end = 0;
            if (strncmp(line, name, nl) == 0 && line[nl] == ':') {
                char *f = strchr(line + nl + 1, ':');
                if (f) {
                    char *g;
                    *uid = (uid_t)strtoul(f + 1, &g, 10);
                    if (*g == ':') {
                        *gid = (gid_t)strtoul(g + 1, NULL, 10);
                        found = 1;
                        break;
                    }
                }
            }
            line = end ? end + 1 : NULL;
        }
        free(s);
    }
    if (!found && strcmp(name, "root") == 0) {
        *uid = 0;
        *gid = 0;
        found = 1;
    }
    return found;
}

/* /etc/group 每行：组名:x:gid:成员1,成员2 —— 找出所有列了这个用户的组 */
static int groups_of_user(const char *name, gid_t primary, gid_t *out, int max)
{
    int n = 0;
    out[n++] = primary;
    char *s = sk_read_file("/etc/group", NULL);
    if (!s)
        return n;
    for (char *line = s; line && *line && n < max;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        char *f1 = strchr(line, ':');
        char *f2 = f1 ? strchr(f1 + 1, ':') : NULL;
        char *f3 = f2 ? strchr(f2 + 1, ':') : NULL;
        if (f3) {
            gid_t g = (gid_t)strtoul(f2 + 1, NULL, 10);
            char *save = NULL;
            for (char *m = strtok_r(f3 + 1, ",", &save); m; m = strtok_r(NULL, ",", &save)) {
                if (strcmp(m, name) == 0 && g != primary) {
                    out[n++] = g;
                    break;
                }
            }
        }
        line = end ? end + 1 : NULL;
    }
    free(s);
    return n;
}

int applet_id(int argc, char **argv)
{
    char mode = 0;
    int names = 0, real = 0, c;
    while ((c = sb_getopt(argc, argv, "ugGnr", usage)) != -1) {
        switch (c) {
        case 'u':
        case 'g':
        case 'G':
            if (mode && mode != c) {
                sb_err("只能选择 -u、-g、-G 中的一个");
                return 1;
            }
            mode = (char)c;
            break;
        case 'n': names = 1; break;
        case 'r': real = 1; break;
        default:  return 1;
        }
    }
    if ((names || real) && !mode) {
        sb_err("-n 和 -r 只能和 -u、-g 或 -G 一起使用");
        return 1;
    }

    uid_t uid, euid;
    gid_t gid, egid;
    gid_t groups[64];
    int ngroups;
    if (optind < argc) {
        if (!lookup_user(argv[optind], &uid, &gid)) {
            sb_err("'%s': 没有这个用户", argv[optind]);
            return 1;
        }
        euid = uid;
        egid = gid;
        ngroups = groups_of_user(argv[optind], gid, groups, 64);
    } else {
        uid = getuid();
        euid = geteuid();
        gid = getgid();
        egid = getegid();
        /* 附加组列表由内核保存在进程凭证里，getgroups() 取出来；主组不一定在里面 */
        groups[0] = egid;
        int k = getgroups(63, groups + 1);
        ngroups = 1;
        for (int i = 1; i <= k; i++)
            if (groups[i] != egid)
                groups[ngroups++] = groups[i];
    }

    char nb[64];
    if (mode == 'u' || mode == 'g') {
        unsigned v = mode == 'u' ? (unsigned)(real ? uid : euid) : (unsigned)(real ? gid : egid);
        if (names)
            puts(mode == 'u' ? sk_user_name((uid_t)v, nb, sizeof nb) : sk_group_name((gid_t)v, nb, sizeof nb));
        else
            printf("%u\n", v);
        return 0;
    }
    if (mode == 'G') {
        for (int i = 0; i < ngroups; i++) {
            if (i)
                putchar(' ');
            if (names)
                fputs(sk_group_name(groups[i], nb, sizeof nb), stdout);
            else
                printf("%u", (unsigned)groups[i]);
        }
        putchar('\n');
        return 0;
    }
    printf("用户id=%u(%s)", (unsigned)uid, sk_user_name(uid, nb, sizeof nb));
    printf(" 组id=%u(%s)", (unsigned)gid, sk_group_name(gid, nb, sizeof nb));
    if (euid != uid)
        printf(" 有效用户id=%u(%s)", (unsigned)euid, sk_user_name(euid, nb, sizeof nb));
    if (egid != gid)
        printf(" 有效组id=%u(%s)", (unsigned)egid, sk_group_name(egid, nb, sizeof nb));
    printf(" 组=");
    for (int i = 0; i < ngroups; i++)
        printf("%s%u(%s)", i ? "," : "", (unsigned)groups[i], sk_group_name(groups[i], nb, sizeof nb));
    putchar('\n');
    return 0;
}
