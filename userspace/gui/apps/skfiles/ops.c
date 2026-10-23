/*
 * ops.c — skfiles 的文件系统操作（和界面无关，只用系统调用）
 *
 * 文件管理器归根到底只是把几个系统调用包装成了鼠标操作：
 *
 *   看文件夹里有什么   opendir / readdir（底层是 getdents64 系统调用）
 *   看一个文件的属性   lstat / stat / fstatat（读 inode 里的信息）
 *   新建文件夹         mkdir             新建空文件   open(O_CREAT | O_EXCL)
 *   重命名             rename            删除         unlink（文件）、rmdir（空目录）
 *   复制               open + read/write 循环
 *
 * “复制文件”在 Unix 里并不是一个系统调用：内核只认识“打开、读、写、关闭”，
 * 复制就是从一个文件读出来、写进另一个文件。（Linux 后来加了 copy_file_range 之类的
 * 捷径让数据不必经过用户态，这里为了讲清楚原理，用最朴素的 read/write 循环。）
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sketch/theme.h>
#include "skutil.h"
#include "skfiles.h"

/* ================================================================ 分类：图标和“类型”列 */

/* 按扩展名认文件。真正的系统（file 命令、桌面环境）还会读文件开头的“魔数”，
 * 但那要为每个文件打开一次——几百个文件的文件夹（例如 /proc）在慢速模拟器里会卡，
 * 所以列表里只看名字；双击打开时才真正读一下内容（见 fm_sniff_text）。 */
static const struct {
    const char *ext;
    const char *type;
    int         icon;
} ext_types[] = {
    { "txt", "文本文件", SK_ICON_TEXT },      { "md", "Markdown 文档", SK_ICON_TEXT },
    { "c", "C 源代码", SK_ICON_TEXT },        { "h", "C 头文件", SK_ICON_TEXT },
    { "s", "汇编源代码", SK_ICON_TEXT },      { "sh", "Shell 脚本", SK_ICON_TEXT },
    { "py", "Python 脚本", SK_ICON_TEXT },    { "conf", "配置文件", SK_ICON_TEXT },
    { "cfg", "配置文件", SK_ICON_TEXT },      { "ini", "配置文件", SK_ICON_TEXT },
    { "log", "日志", SK_ICON_TEXT },          { "json", "JSON 数据", SK_ICON_TEXT },
    { "csv", "表格数据", SK_ICON_TEXT },      { "xml", "XML 文档", SK_ICON_TEXT },
    { "html", "网页", SK_ICON_TEXT },         { "css", "样式表", SK_ICON_TEXT },
    { "js", "JavaScript 脚本", SK_ICON_TEXT }, { "mk", "Makefile", SK_ICON_TEXT },
    { "ppm", "PPM 图片", SK_ICON_IMAGE },     { "pgm", "PGM 图片", SK_ICON_IMAGE },
    { "pbm", "PBM 图片", SK_ICON_IMAGE },     { "png", "PNG 图片", SK_ICON_IMAGE },
    { "bmp", "BMP 图片", SK_ICON_IMAGE },     { "jpg", "JPEG 图片", SK_ICON_IMAGE },
    { "jpeg", "JPEG 图片", SK_ICON_IMAGE },   { "gif", "GIF 图片", SK_ICON_IMAGE },
    { "skf", "SketchOS 字体", SK_ICON_FILE }, { "gz", "gzip 压缩包", SK_ICON_FILE },
    { "tar", "tar 归档", SK_ICON_FILE },      { "cpio", "cpio 归档", SK_ICON_FILE },
    { "o", "目标文件", SK_ICON_FILE },        { "a", "静态库", SK_ICON_FILE },
};

/* 没有扩展名但大家都知道是文本的文件 */
static const char *const text_names[] = { "README", "Makefile", "LICENSE", "COPYING", "TODO", NULL };

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
/* 空格和 ASCII 标点（排序的第一轮忽略它们） */
static int is_punct(int c) { return c < 0x80 && !is_digit(c) && !(lower(c) >= 'a' && lower(c) <= 'z'); }

/* 扩展名（不含点）；没有扩展名（或者整个名字就是 .xxx 这种隐藏文件）返回 NULL */
static const char *extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1])
        return NULL;
    return dot + 1;
}

static int ext_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int find_ext(const char *name)
{
    const char *ext = extension(name);
    if (!ext)
        return -1;
    for (size_t i = 0; i < SK_ARRAY_LEN(ext_types); i++)
        if (ext_equal(ext, ext_types[i].ext))
            return (int)i;
    return -1;
}

int fm_is_image_name(const char *name)
{
    const char *ext = extension(name);
    return ext && (ext_equal(ext, "ppm") || ext_equal(ext, "pgm") || ext_equal(ext, "pbm"));
}

/* 根据（跟随链接之后的）类型和名字，决定图标和“类型”列的文字。
 * name 是用来看扩展名的名字：普通文件就是自己的名字，符号链接用它指向的那个文件的名字 */
static void classify(fm_entry *e, const char *name, mode_t mode)
{
    const char *type = "文件";
    int icon = SK_ICON_FILE;
    if (S_ISDIR(mode)) {
        type = "文件夹";
        icon = SK_ICON_FOLDER;
    } else if (S_ISCHR(mode)) {
        type = "字符设备";              /* /dev/tty、/dev/null：按字节流读写的设备 */
        icon = SK_ICON_CHIP;
    } else if (S_ISBLK(mode)) {
        type = "块设备";                /* 磁盘：按固定大小的块读写 */
        icon = SK_ICON_CHIP;
    } else if (S_ISFIFO(mode)) {
        type = "命名管道";
    } else if (S_ISSOCK(mode)) {
        type = "套接字";                /* 例如 /run/sketch/display.sock */
    } else if (S_ISREG(mode)) {
        int k = find_ext(name);
        int exec = (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
        if (k >= 0) {
            type = ext_types[k].type;
            icon = ext_types[k].icon;
            if (exec && icon != SK_ICON_IMAGE)
                icon = SK_ICON_PROGRAM;  /* 可执行的脚本：双击就运行它 */
        } else if (exec) {
            type = "可执行程序";
            icon = SK_ICON_PROGRAM;
        } else {
            for (int i = 0; text_names[i]; i++)
                if (strcmp(name, text_names[i]) == 0) {
                    type = "文本文件";
                    icon = SK_ICON_TEXT;
                }
            const char *ext = extension(name);
            if (icon == SK_ICON_FILE && ext && strlen(ext) <= 6) {
                /* 不认识的扩展名：显示成“ZIP 文件”这样 */
                char up[8];
                size_t i;
                for (i = 0; ext[i] && i < sizeof up - 1; i++)
                    up[i] = (char)((ext[i] >= 'a' && ext[i] <= 'z') ? ext[i] - 32 : ext[i]);
                up[i] = 0;
                snprintf(e->type_str, sizeof e->type_str, "%s 文件", up);
                e->icon = icon;
                return;
            }
        }
    }
    sk_strlcpy(e->type_str, type, sizeof e->type_str);
    e->icon = icon;
}

/* ================================================================ 读目录 */

/*
 * 目录在 Linux 里也是一种文件，它的内容是一张“名字 → inode 号”的表。readdir 只给出名字
 * （和一个可能不准的类型提示 d_type），大小、时间、权限都在 inode 里，要再用 stat 去问。
 *
 * 这里用 fstatat(目录fd, 名字, ...)：路径相对于一个已经打开的目录来解析，
 * 不用每次拼出完整路径、让内核从根目录开始一层层重新查找——这就是 *at 系列系统调用的意义。
 *
 * lstat 和 stat 的区别：遇到符号链接时，lstat 看链接本身，stat 看它指向的东西。
 * 显示时我们要知道“这是个链接”（lstat），决定双击做什么时要看它指向什么（stat）。
 */
static void fill_entry(fm_entry *e, int dfd, const struct stat *lst)
{
    e->lmode = lst->st_mode;
    e->is_link = S_ISLNK(lst->st_mode);
    e->hidden = e->name[0] == '.';
    struct stat st = *lst;
    if (e->is_link && fstatat(dfd, e->name, &st, 0) != 0) {
        /* 失效的链接（指向的文件不存在），或者没有权限看目标 */
        e->broken = 1;
        sk_strlcpy(e->type_str, "失效的链接", sizeof e->type_str);
        e->icon = SK_ICON_FILE;
        e->size = 0;
        e->mtime = lst->st_mtime;
    } else {
        e->is_dir = S_ISDIR(st.st_mode);
        e->size = (long long)st.st_size;
        e->mtime = st.st_mtime;
        /* 链接的“类型”看它指向的文件叫什么（link → notes.txt 是文本文件）。
         * readlinkat 只对链接才调用，普通文件不多花一次系统调用 */
        char target[FM_DEEP_PATH];
        const char *kind_name = e->name;
        ssize_t tl = e->is_link ? readlinkat(dfd, e->name, target, sizeof target - 1) : -1;
        if (tl > 0) {
            target[tl] = 0;
            const char *slash = strrchr(target, '/');
            kind_name = slash && slash[1] ? slash + 1 : target;
        }
        classify(e, kind_name, st.st_mode);
        if (e->is_link) {
            /* “文件夹（链接）”：一眼看出它是个快捷方式 */
            size_t len = strlen(e->type_str);
            snprintf(e->type_str + len, sizeof e->type_str - len, "（链接）");
        }
        if (S_ISREG(st.st_mode))
            sk_fmt_size(e->size_str, sizeof e->size_str, (uint64_t)st.st_size);
    }
    struct tm tm;
    if (localtime_r(&e->mtime, &tm))
        strftime(e->time_str, sizeof e->time_str, "%Y-%m-%d %H:%M", &tm);
}

int fm_list_dir(const char *dir, fm_entry **out, int *count)
{
    *out = NULL;
    *count = 0;
    DIR *dp = opendir(dir);
    if (!dp)
        return errno;
    int dfd = dirfd(dp);
    fm_entry *ents = NULL;
    int n = 0, cap = 0, err = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dp);
        if (!de) {
            err = errno;            /* NULL + errno==0 表示读完了 */
            break;
        }
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;               /* “.”是自己，“..”是上一级：每个目录里都有，不显示 */
        struct stat lst;
        int have = fstatat(dfd, nm, &lst, AT_SYMLINK_NOFOLLOW) == 0;
        if (!have && errno == ENOENT)
            continue;               /* 读目录和 stat 之间它被删掉了（/proc 里进程退出就会这样） */
        if (n == cap) {
            int ncap = cap ? cap * 2 : 64;
            fm_entry *ne = realloc(ents, (size_t)ncap * sizeof *ne);
            if (!ne) {
                err = ENOMEM;
                break;
            }
            ents = ne;
            cap = ncap;
        }
        fm_entry *e = &ents[n];
        memset(e, 0, sizeof *e);
        e->name = strdup(nm);
        if (!e->name) {
            err = ENOMEM;
            break;
        }
        n++;
        if (have) {
            fill_entry(e, dfd, &lst);
        } else {
            /* 连 lstat 都失败了（例如权限不够）：只知道名字，用 d_type 猜一下是不是目录 */
            e->hidden = nm[0] == '.';
            e->is_dir = de->d_type == DT_DIR;
            e->broken = !e->is_dir;
            e->icon = e->is_dir ? SK_ICON_FOLDER : SK_ICON_FILE;
            sk_strlcpy(e->type_str, e->is_dir ? "文件夹" : "无法读取", sizeof e->type_str);
            e->lmode = e->is_dir ? S_IFDIR : 0;
        }
    }
    if (closedir(dp) != 0 && !err)
        err = errno;
    if (err) {
        fm_free_entries(ents, n);
        return err;
    }
    *out = ents;
    *count = n;
    return 0;
}

void fm_free_entries(fm_entry *e, int n)
{
    if (!e)
        return;
    for (int i = 0; i < n; i++)
        free(e[i].name);
    free(e);
}

/* ================================================================ 排序 */

/* 在 [p, pe) 和 [q, qe) 两段之间做“自然排序”比较。
 * skip=1 时跳过空格和 ASCII 标点（但开头的“.”不跳过，隐藏文件仍然排在一起） */
static int nat_cmp(const unsigned char *p, const unsigned char *pe,
                   const unsigned char *q, const unsigned char *qe, int skip)
{
    const unsigned char *p0 = p, *q0 = q;
    for (;;) {
        if (skip) {
            while (p < pe && p != p0 && is_punct(*p))
                p++;
            while (q < qe && q != q0 && is_punct(*q))
                q++;
        }
        if (p >= pe || q >= qe)
            break;
        if (is_digit(*p) && is_digit(*q)) {
            /* 一串数字按数值比：先去掉前导 0，位数多的大；位数一样再逐位比 */
            while (p + 1 < pe && *p == '0' && is_digit(p[1]))
                p++;
            while (q + 1 < qe && *q == '0' && is_digit(q[1]))
                q++;
            const unsigned char *pd = p, *qd = q;
            while (pd < pe && is_digit(*pd))
                pd++;
            while (qd < qe && is_digit(*qd))
                qd++;
            if (pd - p != qd - q)
                return (pd - p) < (qd - q) ? -1 : 1;
            int c = memcmp(p, q, (size_t)(pd - p));
            if (c)
                return c;
            p = pd;
            q = qd;
            continue;
        }
        /* 中文等非 ASCII 字符按 UTF-8 字节比较，结果和按码点比较一样（UTF-8 的设计保证了这一点） */
        int ca = lower(*p), cb = lower(*q);
        if (ca != cb)
            return ca - cb;
        p++;
        q++;
    }
    return (p < pe) - (q < qe);       /* 前面都一样：短的在前 */
}

/* 名字里“扩展名”开始的位置（最后一个点；开头的点不算），没有扩展名返回字符串末尾 */
static const char *ext_start(const char *s)
{
    const char *dot = strrchr(s, '.');
    return (dot && dot != s) ? dot : s + strlen(s);
}

int fm_name_cmp(const char *a, const char *b)
{
    /* 第一轮：先比主名再比扩展名，并且忽略空格和标点——
     * 这样“notes.txt”排在“notes (副本).txt”前面，“副本”又排在“副本 2”前面 */
    const unsigned char *ua = (const unsigned char *)a, *ub = (const unsigned char *)b;
    const unsigned char *ea = (const unsigned char *)ext_start(a), *eb = (const unsigned char *)ext_start(b);
    int r = nat_cmp(ua, ea, ub, eb, 1);
    if (r == 0)
        r = nat_cmp(ea, ea + strlen((const char *)ea), eb, eb + strlen((const char *)eb), 1);
    if (r == 0)    /* 第二轮：只差在标点上（a-b 和 a_b），逐字比较，保证顺序确定 */
        r = nat_cmp(ua, ua + strlen(a), ub, ub + strlen(b), 0);
    return r;
}

static int g_sort_col, g_sort_desc;     /* qsort 的比较函数拿不到额外参数（单线程，安全） */

static int entry_cmp(const void *pa, const void *pb)
{
    const fm_entry *a = pa, *b = pb;
    if (a->is_dir != b->is_dir)
        return b->is_dir - a->is_dir;   /* 文件夹永远排在前面（升序降序都一样） */
    int r = 0;
    switch (g_sort_col) {
    case 1:
        if (!a->is_dir)                 /* 文件夹的“大小”只是目录表本身，比它没意义 */
            r = (a->size > b->size) - (a->size < b->size);
        break;
    case 2:
        r = strcmp(a->type_str, b->type_str);
        break;
    case 3:
        r = (a->mtime > b->mtime) - (a->mtime < b->mtime);
        break;
    default:
        break;
    }
    if (r == 0)
        r = fm_name_cmp(a->name, b->name);
    if (r == 0)
        r = strcmp(a->name, b->name);   /* 保证顺序完全确定 */
    return g_sort_desc ? -r : r;
}

void fm_sort(fm_entry *e, int n, int col, int desc)
{
    if (n < 2)
        return;
    g_sort_col = col;
    g_sort_desc = desc;
    qsort(e, (size_t)n, sizeof *e, entry_cmp);
}

/* ================================================================ 内容嗅探 */

int fm_sniff_text(const char *path, int *is_text)
{
    *is_text = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    unsigned char buf[4096];
    size_t got = 0;
    int err = 0;
    /* read 可能一次读不满（/proc 文件、管道、被信号打断……），所以要循环 */
    while (got < sizeof buf) {
        ssize_t r = read(fd, buf + got, sizeof buf - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            err = errno;
            break;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    if (err)
        return err;
    /* 文本文件里不会有 0 字节；程序、图片、压缩包里几乎一定有 */
    *is_text = memchr(buf, 0, got) == NULL;
    return 0;
}

/* ================================================================ 权限和类型 */

void fm_mode_string(mode_t m, char *out)
{
    /* 权限位一共 12 位：rwx 三组（所有者、同组用户、其他人）各 3 位，再加 3 个特殊位：
     * setuid/setgid（运行程序时临时获得文件所有者/组的身份，passwd 命令靠它改 /etc/shadow）
     * 和粘滞位（/tmp 上：人人能建文件，但只能删自己的）。ls -l 把特殊位画在 x 的位置上。 */
    out[0] = (m & S_IRUSR) ? 'r' : '-';
    out[1] = (m & S_IWUSR) ? 'w' : '-';
    out[2] = (m & S_ISUID) ? ((m & S_IXUSR) ? 's' : 'S') : ((m & S_IXUSR) ? 'x' : '-');
    out[3] = (m & S_IRGRP) ? 'r' : '-';
    out[4] = (m & S_IWGRP) ? 'w' : '-';
    out[5] = (m & S_ISGID) ? ((m & S_IXGRP) ? 's' : 'S') : ((m & S_IXGRP) ? 'x' : '-');
    out[6] = (m & S_IROTH) ? 'r' : '-';
    out[7] = (m & S_IWOTH) ? 'w' : '-';
    out[8] = (m & S_ISVTX) ? ((m & S_IXOTH) ? 't' : 'T') : ((m & S_IXOTH) ? 'x' : '-');
    out[9] = 0;
}

const char *fm_kind_name(mode_t m)
{
    if (S_ISREG(m))  return "普通文件";
    if (S_ISDIR(m))  return "目录";
    if (S_ISLNK(m))  return "符号链接";
    if (S_ISCHR(m))  return "字符设备";
    if (S_ISBLK(m))  return "块设备";
    if (S_ISFIFO(m)) return "命名管道";
    if (S_ISSOCK(m)) return "套接字";
    return "未知类型";
}

const char *fm_strerror(int err)
{
    switch (err) {
    case FM_ERR_SPECIAL:
        return "这是设备文件、管道或套接字，不能复制";
    case FM_ERR_MOUNTPOINT:
        return "这是另一个文件系统的挂载点（例如 /proc、/sys），为了安全不会跨文件系统删除";
    case FM_ERR_INSIDE:
        return "不能把文件夹复制到它自己里面（那样会无穷无尽地复制下去）";
    default:
        return sk_strerror(err);
    }
}

/* ================================================================ 递归删除和复制 */

static void set_err(char *errpath, size_t errlen, const char *path)
{
    if (errpath && errlen)
        sk_strlcpy(errpath, path, errlen);
}

/* 读出目录里所有的名字（不含 . 和 ..）。先全部读完再关掉目录，
 * 然后才去处理每一项：这样递归很深时也不会同时打开一大堆目录 fd。 */
static int read_names(const char *dir, char ***names_out, int *n_out)
{
    *names_out = NULL;
    *n_out = 0;
    DIR *dp = opendir(dir);
    if (!dp)
        return errno;
    char **names = NULL;
    int n = 0, cap = 0, err = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dp);
        if (!de) {
            err = errno;
            break;
        }
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        if (n == cap) {
            int ncap = cap ? cap * 2 : 16;
            char **nn = realloc(names, (size_t)ncap * sizeof *nn);
            if (!nn) {
                err = ENOMEM;
                break;
            }
            names = nn;
            cap = ncap;
        }
        if (!(names[n] = strdup(nm))) {
            err = ENOMEM;
            break;
        }
        n++;
    }
    closedir(dp);
    if (err) {
        for (int i = 0; i < n; i++)
            free(names[i]);
        free(names);
        return err;
    }
    *names_out = names;
    *n_out = n;
    return 0;
}

static void free_names(char **names, int n)
{
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* path 是 len 字节长的路径，缓冲区大小 FM_DEEP_PATH；子项名字直接接在后面，处理完再截回来。
 * 整个递归共用这一块缓冲区，每一层只占很少的栈空间。 */
static int delete_rec(char *path, size_t len, dev_t dev, char *errpath, size_t errlen)
{
    struct stat st;
    /* 用 lstat：如果是指向文件夹的符号链接，只删除链接本身，绝不能顺着链接去删别人的文件 */
    if (lstat(path, &st) != 0) {
        set_err(errpath, errlen, path);
        return errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        /* unlink 删除的是“名字”（目录里的那一行）。inode 的硬链接数降到 0、
         * 并且没有进程还打开着它时，内核才真正释放它的数据。 */
        if (unlink(path) != 0) {
            set_err(errpath, errlen, path);
            return errno;
        }
        return 0;
    }
    if (st.st_dev != dev) {
        /* 设备号不同 = 这里挂载着另一个文件系统（/proc、/sys、/dev……）。
         * 和 rm --one-file-system 一样，不跨过去删 */
        set_err(errpath, errlen, path);
        return FM_ERR_MOUNTPOINT;
    }
    char **names;
    int n;
    int err = read_names(path, &names, &n);
    if (err) {
        set_err(errpath, errlen, path);
        return err;
    }
    for (int i = 0; i < n && !err; i++) {
        size_t nl = strlen(names[i]);
        if (len + 1 + nl >= FM_DEEP_PATH) {
            set_err(errpath, errlen, path);
            err = ENAMETOOLONG;
            break;
        }
        path[len] = '/';
        memcpy(path + len + 1, names[i], nl + 1);
        err = delete_rec(path, len + 1 + nl, dev, errpath, errlen);
        path[len] = 0;
    }
    free_names(names, n);
    if (err)
        return err;
    if (rmdir(path) != 0) {
        set_err(errpath, errlen, path);
        return errno;
    }
    return 0;
}

int fm_delete_tree(const char *path, dev_t parent_dev, char *errpath, size_t errlen)
{
    /*
     * 为什么不能直接 rmdir 一个有内容的文件夹？
     * 内核的 rmdir 只删除**空**目录，否则返回 ENOTEMPTY。因为目录项指向 inode，如果允许
     * 直接删掉非空目录，里面那些文件就再也没有名字可以找到了，却还占着空间。
     * 所以“删除文件夹”在用户态是一个递归过程（后序遍历）：先删掉里面的每一项
     * （子文件夹同样先清空），最后文件夹空了，再 rmdir 它。rm -r 就是这么做的。
     */
    char buf[FM_DEEP_PATH];
    size_t len = strlen(path);
    if (len >= sizeof buf) {
        set_err(errpath, errlen, path);
        return ENAMETOOLONG;
    }
    memcpy(buf, path, len + 1);
    return delete_rec(buf, len, parent_dev, errpath, errlen);
}

/* 复制一个普通文件的内容。出错时 errpath 是出错的那一边（读源文件还是写目标文件） */
static int copy_file(const char *src, const char *dst, mode_t mode, char *errpath, size_t errlen)
{
    static char buf[64 * 1024];     /* 一次搬 64 KB：太小了系统调用次数多，太大了浪费内存 */
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        set_err(errpath, errlen, src);
        return errno;
    }
    /* O_EXCL：如果目标已经存在就失败，绝不覆盖别人的文件。
     * 先用 0600 创建，写完再 fchmod 成和原文件一样的权限：一来 open 的权限参数会被进程的
     * umask（通常是 022）屏蔽掉一些位，二来只读的原文件如果一开始就建成只读，我们自己就写不进去了。 */
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        int e = errno;
        close(in);
        set_err(errpath, errlen, dst);
        return e;
    }
    int err = 0;
    const char *where = dst;
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            err = errno;
            where = src;
            break;
        }
        if (r == 0)
            break;                  /* 读到文件末尾 */
        /* write 也可能只写进去一部分（例如被信号打断、磁盘快满），剩下的要接着写 */
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                err = errno;
                break;
            }
            off += w;
        }
        if (err)
            break;
    }
    if (!err && fchmod(out, mode & 07777) != 0)
        err = errno;
    /* close 也要检查：有些文件系统直到关闭时才报告写入错误 */
    if (close(out) != 0 && !err)
        err = errno;
    close(in);
    if (err) {
        set_err(errpath, errlen, where);
        unlink(dst);                /* 复制了一半的文件没有用，删掉 */
    }
    return err;
}

static int copy_rec(char *src, size_t sl, char *dst, size_t dl, char *errpath, size_t errlen)
{
    struct stat st;
    if (lstat(src, &st) != 0) {
        set_err(errpath, errlen, src);
        return errno;
    }
    if (S_ISLNK(st.st_mode)) {
        /* 符号链接：复制“链接本身”（里面存的目标路径），而不是它指向的东西。
         * 否则一个指向上级目录的链接会让复制永远停不下来。 */
        char target[FM_DEEP_PATH];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) {
            set_err(errpath, errlen, src);
            return errno;
        }
        target[n] = 0;
        if (symlink(target, dst) != 0) {
            set_err(errpath, errlen, dst);
            return errno;
        }
        return 0;
    }
    if (S_ISREG(st.st_mode))
        return copy_file(src, dst, st.st_mode, errpath, errlen);
    if (!S_ISDIR(st.st_mode)) {
        set_err(errpath, errlen, src);
        return FM_ERR_SPECIAL;
    }
    /* 文件夹：先建一个自己能写的空文件夹，把内容都复制进去，最后再改成原来的权限
     * （如果原文件夹是只读的 0555，一开始就建成只读，就没法往里面放东西了） */
    if (mkdir(dst, 0700) != 0) {
        set_err(errpath, errlen, dst);
        return errno;
    }
    char **names;
    int n;
    int err = read_names(src, &names, &n);
    if (err) {
        set_err(errpath, errlen, src);
        return err;
    }
    for (int i = 0; i < n && !err; i++) {
        size_t nl = strlen(names[i]);
        if (sl + 1 + nl >= FM_DEEP_PATH || dl + 1 + nl >= FM_DEEP_PATH) {
            set_err(errpath, errlen, src);
            err = ENAMETOOLONG;
            break;
        }
        src[sl] = '/';
        memcpy(src + sl + 1, names[i], nl + 1);
        dst[dl] = '/';
        memcpy(dst + dl + 1, names[i], nl + 1);
        err = copy_rec(src, sl + 1 + nl, dst, dl + 1 + nl, errpath, errlen);
        src[sl] = 0;
        dst[dl] = 0;
    }
    free_names(names, n);
    if (!err && chmod(dst, st.st_mode & 07777) != 0) {
        set_err(errpath, errlen, dst);
        err = errno;
    }
    return err;
}

int fm_copy_tree(const char *src, const char *dst, char *errpath, size_t errlen)
{
    char s[FM_DEEP_PATH], d[FM_DEEP_PATH];
    size_t sl = strlen(src), dl = strlen(dst);
    if (sl >= sizeof s || dl >= sizeof d) {
        set_err(errpath, errlen, src);
        return ENAMETOOLONG;
    }
    memcpy(s, src, sl + 1);
    memcpy(d, dst, dl + 1);
    return copy_rec(s, sl, d, dl, errpath, errlen);
}

int fm_path_inside(const char *dir, const char *child)
{
    /* realpath 把 . .. 和所有符号链接都解析掉，得到“真实”的绝对路径，
     * 这样 /root/a 和 /root/link-to-a/b 也能被认出是同一棵树 */
    char rd[PATH_MAX], rc[PATH_MAX];
    if (!realpath(dir, rd) || !realpath(child, rc))
        return 0;
    size_t n = strlen(rd);
    if (strcmp(rd, "/") == 0)
        return 1;
    return strncmp(rd, rc, n) == 0 && (rc[n] == 0 || rc[n] == '/');
}

/* ================================================================ 起名字 */

/* dir 里有没有叫 name 的东西（包括失效的符号链接，所以用 lstat）。
 * lstat 因为别的原因失败（例如没有权限）时当作“没有”：真正创建时用的 O_EXCL / mkdir
 * 会再检查一次，那时报出来的才是真实的错误（权限不够），而不是“已存在” */
static int name_taken(const char *dir, const char *name)
{
    char full[FM_DEEP_PATH];
    sk_path_join(full, sizeof full, dir, name);
    struct stat st;
    return lstat(full, &st) == 0;
}

int fm_copy_name(const char *dir, const char *name, int is_dir, char *out, size_t n)
{
    if (!name_taken(dir, name)) {
        if (strlen(name) >= n)
            return ENAMETOOLONG;
        sk_strlcpy(out, name, n);
        return 0;
    }
    /* “照片.ppm”的副本叫“照片 (副本).ppm”：扩展名留在最后，双击副本还能用同一个应用打开 */
    char stem[FM_NAME_MAX];
    const char *ext = is_dir ? NULL : strrchr(name, '.');
    if (ext == name)
        ext = NULL;                 /* “.bashrc”整个是名字，没有扩展名 */
    size_t sl = ext ? (size_t)(ext - name) : strlen(name);
    if (sl >= sizeof stem)
        sl = sizeof stem - 1;
    memcpy(stem, name, sl);
    stem[sl] = 0;
    for (int i = 1; i < 1000; i++) {
        char cand[FM_NAME_MAX * 2];
        if (i == 1)
            snprintf(cand, sizeof cand, "%s (副本)%s", stem, ext ? ext : "");
        else
            snprintf(cand, sizeof cand, "%s (副本 %d)%s", stem, i, ext ? ext : "");
        if (strlen(cand) >= FM_NAME_MAX || strlen(cand) >= n)
            return ENAMETOOLONG;
        if (!name_taken(dir, cand)) {
            sk_strlcpy(out, cand, n);
            return 0;
        }
    }
    return EEXIST;
}

int fm_fresh_name(const char *dir, const char *stem, const char *ext, char *out, size_t n)
{
    for (int i = 1; i < 1000; i++) {
        char cand[FM_NAME_MAX];
        if (i == 1)
            snprintf(cand, sizeof cand, "%s%s", stem, ext);
        else
            snprintf(cand, sizeof cand, "%s %d%s", stem, i, ext);
        if (!name_taken(dir, cand)) {
            sk_strlcpy(out, cand, n);
            return 0;
        }
    }
    return EEXIST;
}
