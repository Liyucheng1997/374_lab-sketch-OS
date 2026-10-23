/*
 * keymap.c — 键盘布局：把“按了哪个键”翻译成“输入了哪个字符”
 *
 * 键盘硬件（以及内核的 evdev 接口）只报告“扫描码/键码”：KEY_A = 30 表示“A 这个位置的键”，
 * 它并不知道你想输入的是 'a'、'A' 还是法语键盘上的 'q'。把键码变成字符要结合：
 *   - 键盘布局（我们只实现美式 US 布局）；
 *   - 修饰键状态：Shift 按住没有、Caps Lock / Num Lock 开着没有。
 * 这一步在 Linux 桌面上通常由 xkbcommon 库完成；文字终端里由内核的 keyboard.c +
 * 键盘映射表 (loadkeys) 完成。我们自己写一个最小版本。
 */
#include "sketchd.h"

#include <linux/input-event-codes.h>

/* 美式布局：键码 → { 普通, 按住 Shift }。0 表示这个键不产生字符 */
static const uint16_t g_us[][2] = {
    [KEY_1] = { '1', '!' },  [KEY_2] = { '2', '@' },  [KEY_3] = { '3', '#' },
    [KEY_4] = { '4', '$' },  [KEY_5] = { '5', '%' },  [KEY_6] = { '6', '^' },
    [KEY_7] = { '7', '&' },  [KEY_8] = { '8', '*' },  [KEY_9] = { '9', '(' },
    [KEY_0] = { '0', ')' },  [KEY_MINUS] = { '-', '_' }, [KEY_EQUAL] = { '=', '+' },
    [KEY_Q] = { 'q', 'Q' },  [KEY_W] = { 'w', 'W' },  [KEY_E] = { 'e', 'E' },
    [KEY_R] = { 'r', 'R' },  [KEY_T] = { 't', 'T' },  [KEY_Y] = { 'y', 'Y' },
    [KEY_U] = { 'u', 'U' },  [KEY_I] = { 'i', 'I' },  [KEY_O] = { 'o', 'O' },
    [KEY_P] = { 'p', 'P' },  [KEY_LEFTBRACE] = { '[', '{' }, [KEY_RIGHTBRACE] = { ']', '}' },
    [KEY_A] = { 'a', 'A' },  [KEY_S] = { 's', 'S' },  [KEY_D] = { 'd', 'D' },
    [KEY_F] = { 'f', 'F' },  [KEY_G] = { 'g', 'G' },  [KEY_H] = { 'h', 'H' },
    [KEY_J] = { 'j', 'J' },  [KEY_K] = { 'k', 'K' },  [KEY_L] = { 'l', 'L' },
    [KEY_SEMICOLON] = { ';', ':' }, [KEY_APOSTROPHE] = { '\'', '"' }, [KEY_GRAVE] = { '`', '~' },
    [KEY_BACKSLASH] = { '\\', '|' },
    [KEY_Z] = { 'z', 'Z' },  [KEY_X] = { 'x', 'X' },  [KEY_C] = { 'c', 'C' },
    [KEY_V] = { 'v', 'V' },  [KEY_B] = { 'b', 'B' },  [KEY_N] = { 'n', 'N' },
    [KEY_M] = { 'm', 'M' },  [KEY_COMMA] = { ',', '<' }, [KEY_DOT] = { '.', '>' },
    [KEY_SLASH] = { '/', '?' }, [KEY_SPACE] = { ' ', ' ' },
    [KEY_102ND] = { '\\', '|' },      /* 欧洲键盘左 Shift 旁边多出的那个键 */
    /* 控制字符：协议约定 回车=0x0D 退格=0x08 Tab=0x09 Esc=0x1B */
    [KEY_ENTER] = { 0x0D, 0x0D }, [KEY_BACKSPACE] = { 0x08, 0x08 },
    [KEY_TAB] = { 0x09, 0x09 }, [KEY_ESC] = { 0x1B, 0x1B },
    /* 小键盘上不受 NumLock 影响的键 */
    [KEY_KPSLASH] = { '/', '/' }, [KEY_KPASTERISK] = { '*', '*' },
    [KEY_KPMINUS] = { '-', '-' }, [KEY_KPPLUS] = { '+', '+' },
    [KEY_KPENTER] = { 0x0D, 0x0D }, [KEY_KPEQUAL] = { '=', '=' },
};

/* 小键盘数字键：NumLock 开 → 数字；关 → 方向/编辑键（键码被改写，不产生字符） */
static const struct { uint16_t key; char digit; uint16_t nav; } g_keypad[] = {
    { KEY_KP7, '7', KEY_HOME }, { KEY_KP8, '8', KEY_UP },     { KEY_KP9, '9', KEY_PAGEUP },
    { KEY_KP4, '4', KEY_LEFT }, { KEY_KP5, '5', 0 },          { KEY_KP6, '6', KEY_RIGHT },
    { KEY_KP1, '1', KEY_END },  { KEY_KP2, '2', KEY_DOWN },   { KEY_KP3, '3', KEY_PAGEDOWN },
    { KEY_KP0, '0', KEY_INSERT }, { KEY_KPDOT, '.', KEY_DELETE },
};

int keymap_update(skd_keymap_state *st, uint32_t keycode, int value)
{
    int down = value != 0;          /* 1=按下 2=自动重复 都算“按着” */
    switch (keycode) {
    case KEY_LEFTSHIFT:  st->shift_l = down; break;
    case KEY_RIGHTSHIFT: st->shift_r = down; break;
    case KEY_LEFTCTRL:   st->ctrl_l = down; break;
    case KEY_RIGHTCTRL:  st->ctrl_r = down; break;
    case KEY_LEFTALT:    st->alt_l = down; break;
    case KEY_RIGHTALT:   st->alt_r = down; break;
    case KEY_LEFTMETA:   st->meta_l = down; break;
    case KEY_RIGHTMETA:  st->meta_r = down; break;
    case KEY_CAPSLOCK:
        /* 锁定键只在“按下”的那一刻切换；按住产生的自动重复 (value=2) 不算 */
        if (value == 1) {
            st->caps_lock = !st->caps_lock;
            return 1;
        }
        break;
    case KEY_NUMLOCK:
        if (value == 1) {
            st->num_lock = !st->num_lock;
            return 1;
        }
        break;
    default:
        break;
    }
    return 0;
}

uint32_t keymap_modifiers(const skd_keymap_state *st)
{
    uint32_t m = 0;
    if (st->shift_l || st->shift_r) m |= SK_MOD_SHIFT;
    if (st->ctrl_l || st->ctrl_r)   m |= SK_MOD_CTRL;
    if (st->alt_l || st->alt_r)     m |= SK_MOD_ALT;
    if (st->meta_l || st->meta_r)   m |= SK_MOD_SUPER;
    if (st->caps_lock)              m |= SK_MOD_CAPS;
    return m;
}

uint32_t keymap_translate(const skd_keymap_state *st, uint32_t *keycode)
{
    uint32_t k = *keycode;
    for (size_t i = 0; i < SK_ARRAY_LEN(g_keypad); i++) {
        if (g_keypad[i].key != k)
            continue;
        if (st->num_lock)
            return (uint32_t)g_keypad[i].digit;
        if (g_keypad[i].nav)
            *keycode = g_keypad[i].nav;
        return 0;
    }
    if (k >= SK_ARRAY_LEN(g_us))
        return 0;
    int shift = st->shift_l || st->shift_r;
    uint32_t base = g_us[k][0];
    if (base == 0)
        return 0;
    /* Caps Lock 只影响字母：开着时相当于对字母反转 Shift（Shift+Caps 又变回小写） */
    if (base >= 'a' && base <= 'z' && st->caps_lock)
        shift = !shift;
    /* Ctrl+字母：协议约定仍给出字母本身（应用自己根据 modifiers 判断 Ctrl+C 等） */
    return g_us[k][shift ? 1 : 0];
}
