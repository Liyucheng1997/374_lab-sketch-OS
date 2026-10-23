/*
 * undo.c — 撤销/重做：按小块做“写时复制”
 *
 * 最笨的撤销办法是每画一笔之前把整张图复制一份：640×420 的图一份就是 1 MB，
 * 记 50 步就是 50 MB——SketchOS 的虚拟机总共才 256 MB。可大多数笔画只改了很小一块。
 *
 * 所以这里借用操作系统里“写时复制”（copy-on-write）的思路：
 *   内核 fork() 时并不复制父进程的内存，而是让父子进程共享同一批物理页，
 *   等到有人要“写”某一页时，才把那一页复制一份。只有真正被改的页才花内存。
 * 画板把图片切成 64×64 的小块（相当于“页”）。一次操作（一笔、一个图形、一次填充）
 * 开始后，每次要往某个区域画之前先调用 undo_touch()：区域里还没保存过的块，
 * 先把“改之前”的像素复制一份存起来。操作结束时，存下来的这些块就是一条撤销记录。
 *
 * 撤销 = 把记录里的块和图片里的块“交换”：图片恢复成改之前的样子，
 *        记录里留下的正好是改之后的样子——这条记录直接变成重做记录，不用再复制。
 * 重做 = 再交换一次。
 *
 * 内存有上限：记录总量超过 UNDO_BUDGET 或步数超过 UNDO_MAX_LEVELS 时，丢掉最旧的记录。
 *
 * “版本号”：每条记录知道自己之前、之后的图片版本。保存时记下当时的版本，
 * 以后只要当前版本和它不同，就说明“改过了，需要保存”——撤销回保存时的样子，标题上的 * 也会消失。
 */
#include "skpaint.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int       tx, ty;       /* 块坐标（第几列、第几行块） */
    uint32_t *px;           /* 块里的像素（宽 tw × 高 th，紧密排列） */
} undo_tile;

struct undo_op {
    undo_tile *tiles;
    int        n, cap;
    size_t     bytes;
    unsigned   before, after;   /* 操作前、后的图片版本 */
};

static void op_free(undo_op *op)
{
    if (!op)
        return;
    for (int i = 0; i < op->n; i++)
        free(op->tiles[i].px);
    free(op->tiles);
    free(op);
}

/* 第 (tx,ty) 块在图片里的矩形（最右/最下的块可能不满 64） */
static sk_rect tile_rect(const undo_hist *h, int tx, int ty)
{
    int x = tx * UNDO_TILE, y = ty * UNDO_TILE;
    int w = h->img->w - x < UNDO_TILE ? h->img->w - x : UNDO_TILE;
    int hh = h->img->h - y < UNDO_TILE ? h->img->h - y : UNDO_TILE;
    return sk_rect_make(x, y, w, hh);
}

static void clear_list(undo_hist *h, undo_op **list, int *n)
{
    for (int i = 0; i < *n; i++) {
        h->bytes -= list[i]->bytes;
        op_free(list[i]);
        list[i] = NULL;
    }
    *n = 0;
}

void undo_free(undo_hist *h)
{
    clear_list(h, h->undo, &h->nundo);
    clear_list(h, h->redo, &h->nredo);
    op_free(h->cur);
    h->cur = NULL;
    free(h->saved);
    h->saved = NULL;
    h->bytes = 0;
}

int undo_reset(undo_hist *h, sk_canvas *img)
{
    undo_free(h);
    h->img = img;
    h->tiles_x = (img->w + UNDO_TILE - 1) / UNDO_TILE;
    h->tiles_y = (img->h + UNDO_TILE - 1) / UNDO_TILE;
    h->saved = calloc((size_t)h->tiles_x * h->tiles_y, 1);
    h->oom = 0;
    h->version = ++h->next_version;     /* 新图片 = 新版本（版本号永远递增，不会和旧的混淆） */
    return h->saved ? 0 : -1;
}

void undo_begin(undo_hist *h)
{
    if (h->cur)
        undo_end(h);
    h->cur = calloc(1, sizeof *h->cur);
    if (!h->cur) {
        h->oom = 1;
        return;
    }
    h->cur->before = h->version;
    h->oom = 0;
    if (h->saved)
        memset(h->saved, 0, (size_t)h->tiles_x * h->tiles_y);
}

int undo_in_progress(const undo_hist *h) { return h->cur != NULL; }

/* 把一块“改之前”的像素复制进当前操作的记录 */
static int save_tile(undo_hist *h, int tx, int ty)
{
    undo_op *op = h->cur;
    if (op->n == op->cap) {
        int ncap = op->cap ? op->cap * 2 : 16;
        undo_tile *nt = realloc(op->tiles, sizeof *nt * (size_t)ncap);
        if (!nt)
            return -1;
        op->tiles = nt;
        op->cap = ncap;
    }
    sk_rect r = tile_rect(h, tx, ty);
    size_t bytes = (size_t)r.w * r.h * 4;
    uint32_t *px = malloc(bytes);
    if (!px)
        return -1;
    /* 整行 memcpy：比逐像素拷贝快得多（网页版的 CPU 很慢） */
    for (int y = 0; y < r.h; y++)
        memcpy(px + (size_t)y * r.w, h->img->pixels + (size_t)(r.y + y) * h->img->stride + r.x, (size_t)r.w * 4);
    op->tiles[op->n].tx = tx;
    op->tiles[op->n].ty = ty;
    op->tiles[op->n].px = px;
    op->n++;
    op->bytes += bytes;
    h->bytes += bytes;
    return 0;
}

void undo_touch(undo_hist *h, sk_rect r)
{
    if (!h->cur || h->oom)
        return;
    if (!h->saved) {
        h->oom = 1;                     /* 没法记录：至少要让 undo_end 知道图片可能被改了 */
        return;
    }
    r = sk_rect_intersect(r, sk_rect_make(0, 0, h->img->w, h->img->h));
    if (sk_rect_empty(r))
        return;
    int tx0 = r.x / UNDO_TILE, tx1 = (r.x + r.w - 1) / UNDO_TILE;
    int ty0 = r.y / UNDO_TILE, ty1 = (r.y + r.h - 1) / UNDO_TILE;
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            uint8_t *s = &h->saved[ty * h->tiles_x + tx];
            if (*s)
                continue;
            if (save_tile(h, tx, ty) != 0) {
                h->oom = 1;
                return;
            }
            *s = 1;
        }
    }
}

/* 块的内容和现在的图片完全一样吗？（这一块其实没被改） */
static int tile_unchanged(const undo_hist *h, const undo_tile *t)
{
    sk_rect r = tile_rect(h, t->tx, t->ty);
    for (int y = 0; y < r.h; y++)
        if (memcmp(t->px + (size_t)y * r.w, h->img->pixels + (size_t)(r.y + y) * h->img->stride + r.x,
                   (size_t)r.w * 4) != 0)
            return 0;
    return 1;
}

/* 去掉没有真正改变的块：笔画包围盒边上的块、在白纸上用橡皮擦……
 * 这样记录更省内存，而且“什么都没改”的操作不会让标题出现 *。 */
static void prune_unchanged(undo_hist *h, undo_op *op)
{
    int k = 0;
    for (int i = 0; i < op->n; i++) {
        if (tile_unchanged(h, &op->tiles[i])) {
            sk_rect r = tile_rect(h, op->tiles[i].tx, op->tiles[i].ty);
            size_t bytes = (size_t)r.w * r.h * 4;
            op->bytes -= bytes;
            h->bytes -= bytes;
            free(op->tiles[i].px);
        } else {
            op->tiles[k++] = op->tiles[i];
        }
    }
    op->n = k;
}

/* 丢掉最旧的一条撤销记录 */
static void drop_oldest(undo_hist *h)
{
    undo_op *old = h->undo[0];
    h->bytes -= old->bytes;
    op_free(old);
    memmove(h->undo, h->undo + 1, sizeof h->undo[0] * (size_t)(h->nundo - 1));
    h->undo[--h->nundo] = NULL;
}

int undo_end(undo_hist *h)
{
    undo_op *op = h->cur;
    h->cur = NULL;
    if (!op) {
        if (!h->oom)
            return UNDO_NOTHING;
        /* 连记录本身都没分配成功：图片可能已经改了，旧记录也对不上了 */
        clear_list(h, h->undo, &h->nundo);
        clear_list(h, h->redo, &h->nredo);
        h->version = ++h->next_version;
        h->oom = 0;
        return UNDO_NO_MEMORY;
    }
    if (h->oom) {
        /* 中途内存不足：这一步的记录不完整，撤销它只会恢复一部分，干脆全部清空 */
        h->bytes -= op->bytes;
        op_free(op);
        clear_list(h, h->undo, &h->nundo);
        clear_list(h, h->redo, &h->nredo);
        h->version = ++h->next_version;
        h->oom = 0;
        return UNDO_NO_MEMORY;
    }
    prune_unchanged(h, op);
    if (op->n == 0) {
        op_free(op);
        return UNDO_NOTHING;
    }
    /* 有了新的操作，之前撤销掉的那些就不能再重做了 */
    clear_list(h, h->redo, &h->nredo);
    op->after = h->version = ++h->next_version;
    if (h->nundo == UNDO_MAX_LEVELS)
        drop_oldest(h);
    h->undo[h->nundo++] = op;
    while (h->nundo > 0 && h->bytes > UNDO_BUDGET)
        drop_oldest(h);
    return h->nundo > 0 && h->undo[h->nundo - 1] == op ? UNDO_RECORDED : UNDO_TOO_BIG;
}

/* 记录里的块和图片里的块互换，返回改动范围 */
static sk_rect swap_tiles(undo_hist *h, undo_op *op)
{
    sk_rect changed = sk_rect_make(0, 0, 0, 0);
    uint32_t tmp[UNDO_TILE];
    for (int i = 0; i < op->n; i++) {
        sk_rect r = tile_rect(h, op->tiles[i].tx, op->tiles[i].ty);
        size_t rowbytes = (size_t)r.w * 4;
        for (int y = 0; y < r.h; y++) {
            uint32_t *a = h->img->pixels + (size_t)(r.y + y) * h->img->stride + r.x;
            uint32_t *b = op->tiles[i].px + (size_t)y * r.w;
            memcpy(tmp, a, rowbytes);
            memcpy(a, b, rowbytes);
            memcpy(b, tmp, rowbytes);
        }
        changed = sk_rect_union(changed, r);
    }
    return changed;
}

int undo_undo(undo_hist *h, sk_rect *changed)
{
    if (h->cur)
        undo_end(h);
    if (h->nundo == 0)
        return 0;
    undo_op *op = h->undo[--h->nundo];
    h->undo[h->nundo] = NULL;
    sk_rect r = swap_tiles(h, op);
    h->redo[h->nredo++] = op;          /* nredo + nundo 不会超过 UNDO_MAX_LEVELS */
    h->version = op->before;
    if (changed)
        *changed = r;
    return 1;
}

int undo_redo(undo_hist *h, sk_rect *changed)
{
    if (h->cur)
        undo_end(h);
    if (h->nredo == 0)
        return 0;
    undo_op *op = h->redo[--h->nredo];
    h->redo[h->nredo] = NULL;
    sk_rect r = swap_tiles(h, op);
    h->undo[h->nundo++] = op;
    h->version = op->after;
    if (changed)
        *changed = r;
    return 1;
}
