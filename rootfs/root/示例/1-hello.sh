#!/bin/sksh
# 1-hello.sh — 第一个脚本：变量、命令替换、退出状态
#
# 运行方法：  sh ~/示例/1-hello.sh
#   或者：    cd ~/示例   然后   ./1-hello.sh
#
# 第一行 "#!/bin/sksh" 叫 shebang：直接运行这个文件时，内核读到开头的 #!，
# 就改为运行 /bin/sksh，并把这个脚本的路径作为参数交给它。

# 变量：等号两边不能有空格；用的时候前面加 $
who="SketchOS 的新朋友"
echo "你好，$who！"

# $(命令) 会被替换成这个命令的输出
kernel=$(uname -r)
host=$(cat /etc/hostname)
echo "这台电脑叫 $host，运行的 Linux 内核版本是 $kernel"
echo "现在的时间是：$(date)"

# $? 是上一条命令的“退出状态”：0 表示成功，非 0 表示失败
ls /etc/hostname > /dev/null
echo "ls 一个存在的文件，退出状态是 $?"
ls /不存在的目录 2> /dev/null
echo "ls 一个不存在的目录，退出状态是 $?"
