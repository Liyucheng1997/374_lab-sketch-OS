/*
 * main.c — sksh 的入口：解析命令行参数，决定运行模式，执行启动脚本，进入主循环
 *
 *   sksh                交互式（stdin 和 stderr 都是终端时）
 *   sksh -l / -sksh     登录 shell：先执行 /etc/profile 和 ~/.skshrc，显示 /etc/motd
 *   sksh -c '命令' [名字 参数...]
 *   sksh 脚本.sh [参数...]
 *   命令 | sksh          从标准输入读脚本
 *
 * 脚本第一行写 #!/bin/sh 时，是内核（fs/binfmt_script.c）读到 #! 之后，
 * 把它改写成执行 /bin/sh 脚本.sh 参数...，所以 shell 必须接受“脚本路径”参数。
 */
#include "sksh.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void usage(FILE *out)
{
    fprintf(out,
            "用法: sksh [选项] [脚本 [参数...]]\n"
            "      sksh [选项] -c '命令' [$0 [参数...]]\n"
            "SketchOS 的命令行 shell（版本 %s）\n\n"
            "选项:\n"
            "  -c          执行参数里给出的命令\n"
            "  -i          强制交互模式\n"
            "  -l, --login 作为登录 shell 启动（执行 /etc/profile、~/.skshrc）\n"
            "  -s          从标准输入读命令\n"
            "  -e -u -x -f -v -n -C -m   同 set 的选项（help set）\n"
            "  -o 选项名    同 set -o\n"
            "  --version   显示版本\n"
            "  --help      显示本帮助\n\n"
            "进入 shell 后输入 help 查看内建命令和语法。\n",
            SKSH_VERSION);
}

static int run_set(const char *a, const char *b)
{
    const builtin_def *set = builtin_find("set");
    char *argv[4] = { (char *)"set", (char *)a, (char *)b, NULL };
    return set->fn(b ? 3 : 2, argv);
}

/* 启动时整理几个特殊变量 */
static void init_vars(void)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)getppid());
    var_set("PPID", buf, 0);
    const char *lvl = var_get("SHLVL");
    snprintf(buf, sizeof buf, "%d", lvl ? atoi(lvl) + 1 : 1);
    var_set("SHLVL", buf, V_EXPORT);
    /* 继承来的 PWD 必须真的指向当前目录，否则用 getcwd 的结果 */
    const char *pwd = var_get("PWD");
    struct stat a, b;
    if (!(pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
          a.st_dev == b.st_dev && a.st_ino == b.st_ino)) {
        char *cwd = getcwd(NULL, 0);
        if (cwd) {
            var_set("PWD", cwd, V_EXPORT);
            free(cwd);
        }
    }
    if (!var_get("PATH"))
        var_set("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", V_EXPORT);
    if (!var_get("PS2"))
        var_set("PS2", "> ", 0);
    if (!var_get("PS4"))
        var_set("PS4", "+ ", 0);
    var_set("SKSH_VERSION", SKSH_VERSION, 0);
}

/* 登录时显示一次“今日消息” /etc/motd。
 * 如果 /etc/profile 自己负责显示 motd（SketchOS 的 profile 会按终端选择中文/英文版），
 * 这里就不再重复；~/.hushlogin 存在或者在图形终端里（有 SKETCH_SOCKET）也不显示。 */
static void show_motd(void)
{
    if (var_get("SKSH_MOTD_SHOWN") || var_get("SKETCH_SOCKET"))
        return;             /* 在这个 shell 启动的子登录 shell 里不再重复 */
    var_set("SKSH_MOTD_SHOWN", "1", V_EXPORT);
    char *profile = sk_read_file("/etc/profile", NULL);
    int profile_shows = profile && strstr(profile, "/etc/motd");
    free(profile);
    if (profile_shows)
        return;
    const char *home = var_get("HOME");
    if (home) {
        char *hush = sk_strdup_printf("%s/.hushlogin", home);
        int quiet = access(hush, F_OK) == 0;
        free(hush);
        if (quiet)
            return;
    }
    size_t len;
    char *motd = sk_read_file("/etc/motd", &len);
    if (!motd)
        return;
    fflush(stdout);
    full_write(STDOUT_FILENO, motd, len);
    free(motd);
}

static void source_rc(void)
{
    const char *home = var_get("HOME");
    if (!home || !*home)
        return;
    char *rc = sk_strdup_printf("%s/.skshrc", strcmp(home, "/") == 0 ? "" : home);
    exec_file(rc, 0, NULL, 0);
    free(rc);
}

int main(int argc, char **argv)
{
    /* 让 musl 的 fnmatch/glob 按 UTF-8 处理多字节字符（? 能匹配一个汉字） */
    setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1)
        setlocale(LC_CTYPE, "C.UTF-8");

    sh.rootpid = getpid();
    sh.tty_fd = -1;
    sh.start_time = (long)time(NULL);
    sh.arg0 = argv[0] ? argv[0] : (char *)"sksh";
    srandom((unsigned)time(NULL) ^ (unsigned)getpid());
    vars_init(environ);

    if (argv[0] && argv[0][0] == '-')
        sh.login = 1;       /* 传统约定：login 程序用 "-sh" 作为 argv[0] 启动登录 shell */

    int i = 1, cflag = 0, force_i = 0, read_stdin = 0, want_m = -1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            printf("sksh %s（%s 的命令行 shell）\n", SKSH_VERSION, SKETCHOS_NAME);
            return 0;
        }
        if (strcmp(a, "--login") == 0) {
            sh.login = 1;
            continue;
        }
        if ((a[0] != '-' && a[0] != '+') || !a[1])
            break;
        int on = a[0] == '-';
        for (const char *o = a + 1; *o; o++) {
            switch (*o) {
            case 'c': cflag = on; break;
            case 'i': force_i = on; break;
            case 'l': sh.login = on; break;
            case 's': read_stdin = on; break;
            case 'o':
                if (i + 1 >= argc) {
                    sh_error_noloc("-o: 需要选项名");
                    return 2;
                }
                if (run_set(on ? "-o" : "+o", argv[++i]) != 0)
                    return 2;
                break;
            default: {
                char opt[3] = { a[0], *o, 0 };
                if (*o == 'm') {
                    want_m = on;    /* 作业控制在确定是否交互之后再决定 */
                    break;
                }
                if (run_set(opt, NULL) != 0) {
                    usage(stderr);
                    return 2;
                }
            }
            }
        }
    }

    const char *script = NULL;
    if (cflag) {
        if (i >= argc) {
            sh_error_noloc("-c: 需要一个参数（要执行的命令）");
            return 2;
        }
        sh.cmd_string = argv[i++];
        if (i < argc)
            sh.arg0 = argv[i++];
    } else if (!read_stdin && i < argc) {
        script = argv[i++];
        sh.arg0 = (char *)script;
    }
    pos_set(argc - i, argv + i);

    sh.interactive = force_i || (!cflag && !script && isatty(STDIN_FILENO) && isatty(STDERR_FILENO));
    init_vars();

    if ((sh.interactive && want_m != 0 && isatty(STDIN_FILENO)) || want_m == 1)
        jobs_init();
    if (sh.interactive) {
        signals_init();
        if (!var_get("PS1"))
            var_set("PS1", "\\[\\e[1;32m\\]\\u@\\h\\[\\e[0m\\]:\\[\\e[1;34m\\]\\w\\[\\e[0m\\]\\$ ", 0);
    } else {
        signals_init();
    }

    if (sh.login) {
        exec_file("/etc/profile", 0, NULL, 0);
        source_rc();
        if (sh.interactive)
            show_motd();
    } else if (sh.interactive) {
        source_rc();
    }
    /* 启动脚本可能改了 HOME/HISTFILE，历史文件在它们之后再打开 */
    if (sh.interactive)
        history_init();
    sh.abort = 0;
    sh.script_name = NULL;

    int st;
    if (sh.cmd_string) {
        source *src = source_string(sh.cmd_string, "-c");
        st = exec_source(src, 1);
        source_free(src);
    } else if (script) {
        int fd = open(script, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            int e = errno;
            sh_error_noloc("%s: %s", script, sh_strerror(e));
            return e == ENOENT ? 127 : 126;
        }
        struct stat stt;
        if (fstat(fd, &stt) == 0 && S_ISDIR(stt.st_mode)) {
            sh_error_noloc("%s: %s", script, sh_strerror(EISDIR));
            return 126;
        }
        fd = fd_move_high(fd);
        sh.script_name = script;
        source *src = source_fd(fd, script, 1);
        st = exec_source(src, 1);
        source_free(src);
    } else if (sh.interactive) {
        source *src = source_tty();
        st = exec_source(src, 1);
        source_free(src);
    } else {
        source *src = source_fd(STDIN_FILENO, "stdin", 0);
        st = exec_source(src, 1);
        source_free(src);
    }
    (void)st;
    exit_shell(sh.last_status);
}
