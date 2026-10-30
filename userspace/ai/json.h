/*
 * json.h — skaid 用的小型 JSON 库（解析成一棵树、修改、再序列化回去）
 *
 * Claude 的 Messages API 说的是 JSON：请求体是一个大对象（system、tools、messages……），
 * 流式回应的每个事件也是一行 JSON。我们只依赖 musl，不引入第三方库，所以自己写一个够用的：
 *   - 解析：递归下降，支持完整的 JSON 语法（含 \uXXXX 和代理对 → UTF-8）；
 *   - 树：每个节点是 jnode，数组/对象的孩子用单向链表串起来（保持原来的顺序）；
 *   - 序列化：紧凑格式（没有空格换行）；字符串里不合法的 UTF-8 字节换成 U+FFFD，
 *     因为命令的输出可能是任意字节，而 API 只接受合法的 UTF-8。
 * 数字统一存成 double（我们用到的数字都是 token 数、下标这样的小整数）。
 */
#ifndef SKAI_JSON_H
#define SKAI_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jnode {
    jtype t;
    char *key;              /* 作为对象成员时的键名，否则为 NULL */
    char *s;                /* J_STR 的内容（UTF-8，以 \0 结尾；内容里可能本身含 \0，长度见 slen） */
    size_t slen;
    double n;               /* J_NUM */
    int b;                  /* J_BOOL */
    struct jnode *child;    /* J_ARR / J_OBJ 的第一个孩子 */
    struct jnode *last;     /* 最后一个孩子（追加用） */
    struct jnode *next;     /* 兄弟 */
} jnode;

/* 解析 len 字节的文本。失败返回 NULL，*err（可为 NULL）指向一条静态的错误说明 */
jnode *json_parse(const char *text, size_t len, const char **err);
void   json_free(jnode *n);

/* 序列化（紧凑格式），返回 malloc 的字符串，*len 可为 NULL */
char  *json_dump(const jnode *n, size_t *len);

/* ---------- 构造 ---------- */
jnode *json_null(void);
jnode *json_bool(int b);
jnode *json_num(double v);
jnode *json_str(const char *s);                 /* 复制 s */
jnode *json_strn(const char *s, size_t len);
jnode *json_arr(void);
jnode *json_obj(void);
jnode *json_clone(const jnode *n);
/* 把 child 追加到数组/对象的末尾（对象时 key 是键名，数组时传 NULL）。返回 child，方便链式调用 */
jnode *json_add(jnode *parent, const char *key, jnode *child);
/* 对象：设置成员（已有同名成员就替换掉它） */
void   json_set(jnode *obj, const char *key, jnode *child);
/* 对象：删除成员 */
void   json_del(jnode *obj, const char *key);

/* ---------- 读取 ---------- */
jnode      *json_get(const jnode *obj, const char *key);      /* 不存在或 obj 不是对象 → NULL */
const char *json_get_str(const jnode *obj, const char *key);  /* 不是字符串 → NULL */
double      json_get_num(const jnode *obj, const char *key, double def);
int         json_get_bool(const jnode *obj, const char *key, int def);
int         json_count(const jnode *arr);
jnode      *json_at(const jnode *arr, int i);

/* 把字符串追加到 J_STR 节点后面（流式回应里一段段拼接文字用） */
void json_str_append(jnode *n, const char *s, size_t len);

#endif /* SKAI_JSON_H */
