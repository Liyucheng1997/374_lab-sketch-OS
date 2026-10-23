/*
 * lib_proc.c — 进程信息（ps、pstree、top、killall、lsof 共用）
 *
 * 内核为每个进程在 /proc 下建一个目录 /proc/<pid>/：
 *   stat     一行数字：状态、父进程、进程组、会话、终端、CPU 时间、内存……
 *   cmdline  启动参数（用 NUL 分隔）
 *   fd/      打开的文件描述符（每个都是符号链接，指向真正打开的文件）
 * 这些“文件”并不在磁盘上，每次读的时候内核现场生成内容。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>

int sb_proc_extra(sb_proc *p)
{
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)p->info.pid);
    if (sk_read_line(path, buf, sizeof buf) != 0)
        return -1;
    /* 进程名在括号里，而且可能含空格和括号，所以从最后一个 ')' 之后开始解析 */
    char *rp = strrchr(buf, ')');
    if (!rp || !rp[1])
        return -1;
    char state;
    int ppid, pgrp, session, tty, tpgid;
    if (sscanf(rp + 2, "%c %d %d %d %d %d", &state, &ppid, &pgrp, &session, &tty, &tpgid) != 6)
        return -1;
    p->pgrp = pgrp;
    p->session = session;
    p->tty_nr = tty;
    p->tpgid = tpgid;

    /* /proc/<pid> 目录的属主就是进程的有效用户 */
    struct stat st;
    snprintf(path, sizeof path, "/proc/%d", (int)p->info.pid);
    p->uid = stat(path, &st) == 0 ? st.st_uid : 0;
    return 0;
}

sb_proc *sb_list_procs(int *count)
{
    int n = 0;
    sk_procinfo *base = sk_list_procs(&n);
    *count = 0;
    if (!base)
        return NULL;
    sb_proc *arr = calloc((size_t)(n ? n : 1), sizeof *arr);
    if (!arr) {
        free(base);
        return NULL;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        arr[k].info = base[i];
        /* 进程可能刚好退出了，这时丢掉它 */
        if (sb_proc_extra(&arr[k]) == 0)
            k++;
    }
    free(base);
    *count = k;
    return arr;
}

void sb_tty_name(int tty_nr, char *buf, size_t n)
{
    if (tty_nr == 0) {
        sk_strlcpy(buf, "?", n);
        return;
    }
    /* 设备号 = 主设备号（哪个驱动）+ 次设备号（驱动管理的第几个设备） */
    unsigned maj = major((dev_t)tty_nr), min = minor((dev_t)tty_nr);
    if (maj == 4 && min < 64)
        snprintf(buf, n, "tty%u", min);            /* 虚拟控制台 */
    else if (maj == 4)
        snprintf(buf, n, "ttyS%u", min - 64);      /* 串口 */
    else if (maj >= 136 && maj <= 143)
        snprintf(buf, n, "pts/%u", (maj - 136) * 256 + min);   /* 伪终端（skterm 用） */
    else if (maj == 5 && min == 1)
        sk_strlcpy(buf, "console", n);
    else if (maj == 5 && min == 0)
        sk_strlcpy(buf, "tty", n);
    else
        snprintf(buf, n, "%u,%u", maj, min);
}

const char *sb_proc_cmd(const sk_procinfo *p, char *buf, size_t n)
{
    if (p->state == 'Z')
        snprintf(buf, n, "[%s] <defunct>", p->comm);
    else if (p->is_kernel_thread || !p->cmdline[0])
        snprintf(buf, n, "[%s]", p->comm);   /* 内核线程没有命令行，用方括号和普通进程区分 */
    else
        sk_strlcpy(buf, p->cmdline, n);
    return buf;
}

long long sb_boot_time(void)
{
    char *s = sk_read_file("/proc/stat", NULL);
    if (!s)
        return 0;
    long long bt = 0;
    char *p = strstr(s, "\nbtime ");
    if (p)
        bt = strtoll(p + 7, NULL, 10);
    free(s);
    return bt;
}
