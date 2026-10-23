/*
 * minish.c — 测试用的“替身” shell（只用于 sinit 开机测试）
 *
 * 真正的 sksh 由别的同学在并行开发。为了让 sinit 的测试不依赖它，这里提供一个极简 shell：
 * 读一行，按 ';' 切成几条命令，每条用空白切分（支持引号），内建 echo/cd/exit，
 * 其它命令 fork + execve。足够配合 tools/qemu_harness.py 的 run_serial() 使用。
 * 提示符以 "# " 结尾，和真正的 sksh 一致。
 */
#define _GNU_SOURCE
#include "skutil.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int run(char **argv)
{
    if (!strcmp(argv[0], "echo")) {
        for (int i = 1; argv[i]; i++)
            printf("%s%s", i > 1 ? " " : "", argv[i]);
        printf("\n");
        fflush(stdout);
        return 0;
    }
    if (!strcmp(argv[0], "cd"))
        return chdir(argv[1] ? argv[1] : "/root") == 0 ? 0 : 1;
    if (!strcmp(argv[0], "exit"))
        exit(argv[1] ? atoi(argv[1]) : 0);

    char path[256];
    if (strchr(argv[0], '/'))
        sk_strlcpy(path, argv[0], sizeof path);
    else {
        snprintf(path, sizeof path, "/bin/%s", argv[0]);
        if (access(path, X_OK) != 0)
            snprintf(path, sizeof path, "/sbin/%s", argv[0]);
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        execve(path, argv, environ);
        fprintf(stderr, "minish: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (pid < 0)
        return 1;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int main(int argc, char **argv)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "-l"))
        printf("minish：sinit 测试用的替身 shell (pid %d)\n", (int)getpid());
    char line[2048];
    for (;;) {
        char cwd[256] = "?";
        if (!getcwd(cwd, sizeof cwd))
            sk_strlcpy(cwd, "?", sizeof cwd);
        printf("root@sketchos:%s# ", cwd);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            return 0;
        line[strcspn(line, "\r\n")] = 0;
        /* 按 ';' 切分（引号里的分号不算） */
        char *seg = line;
        for (;;) {
            char *p = seg, q = 0;
            for (; *p; p++) {
                if (q) {
                    if (*p == q)
                        q = 0;
                } else if (*p == '\'' || *p == '"') {
                    q = *p;
                } else if (*p == ';') {
                    break;
                }
            }
            int last = !*p;
            *p = 0;
            char *av[64];
            int ac = sk_split_args(seg, av, 63);
            av[ac] = NULL;
            if (ac > 0)
                run(av);
            if (last)
                break;
            seg = p + 1;
        }
    }
}
