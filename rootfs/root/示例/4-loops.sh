#!/bin/sksh
# 4-loops.sh — 循环：for 和 while
#
# for 把一组单词挨个放进变量里；while 只要条件命令成功（退出状态 0）就一直循环。

# 数一数几个特殊目录里有多少项。/proc 里的数目每次运行都可能不一样——
# 因为每个数字目录就是一个进程，进程来来去去。
for dir in /proc /sys /dev /tmp /run; do
    echo "$dir 里有 $(ls $dir | wc -l) 项"
done

# 通配符 * 由 shell 展开成匹配的文件名列表
echo "/etc/sketch 下的配置文件："
for f in /etc/sketch/*; do
    echo "  $f"
done

# while + read：一行一行地读文件。read 读到文件末尾时返回非 0，循环就结束了
echo "系统认识的 shell（/etc/shells）："
while read line; do
    echo "  -> $line"
done < /etc/shells

# 循环也能接在管道后面：给 /etc/passwd 里的每个用户打个招呼
cat /etc/passwd | while read entry; do
    echo "用户记录：$entry"
done
