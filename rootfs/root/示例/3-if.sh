#!/bin/sksh
# 3-if.sh — 条件判断：if / elif / else、test（也写作 [ ]）、&& 和 ||
#
# shell 的 if 判断的不是“真假”，而是命令的退出状态：0 就当作“真”。
# [ -d /proc/1 ] 其实是在运行 test 命令，它检查条件，成立就返回 0。

if [ -d /proc/1 ]; then
    echo "找到了 /proc/1：1 号进程 sinit 正在运行"
fi

# grep 找到匹配的行就返回 0；这里只关心找没找到，输出扔进 /dev/null
if grep sketch.nogui /proc/cmdline > /dev/null; then
    echo "现在是文字模式：内核命令行里有 sketch.nogui"
elif [ -e /run/sketch/display.sock ]; then
    echo "图形桌面正在运行（显示服务器的套接字 /run/sketch/display.sock 存在）"
else
    echo "图形桌面还没有启动"
fi

# 字符串比较
host=$(cat /etc/hostname)
if [ "$host" = "sketchos" ]; then
    echo "主机名还是默认的 sketchos"
else
    echo "主机名被改成了 $host"
fi

# &&：前面成功了才执行后面；||：前面失败了才执行后面
[ -f /etc/os-release ] && echo "有 /etc/os-release，系统身份证在这里"
[ -f /etc/不存在 ] || echo "没有 /etc/不存在 这个文件（当然没有）"
cd /tmp && echo "成功进入了 $(pwd)"
