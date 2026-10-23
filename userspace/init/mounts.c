/*
 * mounts.c — 挂载虚拟文件系统
 *
 * 在 Linux 里，“文件系统”不一定在磁盘上。下面这些都是内核在内存里现场生成的：
 *   proc      /proc      进程和内核的状态（ps、top、free 都是读它）
 *   sysfs     /sys       设备、驱动、内核对象组成的树
 *   devtmpfs  /dev       内核为每个检测到的设备自动创建设备文件（/dev/tty1、/dev/fb0、/dev/input/…）
 *   devpts    /dev/pts   伪终端：图形终端 skterm 里的 shell 就运行在 /dev/pts/N 上
 *   tmpfs     /tmp /run /dev/shm   放在内存里的普通文件系统，关机后内容就没了
 * 内核只准备好根文件系统（这里是 initramfs 解压出来的内存根目录），
 * 其它文件系统都要 1 号进程自己挂载——这就是“用户态的第一件事”。
 *
 * 挂载选项里的 MS_NOSUID / MS_NODEV / MS_NOEXEC 是安全习惯：
 * 例如 /tmp 谁都能写，就不应该允许在里面放“设备文件”(nodev) 或 setuid 程序 (nosuid)。
 */
#define _GNU_SOURCE
#include "sinit.h"
#include "skutil.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char   *source;
    const char   *target;
    const char   *fstype;
    unsigned long flags;
    const char   *data;     /* 文件系统自己的选项 */
    mode_t        mode;     /* 挂载点不存在时用这个权限创建 */
} mount_entry;

/* 顺序很重要：/dev/pts、/dev/shm 要在 /dev 挂好之后才能创建 */
static const mount_entry k_mounts[] = {
    { "proc",     "/proc",    "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL,        0555 },
    { "sysfs",    "/sys",     "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL,        0555 },
    { "devtmpfs", "/dev",     "devtmpfs", MS_NOSUID,                        "mode=0755", 0755 },
    /* ptmxmode=666：任何人都能打开 /dev/pts/ptmx 申请新的伪终端；
     * mode=620,gid=5：新建的 /dev/pts/N 只有主人可读写、tty 组可写（write/wall 用） */
    { "devpts",   "/dev/pts", "devpts",   MS_NOSUID | MS_NOEXEC,
      "gid=5,mode=620,ptmxmode=666", 0755 },
    /* /dev/shm：POSIX 共享内存 (shm_open) 的家 */
    { "tmpfs",    "/dev/shm", "tmpfs",    MS_NOSUID | MS_NODEV,             "mode=1777", 01777 },
    /* 1777 里的 1 是“粘滞位”：人人都能在 /tmp 建文件，但只能删自己的 */
    { "tmpfs",    "/tmp",     "tmpfs",    MS_NOSUID | MS_NODEV,             "mode=1777", 01777 },
    /* /run：运行时状态（套接字、pid 文件、/run/sinit/status），每次开机都是空的 */
    { "tmpfs",    "/run",     "tmpfs",    MS_NOSUID | MS_NODEV,             "mode=0755", 0755 },
};

/* 挂载阶段日志系统还没准备好（/dev/kmsg 要等 devtmpfs 挂上才有），先把问题记下来 */
static char g_problems[8][160];
static int  g_nproblems;

static void problem(const char *fmt, const char *a, const char *b)
{
    if (g_nproblems < (int)SK_ARRAY_LEN(g_problems))
        snprintf(g_problems[g_nproblems++], sizeof g_problems[0], fmt, a, b);
}

/* 判断 path 是不是一个挂载点：如果它和它的父目录不在同一个文件系统 (st_dev 不同)，
 * 就说明有别的文件系统挂在这里——这正是 mountpoint 命令的原理。 */
static int is_mountpoint(const char *path)
{
    struct stat st, parent;
    char up[SVC_PATH_MAX];
    snprintf(up, sizeof up, "%s/..", path);
    if (stat(path, &st) != 0 || stat(up, &parent) != 0)
        return 0;
    return st.st_dev != parent.st_dev;
}

int mount_early(void)
{
    int failed = 0;
    for (size_t i = 0; i < SK_ARRAY_LEN(k_mounts); i++) {
        const mount_entry *m = &k_mounts[i];
        if (mkdir(m->target, m->mode) != 0 && errno != EEXIST) {
            problem("无法创建挂载点 %s: %s", m->target, sk_strerror(errno));
            failed++;
            continue;
        }
        if (is_mountpoint(m->target))
            continue;                        /* 已经挂过了（比如内核自动挂了 /dev） */
        if (mount(m->source, m->target, m->fstype, m->flags, m->data) != 0) {
            if (errno == EBUSY)              /* 同一个文件系统已经挂在这里了 */
                continue;
            problem("挂载 %s 失败: %s", m->target, sk_strerror(errno));
            failed++;
        }
    }

    /* 运行时目录 */
    static const char *const dirs[] = { "/run/sketch", SINIT_STATUS_DIR, "/var/log" };
    for (size_t i = 0; i < SK_ARRAY_LEN(dirs); i++)
        if (sk_mkdir_p(dirs[i], 0755) != 0)
            problem("无法创建目录 %s: %s", dirs[i], sk_strerror(errno));

    /* /dev/fd、/dev/stdin……这些是“约定俗成”的符号链接（devtmpfs 不会创建它们）。
     * /proc/self 永远指向“正在访问它的那个进程”，所以 /dev/stdin 对每个进程
     * 都指向它自己的 0 号文件描述符。 */
    static const char *const links[][2] = {
        { "/proc/self/fd",   "/dev/fd" },
        { "/proc/self/fd/0", "/dev/stdin" },
        { "/proc/self/fd/1", "/dev/stdout" },
        { "/proc/self/fd/2", "/dev/stderr" },
    };
    for (size_t i = 0; i < SK_ARRAY_LEN(links); i++)
        if (symlink(links[i][0], links[i][1]) != 0 && errno != EEXIST)
            problem("无法创建符号链接 %s: %s", links[i][1], sk_strerror(errno));
    return failed;
}

void mount_early_report(void)
{
    for (int i = 0; i < g_nproblems; i++)
        sk_log_console("%s", g_problems[i]);
    g_nproblems = 0;
}

void mount_teardown(void)
{
    /* initramfs 的内容本来就在内存里，卸载并不会“保存”什么；
     * 但按顺序卸载是好习惯：真正的磁盘文件系统必须卸载（或只读重挂）后才能断电，
     * 否则缓存里还没写回磁盘的数据就丢了。倒序卸载，先卸子目录上的。 */
    static const char *const targets[] = { "/run", "/tmp", "/dev/shm", "/dev/pts" };
    for (size_t i = 0; i < SK_ARRAY_LEN(targets); i++) {
        if (!is_mountpoint(targets[i]))
            continue;
        if (umount2(targets[i], 0) == 0)
            continue;
        /* 还有进程开着里面的文件 (EBUSY)：用“懒卸载”，从目录树上摘掉，等没人用时再真正释放 */
        if (umount2(targets[i], MNT_DETACH) != 0)
            sk_log("卸载 %s 失败: %s", targets[i], sk_strerror(errno));
    }
}
