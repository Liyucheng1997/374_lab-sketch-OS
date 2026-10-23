/*
 * lsof — 列出进程打开的文件
 *
 * “文件描述符”(fd) 是进程打开文件后拿到的一个小整数：0 标准输入、1 标准输出、2 标准错误，
 * 之后打开的依次是 3、4……。内核在 /proc/<pid>/fd/ 下为每个 fd 放一个符号链接，指向真正打开的东西：
 * 普通文件、设备、管道（pipe:[编号]）、套接字（socket:[编号]）……
 * 同一个管道两端的编号相同，借此可以看出哪两个进程通过管道相连（比如 sketchd 和它启动的应用）。
 */
#include "skbox.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(lsof, "列出进程打开的文件")

static const char usage[] =
    "用法: lsof [-p 进程号] [文件...]\n"
    "列出每个进程打开的文件：当前目录(cwd)、根目录(rtd)、程序文件(txt) 和所有文件描述符。\n"
    "FD 列的数字后面：r 只读  w 只写  u 读写。\n\n"
    "  -p 进程号   只看这个进程（可以用逗号分隔多个）\n"
    "  文件        只显示打开了这些文件的进程\n";

static char **g_files;     /* 要筛选的文件（规范化后的路径） */
static int g_nfiles;
static pid_t g_only[64];
static int g_nonly;

static const char *type_of(const char *path, const char *target)
{
    if (strncmp(target, "pipe:", 5) == 0)
        return "FIFO";
    if (strncmp(target, "socket:", 7) == 0)
        return "sock";
    if (strncmp(target, "anon_inode:", 11) == 0)
        return "a_inode";
    struct stat st;
    if (stat(path, &st) != 0)
        return "?";
    switch (st.st_mode & S_IFMT) {
    case S_IFREG:  return "REG";
    case S_IFDIR:  return "DIR";
    case S_IFCHR:  return "CHR";
    case S_IFBLK:  return "BLK";
    case S_IFIFO:  return "FIFO";
    case S_IFSOCK: return "sock";
    }
    return "?";
}

static int file_wanted(const char *target)
{
    if (!g_nfiles)
        return 1;
    for (int i = 0; i < g_nfiles; i++)
        if (strcmp(g_files[i], target) == 0)
            return 1;
    return 0;
}

static int pid_wanted(pid_t pid)
{
    if (!g_nonly)
        return 1;
    for (int i = 0; i < g_nonly; i++)
        if (g_only[i] == pid)
            return 1;
    return 0;
}

static void row(const sk_procinfo *p, const char *user, const char *fd, const char *type, const char *name)
{
    printf("%-10.10s %6d %-8.8s %5s %-7s %s\n", p->comm, (int)p->pid, user, fd, type, name);
}

static void show_link(const sk_procinfo *p, const char *user, const char *label, const char *link_name)
{
    char path[64], target[PATH_MAX];
    snprintf(path, sizeof path, "/proc/%d/%s", (int)p->pid, link_name);
    ssize_t n = readlink(path, target, sizeof target - 1);
    if (n < 0)
        return;
    target[n] = 0;
    if (file_wanted(target))
        row(p, user, label, type_of(path, target), target);
}

int applet_lsof(int argc, char **argv)
{
    int c;
    while ((c = sb_getopt(argc, argv, "p:", usage)) != -1) {
        if (c != 'p')
            return 1;
        char *copy = strdup(optarg), *save = NULL;
        for (char *t = strtok_r(copy, ",", &save); t && g_nonly < 64; t = strtok_r(NULL, ",", &save)) {
            long long v;
            if (sb_parse_ll(t, &v) != 0) {
                sb_err("无效的进程号 '%s'", t);
                return 1;
            }
            g_only[g_nonly++] = (pid_t)v;
        }
        free(copy);
    }
    if (optind < argc) {
        g_files = calloc((size_t)(argc - optind), sizeof *g_files);
        for (int i = optind; i < argc && g_files; i++) {
            char real[PATH_MAX];
            if (!realpath(argv[i], real)) {
                sb_perr("%s", argv[i]);
                return 1;
            }
            g_files[g_nfiles++] = strdup(real);
        }
    }

    int n;
    sb_proc *procs = sb_list_procs(&n);
    if (!procs) {
        sb_perr("无法读取 /proc");
        return 1;
    }
    printf("%-10s %6s %-8s %5s %-7s %s\n", "COMMAND", "PID", "USER", "FD", "TYPE", "NAME");
    int found = 0;
    for (int i = 0; i < n; i++) {
        sk_procinfo *p = &procs[i].info;
        if (!pid_wanted(p->pid) || p->is_kernel_thread)
            continue;
        found++;
        char user[64];
        sk_user_name(procs[i].uid, user, sizeof user);
        show_link(p, user, "cwd", "cwd");
        show_link(p, user, "rtd", "root");
        show_link(p, user, "txt", "exe");

        char dirpath[64];
        snprintf(dirpath, sizeof dirpath, "/proc/%d/fd", (int)p->pid);
        DIR *d = opendir(dirpath);
        if (!d)
            continue;   /* 别人的进程（没有权限）或者进程刚退出 */
        /* 按 fd 编号排序输出 */
        int fds[1024], nfd = 0;
        struct dirent *e;
        while ((e = readdir(d)) && nfd < 1024)
            if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
                fds[nfd++] = atoi(e->d_name);
        closedir(d);
        for (int a = 1; a < nfd; a++)
            for (int b = a; b > 0 && fds[b - 1] > fds[b]; b--) {
                int t = fds[b];
                fds[b] = fds[b - 1];
                fds[b - 1] = t;
            }
        for (int k = 0; k < nfd; k++) {
            char path[64], target[PATH_MAX], info[64], fdlabel[16];
            snprintf(path, sizeof path, "/proc/%d/fd/%d", (int)p->pid, fds[k]);
            ssize_t len = readlink(path, target, sizeof target - 1);
            if (len < 0)
                continue;
            target[len] = 0;
            if (!file_wanted(target))
                continue;
            /* fdinfo 里的 flags 是 open() 时的标志（八进制），低两位是读写方式 */
            char mode = ' ';
            snprintf(info, sizeof info, "/proc/%d/fdinfo/%d", (int)p->pid, fds[k]);
            char *fi = sk_read_file(info, NULL);
            if (fi) {
                char *fl = strstr(fi, "flags:");
                if (fl) {
                    unsigned long flags = strtoul(fl + 6, NULL, 8);
                    int acc = (int)(flags & O_ACCMODE);
                    mode = acc == O_RDONLY ? 'r' : acc == O_WRONLY ? 'w' : 'u';
                }
                free(fi);
            }
            snprintf(fdlabel, sizeof fdlabel, "%d%c", fds[k], mode);
            row(p, user, fdlabel, type_of(path, target), target);
        }
    }
    free(procs);
    for (int i = 0; i < g_nfiles; i++)
        free(g_files[i]);
    free(g_files);
    return g_nonly && !found ? 1 : 0;
}
