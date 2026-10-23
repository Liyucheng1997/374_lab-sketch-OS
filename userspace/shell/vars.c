/*
 * vars.c — shell 变量、环境变量、位置参数、别名、函数表
 *
 * shell 变量 vs 环境变量：
 *   shell 变量只活在 sksh 这个进程的内存里；被 export 的变量才会在 execve() 时作为 envp
 *   传给新程序。内核在 execve 时把 argv 和 envp 复制到新程序的栈顶，C 库再把它们变成
 *   main(argc, argv) 和 environ。所以“环境变量会被子进程继承”只是 shell 每次都把它们
 *   放进 envp 而已，内核本身并不知道什么是“环境变量”。
 */
#include "sksh.h"

#include <stdlib.h>
#include <string.h>

/* ======================= 变量表（哈希表） ======================= */

typedef struct var {
    char       *name;
    char       *value;      /* NULL = 只声明（例如 export X 但 X 没赋值） */
    int         flags;
    struct var *next;
} var;

#define VHASH 256
static var  *vtab[VHASH];
static char **env_cache;
static int    env_dirty = 1;

static unsigned vhash(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h % VHASH;
}

static var *vlookup(const char *name)
{
    for (var *v = vtab[vhash(name)]; v; v = v->next)
        if (strcmp(v->name, name) == 0)
            return v;
    return NULL;
}

static var *vcreate(const char *name)
{
    var *v = xcalloc(1, sizeof *v);
    v->name = xstrdup(name);
    unsigned h = vhash(name);
    v->next = vtab[h];
    vtab[h] = v;
    return v;
}

int is_name_n(const char *s, size_t n)
{
    if (n == 0 || !(s[0] == '_' || (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')))
        return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return 0;
    }
    return 1;
}

int is_name(const char *s)
{
    return is_name_n(s, strlen(s));
}

void vars_init(char **envp)
{
    for (char **e = envp; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || !is_name_n(*e, (size_t)(eq - *e)))
            continue;       /* 名字不合法的环境变量（极少见）直接丢弃 */
        char *name = xstrndup(*e, (size_t)(eq - *e));
        var_set(name, eq + 1, V_EXPORT);
        free(name);
    }
}

const char *var_get(const char *name)
{
    var *v = vlookup(name);
    return v ? v->value : NULL;
}

int var_set(const char *name, const char *value, int add_flags)
{
    var *v = vlookup(name);
    if (v && (v->flags & V_READONLY)) {
        sh_error("%s: 只读变量", name);
        return -1;
    }
    if (!v)
        v = vcreate(name);
    char *nv = value ? xstrdup(value) : NULL;
    free(v->value);
    v->value = nv;
    v->flags |= add_flags;
    if (v->flags & V_EXPORT)
        env_dirty = 1;
    return 0;
}

int var_unset(const char *name)
{
    unsigned h = vhash(name);
    for (var **pp = &vtab[h]; *pp; pp = &(*pp)->next) {
        var *v = *pp;
        if (strcmp(v->name, name) != 0)
            continue;
        if (v->flags & V_READONLY) {
            sh_error("%s: 不能取消设置：只读变量", name);
            return -1;
        }
        if (v->flags & V_EXPORT)
            env_dirty = 1;
        *pp = v->next;
        free(v->name);
        free(v->value);
        free(v);
        return 0;
    }
    return 0;
}

int var_flags(const char *name)
{
    var *v = vlookup(name);
    return v ? v->flags : -1;
}

void var_add_flags(const char *name, int flags)
{
    var *v = vlookup(name);
    if (!v)
        v = vcreate(name);
    v->flags |= flags;
    env_dirty = 1;
}

void var_clear_flags(const char *name, int flags)
{
    var *v = vlookup(name);
    if (v) {
        v->flags &= ~flags;
        env_dirty = 1;
    }
}

/* 构造传给 execve 的环境 "NAME=value" 数组。变量没变就复用上次的结果。 */
char **var_environ(void)
{
    if (!env_dirty && env_cache)
        return env_cache;
    if (env_cache) {
        for (char **e = env_cache; *e; e++)
            free(*e);
        free(env_cache);
    }
    size_t n = 0;
    for (int i = 0; i < VHASH; i++)
        for (var *v = vtab[i]; v; v = v->next)
            if ((v->flags & V_EXPORT) && v->value)
                n++;
    env_cache = xcalloc(n + 1, sizeof(char *));
    size_t k = 0;
    for (int i = 0; i < VHASH; i++)
        for (var *v = vtab[i]; v; v = v->next)
            if ((v->flags & V_EXPORT) && v->value)
                env_cache[k++] = sk_strdup_printf("%s=%s", v->name, v->value);
    env_cache[k] = NULL;
    env_dirty = 0;
    return env_cache;
}

static int cmp_var(const void *a, const void *b)
{
    return strcmp((*(var *const *)a)->name, (*(var *const *)b)->name);
}

void var_foreach_sorted(var_cb cb, void *ud)
{
    size_t n = 0;
    for (int i = 0; i < VHASH; i++)
        for (var *v = vtab[i]; v; v = v->next)
            n++;
    var **arr = xcalloc(n + 1, sizeof *arr);
    size_t k = 0;
    for (int i = 0; i < VHASH; i++)
        for (var *v = vtab[i]; v; v = v->next)
            arr[k++] = v;
    qsort(arr, n, sizeof *arr, cmp_var);
    for (size_t i = 0; i < n; i++)
        cb(arr[i]->name, arr[i]->value, arr[i]->flags, ud);
    free(arr);
}

/* ======================= 保存/恢复（local 与临时赋值） ======================= */

struct varsave {
    char           *name;
    char           *value;
    int             flags;
    int             existed;
    struct varsave *next;
};

varsave *var_save(varsave *list, const char *name)
{
    varsave *s = xcalloc(1, sizeof *s);
    var *v = vlookup(name);
    s->name = xstrdup(name);
    s->existed = v != NULL;
    if (v) {
        s->value = v->value ? xstrdup(v->value) : NULL;
        s->flags = v->flags;
    }
    s->next = list;
    return s;
}

/* 链表头是最后保存的；按顺序恢复，最早保存的（真正的原值）最后写回 */
void var_restore(varsave *list)
{
    while (list) {
        varsave *n = list->next;
        var *v = vlookup(list->name);
        if (v)
            v->flags &= ~V_READONLY;
        if (list->existed) {
            if (!v)
                v = vcreate(list->name);
            free(v->value);
            v->value = list->value;
            list->value = NULL;
            v->flags = list->flags;
        } else if (v) {
            var_unset(list->name);
        }
        env_dirty = 1;
        free(list->name);
        free(list->value);
        free(list);
        list = n;
    }
}

typedef struct scope {
    varsave      *saves;
    struct scope *prev;
} scope;

static scope *scopes;

void var_scope_push(void)
{
    scope *s = xcalloc(1, sizeof *s);
    s->prev = scopes;
    scopes = s;
}

void var_scope_pop(void)
{
    scope *s = scopes;
    if (!s)
        return;
    scopes = s->prev;
    var_restore(s->saves);
    free(s);
}

int var_in_function(void)
{
    return scopes != NULL;
}

void var_make_local(const char *name)
{
    if (!scopes)
        return;
    for (varsave *s = scopes->saves; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return;         /* 本层已经是局部变量 */
    scopes->saves = var_save(scopes->saves, name);
}

/* ======================= 位置参数 ======================= */

static posparams g_pos;

static void pos_free_list(posparams *p)
{
    for (int i = 0; i < p->n; i++)
        free(p->v[i]);
    free(p->v);
    p->n = 0;
    p->v = NULL;
}

void pos_set(int n, char **v)
{
    posparams np;
    np.n = n;
    np.v = xcalloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++)
        np.v[i] = xstrdup(v[i]);
    pos_free_list(&g_pos);
    g_pos = np;
}

void pos_swap(posparams *saved)
{
    posparams t = g_pos;
    g_pos = *saved;
    *saved = t;
}

void pos_release(posparams *p)
{
    pos_free_list(p);
}

int pos_count(void)
{
    return g_pos.n;
}

const char *pos_get(int i)
{
    if (i < 1 || i > g_pos.n)
        return NULL;
    return g_pos.v[i - 1];
}

int pos_shift(int n)
{
    if (n < 0 || n > g_pos.n)
        return -1;
    for (int i = 0; i < n; i++)
        free(g_pos.v[i]);
    memmove(g_pos.v, g_pos.v + n, sizeof(char *) * (size_t)(g_pos.n - n));
    g_pos.n -= n;
    g_pos.v[g_pos.n] = NULL;
    return 0;
}

/* ======================= 别名 ======================= */

typedef struct alias {
    char         *name;
    char         *value;
    struct alias *next;
} alias;

static alias *aliases;

const char *alias_get(const char *name)
{
    for (alias *a = aliases; a; a = a->next)
        if (strcmp(a->name, name) == 0)
            return a->value;
    return NULL;
}

void alias_set(const char *name, const char *value)
{
    for (alias *a = aliases; a; a = a->next)
        if (strcmp(a->name, name) == 0) {
            free(a->value);
            a->value = xstrdup(value);
            return;
        }
    alias *a = xcalloc(1, sizeof *a);
    a->name = xstrdup(name);
    a->value = xstrdup(value);
    a->next = aliases;
    aliases = a;
}

int alias_unset(const char *name)
{
    for (alias **pp = &aliases; *pp; pp = &(*pp)->next)
        if (strcmp((*pp)->name, name) == 0) {
            alias *a = *pp;
            *pp = a->next;
            free(a->name);
            free(a->value);
            free(a);
            return 0;
        }
    return -1;
}

void alias_clear(void)
{
    while (aliases) {
        alias *a = aliases;
        aliases = a->next;
        free(a->name);
        free(a->value);
        free(a);
    }
}

static int cmp_alias(const void *a, const void *b)
{
    return strcmp((*(alias *const *)a)->name, (*(alias *const *)b)->name);
}

void alias_foreach_sorted(var_cb cb, void *ud)
{
    size_t n = 0;
    for (alias *a = aliases; a; a = a->next)
        n++;
    alias **arr = xcalloc(n + 1, sizeof *arr);
    size_t k = 0;
    for (alias *a = aliases; a; a = a->next)
        arr[k++] = a;
    qsort(arr, n, sizeof *arr, cmp_alias);
    for (size_t i = 0; i < n; i++)
        cb(arr[i]->name, arr[i]->value, 0, ud);
    free(arr);
}

/* ======================= 函数 =======================
 * 函数体就是一棵语法树。树所在的内存池通过引用计数保持存活，
 * 直到函数被重新定义或 unset。 */

static shfunc *funcs;

shfunc *func_get(const char *name)
{
    for (shfunc *f = funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0)
            return f;
    return NULL;
}

void func_set(const char *name, node *body, arena *ar)
{
    arena_ref(ar);
    for (shfunc *f = funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) {
            arena *old = f->ar;
            f->body = body;
            f->ar = ar;
            arena_unref(old);
            return;
        }
    shfunc *f = xcalloc(1, sizeof *f);
    f->name = xstrdup(name);
    f->body = body;
    f->ar = ar;
    f->next = funcs;
    funcs = f;
}

int func_unset(const char *name)
{
    for (shfunc **pp = &funcs; *pp; pp = &(*pp)->next)
        if (strcmp((*pp)->name, name) == 0) {
            shfunc *f = *pp;
            *pp = f->next;
            arena_unref(f->ar);
            free(f->name);
            free(f);
            return 0;
        }
    return -1;
}

void func_foreach(void (*cb)(shfunc *f, void *ud), void *ud)
{
    for (shfunc *f = funcs; f; f = f->next)
        cb(f, ud);
}
