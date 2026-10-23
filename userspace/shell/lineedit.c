/*
 * lineedit.c — 交互式行编辑器 + 命令历史 + Tab 补全
 *
 * 终端的两种模式：
 *   规范模式(canonical/cooked)：终端驱动（内核的 tty 层）自己负责回显和行编辑，
 *     用户按回车后 read() 才返回一整行。Backspace 能用，但 ← → ↑ 都不能用。
 *   原始模式(raw)：每按一个键 read() 就返回，内核不回显、不处理 Ctrl-C（关掉 ISIG）。
 *     光标移动、历史、补全就全靠程序自己实现了——这就是这个文件做的事。
 * 用 tcgetattr/tcsetattr 修改 termios 结构切换模式；读完一行一定要恢复原来的模式，
 * 否则接下来运行的程序（比如 cat）会拿到一个处于原始模式的终端。
 *
 * 屏幕上的位置：提示符和输入内容可能超过一行宽，终端会自动折行；汉字占 2 列，
 * 如果一行只剩 1 列放不下汉字，终端会把它挪到下一行。所以每次重画时都要从
 * 头计算每个字符落在第几行第几列，再用 ESC [ n A（上移）ESC [ n C（右移）等
 * ANSI 转义序列把光标挪到正确的位置。终端宽度不知道时（串口通常是 0），
 * 就当作一行无限长。
 */
#include "sksh.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/* ======================= 历史记录 ======================= */

#define HIST_MAX 200

static char *hist[HIST_MAX];
static int   hist_n;
static long  hist_base = 1;     /* hist[0] 的编号（history 命令显示用） */
static char *hist_file;

static void hist_push(const char *line)
{
    if (hist_n == HIST_MAX) {
        free(hist[0]);
        memmove(hist, hist + 1, sizeof(char *) * (HIST_MAX - 1));
        hist_n--;
        hist_base++;
    }
    hist[hist_n++] = xstrdup(line);
}

void history_init(void)
{
    const char *hf = var_get("HISTFILE");
    const char *home = var_get("HOME");
    free(hist_file);
    hist_file = NULL;
    if (hf && *hf)
        hist_file = xstrdup(hf);
    else if (home && *home)
        hist_file = sk_strdup_printf("%s/.sksh_history", strcmp(home, "/") == 0 ? "" : home);
    if (!hist_file)
        return;
    size_t len;
    char *data = sk_read_file(hist_file, &len);
    if (!data)
        return;
    int total = 0;
    for (char *line = data; *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (*line) {
            hist_push(line);
            total++;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    free(data);
    hist_base = 1;
    /* 文件比内存里保存的多很多时，重写一次，免得它无限增长 */
    if (total > HIST_MAX + 100) {
        char *tmp = sk_strdup_printf("%s.tmp%d", hist_file, (int)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            int ok = 1;
            for (int i = 0; i < hist_n && ok; i++)
                ok = full_write(fd, hist[i], strlen(hist[i])) >= 0 && full_write(fd, "\n", 1) >= 0;
            close(fd);
            if (!ok || rename(tmp, hist_file) < 0)
                unlink(tmp);
        }
        free(tmp);
    }
}

void history_add(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return;
    char *copy = xstrdup(line);
    for (char *c = copy; *c; c++)
        if (*c == '\n')
            *c = ' ';
    if (hist_n > 0 && strcmp(hist[hist_n - 1], copy) == 0) {
        free(copy);
        return;         /* 和上一条相同就不重复记录 */
    }
    hist_push(copy);
    if (hist_file) {
        int fd = open(hist_file, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) {
            strbuf b;
            sb_init(&b);
            sb_puts(&b, copy);
            sb_putc(&b, '\n');
            full_write(fd, b.s, b.len);
            sb_free(&b);
            close(fd);
        }
    }
    free(copy);
}

int history_count(void)
{
    return hist_n;
}

int bi_history(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        for (int i = 0; i < hist_n; i++)
            free(hist[i]);
        hist_base += hist_n;
        hist_n = 0;
        if (hist_file) {
            int fd = open(hist_file, O_WRONLY | O_TRUNC | O_CLOEXEC);
            if (fd >= 0)
                close(fd);
        }
        return 0;
    }
    int start = 0;
    if (argc > 1) {
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (*end || n < 0) {
            sh_error("history: %s: 需要数字参数\n用法: history [n] | history -c", argv[1]);
            return 2;
        }
        if (n < hist_n)
            start = hist_n - (int)n;
    }
    for (int i = start; i < hist_n; i++)
        printf("%5ld  %s\n", hist_base + i, hist[i]);
    return 0;
}

/* ======================= 终端 ======================= */

enum {
    K_UP = 0x110000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL,
    K_WLEFT, K_WRIGHT, K_WDEL_FWD, K_WDEL_BACK, K_NONE,
};

typedef struct {
    strbuf buf;             /* 正在编辑的内容（UTF-8） */
    size_t pos;             /* 光标位置（字节偏移，总在字符边界上） */
    const char *prompt;
    const char *plast;      /* 提示符的最后一行（重画时只画这一行） */
    int    cols;            /* 终端宽度，0 = 未知（不折行） */
    int    cur_row, cur_col;/* 光标在屏幕上的位置（相对于提示符最后一行的开头） */
    strbuf out;             /* 攒起来一次写出，串口/网页模拟器里能快很多 */
    int    hidx;
    char  *draft;           /* 翻历史前正在输入的内容 */
    int    last_tab;
} ledit;

static char *killbuf;
static struct termios saved_tio;

int lineedit_available(void)
{
    struct termios t;
    const char *term = var_get("TERM");
    if (term && strcmp(term, "dumb") == 0)
        return 0;
    return isatty(STDIN_FILENO) && isatty(STDERR_FILENO) && tcgetattr(STDIN_FILENO, &t) == 0;
}

static int enter_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &saved_tio) < 0)
        return -1;
    struct termios raw = saved_tio;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INLCR | IGNCR | ISTRIP | IXON);
    raw.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    /* TCSADRAIN 而不是 TCSAFLUSH：用户提前敲进来的字符（type-ahead）不能丢 */
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) < 0)
        return -1;
    return 0;
}

static void leave_raw(void)
{
    tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_tio);
}

static int term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    const char *c = var_get("COLUMNS");
    if (c && atoi(c) > 0)
        return atoi(c);
    return 0;
}

static void flush_out(ledit *e)
{
    if (e->out.len) {
        full_write(STDERR_FILENO, e->out.s, e->out.len);
        sb_clear(&e->out);
    }
}

/* 每次只从终端读 1 个字节：回车之后已经敲进来（或粘贴进来）的内容可能属于下一条命令，
 * 甚至属于接下来运行的程序（比如 cat），必须留在内核的终端缓冲区里，不能被我们多读走。
 * 还有输入在排队时先不刷新输出，这样粘贴一长串时回显可以攒成一次 write。 */
static int input_pending(int timeout_ms)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    for (;;) {
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0 && errno == EINTR)
            continue;
        return r > 0;
    }
}

static int getbyte(ledit *e, int timeout_ms)
{
    if (!input_pending(0)) {
        flush_out(e);
        if (timeout_ms >= 0 && !input_pending(timeout_ms))
            return -2;      /* 超时：单独按了 ESC */
    }
    for (;;) {
        unsigned char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR) {
            if (g_sigint) {
                g_sigint = 0;
                return 3;   /* 别的进程给我们发了 SIGINT：当作 Ctrl-C */
            }
            continue;
        }
        if (r <= 0)
            return -1;      /* 终端挂断 / 文件结束 */
        return c;
    }
}

static int read_key(ledit *e)
{
    int c = getbyte(e, -1);
    if (c != 27)
        return c;
    int c2 = getbyte(e, 60);
    if (c2 < 0)
        return K_NONE;
    if (c2 == '[') {
        char params[16];
        int np = 0, f;
        for (;;) {
            f = getbyte(e, 60);
            if (f < 0)
                return K_NONE;
            if (f >= 0x40 && f <= 0x7E)
                break;
            if (np < (int)sizeof params - 1)
                params[np++] = (char)f;
        }
        params[np] = 0;
        int mod = strchr(params, ';') != NULL;      /* 1;5C = Ctrl-→ */
        switch (f) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return mod ? K_WRIGHT : K_RIGHT;
        case 'D': return mod ? K_WLEFT : K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case '~':
            switch (atoi(params)) {
            case 1: case 7: return K_HOME;
            case 4: case 8: return K_END;
            case 3: return K_DEL;
            default: return K_NONE;
            }
        default:
            return K_NONE;
        }
    }
    if (c2 == 'O') {
        int f = getbyte(e, 60);
        switch (f) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        default: return K_NONE;
        }
    }
    switch (c2) {           /* Alt-键：终端发送 ESC + 键 */
    case 'b': case 'B': return K_WLEFT;
    case 'f': case 'F': return K_WRIGHT;
    case 'd': case 'D': return K_WDEL_FWD;
    case 127: case 8: return K_WDEL_BACK;
    default: return K_NONE;
    }
}

/* ======================= 屏幕位置计算 ======================= */

static size_t skip_esc(const char *s, size_t n, size_t i)
{
    i++;
    if (i < n && s[i] == '[') {
        i++;
        while (i < n && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E))
            i++;
        return i < n ? i + 1 : n;
    }
    if (i < n && s[i] == ']') {
        while (i < n && s[i] != '\a')
            i++;
        return i < n ? i + 1 : n;
    }
    return i < n ? i + 1 : n;
}

/* 从 (row,col) 开始“写” s 之后到达的位置。col 可能等于 cols（停在行尾，等待折行） */
static void advance(const char *s, size_t n, int cols, int *row, int *col)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            i = skip_esc(s, n, i);
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            i++;
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        int w = cp_width(cp);
        if (cols > 0 && w > 0 && *col + w > cols) {
            (*row)++;           /* 放不下：终端把它挪到下一行 */
            *col = 0;
        }
        *col += w;
        i += (size_t)k;
    }
}

/* 光标处的字符会显示在哪里 */
static void pos_of(ledit *e, size_t pos, int *row, int *col)
{
    *row = 0;
    *col = 0;
    advance(e->plast, strlen(e->plast), e->cols, row, col);
    advance(e->buf.s ? e->buf.s : "", pos, e->cols, row, col);
    if (e->cols > 0) {
        int w = 0;
        if (pos < e->buf.len) {
            uint32_t cp;
            utf8_decode(e->buf.s + pos, e->buf.len - pos, &cp);
            w = cp_width(cp);
        }
        if (*col >= e->cols || (w > 0 && *col + w > e->cols)) {
            (*row)++;
            *col = 0;
        }
    }
}

static void move_to(ledit *e, int row, int col)
{
    if (row < e->cur_row)
        sb_printf(&e->out, "\033[%dA", e->cur_row - row);
    else if (row > e->cur_row)
        sb_printf(&e->out, "\033[%dB", row - e->cur_row);
    if (col != e->cur_col || row != e->cur_row) {
        sb_putc(&e->out, '\r');
        if (col > 0)
            sb_printf(&e->out, "\033[%dC", col);
    }
    e->cur_row = row;
    e->cur_col = col;
}

/* 整行重画：回到提示符开头，清掉下面的内容，重新输出提示符最后一行和输入内容 */
static void refresh(ledit *e)
{
    if (e->cur_row > 0)
        sb_printf(&e->out, "\033[%dA", e->cur_row);
    sb_puts(&e->out, "\r\033[J");
    sb_puts(&e->out, e->plast);
    sb_putn(&e->out, sb_str(&e->buf), e->buf.len);
    int row = 0, col = 0;
    advance(e->plast, strlen(e->plast), e->cols, &row, &col);
    advance(sb_str(&e->buf), e->buf.len, e->cols, &row, &col);
    if (e->cols > 0 && col >= e->cols) {
        sb_puts(&e->out, "\r\n");   /* 恰好写满一行：主动换行，让终端状态和计算一致 */
        row++;
        col = 0;
    }
    e->cur_row = row;
    e->cur_col = col;
    int tr, tc;
    pos_of(e, e->pos, &tr, &tc);
    move_to(e, tr, tc);
}

static void place_cursor(ledit *e)
{
    int r, c;
    pos_of(e, e->pos, &r, &c);
    move_to(e, r, c);
}

/* 在提示符之前的那些行原样输出（多行提示符），然后从最后一行开始重画 */
static void redraw_full(ledit *e)
{
    size_t head = (size_t)(e->plast - e->prompt);
    sb_putn(&e->out, e->prompt, head);
    e->cur_row = 0;
    e->cur_col = 0;
    refresh(e);
}

/* ======================= 编辑操作 ======================= */

static size_t prev_char(const strbuf *b, size_t pos)
{
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)b->s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static size_t next_char(const strbuf *b, size_t pos)
{
    if (pos >= b->len)
        return b->len;
    pos += (size_t)utf8_seqlen((unsigned char)b->s[pos]);
    return pos > b->len ? b->len : pos;
}

static int is_word_byte(unsigned char c)
{
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static void buf_insert(ledit *e, const char *s, size_t n)
{
    strbuf *b = &e->buf;
    size_t old = b->len;
    sb_putn(b, s, n);                   /* 先扩容 */
    memmove(b->s + e->pos + n, b->s + e->pos, old - e->pos);
    memcpy(b->s + e->pos, s, n);
    b->s[b->len] = 0;
    e->pos += n;
}

static void buf_delete(ledit *e, size_t from, size_t to)
{
    strbuf *b = &e->buf;
    if (to <= from)
        return;
    memmove(b->s + from, b->s + to, b->len - to);
    sb_setlen(b, b->len - (to - from));
    if (e->pos > to)
        e->pos -= to - from;
    else if (e->pos > from)
        e->pos = from;
}

static void set_kill(const char *s, size_t n)
{
    free(killbuf);
    killbuf = xstrndup(s, n);
}

static void insert_text(ledit *e, const char *s, size_t n)
{
    int at_end = e->pos == e->buf.len;
    buf_insert(e, s, n);
    if (at_end) {
        /* 最常见的情况：在行尾打字，只输出这几个字符，不重画 */
        int r = e->cur_row, c = e->cur_col;
        advance(s, n, e->cols, &r, &c);
        sb_putn(&e->out, s, n);
        if (e->cols > 0 && c >= e->cols) {
            sb_puts(&e->out, "\r\n");
            r++;
            c = 0;
        }
        e->cur_row = r;
        e->cur_col = c;
        return;
    }
    refresh(e);
}

static void set_line(ledit *e, const char *s)
{
    sb_clear(&e->buf);
    sb_puts(&e->buf, s);
    e->pos = e->buf.len;
    refresh(e);
}

static void hist_move(ledit *e, int dir)
{
    int ni = e->hidx + dir;
    if (ni < 0 || ni > hist_n) {
        sb_putc(&e->out, '\a');
        return;
    }
    if (e->hidx == hist_n) {
        free(e->draft);
        e->draft = xstrdup(sb_str(&e->buf));
    }
    e->hidx = ni;
    set_line(e, ni == hist_n ? (e->draft ? e->draft : "") : hist[ni]);
}

/* ======================= Tab 补全 ======================= */

typedef struct {
    strvec names;
    strbuf isdir;       /* 每个候选一个字节：1 = 目录 */
} cands;

static void cand_add(cands *c, const char *name, int dir)
{
    sv_push(&c->names, xstrdup(name));
    sb_putc(&c->isdir, dir ? 1 : 0);
}

static void cand_add_cb(const char *name, const char *value, int flags, void *ud)
{
    void **a = ud;
    const char *prefix = a[1];
    if (strncmp(name, prefix, strlen(prefix)) == 0)
        cand_add(a[0], name, 0);
}

static void cand_add_func(shfunc *f, void *ud)
{
    cand_add_cb(f->name, NULL, 0, ud);
}

static void complete_files(cands *c, const char *prefix, int exec_only)
{
    const char *slash = strrchr(prefix, '/');
    char *dirpart = slash ? xstrndup(prefix, (size_t)(slash - prefix) + 1) : xstrdup("");
    const char *base = slash ? slash + 1 : prefix;
    char *opendir_path;
    if (dirpart[0] == '~') {
        const char *rest = strchr(dirpart, '/');
        char *user = xstrndup(dirpart + 1, (size_t)(rest - dirpart - 1));
        char *home = tilde_lookup(user);
        free(user);
        opendir_path = home ? sk_strdup_printf("%s%s", home, rest) : xstrdup(dirpart);
        free(home);
    } else {
        opendir_path = xstrdup(*dirpart ? dirpart : ".");
    }
    DIR *d = opendir(opendir_path);
    if (d) {
        struct dirent *de;
        size_t bl = strlen(base);
        while ((de = readdir(d)) != NULL) {
            const char *n = de->d_name;
            if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
                continue;
            if (n[0] == '.' && base[0] != '.')
                continue;
            if (strncmp(n, base, bl) != 0)
                continue;
            char *full = sk_strdup_printf("%s/%s", opendir_path, n);
            struct stat st;
            int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            int ok = !exec_only || isdir || access(full, X_OK) == 0;
            free(full);
            if (ok)
                cand_add(c, n, isdir);
        }
        closedir(d);
    }
    free(opendir_path);
    free(dirpart);
}

static void complete_commands(cands *c, const char *prefix)
{
    size_t pl = strlen(prefix);
    for (int i = 0; i < builtin_count(); i++)
        if (strncmp(builtin_name(i), prefix, pl) == 0)
            cand_add(c, builtin_name(i), 0);
    static const char *const kws[] = { "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac", "function" };
    for (size_t i = 0; i < SK_ARRAY_LEN(kws); i++)
        if (pl > 0 && strncmp(kws[i], prefix, pl) == 0)
            cand_add(c, kws[i], 0);
    void *ud[2] = { c, (void *)prefix };
    alias_foreach_sorted(cand_add_cb, ud);
    func_foreach(cand_add_func, ud);
    const char *path = var_get("PATH");
    if (!path)
        return;
    const char *p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t dl = colon ? (size_t)(colon - p) : strlen(p);
        char *dir = dl ? xstrndup(p, dl) : xstrdup(".");
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.' || strncmp(de->d_name, prefix, pl) != 0)
                    continue;
                char *full = sk_strdup_printf("%s/%s", dir, de->d_name);
                struct stat st;
                if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0)
                    cand_add(c, de->d_name, 0);
                free(full);
            }
            closedir(d);
        }
        free(dir);
        if (!colon)
            break;
        p = colon + 1;
    }
}

/* 排序去重（isdir 跟着名字一起移动） */
static void cands_sort(cands *c)
{
    int n = c->names.n;
    for (int i = 1; i < n; i++) {           /* 插入排序：候选通常不多 */
        char *s = c->names.v[i];
        char d = c->isdir.s[i];
        int j = i - 1;
        while (j >= 0 && strcmp(c->names.v[j], s) > 0) {
            c->names.v[j + 1] = c->names.v[j];
            c->isdir.s[j + 1] = c->isdir.s[j];
            j--;
        }
        c->names.v[j + 1] = s;
        c->isdir.s[j + 1] = d;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k > 0 && strcmp(c->names.v[k - 1], c->names.v[i]) == 0) {
            free(c->names.v[i]);
            continue;
        }
        c->names.v[k] = c->names.v[i];
        c->isdir.s[k] = c->isdir.s[i];
        k++;
    }
    c->names.n = k;
    if (c->names.v)
        c->names.v[k] = NULL;
}

static void escape_into(strbuf *out, const char *s, size_t n, int in_sq, int in_dq)
{
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (in_sq) {
            /* 单引号里什么都不用转义 */
        } else if (in_dq) {
            if (strchr("$`\"\\", ch))
                sb_putc(out, '\\');
        } else if (strchr(" \t\n\\'\"`$&|;<>()*?[]#!{}", ch)) {
            sb_putc(out, '\\');
        }
        sb_putc(out, ch);
    }
}

static void list_cands(ledit *e, cands *c)
{
    int n = c->names.n;
    size_t save = e->pos;           /* 先把光标移到输入内容的末尾，再另起一行列出候选 */
    e->pos = e->buf.len;
    place_cursor(e);
    e->pos = save;
    sb_puts(&e->out, "\r\n");
    if (n > 100) {
        sb_printf(&e->out, "显示全部 %d 个可能的补全？(y/n) ", n);
        int k = read_key(e);
        sb_puts(&e->out, "\r\n");
        if (k != 'y' && k != 'Y') {
            redraw_full(e);
            return;
        }
    }
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = str_width(c->names.v[i], strlen(c->names.v[i])) + (c->isdir.s[i] ? 1 : 0);
        if (w > maxw)
            maxw = w;
    }
    int width = e->cols > 0 ? e->cols : 80;
    int per_row = width / (maxw + 2);
    if (per_row < 1)
        per_row = 1;
    int rows = (n + per_row - 1) / per_row;
    for (int r = 0; r < rows; r++) {
        for (int k = 0; k < per_row; k++) {
            int idx = k * rows + r;     /* 按列排列，和 ls 一样 */
            if (idx >= n)
                break;
            const char *name = c->names.v[idx];
            sb_puts(&e->out, name);
            int w = str_width(name, strlen(name));
            if (c->isdir.s[idx]) {
                sb_putc(&e->out, '/');
                w++;
            }
            if ((k + 1) * rows + r < n)
                for (; w < maxw + 2; w++)
                    sb_putc(&e->out, ' ');
        }
        sb_puts(&e->out, "\r\n");
    }
    redraw_full(e);
}

static void complete(ledit *e)
{
    const char *b = sb_str(&e->buf);
    /* 找到光标所在单词的开头，同时记录引号状态 */
    size_t ws = 0;
    int sq = 0, dq = 0;
    for (size_t i = 0; i < e->pos; i++) {
        char ch = b[i];
        if (sq) {
            if (ch == '\'')
                sq = 0;
            continue;
        }
        if (ch == '\\') {
            i++;
            continue;
        }
        if (dq) {
            if (ch == '"')
                dq = 0;
            continue;
        }
        if (ch == '\'')
            sq = 1;
        else if (ch == '"')
            dq = 1;
        else if (strchr(" \t\n;&|<>()`", ch))
            ws = i + 1;
    }
    /* 去掉引号和反斜杠，得到真正要匹配的前缀 */
    strbuf pre;
    sb_init(&pre);
    sb_putn(&pre, "", 0);
    {
        int q1 = 0, q2 = 0;
        for (size_t i = ws; i < e->pos; i++) {
            char ch = b[i];
            if (q1) {
                if (ch == '\'') q1 = 0;
                else sb_putc(&pre, ch);
            } else if (ch == '\\' && i + 1 < e->pos) {
                sb_putc(&pre, b[++i]);
            } else if (q2) {
                if (ch == '"') q2 = 0;
                else sb_putc(&pre, ch);
            } else if (ch == '\'') {
                q1 = 1;
            } else if (ch == '"') {
                q2 = 1;
            } else {
                sb_putc(&pre, ch);
            }
        }
    }
    /* 是不是命令名的位置：前面是行首、; | & ( 或者 then/do 之类的关键字 */
    int cmdpos = 1;
    {
        size_t k = ws;
        while (k > 0 && (b[k - 1] == ' ' || b[k - 1] == '\t'))
            k--;
        if (k > 0 && !strchr(";&|(`\n", b[k - 1])) {
            size_t s = k;
            while (s > 0 && !strchr(" \t;&|(`", b[s - 1]))
                s--;
            static const char *const kw[] = { "then", "do", "else", "elif", "if", "while", "until", "!", "time", "exec", "command" };
            cmdpos = 0;
            for (size_t i = 0; i < SK_ARRAY_LEN(kw); i++)
                if (k - s == strlen(kw[i]) && memcmp(b + s, kw[i], k - s) == 0)
                    cmdpos = 1;
        }
    }
    const char *prefix = sb_str(&pre);
    cands c;
    sv_init(&c.names);
    sb_init(&c.isdir);
    int var_mode = 0;
    const char *match_base;
    if (prefix[0] == '$' && !strchr(prefix, '/')) {
        var_mode = prefix[1] == '{' ? 2 : 1;
        const char *vp = prefix + var_mode;
        void *ud[2] = { &c, (void *)vp };
        var_foreach_sorted(cand_add_cb, ud);
        match_base = vp;
    } else if (cmdpos && !strchr(prefix, '/') && prefix[0] != '~') {
        complete_commands(&c, prefix);
        match_base = prefix;
    } else {
        complete_files(&c, prefix, cmdpos);
        const char *slash = strrchr(prefix, '/');
        match_base = slash ? slash + 1 : prefix;
    }
    cands_sort(&c);
    int n = c.names.n;
    if (n == 0) {
        sb_putc(&e->out, '\a');
    } else {
        /* 所有候选的公共前缀 */
        size_t common = strlen(c.names.v[0]);
        for (int i = 1; i < n; i++) {
            size_t k = 0;
            while (k < common && c.names.v[i][k] == c.names.v[0][k])
                k++;
            common = k;
        }
        while (common > 0 && ((unsigned char)c.names.v[0][common] & 0xC0) == 0x80)
            common--;           /* 不要停在汉字的半中间 */
        size_t bl = strlen(match_base);
        strbuf ins;
        sb_init(&ins);
        if (common > bl)
            escape_into(&ins, c.names.v[0] + bl, common - bl, sq, dq);
        if (n == 1) {
            if (var_mode == 2)
                sb_putc(&ins, '}');
            if (c.isdir.s[0]) {
                sb_putc(&ins, '/');
            } else {
                if (sq) sb_putc(&ins, '\'');
                if (dq) sb_putc(&ins, '"');
                sb_putc(&ins, ' ');
            }
        }
        if (ins.len) {
            insert_text(e, ins.s, ins.len);
        } else if (e->last_tab) {
            list_cands(e, &c);
        } else {
            sb_putc(&e->out, '\a');
        }
        sb_free(&ins);
    }
    sv_free(&c.names);
    sb_free(&c.isdir);
    sb_free(&pre);
}

/* ======================= 主循环 ======================= */

static int plain_read(const char *prompt, strbuf *out)
{
    full_write(STDERR_FILENO, prompt, strlen(prompt));
    for (;;) {
        char ch;
        ssize_t r = read(STDIN_FILENO, &ch, 1);
        if (r < 0 && errno == EINTR) {
            if (g_sigint) {
                g_sigint = 0;
                return LE_INTR;
            }
            continue;
        }
        if (r <= 0)
            return out->len ? LE_OK : LE_EOF;
        if (ch == '\n')
            return LE_OK;
        sb_putc(out, ch);
    }
}

int lineedit_read(const char *prompt, strbuf *out)
{
    if (enter_raw() < 0)
        return plain_read(prompt, out);
    ledit e;
    memset(&e, 0, sizeof e);
    sb_init(&e.buf);
    sb_init(&e.out);
    sb_putn(&e.buf, "", 0);
    e.prompt = prompt;
    const char *nl = strrchr(prompt, '\n');
    e.plast = nl ? nl + 1 : prompt;
    e.cols = term_cols();
    e.hidx = hist_n;
    sb_puts(&e.out, prompt);
    advance(e.plast, strlen(e.plast), e.cols, &e.cur_row, &e.cur_col);
    if (e.cols > 0 && e.cur_col >= e.cols) {
        sb_puts(&e.out, "\r\n");
        e.cur_row++;
        e.cur_col = 0;
    }
    int result = LE_OK;
    for (;;) {
        int k = read_key(&e);
        if (k == -1) {
            result = LE_EOF;
            break;
        }
        int was_tab = e.last_tab;
        e.last_tab = 0;
        if (k == '\r' || k == '\n') {
            e.pos = e.buf.len;
            place_cursor(&e);
            sb_puts(&e.out, "\r\n");
            break;
        }
        switch (k) {
        case 3:                 /* Ctrl-C：放弃这一行 */
            e.pos = e.buf.len;
            place_cursor(&e);
            sb_puts(&e.out, "^C\r\n");
            result = LE_INTR;
            goto done;
        case 4:                 /* Ctrl-D */
            if (e.buf.len == 0) {
                result = LE_EOF;
                goto done;
            }
            /* fallthrough */
        case K_DEL:
            if (e.pos < e.buf.len) {
                buf_delete(&e, e.pos, next_char(&e.buf, e.pos));
                refresh(&e);
            }
            break;
        case 8:
        case 127:               /* 退格 */
            if (e.pos > 0) {
                buf_delete(&e, prev_char(&e.buf, e.pos), e.pos);
                refresh(&e);
            }
            break;
        case 1:
        case K_HOME:
            e.pos = 0;
            place_cursor(&e);
            break;
        case 5:
        case K_END:
            e.pos = e.buf.len;
            place_cursor(&e);
            break;
        case 2:
        case K_LEFT:
            e.pos = prev_char(&e.buf, e.pos);
            place_cursor(&e);
            break;
        case 6:
        case K_RIGHT:
            e.pos = next_char(&e.buf, e.pos);
            place_cursor(&e);
            break;
        case K_WLEFT:
            while (e.pos > 0 && !is_word_byte((unsigned char)e.buf.s[e.pos - 1]))
                e.pos = prev_char(&e.buf, e.pos);
            while (e.pos > 0 && is_word_byte((unsigned char)e.buf.s[e.pos - 1]))
                e.pos = prev_char(&e.buf, e.pos);
            place_cursor(&e);
            break;
        case K_WRIGHT:
            while (e.pos < e.buf.len && !is_word_byte((unsigned char)e.buf.s[e.pos]))
                e.pos = next_char(&e.buf, e.pos);
            while (e.pos < e.buf.len && is_word_byte((unsigned char)e.buf.s[e.pos]))
                e.pos = next_char(&e.buf, e.pos);
            place_cursor(&e);
            break;
        case 21:                /* Ctrl-U：删除到行首 */
            if (e.pos > 0) {
                set_kill(e.buf.s, e.pos);
                buf_delete(&e, 0, e.pos);
                refresh(&e);
            }
            break;
        case 11:                /* Ctrl-K：删除到行尾 */
            if (e.pos < e.buf.len) {
                set_kill(e.buf.s + e.pos, e.buf.len - e.pos);
                sb_setlen(&e.buf, e.pos);
                refresh(&e);
            }
            break;
        case 23: {              /* Ctrl-W：删除前一个（空白分隔的）单词 */
            size_t s = e.pos;
            while (s > 0 && (e.buf.s[s - 1] == ' ' || e.buf.s[s - 1] == '\t'))
                s--;
            while (s > 0 && e.buf.s[s - 1] != ' ' && e.buf.s[s - 1] != '\t')
                s--;
            if (s < e.pos) {
                set_kill(e.buf.s + s, e.pos - s);
                buf_delete(&e, s, e.pos);
                refresh(&e);
            }
            break;
        }
        case K_WDEL_BACK: {
            size_t s = e.pos;
            while (s > 0 && !is_word_byte((unsigned char)e.buf.s[s - 1]))
                s--;
            while (s > 0 && is_word_byte((unsigned char)e.buf.s[s - 1]))
                s--;
            if (s < e.pos) {
                set_kill(e.buf.s + s, e.pos - s);
                buf_delete(&e, s, e.pos);
                refresh(&e);
            }
            break;
        }
        case K_WDEL_FWD: {
            size_t t = e.pos;
            while (t < e.buf.len && !is_word_byte((unsigned char)e.buf.s[t]))
                t++;
            while (t < e.buf.len && is_word_byte((unsigned char)e.buf.s[t]))
                t++;
            if (t > e.pos) {
                set_kill(e.buf.s + e.pos, t - e.pos);
                buf_delete(&e, e.pos, t);
                refresh(&e);
            }
            break;
        }
        case 25:                /* Ctrl-Y：粘贴 */
            if (killbuf && *killbuf)
                insert_text(&e, killbuf, strlen(killbuf));
            break;
        case 12:                /* Ctrl-L：清屏 */
            sb_puts(&e.out, "\033[H\033[2J");
            e.cols = term_cols();
            redraw_full(&e);
            break;
        case 16:
        case K_UP:
            hist_move(&e, -1);
            break;
        case 14:
        case K_DOWN:
            hist_move(&e, 1);
            break;
        case '\t':
            e.last_tab = was_tab;
            complete(&e);
            e.last_tab = 1;
            break;
        case K_NONE:
            break;
        default:
            if (k >= 0x80 && k < 0x100) {
                /* UTF-8 多字节字符：把后续字节一起读进来 */
                char tmp[4];
                int n = utf8_seqlen((unsigned char)k), got = 1;
                tmp[0] = (char)k;
                while (got < n) {
                    int c2 = getbyte(&e, 200);
                    if (c2 < 0 || (c2 & 0xC0) != 0x80)
                        break;
                    tmp[got++] = (char)c2;
                }
                if (got == n && n > 1)
                    insert_text(&e, tmp, (size_t)n);
            } else if (k >= 0x20 && k < 0x7F) {
                char ch = (char)k;
                insert_text(&e, &ch, 1);
            }
            break;
        }
    }
done:
    flush_out(&e);
    leave_raw();
    sb_putn(out, sb_str(&e.buf), e.buf.len);
    sb_free(&e.buf);
    sb_free(&e.out);
    free(e.draft);
    return result;
}
