/*
 * skfiles.h — 文件管理器内部接口：ops.c（文件系统操作）提供给 main.c（界面）的函数
 *
 * ops.c 里只有系统调用和数据结构，不碰任何控件；main.c 只管界面和交互。
 * 这样分开以后，“文件管理器到底用了哪些系统调用”一眼就能在 ops.c 里看全。
 *
 * 错误约定：返回 0 表示成功，否则返回错误号（errno 的值，或下面的 FM_ERR_*），
 * 界面用 fm_strerror() 把它翻译成中文。
 */
#ifndef SKFILES_H
#define SKFILES_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define FM_PATH_MAX   1024   /* 当前文件夹路径的上限（与 sk_path_normalize 的缓冲区一致） */
#define FM_DEEP_PATH  4096   /* 递归复制/删除时的路径缓冲（= Linux 的 PATH_MAX） */
#define FM_NAME_MAX   256    /* 一个文件名最多 255 字节（NAME_MAX）+ NUL */

/* 除了 errno 以外，我们自己的几种错误 */
enum {
    FM_ERR_SPECIAL    = 10001,  /* 设备文件、管道、套接字：不能复制 */
    FM_ERR_MOUNTPOINT = 10002,  /* 删除时遇到了另一个文件系统的挂载点 */
    FM_ERR_INSIDE     = 10003,  /* 把文件夹复制到它自己里面 */
};

/* 文件夹里的一项（一次读目录时就把显示用的字符串都算好，重画和排序时不再做任何系统调用） */
typedef struct {
    char     *name;            /* 文件名（malloc） */
    char      size_str[24];    /* “4.9 KB”；文件夹为空 */
    char      time_str[24];    /* “2026-10-23 13:21” */
    char      type_str[48];    /* “文本文件”“文件夹（链接）”…… */
    int       icon;            /* SK_ICON_* */
    int       is_dir;          /* 跟随符号链接之后是不是目录（决定双击 = 进入） */
    int       is_link;         /* 本身是不是符号链接 */
    int       broken;          /* 符号链接指向的东西不存在，或者 stat 失败 */
    int       hidden;          /* 名字以 . 开头 */
    mode_t    lmode;           /* lstat 得到的类型 + 权限位 */
    long long size;            /* 排序用（字节） */
    time_t    mtime;           /* 排序用 */
} fm_entry;

/* 读目录 dir 的全部内容（不含 . 和 ..），*out 是 malloc 的数组。成功返回 0 */
int  fm_list_dir(const char *dir, fm_entry **out, int *count);
void fm_free_entries(fm_entry *e, int n);
/* 排序：文件夹永远在前；col 0=名称 1=大小 2=类型 3=修改时间；desc=1 降序 */
void fm_sort(fm_entry *e, int n, int col, int desc);
/* 文件名比较（“自然排序”）：不分 ASCII 大小写；数字按数值比（2 < 10，/proc 里的进程号就是
 * 这样排的）；先比主名再比扩展名，并且先忽略空格和标点（“a.txt”在“a (副本).txt”前面） */
int  fm_name_cmp(const char *a, const char *b);

/* 名字的扩展名是不是画板能打开的图片（.ppm .pgm .pbm） */
int  fm_is_image_name(const char *name);
/* 名字的扩展名是不是音乐播放器能打开的声音文件（.wav） */
int  fm_is_audio_name(const char *name);
/* 读文件开头 4 KB：没有 NUL 字节就当作文本（*is_text=1）。成功返回 0 */
int  fm_sniff_text(const char *path, int *is_text);

/* 权限位 → "rwxr-xr-x"（out 至少 10 字节，含 setuid/setgid/粘滞位的 s/S/t/T） */
void fm_mode_string(mode_t mode, char *out);
/* 文件类型的中文名：“普通文件”“目录”“符号链接”“字符设备”…… */
const char *fm_kind_name(mode_t mode);

/* 递归删除 path（不跟随符号链接；遇到别的文件系统的挂载点就停下）。
 * parent_dev 是 path 所在文件夹的设备号。出错时 errpath 里是出错的那个路径 */
int  fm_delete_tree(const char *path, dev_t parent_dev, char *errpath, size_t errlen);
/* 递归复制 src → dst（dst 必须不存在），保留权限位；符号链接复制成链接本身 */
int  fm_copy_tree(const char *src, const char *dst, char *errpath, size_t errlen);
/* child 是不是 dir 本身或者在 dir 里面（按真实路径判断，符号链接也算） */
int  fm_path_inside(const char *dir, const char *child);

/* 在 dir 里给 name 的副本起一个不冲突的名字：“a.txt”→“a (副本).txt”→“a (副本 2).txt”…… */
int  fm_copy_name(const char *dir, const char *name, int is_dir, char *out, size_t n);
/* 新建时的默认名字：“新建文件夹”“新建文件夹 2”……；ext 是扩展名（含点，可为 ""） */
int  fm_fresh_name(const char *dir, const char *stem, const char *ext, char *out, size_t n);

/* 错误号 → 中文（errno 交给 sk_strerror，FM_ERR_* 自己解释） */
const char *fm_strerror(int err);

#endif /* SKFILES_H */
