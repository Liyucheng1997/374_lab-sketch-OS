/*
 * sknote — SketchOS 记事本
 *
 * 一个“刚好够用”的纯文本编辑器：新建、打开、编辑、保存 UTF-8 文本文件。
 * 编辑本身（光标、选择、撤销重做、复制粘贴、自动换行、行号）全部由控件库的多行编辑器
 * sk_textarea 完成（libsketch/ui_textarea.c）。这个程序负责编辑器“外面”的事——
 * 而这恰好是操作系统课最值得看的部分：文件是怎么读进内存、又怎么安全地写回磁盘的。
 *
 * ┌──────────────── 读文件：open → fstat → read 循环 ────────────────┐
 *
 *  open()  向内核要一个“文件描述符”（fd）：一个小整数，是进程的打开文件表里的下标，
 *          指向内核里记录“哪个文件、读到哪儿了”的结构。
 *  fstat() 问内核这个 fd 指向的是什么：普通文件、目录、设备还是管道？多大？权限是什么？
 *          （先 open 再对 fd 做 fstat，而不是先 stat(路径) 再 open：两次按路径查找之间
 *            文件可能被换掉，对着同一个 fd 问就不会有这种“检查与使用之间”的漏洞。）
 *  read()  把数据从内核（页缓存）拷贝到我们的缓冲区。注意它可能“读得比要求的少”——短读：
 *          被信号打断、读管道或 /proc 这类特殊文件时，一次只给一部分是完全正常的。
 *          所以必须循环读，直到 read 返回 0（文件结束）。
 *
 * └──────────────────────────────────────────────────────────────────┘
 *
 * ┌──────────────── 写文件：先写临时文件，再 rename 换上去 ────────────────┐
 *
 *  最直接的做法是 open(O_TRUNC) 截断原文件、再写入新内容。可如果写到一半程序崩溃、
 *  断电或者磁盘满了，原文件已经被清空，新内容又没写完——两份都丢了。
 *  所以我们：① 在同一个目录里新建一个临时文件，把全部内容写进去；
 *            ② fsync 让内核把它真正落盘；③ 用 rename() 把临时文件改名成目标文件名。
 *  rename 在同一个文件系统内是原子操作：目录项一次性从旧文件指向新文件，任何时刻
 *  别的进程（以及崩溃重启后的你）看到的要么是完整的旧文件，要么是完整的新文件。
 *  临时文件必须和目标在同一个目录（同一个文件系统），跨文件系统的 rename 会失败（EXDEV）。
 *
 * └────────────────────────────────────────────────────────────────────────┘
 *
 * 其它约定：
 *   - 文件按 UTF-8 解释。含有 NUL 字节（\0）的当作二进制文件拒绝打开，超过 1 MB 也拒绝
 *     （编辑器把全部内容放在内存里，在很慢的网页模拟器里太大的文件会卡）。
 *   - 换行符：Windows 风格的 \r\n 打开时变成 \n（编辑器内部只用 \n），保存时再写回 \r\n，
 *     原来是什么风格就还是什么风格。
 *   - 末尾换行：编辑器里看到什么就保存什么，不会自动在文件末尾补一个换行，也不会删掉它
 *     （文件以 \n 结尾时，编辑器最后会多出一个空行——那一行就代表这个换行）。
 *   - 保存时保留原文件的权限位；新文件的权限是 0666 去掉 umask 屏蔽的位。
 *   - “已修改”由编辑器的撤销栈判断：撤销回到保存时的样子，标题上的 * 也会消失。
 *   - 查找（Ctrl+F）暂时没有：它需要编辑器提供“把光标移到某处 / 选中一段文字”的接口，
 *     ui.h 里的 sk_textarea 目前还没有。
 *
 * 命令行：sknote [文件路径]      （/etc/sketch/apps.conf 里写的是 /bin/sknote %f）
 *   文件存在 → 打开它；不存在 → 空文档，第一次保存时创建它；没有参数 → “未命名”。
 * 快捷键：Ctrl+N 新建   Ctrl+O 打开   Ctrl+S 保存   Ctrl+Shift+S 另存为
 *
 * 调试（自动化测试 tests/apps/test_sknote.py 用）：设置环境变量 SKNOTE_DEBUG=1 后，
 * 程序会向标准输出打印：
 *   sknote: state modified=<0|1> bytes=<字节数> chars=<字符数> lines=<行数> line=<光标行>
 *           col=<光标列> wrap=<0|1> numbers=<0|1> crlf=<0|1> path=<绝对路径，未命名时为空>
 *     —— 文档内容、光标或开关变化后（打字时最多每 150 毫秒一次）
 *   sknote: title <窗口标题>          —— 标题变化时
 *   sknote: saved <字节数> <路径>      —— 保存成功后
 *   sknote: error <消息>              —— 弹出错误框之前
 *   sknote: ask <按钮序号>             —— “要保存吗？”对话框的结果（0 保存 1 不保存 2/-1 取消）
 */
#define _GNU_SOURCE
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include <sketch/rough.h>
#include <skutil.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NOTE_MAX_BYTES  (1024 * 1024)   /* 最多打开 1 MB 的文件 */
#define TOOLBAR_H       48              /* 顶部工具栏高度 */
#define STATUS_H        30              /* 底部状态栏高度 */
#define PAD             10
#define COUNT_DELAY_MS  150             /* 打字时字数统计最多这么久刷新一次 */
#define BG_SEED         0x50A7Eu        /* 窗口纸张纹理的种子（固定：局部重画不出“补丁”） */
#define NAME_MAX_BYTES  44              /* 标题里文件名最多占的字节（标题总共只有 64 字节） */

enum { TOOL_NEW, TOOL_OPEN, TOOL_SAVE, TOOL_SAVE_AS, NTOOLS };

/* 程序的全部状态：回调函数通过 user 指针拿到它 */
typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    sk_widget *tools[NTOOLS];
    sk_widget *chk_wrap, *chk_numbers;
    sk_widget *editor;
    sk_widget *lbl_pos, *lbl_count, *lbl_path;
    char      *path;            /* 当前文件的绝对路径；NULL = 未命名 */
    int        crlf;            /* 文件原来用 Windows 的 \r\n 换行，保存时写回 \r\n */
    int        count_timer;     /* 字数统计的延迟定时器 id，0 = 没有在等 */
    int        busy;            /* 正在显示对话框（嵌套事件循环里），见 on_event 的 CLOSE */
    int        debug;           /* SKNOTE_DEBUG */
    char       title[SK_TITLE_MAX + 16];
} note;

/* ================================================================ 小工具 */

static void dbg(note *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dbg(note *n, const char *fmt, ...)
{
    if (!n->debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("sknote: ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    fflush(stdout);         /* 标准输出接的是管道时默认全缓冲，测试脚本要立刻看到 */
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* 把 src 复制到 dst，最多保留 max 字节；太长时在 UTF-8 字符边界处截断并加“…”。
 * （中文一个字占 3 个字节，从中间切开会得到乱码。UTF-8 的后续字节都是 10xxxxxx。） */
static void copy_ellipsis(char *dst, size_t dsz, const char *src, size_t max)
{
    size_t len = strlen(src);
    if (len <= max) {
        sk_strlcpy(dst, src, dsz);
        return;
    }
    size_t cut = max > 3 ? max - 3 : 0;
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80)
        cut--;
    snprintf(dst, dsz, "%.*s…", (int)cut, src);
}

/* 命令行参数可能是相对路径（相对于进程的当前目录），统一转成绝对路径保存：
 * 以后打开文件对话框、显示在标题和状态栏里都不会有歧义。返回 malloc 的字符串。 */
static char *absolute_path(const char *arg)
{
    char buf[PATH_MAX];
    if (arg[0] == '/') {
        sk_strlcpy(buf, arg, sizeof buf);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) {
            perror("sknote: getcwd");
            sk_strlcpy(cwd, "/", sizeof cwd);
        }
        sk_path_join(buf, sizeof buf, cwd, arg);
    }
    sk_path_normalize(buf);
    return strdup(buf);
}

/* 对话框的起始目录：当前文件所在目录；未命名时用 $HOME/文档（存在的话），否则 $HOME */
static void dialog_dir(note *n, char *buf, size_t sz)
{
    if (n->path) {
        sk_strlcpy(buf, n->path, sz);
        char *slash = strrchr(buf, '/');
        if (slash)
            *(slash == buf ? slash + 1 : slash) = 0;
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/root";
    struct stat st;
    sk_path_join(buf, sz, home, "文档");
    if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
        return;
    sk_strlcpy(buf, home, sz);
}

/* 弹出错误框（阻塞到用户点“确定”） */
static void show_error(note *n, const char *msg)
{
    dbg(n, "error %s", msg);
    n->busy++;
    sk_dialog_message(n->app, "记事本", msg, "确定");
    n->busy--;
}

/* ================================================================ 标题与状态栏 */

/* 窗口标题 “<文件名> - 记事本”，有未保存的修改时前面加 “*”。只在真的变了时才发给服务器 */
static void update_title(note *n)
{
    char name[NAME_MAX_BYTES + 8];
    copy_ellipsis(name, sizeof name, n->path ? base_name(n->path) : "未命名", NAME_MAX_BYTES);
    char t[sizeof n->title];      /* 文件名已经截短过，结果一定放得进 SK_TITLE_MAX（64 字节） */
    snprintf(t, sizeof t, "%s%s - 记事本", sk_textarea_modified(n->editor) ? "*" : "", name);
    if (strcmp(t, n->title) == 0)
        return;
    sk_strlcpy(n->title, t, sizeof n->title);
    sk_window_set_title(n->win, t);
    dbg(n, "title %s", t);
}

static void update_pos(note *n)
{
    int line, col;
    sk_textarea_cursor_pos(n->editor, &line, &col);
    char buf[64];
    snprintf(buf, sizeof buf, "第 %d 行，第 %d 列", line, col);
    sk_widget_set_text(n->lbl_pos, buf);      /* 文字没变时控件库什么也不做（不会重画） */
}

/* 状态栏右边：文件路径。放不下时省略开头（“…/文档/笔记.txt”），文件名是最有用的部分 */
static void update_path_label(note *n)
{
    const char *suffix = n->crlf ? " · Windows 换行" : "";
    const char *p = n->path ? n->path : "未命名（还没有保存过）";
    const sk_font *f = sk_font_get(SK_FONT_UI);
    int maxw = n->lbl_path->r.w - sk_text_width(f, suffix);
    const char *start = p;
    if (n->path && maxw > 0 && sk_text_width(f, p) > maxw) {
        int ew = sk_text_width(f, "…");
        while (*start && sk_text_width(f, start) + ew > maxw)
            sk_utf8_next(&start);              /* 从开头一个字一个字地去掉 */
        const char *slash = strchr(start, '/');
        if (slash && slash[1])
            start = slash;                     /* 尽量从一个“/”处断开 */
    }
    char buf[PATH_MAX + 64];
    snprintf(buf, sizeof buf, "%s%s%s", start != p ? "…" : "", start, suffix);
    sk_widget_set_text(n->lbl_path, buf);
}

/* 统计行数和字符数。要把整篇文字取出来数一遍（1 MB 的文件在网页模拟器里要几十毫秒），
 * 所以打字时不是每个键都数，而是由 schedule_count 推迟一小会儿、合并成一次。 */
static void refresh_count(note *n)
{
    if (n->count_timer) {
        sk_app_remove_timer(n->app, n->count_timer);
        n->count_timer = 0;
    }
    char *text = sk_textarea_get_text(n->editor);
    if (!text)
        return;
    size_t bytes = strlen(text);
    int chars = 0, lines = 1;
    for (const char *s = text; *s;) {
        uint32_t cp = sk_utf8_next(&s);       /* 一次解码一个 UTF-8 字符（1~4 字节） */
        if (cp == '\n')
            lines++;
        else
            chars++;
    }
    free(text);
    char buf[96];
    snprintf(buf, sizeof buf, "%d 行 · %d 个字符", lines, chars);
    sk_widget_set_text(n->lbl_count, buf);

    int line, col;
    sk_textarea_cursor_pos(n->editor, &line, &col);
    dbg(n, "state modified=%d bytes=%zu chars=%d lines=%d line=%d col=%d wrap=%d numbers=%d crlf=%d path=%s",
        sk_textarea_modified(n->editor), bytes, chars, lines, line, col, sk_widget_checked(n->chk_wrap),
        sk_widget_checked(n->chk_numbers), n->crlf, n->path ? n->path : "");
}

static void count_timer_fn(sk_app *app, void *user)
{
    (void)app;
    note *n = user;
    n->count_timer = 0;         /* 一次性定时器：触发时库已经把它注销了 */
    refresh_count(n);
}

static void schedule_count(note *n)
{
    if (!n->count_timer)
        n->count_timer = sk_app_add_timer(n->app, COUNT_DELAY_MS, 0, count_timer_fn, n);
    if (!n->count_timer)        /* 定时器表满了（内存不足）：那就现在数 */
        refresh_count(n);
}

/* 编辑器的内容或光标变了（打字、移动光标、撤销……都会调用） */
static void on_text_change(sk_widget *w, void *user)
{
    (void)w;
    note *n = user;
    update_pos(n);
    update_title(n);
    schedule_count(n);
}

/* 换上一篇新文档：path 的所有权交给 n（可为 NULL = 未命名） */
static void set_document(note *n, char *path, const char *text, int crlf)
{
    free(n->path);
    n->path = path;
    n->crlf = crlf;
    /* set_text 会清空撤销历史、把光标放回开头，并把文档标记为“未修改” */
    sk_textarea_set_text(n->editor, text);
    update_pos(n);
    update_path_label(n);
    update_title(n);
    refresh_count(n);
}

/* ================================================================ 读文件 */

enum { RD_OK = 0, RD_NOENT, RD_ERROR };

/* 把整个文本文件读进内存。成功返回 RD_OK，*out 是 malloc 的缓冲区（末尾补了 NUL），*out_len 是字节数；
 * 文件不存在返回 RD_NOENT；其它失败返回 RD_ERROR，并把给用户看的中文原因写进 err。 */
static int read_text_file(const char *path, char **out, size_t *out_len, char *err, size_t errsz)
{
    *out = NULL;
    *out_len = 0;
    /* O_NONBLOCK：如果 path 是一个命名管道（FIFO），普通的 open 会一直阻塞到有人往里写——
     * 整个图形程序就卡死了。加上它 open 会立刻返回，接着 fstat 会发现“这不是普通文件”。
     * 对普通文件 O_NONBLOCK 没有任何影响。
     * O_CLOEXEC：将来如果 exec 别的程序，这个 fd 自动关闭，不会泄漏给子进程。 */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        if (e == ENOENT)
            return RD_NOENT;
        snprintf(err, errsz, "无法打开“%s”：%s。", path, sk_strerror(e));
        return RD_ERROR;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        snprintf(err, errsz, "无法读取“%s”的信息：%s。", path, sk_strerror(errno));
        close(fd);
        return RD_ERROR;
    }
    if (S_ISDIR(st.st_mode)) {
        snprintf(err, errsz, "无法打开“%s”：这是一个文件夹，不是文本文件。", path);
        close(fd);
        return RD_ERROR;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(err, errsz, "无法打开“%s”：它不是普通文件（可能是设备或管道），记事本只能编辑文本文件。", path);
        close(fd);
        return RD_ERROR;
    }
    if (st.st_size > NOTE_MAX_BYTES) {
        char sz[32];
        sk_fmt_size(sz, sizeof sz, (uint64_t)st.st_size);
        snprintf(err, errsz, "无法打开“%s”：文件太大（%s），记事本最多只能打开 1 MB 的文件。", path, sz);
        close(fd);
        return RD_ERROR;
    }

    /* 按 st_size 分配缓冲区。/proc 里的文件 st_size 是 0（内容是读的时候现生成的），
     * 所以缓冲区不够时还要扩大，一直读到 read 返回 0 为止；最多读 1 MB + 1 字节，
     * 多出来的那 1 字节用来发现“文件比 1 MB 大”。 */
    size_t cap = st.st_size > 0 ? (size_t)st.st_size + 1 : 4096, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) {
        snprintf(err, errsz, "无法打开“%s”：内存不足。", path);
        close(fd);
        return RD_ERROR;
    }
    for (;;) {
        if (len == cap) {
            if (cap > NOTE_MAX_BYTES)
                break;                         /* 已经超过上限，不用再读了 */
            size_t ncap = cap * 2 > (size_t)NOTE_MAX_BYTES + 1 ? (size_t)NOTE_MAX_BYTES + 1 : cap * 2;
            char *nb = realloc(buf, ncap + 1);
            if (!nb) {
                snprintf(err, errsz, "无法打开“%s”：内存不足。", path);
                free(buf);
                close(fd);
                return RD_ERROR;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR)
                continue;                      /* 被信号打断：什么也没读到，再来一次 */
            snprintf(err, errsz, "读取“%s”时出错：%s。", path, sk_strerror(errno));
            free(buf);
            close(fd);
            return RD_ERROR;
        }
        if (r == 0)
            break;                             /* 文件结束 */
        len += (size_t)r;                      /* 短读也没关系：下一轮接着往后读 */
    }
    close(fd);          /* 只读的 fd：关闭不会丢数据，返回值不重要 */
    if (len > NOTE_MAX_BYTES) {
        snprintf(err, errsz, "无法打开“%s”：文件太大，记事本最多只能打开 1 MB 的文件。", path);
        free(buf);
        return RD_ERROR;
    }
    /* 文本文件里不会有 NUL 字节；有的话多半是程序、图片、压缩包之类的二进制文件 */
    if (memchr(buf, '\0', len)) {
        snprintf(err, errsz, "无法打开“%s”：它看起来是二进制文件（含有 NUL 字节），记事本只能编辑文本文件。", path);
        free(buf);
        return RD_ERROR;
    }
    buf[len] = 0;
    *out = buf;
    *out_len = len;
    return RD_OK;
}

/* 打开 path（绝对路径）换掉当前文档。成功（包括“文件不存在、当作新文件”）返回 1；
 * 失败时弹出错误框、保持当前文档不变，返回 0 */
static int open_file(note *n, const char *path)
{
    char *buf;
    size_t len;
    char err[PATH_MAX + 256];
    int r = read_text_file(path, &buf, &len, err, sizeof err);
    if (r == RD_ERROR) {
        show_error(n, err);
        return 0;
    }
    char *p = strdup(path);
    if (!p) {
        free(buf);
        show_error(n, "内存不足。");
        return 0;
    }
    if (r == RD_NOENT) {
        set_document(n, p, "", 0);             /* 新文件：第一次保存时才创建 */
        return 1;
    }
    /* 只要出现过 \r\n 就当作 Windows 风格的文件（编辑器会把 \r\n 统一成 \n） */
    int crlf = memmem(buf, len, "\r\n", 2) != NULL;
    set_document(n, p, buf, crlf);
    free(buf);
    return 1;
}

/* ================================================================ 写文件 */

/* 把 len 字节全部写出去。write 也可能“写得比要求的少”（短写：被信号打断、磁盘快满……），
 * 所以和 read 一样要循环。返回 0 成功，-1 失败（原因在 errno） */
static int write_all(int fd, const char *p, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* umask 是进程的“权限屏蔽字”：新建文件时内核会去掉这些权限位（通常是 022：别人不能写）。
 * 这个系统调用只能“设置新值并返回旧值”，所以读它要调用两次。 */
static mode_t current_umask(void)
{
    mode_t m = umask(0);
    umask(m);
    return m;
}

/* 原子地把 data 写到 path（原理见文件开头）。成功返回 0；失败返回 -1，把原因写进 err */
static int write_file_atomic(const char *path, const char *data, size_t len, char *err, size_t errsz)
{
    char target[PATH_MAX];
    sk_strlcpy(target, path, sizeof target);
    /* path 是符号链接时，要改的是它指向的那个文件——直接 rename 会把链接本身换成普通文件 */
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        char real[PATH_MAX];
        if (realpath(path, real))
            sk_strlcpy(target, real, sizeof target);
    }

    mode_t mode;
    if (stat(target, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            snprintf(err, errsz, "无法保存“%s”：这是一个文件夹。", path);
            return -1;
        }
        mode = st.st_mode & 07777;             /* 保留原文件的权限位 */
    } else if (errno == ENOENT) {
        mode = 0666 & ~current_umask();        /* 新文件：和 open(O_CREAT, 0666) 的效果一样 */
    } else {
        snprintf(err, errsz, "无法保存“%s”：%s。", path, sk_strerror(errno));
        return -1;
    }

    /* 临时文件：同一个目录下的隐藏文件 .<文件名>.sknote-XXXXXX。
     * mkostemp 把 XXXXXX 换成随机字符并用 O_CREAT|O_EXCL 创建：保证是一个全新的文件，
     * 不会覆盖已有的东西（也不会被别人抢先放一个同名的符号链接）。 */
    char dir[PATH_MAX], tmp[PATH_MAX + 64];
    sk_strlcpy(dir, target, sizeof dir);
    char *slash = strrchr(dir, '/');
    const char *base = target;
    if (slash) {
        base = target + (slash - dir) + 1;
        *(slash == dir ? slash + 1 : slash) = 0;
    } else {
        sk_strlcpy(dir, ".", sizeof dir);
    }
    if (snprintf(tmp, sizeof tmp, "%s%s.%s.sknote-XXXXXX", dir, strcmp(dir, "/") == 0 ? "" : "/", base) >=
        (int)sizeof tmp) {
        snprintf(err, errsz, "无法保存“%s”：%s。", path, sk_strerror(ENAMETOOLONG));
        return -1;
    }
    int fd = mkostemp(tmp, O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errsz, "无法保存“%s”：不能在文件夹 %s 里创建文件（%s）。", path, dir, sk_strerror(errno));
        return -1;
    }
    /* 写入 → 设权限（mkostemp 创建的文件是 0600）→ fsync。
     * fsync：write 只是把数据交给内核的页缓存，内核过一会儿才真正写到磁盘；如果在那之前
     * 就 rename 而又恰好断电，有的文件系统上会出现“新名字指向一个空文件”。fsync 等到落盘才返回。 */
    if (write_all(fd, data, len) < 0 || fchmod(fd, mode) < 0 || fsync(fd) < 0) {
        int e = errno;
        close(fd);
        unlink(tmp);
        snprintf(err, errsz, "无法保存“%s”：%s。", path, sk_strerror(e));
        return -1;
    }
    /* 写文件的 fd 一定要检查 close 的返回值：有的文件系统（例如网络文件系统）直到 close 才报告写入错误 */
    if (close(fd) < 0) {
        int e = errno;
        unlink(tmp);
        snprintf(err, errsz, "无法保存“%s”：%s。", path, sk_strerror(e));
        return -1;
    }
    if (rename(tmp, target) < 0) {
        int e = errno;
        unlink(tmp);
        snprintf(err, errsz, "无法保存“%s”：%s。", path, sk_strerror(e));
        return -1;
    }
    /* 改名本身记录在目录里；把目录也 fsync 一下，改名才算真正落盘（不支持的文件系统会报错，忽略即可） */
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
}

/* 把编辑器内容保存到 path。成功返回 1，失败弹出错误框并返回 0 */
static int save_to(note *n, const char *path)
{
    char *text = sk_textarea_get_text(n->editor);
    if (!text) {
        show_error(n, "无法保存：内存不足。");
        return 0;
    }
    size_t len = strlen(text);
    char *data = text;
    size_t dlen = len;
    if (n->crlf) {
        /* 编辑器里只有 \n；原文件是 Windows 风格的，就把每个 \n 换回 \r\n */
        size_t nl = 0;
        for (size_t i = 0; i < len; i++)
            nl += text[i] == '\n';
        data = malloc(len + nl + 1);
        if (!data) {
            free(text);
            show_error(n, "无法保存：内存不足。");
            return 0;
        }
        dlen = 0;
        for (size_t i = 0; i < len; i++) {
            if (text[i] == '\n')
                data[dlen++] = '\r';
            data[dlen++] = text[i];
        }
        data[dlen] = 0;
    }
    char err[PATH_MAX * 2 + 256];
    int ok = write_file_atomic(path, data, dlen, err, sizeof err) == 0;
    if (data != text)
        free(data);
    free(text);
    if (!ok) {
        show_error(n, err);
        return 0;
    }
    if (!n->path || strcmp(n->path, path) != 0) {
        char *p = strdup(path);
        if (p) {
            free(n->path);
            n->path = p;
        }
    }
    sk_textarea_set_modified(n->editor, 0);    /* 现在的状态就是“已保存”的状态 */
    dbg(n, "saved %zu %s", dlen, path);
    update_path_label(n);
    update_title(n);
    refresh_count(n);
    return 1;
}

/* ================================================================ 命令 */

/* 另存为：选一个文件名再保存。返回 1 = 保存成功 */
static int cmd_save_as(note *n)
{
    char dir[PATH_MAX];
    dialog_dir(n, dir, sizeof dir);
    /* 对话框是“阻塞式”的：内部跑一个嵌套事件循环，用户选完才返回（见 ui_dialogs.c）。
     * 文件已经存在时，对话框自己会先问一句“要替换它吗？” */
    n->busy++;
    char *p = sk_dialog_file(n->app, "另存为", dir, 1, n->path ? base_name(n->path) : "未命名.txt");
    n->busy--;
    if (!p)
        return 0;
    int ok = save_to(n, p);
    free(p);
    return ok;
}

/* 保存：有文件名就直接保存，未命名时等于“另存为” */
static int cmd_save(note *n)
{
    return n->path ? save_to(n, n->path) : cmd_save_as(n);
}

/* 有未保存的修改时问一句。返回 1 = 可以继续（已保存或不要了），0 = 用户取消 */
static int confirm_discard(note *n)
{
    if (!sk_textarea_modified(n->editor))
        return 1;
    char name[NAME_MAX_BYTES + 8], msg[256];
    copy_ellipsis(name, sizeof name, n->path ? base_name(n->path) : "未命名", NAME_MAX_BYTES);
    snprintf(msg, sizeof msg, "“%s”的内容已经修改，还没有保存。\n要保存吗？", name);
    n->busy++;
    int r = sk_dialog_message(n->app, "记事本", msg, "保存|不保存|取消");
    n->busy--;
    dbg(n, "ask %d", r);
    if (r == 0)
        return cmd_save(n);        /* 保存失败或在“另存为”里取消了：也不能继续 */
    return r == 1;                 /* 1 = 不保存；2 = 取消；-1 = Esc 或关掉了对话框 */
}

static void cmd_new(note *n)
{
    if (!confirm_discard(n))
        return;
    set_document(n, NULL, "", 0);
}

static void cmd_open(note *n)
{
    if (!confirm_discard(n))
        return;
    char dir[PATH_MAX];
    dialog_dir(n, dir, sizeof dir);
    n->busy++;
    char *p = sk_dialog_file(n->app, "打开", dir, 0, NULL);
    n->busy--;
    if (!p)
        return;
    open_file(n, p);
    free(p);
}

/* ================================================================ 回调 */

static void on_tool(sk_widget *w, void *user)
{
    note *n = user;
    switch (w->id) {
    case TOOL_NEW: cmd_new(n); break;
    case TOOL_OPEN: cmd_open(n); break;
    case TOOL_SAVE: cmd_save(n); break;
    case TOOL_SAVE_AS: cmd_save_as(n); break;
    default: break;
    }
}

static void on_wrap(sk_widget *w, void *user)
{
    note *n = user;
    sk_textarea_set_wrap(n->editor, sk_widget_checked(w));
    refresh_count(n);
}

static void on_numbers(sk_widget *w, void *user)
{
    note *n = user;
    sk_textarea_set_line_numbers(n->editor, sk_widget_checked(w));
    refresh_count(n);
}

/* 窗口级事件：编辑器不处理的快捷键（它把 Ctrl+字母 里自己不认识的交出来），以及关闭窗口 */
static int on_event(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    note *n = user;
    if (e->type == SK_EVENT_CLOSE) {
        /* 对话框开着时（例如正在问“要保存吗”），主窗口照样能收到关闭请求：忽略重复的那一次 */
        if (n->busy)
            return 1;
        /* 返回 0 = 让库执行默认动作：销毁窗口。最后一个窗口没了，sk_app_run 就返回 */
        return confirm_discard(n) ? 0 : 1;
    }
    if (e->type == SK_EVENT_KEY && e->key.state == 1 && !n->busy) {
        unsigned mods = e->key.modifiers;
        if ((mods & SK_MOD_CTRL) && !(mods & (SK_MOD_ALT | SK_MOD_SUPER))) {
            switch (e->key.keycode) {
            case KEY_N: cmd_new(n); return 1;
            case KEY_O: cmd_open(n); return 1;
            case KEY_S:
                if (mods & SK_MOD_SHIFT)
                    cmd_save_as(n);
                else
                    cmd_save(n);
                return 1;
            default: break;
            }
        }
    }
    return 0;
}

/* ================================================================ 布局与背景 */

static const int tool_w[NTOOLS] = { 82, 82, 82, 100 };
#define TOOLS_END (PAD + 82 * 3 + 100 + 3 * 4)          /* 工具按钮右边缘的 x */

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    note *n = user;
    int x = PAD;
    for (int i = 0; i < NTOOLS; i++) {
        sk_widget_set_rect(n->tools[i], sk_rect_make(x, 6, tool_w[i], 36));
        x += tool_w[i] + 4;
    }
    x = TOOLS_END + 22;                                  /* 中间留给一条竖的分隔线 */
    sk_widget_set_rect(n->chk_wrap, sk_rect_make(x, 10, 108, 28));
    sk_widget_set_rect(n->chk_numbers, sk_rect_make(x + 114, 10, 72, 28));

    int top = TOOLBAR_H + 6, bottom = H - STATUS_H - 6;
    sk_widget_set_rect(n->editor, sk_rect_make(PAD, top, W - 2 * PAD, bottom - top));

    int sy = H - STATUS_H + 1, sh = STATUS_H - 2;
    sk_widget_set_rect(n->lbl_pos, sk_rect_make(PAD + 4, sy, 150, sh));
    sk_widget_set_rect(n->lbl_count, sk_rect_make(PAD + 158, sy, 190, sh));
    int px = PAD + 356;
    sk_widget_set_rect(n->lbl_path, sk_rect_make(px, sy, W - px - PAD - 4 > 0 ? W - px - PAD - 4 : 0, sh));
    update_path_label(n);                                /* 宽度变了：路径可能要重新省略 */
}

/* 窗口背景：纸；顶部工具栏和底部状态栏用深一点的纸（SK_PAPER_DARK），交界处各一条手绘分隔线。
 * 只画 dirty 这一块（控件库只重画变了的地方，这里也一样）。 */
static void background(sk_ui *ui, sk_canvas *c, sk_rect dirty, void *user)
{
    (void)ui;
    note *n = user;
    int W = sk_window_width(n->win), H = sk_window_height(n->win);
    sk_paper_fill(c, dirty, SK_PAPER, 3, BG_SEED);
    sk_rect bar = sk_rect_intersect(dirty, sk_rect_make(0, 0, W, TOOLBAR_H));
    if (!sk_rect_empty(bar))
        sk_paper_fill(c, bar, SK_PAPER_DARK, 3, BG_SEED);
    sk_rect st = sk_rect_intersect(dirty, sk_rect_make(0, H - STATUS_H - 2, W, STATUS_H + 2));
    if (!sk_rect_empty(st))
        sk_paper_fill(c, st, SK_PAPER_DARK, 3, BG_SEED);
    /* 分隔线有 ±3 像素的手绘抖动，只要脏区域碰到这条带子就画（被裁剪在 dirty 里，种子固定） */
    if (dirty.y <= TOOLBAR_H + 2 && dirty.y + dirty.h >= TOOLBAR_H - 4)
        sk_draw_separator(c, 0, W, TOOLBAR_H - 1, 0x7001u);
    if (dirty.y <= H - STATUS_H + 1 && dirty.y + dirty.h >= H - STATUS_H - 5)
        sk_draw_separator(c, 0, W, H - STATUS_H - 2, 0x57A7u);
    /* 工具按钮和开关之间的一条短竖线 */
    int vx = TOOLS_END + 9;
    if (dirty.x <= vx + 4 && dirty.x + dirty.w >= vx - 4 && dirty.y <= TOOLBAR_H) {
        sk_rough_style s;
        sk_style_pencil(&s, 0x5E9u);
        s.stroke = SK_INK_FAINT;
        s.stroke_width = 1.2f;
        sk_rough_line(c, (float)vx, 12.0f, (float)vx, (float)(TOOLBAR_H - 12), &s);
    }
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    note n;
    memset(&n, 0, sizeof n);
    const char *dv = getenv("SKNOTE_DEBUG");
    n.debug = dv && *dv && strcmp(dv, "0") != 0;
    if (argc > 2) {
        fprintf(stderr, "用法: sknote [文件路径]\n");
        return 2;
    }
    char *path = (argc == 2 && argv[1][0]) ? absolute_path(argv[1]) : NULL;

    n.app = sk_app_connect("sknote");
    if (!n.app)
        return 1;
    n.win = sk_window_create(n.app, "记事本", 760, 540, SK_WIN_RESIZABLE, SK_GREEN);
    if (!n.win) {
        sk_app_disconnect(n.app);
        return 1;
    }
    sk_window_set_min_size(n.win, 600, 300);
    n.ui = sk_ui_new(n.win);
    if (!n.ui) {
        sk_app_disconnect(n.app);
        return 1;
    }

    /* 控件先随便给个位置，真正的位置由 layout() 统一计算 */
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    static const int icons[NTOOLS] = { SK_ICON_FILE, SK_ICON_OPEN, SK_ICON_SAVE, SK_ICON_SAVE };
    static const char *const labels[NTOOLS] = { "新建", "打开", "保存", "另存为" };
    for (int i = 0; i < NTOOLS; i++) {
        n.tools[i] = sk_icon_button_new(n.ui, z, icons[i], labels[i], on_tool, &n);
        if (!n.tools[i])
            return 1;
        n.tools[i]->id = i;
        sk_widget_set_accent(n.tools[i], SK_GREEN);
    }
    n.chk_wrap = sk_checkbox_new(n.ui, z, "自动换行", 1, on_wrap, &n);
    n.chk_numbers = sk_checkbox_new(n.ui, z, "行号", 0, on_numbers, &n);
    n.editor = sk_textarea_new(n.ui, z, "");
    n.lbl_pos = sk_label_new(n.ui, z, "", SK_FONT_UI, SK_ALIGN_LEFT);
    n.lbl_count = sk_label_new(n.ui, z, "", SK_FONT_UI, SK_ALIGN_LEFT);
    n.lbl_path = sk_label_new(n.ui, z, "", SK_FONT_UI, SK_ALIGN_RIGHT);
    if (!n.chk_wrap || !n.chk_numbers || !n.editor || !n.lbl_pos || !n.lbl_count || !n.lbl_path) {
        fprintf(stderr, "sknote: 内存不足\n");
        return 1;
    }
    sk_label_set_color(n.lbl_pos, SK_INK_SOFT);
    sk_label_set_color(n.lbl_count, SK_INK_SOFT);
    sk_label_set_color(n.lbl_path, SK_INK_SOFT);
    sk_widget_set_callbacks(n.editor, NULL, on_text_change, &n);

    sk_ui_set_background(n.ui, background, &n);
    sk_ui_set_layout(n.ui, layout, &n);
    sk_ui_set_handler(n.ui, on_event, &n);
    sk_ui_focus(n.ui, n.editor);               /* 一打开就可以直接打字 */

    /* 先是一篇空的“未命名”文档；再打开命令行给的文件。打开失败时，先把主窗口画出来，
     * 错误框才会出现在它上面——用户关掉错误框后得到的就是这篇空文档。 */
    set_document(&n, NULL, "", 0);
    if (path) {
        sk_ui_redraw_now(n.ui);
        open_file(&n, path);
        free(path);
    }

    int rc = sk_app_run(n.app);
    free(n.path);
    sk_ui_free(n.ui);
    sk_app_disconnect(n.app);
    return rc;
}
