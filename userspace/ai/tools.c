/*
 * tools.c — AI 能用的“手”：运行命令、读写文件、列目录、打开应用
 *
 * 模型自己不能碰这台电脑，它只能“说”：在回答里放一个 tool_use 块，写明想用哪个工具、
 * 参数是什么（JSON）。真正动手的是这里的代码——它们运行在 skaid 进程里，以 root 身份，
 * 用的就是普通程序用的那些系统调用（fork/exec、open/read/write、opendir……）。
 * 做完以后把结果写成文字交还给模型，模型再决定下一步。这一来一回就是“智能体循环”。
 *
 * ┌──────────────── 运行命令：fork + exec + 管道 + 超时 ────────────────┐
 *
 *  1. pipe() 建一根管道：内核里的一块缓冲区，一头写、一头读。
 *  2. fork() 出一个子进程。子进程里：setsid() 开一个新会话（没有控制终端，也成了一个新的
 *     进程组的组长）；把标准输出和标准错误 dup2 到管道的写端，标准输入接 /dev/null；
 *     然后 exec 成 sksh -c "命令"。
 *  3. 父进程（skaid）用 poll() 等管道里的数据，边等边读，同时盯着时钟。
 *     命令结束时，子进程退出、管道写端被内核关闭，read() 返回 0。
 *  4. 超时了就 kill(-pid, SIGKILL)：负的 pid 表示“整个进程组”，命令启动的所有子孙进程
 *     （比如管道里的每一个程序）一起杀掉。最后 waitpid() 回收子进程、拿到退出码。
 *
 * └─────────────────────────────────────────────────────────────────────┘
 *
 * 注意：工具执行期间 skaid 在这里同步等待（最多 120 秒），这段时间不处理别的客户端——
 * 对一台只有一个用户的教学电脑来说足够了，代码也简单得多。
 */
#include "skaid.h"

#include <sketch/client.h>
#include <skutil.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OUT_HEAD      20000      /* 命令输出最多保留开头这么多字节 …… */
#define OUT_TAIL      8000       /* …… 和结尾这么多字节（中间的省略掉，免得撑爆对话） */
#define READ_MAX      (100 * 1024)
#define DIR_MAX       400
#define APPS_CONF     "/etc/sketch/apps.conf"

/* ================================================================ 工具定义（给 API 看的 JSON Schema） */

static jnode *prop(jnode *props, const char *name, const char *type, const char *desc)
{
    jnode *p = json_add(props, name, json_obj());
    json_add(p, "type", json_str(type));
    json_add(p, "description", json_str(desc));
    return p;
}

static jnode *tool(jnode *arr, const char *name, const char *desc, jnode **props, const char *const *required)
{
    jnode *t = json_add(arr, NULL, json_obj());
    json_add(t, "name", json_str(name));
    json_add(t, "description", json_str(desc));
    jnode *schema = json_add(t, "input_schema", json_obj());
    json_add(schema, "type", json_str("object"));
    *props = json_add(schema, "properties", json_obj());
    jnode *req = json_add(schema, "required", json_arr());
    for (int i = 0; required && required[i]; i++)
        json_add(req, NULL, json_str(required[i]));
    /* 参数（比如要写进文件的整篇文字）一边生成一边流过来，不用等模型写完整个参数；
     * 网页桥发现用的不是官方 API 地址时会把这个字段去掉（有的代理不认识它） */
    json_add(t, "eager_input_streaming", json_bool(1));
    return t;
}

jnode *tools_definitions(void)
{
    jnode *arr = json_arr(), *p;

    static const char *const r_cmd[] = { "command", NULL };
    tool(arr, "run_command",
         "在 SketchOS 里用 sksh（类似 sh 的 shell）执行一条命令行，返回退出码和输出（标准输出+标准错误）。"
         "支持管道、重定向、&&、||、变量、if/for/while。工作目录是 /root。标准输入是 /dev/null，"
         "所以不要运行需要交互的程序；图形应用请用 open_app 打开。", &p, r_cmd);
    prop(p, "command", "string", "要执行的命令行");
    prop(p, "timeout_seconds", "integer", "超时秒数，默认 30，最大 120");

    static const char *const r_path[] = { "path", NULL };
    tool(arr, "read_file", "读取一个文本文件的内容（UTF-8，最多 100 KB）。", &p, r_path);
    prop(p, "path", "string", "文件路径；相对路径从 /root 算起");

    static const char *const r_write[] = { "path", "content", NULL };
    tool(arr, "write_file",
         "把文字写进文件（不存在就创建，连同缺少的上级目录；已存在就整个覆盖）。append 为 true 时追加到末尾。",
         &p, r_write);
    prop(p, "path", "string", "文件路径；相对路径从 /root 算起");
    prop(p, "content", "string", "要写入的完整内容");
    prop(p, "append", "boolean", "true = 追加到文件末尾，而不是覆盖");

    static const char *const r_edit[] = { "path", "old_text", "new_text", NULL };
    tool(arr, "edit_file",
         "修改文件里的一段文字：old_text 必须在文件里恰好出现一次，把它换成 new_text。适合小改动。",
         &p, r_edit);
    prop(p, "path", "string", "文件路径；相对路径从 /root 算起");
    prop(p, "old_text", "string", "要替换掉的原文（必须和文件里的一字不差，且只出现一次）");
    prop(p, "new_text", "string", "替换成的新文字");

    tool(arr, "list_directory", "列出一个目录里的文件和子目录（名字、类型、大小）。", &p, r_path);
    prop(p, "path", "string", "目录路径；相对路径从 /root 算起");

    static const char *const r_app[] = { "app", NULL };
    tool(arr, "open_app",
         "在 SketchOS 的图形桌面上打开一个应用窗口，给用户看。app 是应用 id：terminal 终端、files 文件管理器、"
         "notepad 记事本、paint 画板、music 音乐、monitor 系统监视器、clock 时钟、about 关于本机、ai AI 助手。"
         "argument 是可选的参数：文件类应用是要打开的文件或目录路径；terminal 的参数是目录（在那里打开）"
         "或一条命令（在终端窗口里运行给用户看）。", &p, r_app);
    prop(p, "app", "string", "应用 id，见 /etc/sketch/apps.conf");
    prop(p, "argument", "string", "可选：文件路径、目录或命令");
    return arr;
}

/* ================================================================ 小工具 */

static char *xprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *xprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0)
        s = NULL;
    va_end(ap);
    return s ? s : strdup("");
}

/* 相对路径从 /root 算起，再规范化（去掉 . 和 ..） */
static char *resolve(const char *path)
{
    char buf[4096];
    if (path[0] == '/')
        sk_strlcpy(buf, path, sizeof buf);
    else if (path[0] == '~' && (path[1] == '/' || !path[1]))
        snprintf(buf, sizeof buf, "/root%s", path + 1);
    else
        snprintf(buf, sizeof buf, "/root/%s", path);
    sk_path_normalize(buf);
    return strdup(buf);
}

/* 摘要：只取第一行的前 max 个字符（按 UTF-8 字符算，不会切断汉字） */
static char *first_line(const char *s, int max_cols)
{
    while (*s == '\n')
        s++;
    size_t n = strcspn(s, "\n");
    char *line = strndup(s, n);
    if (!line)
        return strdup("");
    size_t fit = sk_utf8_fit(line, max_cols, NULL);
    if (fit < strlen(line)) {
        line[fit] = 0;
        char *r = xprintf("%s…", line);
        free(line);
        return r;
    }
    return line;
}

/* ================================================================ run_command */

typedef struct {
    char head[OUT_HEAD];
    size_t head_len;
    char tail[OUT_TAIL];        /* 环形缓冲：总是保存最后 OUT_TAIL 字节 */
    size_t tail_pos, total;
} outbuf;

static void out_put(outbuf *o, const char *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (o->head_len < OUT_HEAD)
            o->head[o->head_len++] = d[i];
        else {
            o->tail[o->tail_pos % OUT_TAIL] = d[i];
            o->tail_pos++;
        }
    }
    o->total += n;
}

static char *out_text(const outbuf *o)
{
    size_t tail_n = o->tail_pos < OUT_TAIL ? o->tail_pos : OUT_TAIL;
    size_t skipped = o->total - o->head_len - tail_n;
    char *s = malloc(o->head_len + tail_n + 128);
    if (!s)
        return strdup("");
    memcpy(s, o->head, o->head_len);
    size_t n = o->head_len;
    if (skipped)
        n += (size_t)sprintf(s + n, "\n……（中间省略了 %zu 字节）……\n", skipped);
    size_t start = o->tail_pos < OUT_TAIL ? 0 : o->tail_pos % OUT_TAIL;
    for (size_t i = 0; i < tail_n; i++)
        s[n++] = o->tail[(start + i) % OUT_TAIL];
    s[n] = 0;
    return s;
}

/* 图形应用不会自己退出：如果模型想用 run_command 启动它们，会一直卡到超时 */
static const char *gui_program(const char *cmd)
{
    static const char *const names[] = { "skterm", "skfiles", "sknote", "skpaint", "skmusic",
                                         "skmon", "skclock", "skabout", "skai", "sketchd", NULL };
    while (*cmd == ' ')
        cmd++;
    if (strncmp(cmd, "/bin/", 5) == 0)
        cmd += 5;
    for (int i = 0; names[i]; i++) {
        size_t n = strlen(names[i]);
        if (strncmp(cmd, names[i], n) == 0 && (cmd[n] == 0 || cmd[n] == ' ' || cmd[n] == '&'))
            return names[i];
    }
    return NULL;
}

static char *run_command(const jnode *in, int *is_error)
{
    const char *cmd = json_get_str(in, "command");
    if (!cmd || !*cmd) {
        *is_error = 1;
        return strdup("缺少参数 command");
    }
    const char *gui = gui_program(cmd);
    if (gui) {
        *is_error = 1;
        return xprintf("%s 是图形应用，不会自己结束；请用 open_app 工具打开它。", gui);
    }
    int timeout = (int)json_get_num(in, "timeout_seconds", 30);
    timeout = SK_CLAMP(timeout, 1, 120);

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) {
        *is_error = 1;
        return xprintf("pipe 失败：%s", sk_strerror(errno));
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        *is_error = 1;
        return xprintf("fork 失败：%s", sk_strerror(errno));
    }
    if (pid == 0) {
        setsid();
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0)
            dup2(null, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        if (chdir("/root") < 0)
            (void)chdir("/");
        setenv("TERM", "dumb", 1);           /* 没有终端：让 ls、top 这些程序别输出颜色控制码 */
        setenv("COLUMNS", "100", 1);
        signal(SIGPIPE, SIG_DFL);
        const char *sh = getenv("SKAID_SHELL");      /* 测试时在宿主机上指定 sksh 的位置 */
        if (!sh || !*sh)
            sh = "/bin/sksh";
        execl(sh, "sksh", "-c", cmd, (char *)NULL);
        dprintf(2, "无法运行 %s: %s\n", sh, strerror(errno));
        _exit(127);
    }
    close(p[1]);

    outbuf *o = calloc(1, sizeof *o);
    if (!o)
        abort();
    uint64_t deadline = now_ms() + (uint64_t)timeout * 1000;
    int timed_out = 0;
    char buf[4096];
    for (;;) {
        uint64_t now = now_ms();
        if (now >= deadline) {
            timed_out = 1;
            break;
        }
        struct pollfd pf = { p[0], POLLIN, 0 };
        int r = poll(&pf, 1, (int)(deadline - now));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            continue;            /* 超时：下一圈循环会发现 */
        ssize_t n = read(p[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;               /* 所有写端都关了：命令结束了 */
        out_put(o, buf, (size_t)n);
    }
    close(p[0]);
    if (timed_out)
        kill(-pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    /* 命令自己结束了，但它在后台留下了还开着管道的子孙进程？那些进程不归我们等 */

    char *out = out_text(o);
    free(o);
    char *res;
    if (timed_out)
        res = xprintf("命令运行超过 %d 秒，已被强制结束。\n输出：\n%s", timeout, out);
    else if (WIFSIGNALED(status))
        res = xprintf("命令被信号 %s 终止。\n输出：\n%s",
                      sk_signal_name(WTERMSIG(status)) ? sk_signal_name(WTERMSIG(status)) : "?", out);
    else
        res = xprintf("退出码 %d\n%s", WEXITSTATUS(status), *out ? out : "（没有输出）");
    *is_error = timed_out || WIFSIGNALED(status) || WEXITSTATUS(status) != 0;
    free(out);
    return res;
}

/* ================================================================ 文件 */

static char *read_file(const jnode *in, int *is_error)
{
    const char *path = json_get_str(in, "path");
    if (!path || !*path) {
        *is_error = 1;
        return strdup("缺少参数 path");
    }
    char *full = resolve(path);
    struct stat st;
    char *res;
    if (stat(full, &st) < 0) {
        res = xprintf("%s：%s", full, sk_strerror(errno));
        *is_error = 1;
    } else if (S_ISDIR(st.st_mode)) {
        res = xprintf("%s 是目录，请用 list_directory", full);
        *is_error = 1;
    } else {
        size_t len;
        char *data = sk_read_file(full, &len);
        if (!data) {
            res = xprintf("%s：%s", full, sk_strerror(errno));
            *is_error = 1;
        } else if (memchr(data, 0, len)) {
            res = xprintf("%s 是二进制文件（%zu 字节），不能当文字读", full, len);
            *is_error = 1;
            free(data);
        } else {
            if (len > READ_MAX) {
                data[READ_MAX] = 0;
                res = xprintf("%s\n……（文件共 %zu 字节，只显示了前 %d 字节）", data, len, READ_MAX);
            } else {
                res = len ? strdup(data) : strdup("（空文件）");
            }
            free(data);
        }
    }
    free(full);
    return res;
}

/* 先写临时文件再 rename：写到一半出错也不会把原来的文件弄坏（详见 sknote 开头的说明） */
static int write_atomic(const char *path, const char *data, size_t len, int append)
{
    if (append) {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0)
            return -1;
        ssize_t w = write(fd, data, len);
        int e = errno;
        close(fd);
        errno = e;
        return w == (ssize_t)len ? 0 : -1;
    }
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.skaid-%d", path, (int)getpid());
    struct stat st;
    mode_t mode = stat(path, &st) == 0 ? (st.st_mode & 07777) : 0644;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close(fd);
            unlink(tmp);
            errno = e;
            return -1;
        }
        off += (size_t)w;
    }
    fchmod(fd, mode);
    if (close(fd) < 0 || rename(tmp, path) < 0) {
        int e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    return 0;
}

static char *write_file(const jnode *in, int *is_error)
{
    const char *path = json_get_str(in, "path");
    jnode *content = json_get(in, "content");
    if (!path || !*path || !content || content->t != J_STR) {
        *is_error = 1;
        return strdup("需要参数 path 和 content（字符串）");
    }
    int append = json_get_bool(in, "append", 0);
    char *full = resolve(path);
    char *dir = strdup(full);
    char *slash = dir ? strrchr(dir, '/') : NULL;
    if (slash && slash != dir) {
        *slash = 0;
        sk_mkdir_p(dir, 0755);
    }
    free(dir);
    char *res;
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        res = xprintf("%s 是一个目录", full);
        *is_error = 1;
    } else if (write_atomic(full, content->s, content->slen, append) < 0) {
        res = xprintf("写入 %s 失败：%s", full, sk_strerror(errno));
        *is_error = 1;
    } else {
        res = xprintf("已%s %s（%zu 字节）", append ? "追加到" : "写入", full, content->slen);
    }
    free(full);
    return res;
}

static char *edit_file(const jnode *in, int *is_error)
{
    const char *path = json_get_str(in, "path");
    jnode *old_n = json_get(in, "old_text"), *new_n = json_get(in, "new_text");
    if (!path || !old_n || old_n->t != J_STR || !new_n || new_n->t != J_STR || !old_n->slen) {
        *is_error = 1;
        return strdup("需要参数 path、old_text（不能为空）和 new_text");
    }
    char *full = resolve(path);
    size_t len;
    char *data = sk_read_file(full, &len);
    char *res = NULL;
    *is_error = 1;
    if (!data) {
        res = xprintf("%s：%s", full, sk_strerror(errno));
    } else {
        int count = 0;
        char *first = NULL;
        for (char *p = data; (p = strstr(p, old_n->s)); p += old_n->slen) {
            if (!first)
                first = p;
            count++;
        }
        if (count == 0) {
            res = xprintf("在 %s 里没找到 old_text（要和文件内容一字不差，包括空格和换行）", full);
        } else if (count > 1) {
            res = xprintf("old_text 在 %s 里出现了 %d 次，请多带一些上下文让它唯一", full, count);
        } else {
            size_t pre = (size_t)(first - data);
            size_t nlen = len - old_n->slen + new_n->slen;
            char *out = malloc(nlen + 1);
            if (!out)
                abort();
            memcpy(out, data, pre);
            memcpy(out + pre, new_n->s, new_n->slen);
            memcpy(out + pre + new_n->slen, first + old_n->slen, len - pre - old_n->slen);
            if (write_atomic(full, out, nlen, 0) < 0) {
                res = xprintf("写入 %s 失败：%s", full, sk_strerror(errno));
            } else {
                res = xprintf("已修改 %s（现在 %zu 字节）", full, nlen);
                *is_error = 0;
            }
            free(out);
        }
        free(data);
    }
    free(full);
    return res;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char *list_directory(const jnode *in, int *is_error)
{
    const char *path = json_get_str(in, "path");
    char *full = resolve(path && *path ? path : "/root");
    DIR *d = opendir(full);
    if (!d) {
        *is_error = 1;
        char *r = xprintf("%s：%s", full, sk_strerror(errno));
        free(full);
        return r;
    }
    char **names = NULL;
    int n = 0, cap = 0, more = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n >= DIR_MAX) {
            more++;
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, sizeof *names * (size_t)cap);
            if (!names)
                abort();
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_names);

    size_t cap_s = 256 + (size_t)n * 64, len = 0;
    char *s = malloc(cap_s);
    if (!s)
        abort();
    len += (size_t)snprintf(s, cap_s, "%s 里有 %d 项：\n", full, n + more);
    for (int i = 0; i < n; i++) {
        char p[4200], extra[48] = "";
        snprintf(p, sizeof p, "%s/%s", strcmp(full, "/") ? full : "", names[i]);
        struct stat st;
        const char *kind = "";
        if (lstat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode)) kind = "/";
            else if (S_ISLNK(st.st_mode)) kind = "@";
            else {
                sk_fmt_size(extra, sizeof extra, (uint64_t)st.st_size);
                if (st.st_mode & 0111)
                    kind = "*";
            }
        }
        size_t need = strlen(names[i]) + strlen(extra) + 8;
        if (len + need >= cap_s) {
            cap_s = (cap_s + need) * 2;
            s = realloc(s, cap_s);
            if (!s)
                abort();
        }
        len += (size_t)snprintf(s + len, cap_s - len, "%s%s%s%s\n", names[i], kind, extra[0] ? "  " : "", extra);
        free(names[i]);
    }
    free(names);
    if (more) {
        s = realloc(s, len + 64);
        snprintf(s + len, 64, "……还有 %d 项没有列出\n", more);
    }
    free(full);
    return s;
}

/* ================================================================ open_app */

/* 在 apps.conf 里查应用 id；找到返回 1，name 里填显示名。顺便把所有 id 列进 ids（出错提示用） */
static int find_app(const char *id, char *name, size_t nn, char *ids, size_t idn)
{
    char *conf = sk_read_file(APPS_CONF, NULL);
    int found = 0;
    ids[0] = 0;
    if (!conf)
        return 0;
    for (char *line = strtok(conf, "\n"); line; line = strtok(NULL, "\n")) {
        char *t = sk_trim(line);
        if (!*t || *t == '#')
            continue;
        char *argv[4];
        if (sk_split_args(t, argv, 4) < 3)
            continue;
        if (ids[0])
            sk_strlcat(ids, "、", idn);
        sk_strlcat(ids, argv[0], idn);
        if (strcmp(argv[0], id) == 0) {
            found = 1;
            sk_strlcpy(name, argv[2], nn);
        }
    }
    free(conf);
    return found;
}

static char *open_app(const jnode *in, int *is_error)
{
    const char *id = json_get_str(in, "app");
    const char *arg = json_get_str(in, "argument");
    char name[64] = "", ids[512];
    *is_error = 1;
    if (!id || !*id)
        return strdup("缺少参数 app");
    if (!find_app(id, name, sizeof name, ids, sizeof ids))
        return xprintf("没有 id 为 \"%s\" 的应用。可用的应用：%s", id, ids[0] ? ids : "（读不到 apps.conf）");
    /* 我们自己也当一回“图形应用”：连上显示服务器，请它像双击桌面图标那样启动应用，再断开 */
    sk_app *app = sk_app_connect("skaid");
    if (!app)
        return strdup("连不上图形桌面（sketchd 没有运行？在 sketch.nogui 模式下没有图形界面）");
    sk_app_launch(app, id, arg && *arg ? arg : NULL);
    sk_app_disconnect(app);
    *is_error = 0;
    return xprintf("已在桌面上打开「%s」%s%s", name, arg && *arg ? "，参数：" : "", arg && *arg ? arg : "");
}

/* ================================================================ 分派 */

char *tools_describe(const char *name, const jnode *in)
{
    const char *path = json_get_str(in, "path");
    if (!strcmp(name, "run_command")) {
        const char *c = json_get_str(in, "command");
        char *l = first_line(c ? c : "", 70);
        char *r = xprintf("运行命令：%s", l);
        free(l);
        return r;
    }
    if (!strcmp(name, "read_file"))
        return xprintf("读取文件：%s", path ? path : "?");
    if (!strcmp(name, "write_file")) {
        jnode *c = json_get(in, "content");
        return xprintf("%s：%s（%zu 字节）", json_get_bool(in, "append", 0) ? "追加文件" : "写入文件",
                       path ? path : "?", c && c->t == J_STR ? c->slen : 0);
    }
    if (!strcmp(name, "edit_file"))
        return xprintf("修改文件：%s", path ? path : "?");
    if (!strcmp(name, "list_directory"))
        return xprintf("列出目录：%s", path && *path ? path : "/root");
    if (!strcmp(name, "open_app")) {
        const char *a = json_get_str(in, "argument");
        return xprintf("打开应用：%s%s%s", json_get_str(in, "app") ? json_get_str(in, "app") : "?",
                       a && *a ? " " : "", a && *a ? a : "");
    }
    return xprintf("调用工具：%s", name);
}

char *tools_run(const char *name, const jnode *in, int *is_error, char **summary)
{
    char *res;
    *is_error = 0;
    if (!in || in->t != J_OBJ) {
        *is_error = 1;
        res = strdup("工具参数不是一个 JSON 对象（可能被截断了），请重新调用");
    } else if (!strcmp(name, "run_command")) {
        res = run_command(in, is_error);
    } else if (!strcmp(name, "read_file")) {
        res = read_file(in, is_error);
    } else if (!strcmp(name, "write_file")) {
        res = write_file(in, is_error);
    } else if (!strcmp(name, "edit_file")) {
        res = edit_file(in, is_error);
    } else if (!strcmp(name, "list_directory")) {
        res = list_directory(in, is_error);
    } else if (!strcmp(name, "open_app")) {
        res = open_app(in, is_error);
    } else {
        *is_error = 1;
        res = xprintf("没有叫 %s 的工具", name);
    }
    if (summary) {
        /* 摘要：run_command 跳过“退出码 0”那一行，显示真正的第一行输出 */
        const char *s = res;
        if (!strncmp(s, "退出码 0\n", strlen("退出码 0\n")))
            s += strlen("退出码 0\n");
        *summary = first_line(s, 80);
    }
    return res;
}
