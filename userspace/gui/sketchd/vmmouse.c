/*
 * vmmouse.c — 用户态的 “VMware 绝对坐标鼠标” 驱动
 *
 * 为什么需要它？
 *   PS/2 鼠标只报告“相对移动量”(dx, dy)。在虚拟机/网页模拟器里，宿主机的鼠标指针
 *   和虚拟机里的指针各走各的，很快就对不上（还需要“捕获鼠标”）。VMware 设计了一个
 *   “绝对坐标”通道：虚拟机直接告诉客户机“指针现在在屏幕的 (x, y)”。QEMU 和 v86 都实现了它。
 *
 * 它怎么通信？——I/O 端口上的“后门”(backdoor)
 *   x86 CPU 除了内存地址空间，还有一个独立的 64K “I/O 端口”空间，用 in/out 指令访问
 *   （键盘控制器在 0x60/0x64，串口在 0x3F8……）。VMware 约定：
 *       eax = 0x564D5868 ('VMXh' 魔数)   ecx = 命令号   ebx = 参数   dx = 0x5658
 *       执行 `inl %dx, %eax`
 *   真实硬件上这个端口什么都没有，读出来是 0xFFFFFFFF；在虚拟机里，这条指令会
 *   “陷入”(trap) 到虚拟机监控程序，由它填好 eax/ebx/ecx/edx 再返回——就像一次
 *   从客户机到虚拟机的“超级调用”(hypercall)。
 *
 * 用户态程序能执行 in/out 吗？
 *   默认不能（CPU 的 I/O 权限检查会触发保护异常 → 内核发 SIGSEGV）。Linux 提供
 *   ioperm(端口, 个数, 1) 系统调用：内核在本进程的“I/O 权限位图”里打开这几个端口，
 *   之后本进程就可以直接 in/out 它们。需要 root（CAP_SYS_RAWIO）和内核选项
 *   CONFIG_X86_IOPL_IOPERM。这正是“用户态驱动”的一个小例子。
 *
 * 数据怎么来？
 *   启用绝对模式后，虚拟机把每个鼠标事件放进一个队列：每个事件 4 个 dword
 *   [状态|按钮, x(0..0xFFFF), y(0..0xFFFF), 滚轮]。STATUS 命令返回队列里有几个 dword，
 *   DATA 命令一次取出 4 个。虚拟机放入数据时还会“顺便”产生一个 PS/2 鼠标中断，
 *   所以 /dev/input 里的 PS/2 鼠标设备会收到一个事件——我们就把它当成“有数据了”的闹钟
 *   （见 input.c），不需要轮询。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <errno.h>
#include <string.h>

#if defined(__i386__) || defined(__x86_64__)
#include <sys/io.h>
#define HAVE_IOPORTS 1
#endif

#define VMW_MAGIC               0x564D5868u
#define VMW_PORT                0x5658
#define VMW_CMD_GETVERSION      10
#define VMW_CMD_ABSPOINTER_DATA    39
#define VMW_CMD_ABSPOINTER_STATUS  40
#define VMW_CMD_ABSPOINTER_COMMAND 41
#define VMW_ABS_ENABLE          0x45414552u   /* 'EAER' */
#define VMW_ABS_DISABLE         0x000000F5u
#define VMW_ABS_RELATIVE        0x4C455252u   /* 'LERR' */
#define VMW_ABS_ABSOLUTE        0x53424152u   /* 'SBAR' */
#define VMW_VERSION_ID          0x3442554Au
#define VMW_STATUS_ERROR        0xFFFF0000u
#define VMW_PACKET_RELATIVE     0x00010000u
#define VMW_BTN_LEFT            0x20
#define VMW_BTN_RIGHT           0x10
#define VMW_BTN_MIDDLE          0x08

typedef struct { uint32_t eax, ebx, ecx, edx, esi, edi; } vmw_regs;

static int g_enabled;
static int g_errors;

#ifdef HAVE_IOPORTS
/* 一次后门调用。所有 6 个通用寄存器都可能被虚拟机改写，全部声明为输入输出。 */
static void vmw_call(uint32_t cmd, uint32_t arg, vmw_regs *r)
{
    r->eax = VMW_MAGIC;
    r->ebx = arg;
    r->ecx = cmd;
    r->edx = VMW_PORT;
    r->esi = 0;
    r->edi = 0;
    __asm__ __volatile__("inl %%dx, %%eax"
                         : "+a"(r->eax), "+b"(r->ebx), "+c"(r->ecx), "+d"(r->edx),
                           "+S"(r->esi), "+D"(r->edi)
                         :
                         : "memory");
}

/* 启用绝对坐标模式，步骤与 Linux 内核的 drivers/input/mouse/vmmouse.c 相同 */
static int vmw_enable(void)
{
    vmw_regs r;
    vmw_call(VMW_CMD_ABSPOINTER_COMMAND, VMW_ABS_ENABLE, &r);
    vmw_call(VMW_CMD_ABSPOINTER_STATUS, 0, &r);
    if ((r.eax & 0x0000FFFFu) == 0) {
        sk_log("vmmouse: 启用后队列为空，没有绝对坐标鼠标");
        return -1;
    }
    vmw_call(VMW_CMD_ABSPOINTER_DATA, 1, &r);
    if (r.eax != VMW_VERSION_ID) {
        sk_log("vmmouse: 版本号不对 (%#x)", r.eax);
        vmw_call(VMW_CMD_ABSPOINTER_COMMAND, VMW_ABS_DISABLE, &r);
        return -1;
    }
    vmw_call(VMW_CMD_ABSPOINTER_COMMAND, VMW_ABS_ABSOLUTE, &r);
    return 0;
}
#endif

int vmmouse_init(void)
{
#ifdef HAVE_IOPORTS
    if (ioperm(VMW_PORT, 4, 1) < 0) {
        sk_log("vmmouse: ioperm(0x%x) 失败: %s（需要 root 和 CONFIG_X86_IOPL_IOPERM）",
               VMW_PORT, strerror(errno));
        return -1;
    }
    vmw_regs r;
    vmw_call(VMW_CMD_GETVERSION, ~0u, &r);
    if (r.ebx != VMW_MAGIC) {
        /* 真机上这个端口没有设备：ebx 不会变成魔数 */
        sk_log("vmmouse: 没有检测到 VMware 后门（不在 QEMU/v86/VMware 里？）");
        ioperm(VMW_PORT, 4, 0);
        return -1;
    }
    if (vmw_enable() < 0) {
        ioperm(VMW_PORT, 4, 0);
        return -1;
    }
    g_enabled = 1;
    sk_log("vmmouse: 已启用绝对坐标模式（后门版本 %u）", r.eax);
    return 0;
#else
    sk_log("vmmouse: 当前 CPU 架构没有 I/O 端口");
    return -1;
#endif
}

int vmmouse_read(skd_vmmouse_packet *out, int max)
{
#ifdef HAVE_IOPORTS
    if (!g_enabled)
        return -1;
    int n = 0;
    while (n < max) {
        vmw_regs r;
        vmw_call(VMW_CMD_ABSPOINTER_STATUS, 0, &r);
        uint32_t status = r.eax;
        if ((status & VMW_STATUS_ERROR) == VMW_STATUS_ERROR) {
            /* 虚拟机说队列出错了（例如溢出）：按内核驱动的做法重新启用 */
            if (g_errors++ < 5)
                sk_log("vmmouse: 队列错误，重新启用");
            vmw_enable();
            return n > 0 ? n : -1;
        }
        uint32_t queued = status & 0xFFFFu;
        if (queued == 0)
            break;
        if (queued % 4) {
            /* 一个包是 4 个 dword；不是 4 的倍数说明不同步了 */
            if (g_errors++ < 5)
                sk_log("vmmouse: 队列长度 %u 不是 4 的倍数，重新启用", queued);
            vmw_enable();
            break;
        }
        vmw_call(VMW_CMD_ABSPOINTER_DATA, 4, &r);
        skd_vmmouse_packet *p = &out[n++];
        p->relative = (r.eax & VMW_PACKET_RELATIVE) != 0;
        p->x = (int32_t)r.ebx;
        p->y = (int32_t)r.ecx;
        /* 滚轮：低 8 位是带符号数，正数表示“向下”滚；协议里正数=向上，所以取反 */
        p->wheel = -(int)(int8_t)(r.edx & 0xFF);
        p->buttons = 0;
        if (r.eax & VMW_BTN_LEFT)   p->buttons |= SK_BUTTON_LEFT;
        if (r.eax & VMW_BTN_RIGHT)  p->buttons |= SK_BUTTON_RIGHT;
        if (r.eax & VMW_BTN_MIDDLE) p->buttons |= SK_BUTTON_MIDDLE;
    }
    return n;
#else
    (void)out;
    (void)max;
    return -1;
#endif
}

void vmmouse_shutdown(void)
{
#ifdef HAVE_IOPORTS
    if (!g_enabled)
        return;
    vmw_regs r;
    /* 退回相对模式并关闭：之后 PS/2 鼠标又会正常报告相对移动 */
    vmw_call(VMW_CMD_ABSPOINTER_COMMAND, VMW_ABS_RELATIVE, &r);
    vmw_call(VMW_CMD_ABSPOINTER_COMMAND, VMW_ABS_DISABLE, &r);
    ioperm(VMW_PORT, 4, 0);
    g_enabled = 0;
#endif
}
