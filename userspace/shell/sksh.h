/*
 * sksh.h — SketchOS 命令行 shell（sksh）内部共享的数据结构与函数声明
 *
 * 一个 shell 本质上就是一个“循环”：
 *
 *     读一行 ──► 词法分析(lexer) ──► 语法分析(parser) ──► 抽象语法树(AST)
 *                                                            │
 *     打印提示符 ◄── 等待子进程结束(wait) ◄── fork + exec ◄── 展开(expand)
 *
 * 文件分工：
 *   main.c        启动参数、登录 shell 的启动脚本、主循环入口
 *   input.c       输入来源：字符串(-c)、文件/管道(脚本)、终端(交互)
 *   lineedit.c    交互式行编辑器（终端原始模式、UTF-8、历史记录、Tab 补全）
 *   parser.c      词法分析 + 递归下降语法分析，生成 AST；以及把 AST 还原成文本
 *   expand.c      单词展开：~、$变量、$(命令替换)、$((算术))、分词、通配符
 *   arith.c       $((...)) 算术求值
 *   exec.c        执行器：简单命令、管道、重定向、复合命令、函数
 *   jobs.c        作业控制：进程组、控制终端、前台/后台、Ctrl-Z
 *   signals.c     信号处置、trap、信号名字表、kill
 *   vars.c        变量、环境变量、位置参数、别名、函数表
 *   builtins.c    内建命令（cd、export、set……）
 *   builtin_io.c  内建命令 echo、printf、read、test/[
 *   help.c        中文帮助文本
 *   util.c        内存、字符串缓冲、内存池、UTF-8 宽度、错误输出
 */
#ifndef SKSH_H
#define SKSH_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <termios.h>

#include "skutil.h"

#define SKSH_VERSION SKETCHOS_VERSION

/* shell 自己内部使用的文件描述符（脚本文件、保存的 stdin/stdout……）都挪到这个编号以上，
 * 并且带 O_CLOEXEC（exec 时自动关闭），这样既不会和用户写的 3>file 冲突，也不会泄漏给子进程。 */
#define SH_FD_BASE 64

/* ======================= util.c ======================= */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* 自动增长的字符串缓冲区，s 总是 NUL 结尾（s 可能为 NULL 表示空） */
typedef struct {
    char  *s;
    size_t len, cap;
} strbuf;

void        sb_init(strbuf *b);
void        sb_putc(strbuf *b, int c);
void        sb_putn(strbuf *b, const char *s, size_t n);
void        sb_puts(strbuf *b, const char *s);
void        sb_printf(strbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void        sb_clear(strbuf *b);
void        sb_setlen(strbuf *b, size_t len);
void        sb_free(strbuf *b);
char       *sb_take(strbuf *b);          /* 取走内容（malloc 的字符串），缓冲区复位 */
const char *sb_str(const strbuf *b);     /* 从不返回 NULL */

/* 字符串数组（argv 风格，v[n] 总是 NULL） */
typedef struct {
    char **v;
    int    n, cap;
} strvec;

void sv_init(strvec *v);
void sv_push(strvec *v, char *s);        /* 接管 s 的所有权 */
void sv_free(strvec *v);
char **sv_argv(strvec *v);               /* 保证 v 非空并以 NULL 结尾 */

/* 内存池：一条命令的语法树里的所有节点都从同一个池子里分配，执行完一次性释放。
 * 函数定义会让池子“多活一会儿”（引用计数）。 */
typedef struct arena arena;
arena *arena_new(void);
void  *arena_alloc(arena *a, size_t n);  /* 返回清零的内存 */
char  *arena_strdup(arena *a, const char *s);
char  *arena_strndup(arena *a, const char *s, size_t n);
void   arena_ref(arena *a);
void   arena_unref(arena *a);

/* UTF-8 与显示宽度（汉字在终端里占 2 列） */
int utf8_decode(const char *s, size_t len, uint32_t *cp);   /* 返回消耗的字节数 (>=1) */
int utf8_seqlen(unsigned char lead);
int cp_width(uint32_t cp);
int str_width(const char *s, size_t len);                   /* 跳过 ANSI 转义序列 */
size_t utf8_count(const char *s);                           /* 字符个数 */

/* 输出 */
ssize_t full_write(int fd, const void *buf, size_t n);
ssize_t full_read(int fd, void *buf, size_t n);
void sh_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sh_error_noloc(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *sh_strerror(int err);                           /* 中文错误信息 */
void shell_quote(strbuf *out, const char *s);               /* 需要时加单引号，供 set/alias/export -p 输出 */

/* 文件描述符 */
int fd_move_high(int fd);       /* 复制到 >= SH_FD_BASE 并设 CLOEXEC，关闭原来的；失败返回原 fd */

/* ======================= 语法树 (AST) ======================= */

/* 一个“单词”由若干“片段”组成。例如  "$HOME"/a*'b c'  由 4 段组成：
 *   [变量 HOME, 带引号] [字面 "/a*", 无引号] [字面 "b c", 带引号]
 * 记住每一段有没有被引号保护，展开时才知道哪些部分要分词、哪些 * 要当通配符。 */
enum wpart_kind {
    WP_LIT,         /* 字面文本 */
    WP_PARAM,       /* $name ${name...} */
    WP_CMDSUB,      /* $(...) 或 `...` */
    WP_ARITH,       /* $((...)) */
};

enum param_op {
    PO_NONE,        /* ${x} */
    PO_LEN,         /* ${#x} */
    PO_DEFAULT,     /* ${x-w} ${x:-w} */
    PO_ASSIGN,      /* ${x=w} ${x:=w} */
    PO_ALT,         /* ${x+w} ${x:+w} */
    PO_ERROR,       /* ${x?w} ${x:?w} */
    PO_RM_SSUFFIX,  /* ${x%w}  去掉最短后缀 */
    PO_RM_LSUFFIX,  /* ${x%%w} 去掉最长后缀 */
    PO_RM_SPREFIX,  /* ${x#w}  去掉最短前缀 */
    PO_RM_LPREFIX,  /* ${x##w} 去掉最长前缀 */
};

struct node;
struct word;

typedef struct wpart {
    enum wpart_kind kind;
    unsigned char   quoted;     /* 在单/双引号里（或被反斜杠转义） */
    unsigned char   colon;      /* ${x:-w} 里的冒号：空值也算“未设置” */
    enum param_op   op;
    char           *text;       /* WP_LIT：文本；WP_PARAM：变量名 */
    struct word    *arg;        /* WP_PARAM：操作数；WP_ARITH：表达式 */
    struct node    *cmd;        /* WP_CMDSUB：命令 */
    struct wpart   *next;
} wpart;

typedef struct word {
    wpart       *parts;
    char        *raw;           /* 原始写法（用于显示作业、type 输出） */
    struct word *next;
} word;

enum redir_type {
    R_IN,           /* <file    */
    R_OUT,          /* >file    */
    R_APPEND,       /* >>file   */
    R_CLOBBER,      /* >|file   */
    R_INOUT,        /* <>file   */
    R_DUPIN,        /* n<&m  n<&-  */
    R_DUPOUT,       /* n>&m  n>&-  */
    R_HEREDOC,      /* <<EOF  <<-EOF  <<<word */
    R_OUTERR,       /* &>file   */
    R_APPENDERR,    /* &>>file  */
};

typedef struct redir {
    enum redir_type type;
    int             fd;         /* 被重定向的文件描述符 */
    int             fd_given;   /* 用户写了 n> 这样的编号 */
    word           *target;     /* 文件名 / 目标描述符 / here-doc 定界符 */
    word           *body;       /* here-doc 正文 */
    unsigned char   body_quoted;    /* 定界符带引号：正文不做展开 */
    unsigned char   strip_tabs;     /* <<- */
    unsigned char   herestring;     /* <<< */
    char           *delim;
    struct redir   *next;
} redir;

enum node_type {
    N_SIMPLE,       /* 简单命令：赋值 + 单词 + 重定向 */
    N_PIPELINE,     /* a | b | c */
    N_AND,          /* a && b */
    N_OR,           /* a || b */
    N_LIST,         /* a ; b & c */
    N_SUBSHELL,     /* ( list ) */
    N_BRACE,        /* { list; } */
    N_IF,
    N_WHILE,
    N_UNTIL,
    N_FOR,
    N_CASE,
    N_FUNCDEF,
};

typedef struct list_item {
    struct node      *node;
    int               async;    /* 以 & 结尾：放到后台 */
    struct list_item *next;
} list_item;

typedef struct case_item {
    word             *patterns;
    struct node      *body;
    struct case_item *next;
} case_item;

typedef struct node {
    enum node_type type;
    int            lineno;
    redir         *redirs;      /* 复合命令后面也可以跟重定向：{ ...; } > file */
    union {
        struct { word *assigns; word *args; } simple;
        struct { struct node **cmds; int ncmds; int negate; } pipe;
        struct { struct node *left, *right; } andor;
        struct { list_item *items; } list;
        struct { struct node *body; } group;
        struct { struct node *cond, *then_part, *else_part; } ifn;
        struct { struct node *cond, *body; } loop;
        struct { char *var; word *words; int has_in; struct node *body; } forn;
        struct { word *subject; case_item *items; } casen;
        struct { char *name; struct node *body; } func;
    } u;
} node;

/* ======================= input.c ======================= */

typedef struct source source;
source *source_string(const char *s, const char *name);
source *source_fd(int fd, const char *name, int own_fd);
source *source_tty(void);
void    source_free(source *s);
/* 读一行（含换行符）追加到 out。返回 1 读到；0 文件结束；-1 用户按了 Ctrl-C */
int     source_getline(source *s, strbuf *out, int continuation);
/* 从 stdin 读脚本时，把多读进缓冲区的字节“退回”给内核（lseek），
 * 这样脚本里的命令（比如 read）能读到脚本后面的内容。 */
void    source_sync(source *s);
const char *source_name(const source *s);

/* ======================= parser.c ======================= */

typedef struct parser parser;
enum { PARSE_OK, PARSE_EMPTY, PARSE_EOF, PARSE_ERROR, PARSE_CANCEL };

parser *parser_new(source *src);
void    parser_free(parser *p);
/* 读取并解析一条完整的命令（可能跨多行）。成功时 *out 是语法树，*ar 是它所在的内存池 */
int     parser_next(parser *p, node **out, arena **ar);
/* 刚解析的那条命令的原文（给历史记录用），malloc 的字符串 */
char   *parser_command_text(parser *p);
/* 把一段文本按 here-doc 规则（只展开 $ 和 `，引号是普通字符）解析成单词；出错返回 NULL */
word   *parse_heredoc_text(const char *text, arena *ar);
int     is_keyword(const char *s);
void    deparse(const node *n, strbuf *out);
char   *node_text(const node *n);               /* malloc */

/* ======================= expand.c ======================= */

extern int g_cmdsub_count;      /* 执行过的命令替换次数 */
extern int g_expand_fatal;      /* 最近的展开错误在非交互 shell 里应该导致退出 */

int   expand_args(word *words, strvec *out);    /* 完整展开（含分词、通配符），0 成功 -1 失败 */
char *expand_string(word *w, int assign);       /* 不分词不匹配文件名；assign=1 时处理 a=~/x:~/y */
char *expand_pattern(word *w);                  /* 生成给 fnmatch 用的模式（引号里的 * 被转义） */
char *expand_prompt(const char *ps);            /* PS1/PS2 */
char *tilde_lookup(const char *user);           /* "" → $HOME；"root" → /root；失败 NULL */
int   param_special_get(const char *name, strbuf *out);  /* $? $$ $# ... 返回 1 表示已设置 */

/* ======================= arith.c ======================= */

int arith_eval(const char *expr, long long *result);    /* 0 成功；-1 出错（已打印信息） */

/* ======================= exec.c ======================= */

#define EF_TAIL 1   /* 调用者执行完就退出：最后一个外部命令可以直接 exec，省一次 fork */

extern arena *g_cur_arena;      /* 正在执行的命令所在的内存池（函数定义要引用它） */
int   exec_node(node *n, int flags);
int   exec_source(source *src, int toplevel);   /* 读-解析-执行循环，返回最后的状态 */
int   exec_string(const char *s, const char *name);
int   exec_file(const char *path, int argc, char **argv, int must_exist);  /* source / . */
int   cmdsub_run(node *n, strbuf *out);
int   find_in_path(const char *name, strbuf *out);  /* 在 PATH 里找可执行文件，找到返回 1 */
pid_t sh_fork(void);
void  exit_shell(int status) __attribute__((noreturn));
void  exit_child(int status) __attribute__((noreturn));
void  exec_external(char **argv) __attribute__((noreturn));   /* 子进程里：execve，失败就退出 */

/* ======================= jobs.c ======================= */

typedef struct job job;
void  jobs_init(void);
void  jobs_child_reset(void);            /* fork 出来的子进程清空作业表 */
job  *job_new(const char *cmd);
void  job_free(job *j);
void  job_child_setup(job *j, int foreground);  /* fork 后在子进程里调用：加入进程组、拿终端 */
void  job_add_pid(job *j, pid_t pid);           /* fork 后在父进程里调用 */
int   job_wait_fg(job *j);                      /* 等前台作业结束或停止，返回状态码 */
void  job_put_bg(job *j);                       /* 登记为后台作业 */
void  jobs_notify(void);                        /* 提示符之前：报告结束/停止的后台作业 */
int   jobs_stopped_count(void);
int   jobs_count(void);
void  jobs_restore_terminal(void);              /* 退出时把终端还给原来的前台进程组 */
int   status_from_wait(int wstatus);

/* ======================= signals.c ======================= */

extern volatile sig_atomic_t g_sigint;          /* 交互式 shell 收到了 Ctrl-C */
extern volatile sig_atomic_t g_trap_pending;
void signals_init(void);
void signals_child_reset(int async_noninteractive);
void traps_run_pending(void);
void trap_exit_run(void);
int  trap_exit_set(void);
int  sig_from_name(const char *s);
const char *sig_name(int sig);
const char *sig_desc(int sig);
int  sig_count(void);

/* ======================= vars.c ======================= */

#define V_EXPORT   1
#define V_READONLY 2

void        vars_init(char **envp);
const char *var_get(const char *name);
int         var_set(const char *name, const char *value, int add_flags);  /* -1 只读 */
int         var_unset(const char *name);
int         var_flags(const char *name);            /* 不存在返回 -1 */
void        var_add_flags(const char *name, int flags);
void        var_clear_flags(const char *name, int flags);
char      **var_environ(void);                      /* 缓存的 envp，给 execve 用 */
typedef void (*var_cb)(const char *name, const char *value, int flags, void *ud);
void        var_foreach_sorted(var_cb cb, void *ud);
int         is_name(const char *s);
int         is_name_n(const char *s, size_t n);

/* local：函数里的局部变量（进入函数压一层作用域，退出时恢复旧值） */
void var_scope_push(void);
void var_scope_pop(void);
int  var_in_function(void);
void var_make_local(const char *name);

/* 临时变量：VAR=1 builtin 只在这条命令期间生效 */
typedef struct varsave varsave;
varsave *var_save(varsave *list, const char *name);
void     var_restore(varsave *list);

/* 位置参数 $1 $2 ... */
typedef struct { int n; char **v; } posparams;
void        pos_set(int n, char **v);       /* 复制 */
void        pos_swap(posparams *saved);     /* 与当前交换（函数调用时保存/恢复） */
void        pos_release(posparams *p);      /* 释放 pos_swap 换出来的那一份 */
int         pos_count(void);
const char *pos_get(int i);                 /* 1 起；越界 NULL */
int         pos_shift(int n);

/* 别名 */
const char *alias_get(const char *name);
void        alias_set(const char *name, const char *value);
int         alias_unset(const char *name);
void        alias_clear(void);
void        alias_foreach_sorted(var_cb cb, void *ud);

/* 函数 */
typedef struct shfunc {
    char          *name;
    node          *body;
    arena         *ar;
    struct shfunc *next;
} shfunc;
shfunc *func_get(const char *name);
void    func_set(const char *name, node *body, arena *ar);
int     func_unset(const char *name);
void    func_foreach(void (*cb)(shfunc *f, void *ud), void *ud);

/* ======================= lineedit.c ======================= */

enum { LE_OK, LE_EOF, LE_INTR };
int  lineedit_read(const char *prompt, strbuf *out);   /* 读一行（不含换行） */
int  lineedit_available(void);
void history_init(void);
void history_add(const char *line);
int  history_count(void);

/* ======================= 内建命令 ======================= */

typedef int (*builtin_fn)(int argc, char **argv);
typedef struct {
    const char *name;
    builtin_fn  fn;
    int         special;        /* POSIX “特殊内建命令” */
} builtin_def;

const builtin_def *builtin_find(const char *name);
int  builtin_count(void);
const char *builtin_name(int i);
void help_show_all(FILE *out);
int  help_show_one(FILE *out, const char *name);
const char *help_summary(const char *name);

/* 各个文件里实现的内建命令 */
int bi_echo(int, char **);
int bi_printf(int, char **);
int bi_read(int, char **);
int bi_test(int, char **);
int bi_jobs(int, char **);
int bi_fg(int, char **);
int bi_bg(int, char **);
int bi_wait(int, char **);
int bi_kill(int, char **);
int bi_trap(int, char **);
int bi_history(int, char **);
int bi_help(int, char **);
int bi_exec_args(int, char **);
int bi_command(int, char **);
int bi_command_describe(int argc, char **argv, int verbose);

/* ======================= 全局 shell 状态 ======================= */

typedef struct shell {
    /* 运行模式 */
    int   interactive;      /* 交互式：显示提示符、行编辑 */
    int   login;            /* 登录 shell：执行 /etc/profile */
    int   jobctl;           /* 作业控制（set -m）开启 */
    int   tty_fd;           /* 控制终端（作业控制用），-1 表示没有 */
    pid_t pgid;             /* shell 自己的进程组 */
    pid_t orig_pgid;        /* 启动时终端的前台进程组，退出时还回去 */
    struct termios tmodes;  /* shell 自己的终端设置 */
    int   have_tmodes;
    pid_t rootpid;          /* $$：子 shell 里也不变 */
    int   subshell;         /* 1 = 我们是 fork 出来的子 shell */
    char *arg0;             /* $0 */
    char *cmd_string;       /* -c 的命令 */

    /* 状态 */
    int   last_status;      /* $? */
    pid_t last_bg;          /* $! */
    long  start_time;

    /* set 选项 */
    int   opt_errexit, opt_nounset, opt_xtrace, opt_noglob, opt_noclobber;
    int   opt_verbose, opt_pipefail, opt_noexec, opt_ignoreeof;

    /* 控制流 */
    int   loop_depth, break_n, continue_n;
    int   func_depth, source_depth;
    int   returning, return_status;
    int   cond_depth;       /* >0：在 if/while 条件、&& || 左侧、! 之后，set -e 不生效 */
    int   abort;            /* 1 = 放弃当前整行命令（Ctrl-C、展开错误） */
    int   exit_warned;      /* 已经警告过“有已停止的作业” */

    /* 出错位置 */
    const char *script_name;
    int   lineno;
} shell;

extern shell sh;

/* 当前命令序列是否应该停止执行（break/continue/return/Ctrl-C/错误） */
int sh_should_stop(void);

#endif /* SKSH_H */
