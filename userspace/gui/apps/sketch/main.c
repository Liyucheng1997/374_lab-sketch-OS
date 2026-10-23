/*
 * sketch — 在命令行里和显示服务器打交道的小工具
 *
 *   sketch launch <应用id> [参数]   让桌面启动一个应用（和双击桌面图标一样）
 *   sketch clip get                 打印剪贴板内容
 *   sketch clip set <文字...>       设置剪贴板（没有文字参数时从标准输入读）
 *   sketch info                     显示屏幕尺寸等信息
 *
 * 它本身不开任何窗口：连上 sketchd 的套接字，发一两个请求就退出。
 * 这说明“显示服务器”只是一个普通进程，任何程序——哪怕是终端里的命令——
 * 只要会说它的协议（sketch/proto.h），就能请它做事。
 */
#define _GNU_SOURCE
#include <sketch/client.h>
#include <sketch/proto.h>
#include "../../libsketch/ui_internal.h"   /* sk_clipboard_get_sync：不开窗口也能读剪贴板 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fprintf(out,
            "用法: sketch <命令> [参数]\n"
            "\n"
            "和 SketchOS 显示服务器 sketchd 通信的命令行工具。\n"
            "\n"
            "命令:\n"
            "  launch <应用id> [参数]   启动 /etc/sketch/apps.conf 里的应用，例如:\n"
            "                           sketch launch notepad /root/readme.txt\n"
            "  clip get                 把剪贴板内容输出到标准输出\n"
            "  clip set <文字...>       设置剪贴板；不给文字时从标准输入读取，例如:\n"
            "                           ls | sketch clip set\n"
            "  info                     显示屏幕尺寸和显示服务器信息\n"
            "  -h, --help               显示这段帮助\n"
            "\n"
            "显示服务器的套接字默认是 %s，可以用环境变量 %s 指定。\n",
            SK_SOCKET_PATH, SK_SOCKET_ENV);
}

static sk_app *connect_or_die(void)
{
    sk_app *app = sk_app_connect("sketch");
    if (!app) {
        /* sk_app_connect 已经在 stderr 说明了原因 */
        fprintf(stderr, "sketch: 连接显示服务器失败（图形界面启动了吗？）\n");
        exit(1);
    }
    return app;
}

static int cmd_launch(int argc, char **argv)
{
    if (argc < 1 || argc > 2 || !*argv[0]) {
        fprintf(stderr, "sketch: launch 需要一个应用 id，例如 sketch launch terminal\n");
        return 2;
    }
    if (strlen(argv[0]) >= SK_APPNAME_MAX) {
        fprintf(stderr, "sketch: 应用 id 太长（最多 %d 字节）\n", SK_APPNAME_MAX - 1);
        return 2;
    }
    if (argc == 2 && strlen(argv[1]) >= sizeof(((sk_req_launch *)0)->arg)) {
        fprintf(stderr, "sketch: 参数太长（最多 %zu 字节）\n", sizeof(((sk_req_launch *)0)->arg) - 1);
        return 2;
    }
    sk_app *app = connect_or_die();
    sk_app_launch(app, argv[0], argc == 2 ? argv[1] : NULL);
    /* 请求已经放进套接字；即使我们马上断开，服务器也会先读到它再看到连接关闭 */
    sk_app_disconnect(app);
    printf("已请求启动 %s%s%s\n", argv[0], argc == 2 ? " " : "", argc == 2 ? argv[1] : "");
    return 0;
}

static int cmd_clip_get(void)
{
    sk_app *app = connect_or_die();
    int len = 0;
    char *text = sk_clipboard_get_sync(app, 3000, &len);
    sk_app_disconnect(app);
    if (!text) {
        fprintf(stderr, "sketch: 显示服务器没有回复剪贴板内容\n");
        return 1;
    }
    fwrite(text, 1, (size_t)len, stdout);
    if (isatty(STDOUT_FILENO) && (len == 0 || text[len - 1] != '\n'))
        putchar('\n');                /* 在终端里看时补个换行；管道里原样输出 */
    free(text);
    return 0;
}

static int cmd_clip_set(int argc, char **argv)
{
    char buf[SK_CLIP_MAX + 1];
    size_t n = 0;
    int truncated = 0;
    if (argc > 0) {
        /* 多个参数用空格连起来：sketch clip set 你好 世界 → "你好 世界" */
        for (int i = 0; i < argc; i++) {
            if (i) {
                if (n >= SK_CLIP_MAX) {
                    truncated = 1;
                    break;
                }
                buf[n++] = ' ';
            }
            size_t l = strlen(argv[i]);
            if (l > SK_CLIP_MAX - n) {
                l = SK_CLIP_MAX - n;
                truncated = 1;
            }
            memcpy(buf + n, argv[i], l);
            n += l;
        }
    } else {
        if (isatty(STDIN_FILENO))
            fprintf(stderr, "sketch: 从键盘读取剪贴板内容，按 Ctrl+D 结束\n");
        size_t r;
        while (n < SK_CLIP_MAX && (r = fread(buf + n, 1, SK_CLIP_MAX - n, stdin)) > 0)
            n += r;
        if (n == SK_CLIP_MAX && fgetc(stdin) != EOF)
            truncated = 1;
    }
    if (truncated)
        fprintf(stderr, "sketch: 剪贴板最多 %d 字节，多出的部分被截掉了\n", SK_CLIP_MAX);
    sk_app *app = connect_or_die();
    sk_clipboard_set(app, buf, (int)n);       /* 库会保证不在多字节字符中间截断 */
    sk_app_disconnect(app);
    return 0;
}

static int cmd_info(void)
{
    const char *path = getenv(SK_SOCKET_ENV);
    sk_app *app = connect_or_die();
    printf("显示服务器: %s\n", path && *path ? path : SK_SOCKET_PATH);
    printf("协议版本:   %d\n", SK_PROTO_VERSION);
    printf("屏幕尺寸:   %dx%d\n", sk_app_screen_w(app), sk_app_screen_h(app));
    sk_app_disconnect(app);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(cmd, "launch"))
        return cmd_launch(argc - 2, argv + 2);
    if (!strcmp(cmd, "info") && argc == 2)
        return cmd_info();
    if (!strcmp(cmd, "clip") && argc >= 3) {
        if (!strcmp(argv[2], "get") && argc == 3)
            return cmd_clip_get();
        if (!strcmp(argv[2], "set"))
            return cmd_clip_set(argc - 3, argv + 3);
    }
    fprintf(stderr, "sketch: 不认识的命令或参数不对：%s\n\n", cmd);
    usage(stderr);
    return 2;
}
