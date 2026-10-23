#!/bin/bash
# skbox 自动化测试（在 WSL 里运行）
#
#   bash tests/skbox/run_tests.sh [skbox 程序路径]
#
# skbox 是静态链接的 32 位程序，WSL 可以直接运行。测试方法：
#   1. 用 skbox --install 在临时目录里为每个工具建符号链接（顺便测试按 argv[0] 分发）；
#   2. 在临时目录里造一批文件，同一条命令分别用 skbox 和 GNU coreutils 运行，比较输出和退出状态；
#   3. 语义和 GNU 不同的地方（中文提示、/proc 类工具）做“合理性”检查。
# Ubuntu 26.04 默认的 coreutils 是 Rust 版（uutils），GNU 版的命令带 gnu 前缀（gnuls、gnusort……），
# 有 gnu 前缀版本时优先用它。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKBOX="${1:-${SKBOX:-$HOME/sketchos-work/build/userspace/bin/skbox}}"
[ -x "$SKBOX" ] || { echo "找不到 skbox 程序: $SKBOX" >&2; exit 2; }
SKBOX="$(readlink -f "$SKBOX")"

T="$(mktemp -d /tmp/skbox-test.XXXXXX)"
trap 'cd /; chmod -R u+rwx "$T" 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/w"
"$SKBOX" --install "$T/bin" >/dev/null || { echo "skbox --install 失败" >&2; exit 2; }
SB="$T/bin"

export LC_ALL=C.UTF-8 TZ=UTC0
unset NO_COLOR
PASS=0
FAIL=0
FAILED_NAMES=()

GENV="$(command -v gnuenv || command -v env)"
# GNU 版命令：优先 gnuXXX
g() {
    local name="$1"; shift
    if command -v "gnu$name" >/dev/null 2>&1; then "gnu$name" "$@"; else command "$name" "$@"; fi
}

ok()   { PASS=$((PASS + 1)); }
bad()  { FAIL=$((FAIL + 1)); FAILED_NAMES+=("$1"); echo "  ✗ $1"; }

# same 名称 '我们的命令' 'GNU 的命令'：比较标准输出和退出状态
same() {
    local name="$1" ours="$2" gnu="$3"
    local o1 o2 r1 r2
    o1="$(cd "$T/w" && eval "$ours" 2>/dev/null; echo "rc=$?")"
    o2="$(cd "$T/w" && eval "$gnu" 2>/dev/null; echo "rc=$?")"
    if [ "$o1" == "$o2" ]; then ok; else
        bad "$name"
        diff <(echo "$o1") <(echo "$o2") | head -20 | sed 's/^/      /'
    fi
}

# expect 名称 '命令' '期望输出'
expect() {
    local name="$1" cmd="$2" want="$3" got
    got="$(cd "$T/w" && eval "$cmd" 2>&1)"
    if [ "$got" == "$want" ]; then ok; else
        bad "$name"
        diff <(echo "$got") <(echo "$want") | head -20 | sed 's/^/      /'
    fi
}

# check 名称 '命令'：命令（bash 表达式）成功即通过
check() {
    local name="$1" cmd="$2"
    if (cd "$T/w" && eval "$cmd") >/dev/null 2>&1; then ok; else
        bad "$name"
        (cd "$T/w" && eval "$cmd") 2>&1 | head -10 | sed 's/^/      /'
    fi
}

section() { echo "== $*"; }

# ---------------------------------------------------------------------------
section "分发器"
expect "skbox 不带参数列出工具" "$SKBOX | grep -c '^  ls '" "1"
expect "skbox --list 含说明" "$SKBOX --list | grep -c '列出目录内容'" "1"
expect "skbox <applet>" "$SKBOX echo 你好" "你好"
expect "符号链接分发" "$SB/echo hi" "hi"
expect "[ 映射到 test" "$SB/[ 1 -lt 2 ] && echo yes" "yes"
expect "未知工具" "$SKBOX nosuch; echo rc=\$?" "skbox: 没有叫 'nosuch' 的工具（用 skbox --list 查看全部）
rc=127"
check "每个工具都有 --help" "for a in \$($SKBOX --names); do $SB/\$a --help | grep -q . || { echo \$a; exit 1; }; done"
check "--help 是中文" "for a in \$($SKBOX --names); do $SB/\$a --help | grep -q '用法' || { echo \$a; exit 1; }; done"
expect "错误选项的中文提示" "$SB/cat -Z 2>&1; echo rc=\$?" "cat: 无效的选项 -- 'Z'
请用 'cat --help' 查看用法。
rc=1"

# ---------------------------------------------------------------------------
section "准备测试文件"
cd "$T/w"
mkdir -p d/sub/deep e
for f in alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho \
         sigma tau upsilon phi chi psi omega 汉字文件 "with space" .hidden; do
    printf '%s\n' "$f" > "d/$f"
done
chmod +x d/alpha
ln -s alpha d/link
ln -s nowhere d/broken
mkfifo d/fifo
head -c 1536000 /dev/zero > d/big
printf 'x\n' > d/sub/one; printf 'yy\n' > d/sub/deep/two
touch -d '2020-01-02 03:04:05' d/beta
touch -d '2021-06-07 08:09:10' d/gamma
cat > lines.txt <<'EOF'
banana 3 yellow
apple 10 red
cherry 2 red
apple 10 red
date 25 brown
Banana 7 yellow
elderberry 1 purple
fig 100 purple

grape 03 green
apple 9 green
EOF
printf 'alpha\nalpha\nbeta\nbeta\nbeta\ngamma\nAlpha\nalpha\n' > dup.txt
printf 'a:b:c:d\n1:2:3:4\nno delim here\n:x::y\n' > colon.txt
printf '中文字符测试\nhello world\n汉字abc\n' > cjk.txt
printf 'no newline at end' > nonl.txt
seq 1 2000 > nums.txt
printf 'bin\0ary data match\n' > binary.dat
: > empty.txt

TS=$'+%_m月 %e  %Y\n%_m月 %e %H:%M'

# ---------------------------------------------------------------------------
section "ls"
same "ls 默认（非终端一行一个）" "$SB/ls d" "g ls d"
same "ls -a" "$SB/ls -a d" "g ls -a d"
same "ls -A" "$SB/ls -A d" "g ls -A d"
same "ls -C 80 列" "$SB/ls -C -w 80 d" "g ls -C -T0 -w 80 d"
same "ls -C 40 列" "$SB/ls -C -w 40 d" "g ls -C -T0 -w 40 d"
same "ls -C 20 列" "$SB/ls -C -w 20 d" "g ls -C -T0 -w 20 d"
same "ls -l" "$SB/ls -l d" "g ls -l --time-style=\"\$TS\" d | sed 's/^total/总用量/'"
same "ls -la" "$SB/ls -la d" "g ls -la --time-style=\"\$TS\" d | sed 's/^total/总用量/'"
same "ls -lh" "$SB/ls -lh d" "g ls -lh --time-style=\"\$TS\" d | sed 's/^total/总用量/'"
same "ls -lS" "$SB/ls -lS d" "g ls -lS --time-style=\"\$TS\" d | sed 's/^total/总用量/'"
same "ls -t" "$SB/ls -t d" "g ls -t d"
same "ls -tr" "$SB/ls -tr d" "g ls -tr d"
same "ls -S" "$SB/ls -S d" "g ls -S d"
same "ls -r" "$SB/ls -r d" "g ls -r d"
same "ls -1" "$SB/ls -1 d" "g ls -1 d"
same "ls -R" "$SB/ls -R d" "g ls -R d"
same "ls -R ." "$SB/ls -R ." "g ls -R ."
same "ls -d" "$SB/ls -d d d/sub" "g ls -d d d/sub"
same "ls -F" "$SB/ls -F d" "g ls -F d"
same "ls -i" "$SB/ls -i d" "g ls -i d"
same "ls -ln" "$SB/ls -ln d" "g ls -ln --time-style=\"\$TS\" d | sed 's/^total/总用量/'"
same "ls 多个参数" "$SB/ls d/alpha e d/sub" "g ls d/alpha e d/sub"
same "ls 不存在的文件（退出码 2）" "$SB/ls nope d/alpha" "g ls nope d/alpha"
same "ls -l 设备文件" "$SB/ls -l /dev/null /dev/zero /dev/tty" "g ls -l --time-style=\"\$TS\" /dev/null /dev/zero /dev/tty"
same "ls 跟随命令行上的目录链接" "ln -sfn d dl; $SB/ls dl" "g ls dl"
same "ls -l 不跟随目录链接" "$SB/ls -l dl" "g ls -l --time-style=\"\$TS\" dl"
expect "ls --color=always 目录蓝色" "$SB/ls --color=always -d d | od -An -c | tr -s ' ' | head -1" " 033 [ 0 1 ; 3 4 m d 033 [ 0 m \\n"
check "ls --color 可执行文件绿色" "$SB/ls --color=always d | grep -q \$'\\033\\[01;32malpha'"
check "ls --color 悬空链接" "$SB/ls --color=always d | grep -q \$'\\033\\[40;31;01mbroken'"

# ---------------------------------------------------------------------------
section "cat echo printf"
same "cat" "$SB/cat lines.txt cjk.txt nonl.txt" "g cat lines.txt cjk.txt nonl.txt"
same "cat -n" "$SB/cat -n lines.txt nonl.txt" "g cat -n lines.txt nonl.txt"
same "cat -b" "$SB/cat -b lines.txt" "g cat -b lines.txt"
same "cat -s" "printf 'a\n\n\n\nb\n\n' | $SB/cat -s" "printf 'a\n\n\n\nb\n\n' | g cat -s"
same "cat -E" "$SB/cat -E lines.txt" "g cat -E lines.txt"
same "cat 标准输入 -" "echo x | $SB/cat lines.txt - nonl.txt" "echo x | g cat lines.txt - nonl.txt"
same "cat 不存在" "$SB/cat nope lines.txt" "g cat nope lines.txt"
same "cat 目录" "$SB/cat d" "g cat d"
same "echo" "$SB/echo a  b   c" "g echo a  b   c"
same "echo -n" "$SB/echo -n abc" "g echo -n abc"
same "echo -e" "$SB/echo -e 'a\tb\\\\n\\x41\\0101\\c不输出'" "g echo -e 'a\tb\\\\n\\x41\\0101\\c不输出'"
same "echo -x 原样输出" "$SB/echo -x -n" "g echo -x -n"
same "printf 基本" "$SB/printf '%s=%d\n' a 1 b 2 c" "g printf '%s=%d\n' a 1 b 2 c"
same "printf 宽度精度" "$SB/printf '[%-6s][%6s][%.2s][%05d][%+d][%x][%X][%o][%#x]\n' ab cd efgh 42 7 255 255 8 255" \
                    "g printf '[%-6s][%6s][%.2s][%05d][%+d][%x][%X][%o][%#x]\n' ab cd efgh 42 7 255 255 8 255"
same "printf 浮点" "$SB/printf '%.3f %e %g %G %5.1f\n' 3.14159 12345.678 0.0001 1e20 2.25" \
                  "g printf '%.3f %e %g %G %5.1f\n' 3.14159 12345.678 0.0001 1e20 2.25"
same "printf 星号宽度" "$SB/printf '%*d|%-*d|%.*f\n' 5 42 4 7 2 3.14159" "g printf '%*d|%-*d|%.*f\n' 5 42 4 7 2 3.14159"
same "printf 进制和字符值" "$SB/printf '%d %d %d %d\n' 0x1F 017 \"'A\" -5" "g printf '%d %d %d %d\n' 0x1F 017 \"'A\" -5"
same "printf %b" "$SB/printf '%b|%s\n' 'a\\tb\\0101' 'a\\tb'" "g printf '%b|%s\n' 'a\\tb\\0101' 'a\\tb'"
same "printf %c %%" "$SB/printf '%c%c %% \\101\\x42\\u4e2d\n' hello w" "g printf '%c%c %% \\101\\x42\\u4e2d\n' hello w"
same "printf 无效数字" "$SB/printf '%d\n' abc 12x" "g printf '%d\n' abc 12x"
same "printf 无符号负数" "$SB/printf '%u %x\n' -1 -1" "g printf '%u %x\n' -1 -1"
same "printf \\c 停止" "$SB/printf 'ab\\ccd'" "g printf 'ab\\ccd'"
same "printf 缺参数" "$SB/printf '%s-%d-%s\n' x" "g printf '%s-%d-%s\n' x"

# ---------------------------------------------------------------------------
section "文件操作"
same "mkdir -p" "$SB/mkdir -p m1/a/b/c && find m1 | sort" "g mkdir -p m2/a/b/c && find m2 | sed s/m2/m1/ | sort"
same "mkdir 已存在" "$SB/mkdir m1" "g mkdir m1"
same "mkdir -m" "$SB/mkdir -m 700 mm1 && stat -c %a mm1" "g mkdir -m 700 mm2 && stat -c %a mm2"
same "mkdir -m 符号" "$SB/mkdir -m u=rwx,g=rx,o= mm3 && stat -c %a mm3" "g mkdir -m u=rwx,g=rx,o= mm4 && stat -c %a mm4"
same "rmdir 非空" "$SB/rmdir m1" "g rmdir m1"
same "rmdir -p" "$SB/rmdir -p m1/a/b/c; ls -d m1 2>&1 | head -1" "g mkdir -p m3/a/b/c; g rmdir -p m3/a/b/c; ls -d m1 2>&1 | head -1"
same "cp 文件" "$SB/cp lines.txt c1.txt && cat c1.txt" "g cp lines.txt c2.txt && cat c2.txt"
same "cp 到目录" "mkdir -p cd1 && $SB/cp lines.txt dup.txt cd1 && ls cd1" "mkdir -p cd2 && g cp lines.txt dup.txt cd2 && ls cd2"
same "cp 目录不加 -r" "$SB/cp d cx" "g cp d cx"
same "cp -r 结构" "$SB/cp -r d cr1 && (cd cr1 && find . | sort)" "g cp -r d cr2 && (cd cr2 && find . | sort)"
same "cp -r 内容和链接" "(cd cr1 && cat sub/one sub/deep/two && readlink link broken)" "(cd cr2 && cat sub/one sub/deep/two && readlink link broken)"
same "cp -r 复制到已存在目录" "$SB/cp -r d cr1 && ls cr1/d | head -3" "g cp -r d cr2 && ls cr2/d | head -3"
same "cp -a 保留时间权限" "$SB/cp -a d ca1 && stat -c '%n %a %Y %F' ca1/beta ca1/alpha ca1/fifo | sed s/ca1/X/" \
                        "g cp -a d ca2 && stat -c '%n %a %Y %F' ca2/beta ca2/alpha ca2/fifo | sed s/ca2/X/"
check "cp -r 不能复制到自己里面" "! $SB/cp -r d d/sub/inside 2>/dev/null && [ ! -e d/sub/inside ]"
check "cp 同一个文件" "! $SB/cp lines.txt lines.txt 2>/dev/null"
same "cp -n 不覆盖" "echo old > n1; $SB/cp -n lines.txt n1; cat n1" "echo old > n2; g cp -n lines.txt n2 2>/dev/null; cat n2"
same "mv 重命名" "cp lines.txt mv1 && $SB/mv mv1 mv1b; [ -e mv1 ]; echo \$?; cat mv1b" "cp lines.txt mv2 && g mv mv2 mv2b; [ -e mv2 ]; echo \$?; cat mv2b"
same "mv 到目录" "mkdir -p md1 && cp lines.txt mvx1 && $SB/mv mvx1 md1 && ls md1" "mkdir -p md2 && cp lines.txt mvx1b && g mv mvx1b md2 && ls md2 | sed s/mvx1b/mvx1/"
same "mv 目录" "cp -r d mvd1 && $SB/mv mvd1 mvd1b && ls mvd1b | wc -l" "cp -r d mvd2 && g mv mvd2 mvd2b && ls mvd2b | wc -l"
check "mv 跨文件系统（/dev/shm）" "[ -d /dev/shm ] || exit 0; cp -r d xfs && $SB/mv xfs /dev/shm/skbox-mv-\$\$ && [ ! -e xfs ] && [ -L /dev/shm/skbox-mv-\$\$/link ] && [ \"\$(cat /dev/shm/skbox-mv-\$\$/sub/one)\" = x ] && rm -rf /dev/shm/skbox-mv-\$\$"
same "rm 文件" "cp lines.txt r1 && $SB/rm r1; [ -e r1 ]; echo \$?" "cp lines.txt r2 && g rm r2; [ -e r2 ]; echo \$?"
same "rm 目录不加 -r" "$SB/rm cr1" "g rm cr2"
same "rm -r" "$SB/rm -r cr1 && ls cr1" "g rm -r cr2 && ls cr2"
same "rm -f 不存在" "$SB/rm -f nope1 nope2" "g rm -f nope1 nope2"
same "rm 不存在" "$SB/rm nope1" "g rm nope1"
check "rm -r 拒绝 /" "! $SB/rm -rf / 2>/dev/null"
check "rm 拒绝 .." "! $SB/rm -rf .. 2>/dev/null && [ -d ../w ]"
check "rm -r 只读目录树" "cp -r d ro && chmod -R a-w ro/sub && $SB/rm -rf ro && [ ! -e ro ]"
same "touch 新建" "$SB/touch t1 && stat -c %s t1" "g touch t2 && stat -c %s t2"
same "touch -d" "$SB/touch -d '2022-03-04 05:06:07' t1 && stat -c %Y t1" "g touch -d '2022-03-04 05:06:07' t2 && stat -c %Y t2"
same "touch -t" "$SB/touch -t 202301020304.05 t1 && stat -c %Y t1" "g touch -t 202301020304.05 t2 && stat -c %Y t2"
same "touch -r" "$SB/touch -r d/beta t1 && stat -c %Y t1" "g touch -r d/beta t2 && stat -c %Y t2"
same "touch -a 只改访问时间" "$SB/touch -a -d @1000000000 t1 && stat -c '%X %Y' t1" "g touch -a -d @1000000000 t2 && stat -c '%X %Y' t2"
same "touch -c 不创建" "$SB/touch -c tnope; ls tnope" "g touch -c tnope; ls tnope"
same "ln -s" "$SB/ln -s lines.txt l1 && readlink l1" "g ln -s lines.txt l2 && readlink l2"
same "ln 硬链接" "cp lines.txt hs1 && $SB/ln hs1 h1 && stat -c %h h1" "cp lines.txt hs2 && g ln hs2 h2 && stat -c %h h2"
same "ln 已存在" "$SB/ln -s x l1" "g ln -s x l2"
same "ln -sf" "$SB/ln -sf dup.txt l1 && readlink l1" "g ln -sf dup.txt l2 && readlink l2"
same "ln -s 到目录" "mkdir -p ld1 && $SB/ln -s ../lines.txt ld1 && readlink ld1/lines.txt" "mkdir -p ld2 && g ln -s ../lines.txt ld2 && readlink ld2/lines.txt"
same "ln -sfn 替换目录链接" "ln -sfn d dl1 && $SB/ln -sfn e dl1 && readlink dl1" "ln -sfn d dl2 && g ln -sfn e dl2 && readlink dl2"
same "ln 目录硬链接" "$SB/ln d dh1" "g ln d dh1"
same "pwd" "cd d/sub && $SB/pwd" "cd d/sub && g pwd"
same "pwd -L/-P" "cd dl && $SB/pwd -L && $SB/pwd -P" "cd dl && g pwd -L && g pwd -P"
same "readlink" "$SB/readlink d/link d/broken d/alpha" "g readlink d/link d/broken d/alpha"
same "readlink -f" "$SB/readlink -f dl/link dl/sub/../alpha d/broken d/nope/x ." "g readlink -f dl/link dl/sub/../alpha d/broken d/nope/x ."
same "readlink -e" "$SB/readlink -e dl/link d/broken" "g readlink -e dl/link d/broken"
same "readlink -m" "$SB/readlink -m d/nope/x/../y /a/b/../c" "g readlink -m d/nope/x/../y /a/b/../c"
same "readlink 链接环" "ln -sf loop2 loop1; ln -sf loop1 loop2; $SB/readlink -f loop1" "g readlink -f loop1"
same "basename" "$SB/basename /usr/bin/sksh; $SB/basename note.txt .txt; $SB/basename /; $SB/basename a//; $SB/basename -a x/y z/w; $SB/basename -s .c a.c b.c" \
                 "g basename /usr/bin/sksh; g basename note.txt .txt; g basename /; g basename a//; g basename -a x/y z/w; g basename -s .c a.c b.c"
same "dirname" "$SB/dirname /usr/bin/sksh a/b/ a / //x x ./y" "g dirname /usr/bin/sksh a/b/ a / //x x ./y"

section "chmod"
for m in 755 644 4755 1777 0 u+x go-w a=r u=rwx,g=rx,o= +x -x o+t u+s g=u a+X ug+w,o-rwx =r; do
    same "chmod $m" "cp lines.txt cm1 && chmod 640 cm1 && $SB/chmod $m cm1 && stat -c %a cm1" \
                    "cp lines.txt cm2 && chmod 640 cm2 && g chmod $m cm2 && stat -c %a cm2"
done
same "chmod a+X 目录" "mkdir -p cmd1 && chmod 600 cmd1 && $SB/chmod a+X cmd1 && stat -c %a cmd1" "mkdir -p cmd2 && chmod 600 cmd2 && g chmod a+X cmd2 && stat -c %a cmd2"
same "chmod -R" "cp -r d cmr1 && $SB/chmod -R go-rwx cmr1 && find cmr1 -not -type l -exec stat -c '%a' {} + | sort | uniq -c" \
                "cp -r d cmr2 && g chmod -R go-rwx cmr2 && find cmr2 -not -type l -exec stat -c '%a' {} + | sort | uniq -c"
same "chmod 无效模式" "$SB/chmod 9z9 lines.txt" "g chmod 9z9 lines.txt"
same "chmod 不存在" "$SB/chmod 644 nope" "g chmod 644 nope"

section "stat"
same "stat -c 文件" "$SB/stat -c '%n|%s|%b|%B|%a|%A|%u|%U|%g|%G|%h|%i|%d|%D|%f|%o|%X|%Y|%Z' lines.txt d d/link" \
                   "g stat -c '%n|%s|%b|%B|%a|%A|%u|%U|%g|%G|%h|%i|%d|%D|%f|%o|%X|%Y|%Z' lines.txt d d/link"
same "stat -c %y %N" "$SB/stat -c '%y|%N' d/beta d/link" "g stat -c '%y|%N' d/beta d/link"
same "stat -L" "$SB/stat -L -c '%n %s %F' d/link | sed 's/普通文件/regular file/'" "g stat -L -c '%n %s %F' d/link"
same "stat -c 宽度" "$SB/stat -c '[%10s][%-8n]' lines.txt" "g stat -c '[%10s][%-8n]' lines.txt"
same "stat 设备号" "$SB/stat -c '%t %T' /dev/null /dev/tty" "g stat -c '%t %T' /dev/null /dev/tty"
same "stat --printf" "$SB/stat --printf '%s\\t%n\\n' lines.txt" "g stat --printf '%s\\t%n\\n' lines.txt"
same "stat -f -c" "$SB/stat -f -c '%b %s %c %l' /" "g stat -f -c '%b %s %c %l' /"
check "stat 默认格式" "$SB/stat lines.txt | grep -q '最近修改' && $SB/stat lines.txt | grep -q '(0644/-rw-r--r--)'"
same "stat 不存在" "$SB/stat nope >/dev/null" "g stat nope >/dev/null"

# ---------------------------------------------------------------------------
section "wc head tail"
same "wc" "$SB/wc lines.txt" "g wc lines.txt"
same "wc 多文件" "$SB/wc lines.txt cjk.txt nonl.txt | sed 's/总计/total/'" "g wc lines.txt cjk.txt nonl.txt"
same "wc -l" "$SB/wc -l lines.txt" "g wc -l lines.txt"
same "wc -l 管道" "cat lines.txt | $SB/wc -l" "cat lines.txt | g wc -l"
same "wc 管道" "cat lines.txt | $SB/wc" "cat lines.txt | g wc"
same "wc -w -c" "$SB/wc -w -c cjk.txt nums.txt | sed 's/总计/total/'" "g wc -w -c cjk.txt nums.txt"
same "wc -m" "$SB/wc -m cjk.txt" "g wc -m cjk.txt"
same "wc -lmc" "$SB/wc -lmc cjk.txt" "g wc -lmc cjk.txt"
same "wc -L" "$SB/wc -L lines.txt cjk.txt | sed 's/总计/total/'" "g wc -L lines.txt cjk.txt"
same "wc 空文件" "$SB/wc empty.txt" "g wc empty.txt"
same "wc 不存在" "$SB/wc nope lines.txt > wco.txt; r=\$?; sed 's/总计/total/' wco.txt; (exit \$r)" "g wc nope lines.txt"
same "head" "$SB/head nums.txt" "g head nums.txt"
same "head -n 3" "$SB/head -n 3 lines.txt" "g head -n 3 lines.txt"
same "head -3" "$SB/head -3 lines.txt" "g head -3 lines.txt"
same "head -n -3" "$SB/head -n -3 lines.txt" "g head -n -3 lines.txt"
same "head -n -3 无结尾换行" "$SB/head -n -1 nonl.txt; $SB/head -n -0 nonl.txt" "g head -n -1 nonl.txt; g head -n -0 nonl.txt"
same "head -c" "$SB/head -c 10 lines.txt" "g head -c 10 lines.txt"
same "head -c -5" "$SB/head -c -5 lines.txt" "g head -c -5 lines.txt"
same "head -c 1K" "$SB/head -c 1K nums.txt" "g head -c 1K nums.txt"
same "head 多文件" "$SB/head -n 2 lines.txt dup.txt" "g head -n 2 lines.txt dup.txt"
same "head -q" "$SB/head -q -n 1 lines.txt dup.txt" "g head -q -n 1 lines.txt dup.txt"
same "head 管道" "seq 100 | $SB/head -n 5" "seq 100 | g head -n 5"
check "head 提前退出（yes | head）" "timeout 5 bash -c '$SB/yes | $SB/head -n 3' | grep -c y | grep -q 3"
same "tail" "$SB/tail nums.txt" "g tail nums.txt"
same "tail -n 3" "$SB/tail -n 3 lines.txt" "g tail -n 3 lines.txt"
same "tail -3" "$SB/tail -3 lines.txt" "g tail -3 lines.txt"
same "tail -n +5" "$SB/tail -n +5 lines.txt" "g tail -n +5 lines.txt"
same "tail +5（老写法）" "$SB/tail -n +1995 nums.txt" "g tail -n +1995 nums.txt"
same "tail 无结尾换行" "$SB/tail -n 1 nonl.txt; $SB/tail -n 2 lines.txt" "g tail -n 1 nonl.txt; g tail -n 2 lines.txt"
same "tail -c" "$SB/tail -c 12 lines.txt" "g tail -c 12 lines.txt"
same "tail -c +5" "$SB/tail -c +5 lines.txt" "g tail -c +5 lines.txt"
same "tail 管道" "cat nums.txt | $SB/tail -n 4" "cat nums.txt | g tail -n 4"
same "tail 管道 +N" "cat nums.txt | $SB/tail -n +1998" "cat nums.txt | g tail -n +1998"
same "tail 多文件" "$SB/tail -n 2 lines.txt dup.txt" "g tail -n 2 lines.txt dup.txt"
same "tail 大文件" "seq 1 200000 > bignums.txt; $SB/tail -n 3 bignums.txt" "g tail -n 3 bignums.txt"
check "tail -f 跟踪新增内容" "echo first > tf.txt; ($SB/tail -f -s 0.1 tf.txt > tf.out &  p=\$!; sleep 0.5; echo second >> tf.txt; sleep 0.8; kill \$p); grep -q second tf.out && grep -q first tf.out"

# ---------------------------------------------------------------------------
section "grep"
same "grep 基本" "$SB/grep apple lines.txt" "g grep apple lines.txt"
same "grep -i" "$SB/grep -i banana lines.txt" "g grep -i banana lines.txt"
same "grep -n" "$SB/grep -n red lines.txt" "g grep -n red lines.txt"
same "grep -v" "$SB/grep -v e lines.txt" "g grep -v e lines.txt"
same "grep -c" "$SB/grep -c apple lines.txt dup.txt" "g grep -c apple lines.txt dup.txt"
same "grep -l" "$SB/grep -l alpha lines.txt dup.txt d/alpha" "g grep -l alpha lines.txt dup.txt d/alpha"
same "grep -L" "$SB/grep -L alpha lines.txt dup.txt" "g grep -L alpha lines.txt dup.txt"
same "grep BRE" "$SB/grep '^[a-c].*\\(e\\)\\1\\{0,1\\} [0-9]\\{2\\}' lines.txt" "g grep '^[a-c].*\\(e\\)\\1\\{0,1\\} [0-9]\\{2\\}' lines.txt"
same "grep -E" "$SB/grep -E '^(apple|fig) [0-9]+' lines.txt" "g grep -E '^(apple|fig) [0-9]+' lines.txt"
same "grep -E ?" "$SB/grep -E 'gr?ape' lines.txt" "g grep -E 'gr?ape' lines.txt"
same "grep -F" "$SB/grep -F '[x' lines.txt; $SB/grep -F -e 10 -e 25 lines.txt" "g grep -F '[x' lines.txt; g grep -F -e 10 -e 25 lines.txt"
same "grep -w" "$SB/grep -w 10 lines.txt; $SB/grep -w app lines.txt" "g grep -w 10 lines.txt; g grep -w app lines.txt"
same "grep -x" "$SB/grep -x 'apple 10 red' lines.txt" "g grep -x 'apple 10 red' lines.txt"
same "grep -o" "$SB/grep -o '[0-9]\\+' lines.txt" "g grep -o '[0-9]\\+' lines.txt"
same "grep -on" "$SB/grep -on 'p[a-z]' lines.txt" "g grep -on 'p[a-z]' lines.txt"
same "grep -m" "$SB/grep -m 2 a lines.txt" "g grep -m 2 a lines.txt"
same "grep -e 多个" "$SB/grep -e fig -e date lines.txt" "g grep -e fig -e date lines.txt"
same "grep 多文件" "$SB/grep a lines.txt dup.txt" "g grep a lines.txt dup.txt"
same "grep -h -H" "$SB/grep -h fig lines.txt dup.txt; $SB/grep -H fig lines.txt" "g grep -h fig lines.txt dup.txt; g grep -H fig lines.txt"
same "grep -r" "$SB/grep -r yy d" "g grep -r yy d"
same "grep -r 无目录参数" "cd d && $SB/grep -r x" "cd d && g grep -r x"
same "grep -rl" "$SB/grep -rl a d | sort" "g grep -rl a d | sort"
same "grep -q" "$SB/grep -q apple lines.txt; echo \$?; $SB/grep -q zzz lines.txt" "g grep -q apple lines.txt; echo \$?; g grep -q zzz lines.txt"
same "grep 没找到" "$SB/grep zzz lines.txt" "g grep zzz lines.txt"
same "grep 不存在" "$SB/grep a nope" "g grep a nope"
same "grep -s" "$SB/grep -s a nope lines.txt" "g grep -s a nope lines.txt"
same "grep 中文" "$SB/grep 汉字 cjk.txt; $SB/grep -o '字.' cjk.txt" "g grep 汉字 cjk.txt; g grep -o '字.' cjk.txt"
same "grep 空模式" "$SB/grep '' dup.txt | wc -l" "g grep '' dup.txt | wc -l"
same "grep 管道" "cat lines.txt | $SB/grep -c red" "cat lines.txt | g grep -c red"
same "grep 无结尾换行" "$SB/grep end nonl.txt" "g grep end nonl.txt"
same "grep -f" "printf 'fig\ndate\n' > pats; $SB/grep -f pats lines.txt" "g grep -f pats lines.txt"
expect "grep 二进制文件" "$SB/grep match binary.dat" "匹配到二进制文件 binary.dat"
same "grep -a 二进制" "$SB/grep -a -c match binary.dat" "g grep -a -c match binary.dat"
same "grep 无效正则" "$SB/grep 'a\\{' lines.txt >/dev/null" "g grep 'a\\{' lines.txt >/dev/null"
check "grep --color 高亮" "$SB/grep --color=always fig lines.txt | grep -q \$'\\033\\[01;31mfig'"

# ---------------------------------------------------------------------------
section "find"
same "find 全部" "$SB/find d | sort" "g find d | sort"
same "find -name" "$SB/find d -name '*a' | sort" "g find d -name '*a' | sort"
same "find -iname" "$SB/find . -iname 'ALPHA' | sort" "g find . -iname 'ALPHA' | sort"
same "find -type f" "$SB/find d -type f | sort" "g find d -type f | sort"
same "find -type d" "$SB/find d -type d | sort" "g find d -type d | sort"
same "find -type l" "$SB/find d -type l | sort" "g find d -type l | sort"
same "find -type p" "$SB/find d -type p" "g find d -type p"
same "find -maxdepth" "$SB/find d -maxdepth 1 -type d | sort" "g find d -maxdepth 1 -type d | sort"
same "find -maxdepth 0" "$SB/find d e -maxdepth 0" "g find d e -maxdepth 0"
same "find -mindepth" "$SB/find d -mindepth 2 | sort" "g find d -mindepth 2 | sort"
same "find ! / -not" "$SB/find d ! -name '*a*' -type f | sort; $SB/find d -not -type f | sort" "g find d ! -name '*a*' -type f | sort; g find d -not -type f | sort"
same "find -o" "$SB/find d -name alpha -o -name beta | sort" "g find d -name alpha -o -name beta | sort"
same "find 括号" "$SB/find d \\( -name 'a*' -o -name 'b*' \\) -type f | sort" "g find d \\( -name 'a*' -o -name 'b*' \\) -type f | sort"
same "find -path" "$SB/find d -path '*sub*' | sort" "g find d -path '*sub*' | sort"
same "find -size" "$SB/find d -size +100k; $SB/find d -type f -size -2 | sort" "g find d -size +100k; g find d -type f -size -2 | sort"
same "find -size c" "$SB/find d -type f -size 6c | sort" "g find d -type f -size 6c | sort"
same "find -empty" "mkdir -p d2/emptydir; : > d2/emptyfile; echo x > d2/f; $SB/find d2 -empty | sort" "g find d2 -empty | sort"
same "find -newer" "$SB/find d -newer d/gamma -name '*a' | sort" "g find d -newer d/gamma -name '*a' | sort"
same "find -mtime" "$SB/find d -mtime +1000 | sort" "g find d -mtime +1000 | sort"
same "find -exec ;" "$SB/find d/sub -type f -exec cat {} \\; | sort" "g find d/sub -type f -exec cat {} \\; | sort"
same "find -exec +" "$SB/find d/sub -type f -exec echo X {} + | tr ' ' '\\n' | sort" "g find d/sub -type f -exec echo X {} + | tr ' ' '\\n' | sort"
same "find -print0" "$SB/find d/sub -print0 | tr '\\0' '\\n' | sort" "g find d/sub -print0 | tr '\\0' '\\n' | sort"
same "find -prune" "$SB/find d -name sub -prune -o -type f -print | sort" "g find d -name sub -prune -o -type f -print | sort"
same "find -delete" "cp -r d fd1 && $SB/find fd1 -name '*a' -delete && ls fd1" "cp -r d fd2 && g find fd2 -name '*a' -delete && ls fd2"
same "find 默认当前目录" "cd d/sub && $SB/find | sort" "cd d/sub && g find | sort"
same "find -perm" "$SB/find d -perm 755 | sort" "g find d -perm 755 | sort"
same "find 不存在" "$SB/find nope" "g find nope"

# ---------------------------------------------------------------------------
section "sort uniq cut tr"
same "sort" "$SB/sort lines.txt" "g sort lines.txt"
same "sort -r" "$SB/sort -r lines.txt" "g sort -r lines.txt"
same "sort -n" "$SB/sort -n -k2 lines.txt" "g sort -n -k2 lines.txt"
same "sort -k2n" "$SB/sort -k2n lines.txt" "g sort -k2n lines.txt"
same "sort -k2,2n -k1,1r" "$SB/sort -k2,2n -k1,1r lines.txt" "g sort -k2,2n -k1,1r lines.txt"
same "sort -k3 -k1" "$SB/sort -k3,3 -k1,1 lines.txt" "g sort -k3,3 -k1,1 lines.txt"
same "sort -u" "$SB/sort -u lines.txt" "g sort -u lines.txt"
same "sort -u -k3,3" "$SB/sort -u -k3,3 lines.txt" "g sort -u -k3,3 lines.txt"
same "sort -f" "$SB/sort -f lines.txt" "g sort -f lines.txt"
same "sort -t: -k2" "$SB/sort -t: -k2 colon.txt" "g sort -t: -k2 colon.txt"
same "sort -t: -k3,3r" "$SB/sort -t: -k3,3r colon.txt" "g sort -t: -k3,3r colon.txt"
same "sort -n 数字文件" "g shuf -n 300 nums.txt --random-source=nums.txt | $SB/sort -n | md5sum" "g shuf -n 300 nums.txt --random-source=nums.txt | g sort -n | md5sum"
same "sort -n 负数小数" "printf '%s\n' 3 -1 2.5 -2.25 0 -0 10 .5 abc '' 1e3 007 | $SB/sort -n" "printf '%s\n' 3 -1 2.5 -2.25 0 -0 10 .5 abc '' 1e3 007 | g sort -n"
same "sort -rn" "printf '%s\n' 3 1 2 10 | $SB/sort -rn" "printf '%s\n' 3 1 2 10 | g sort -rn"
same "sort -k1.2,1.3" "$SB/sort -k1.2,1.3 lines.txt" "g sort -k1.2,1.3 lines.txt"
same "sort -b -k2" "printf ' b  2\na   10\n  c 1\n' | $SB/sort -b -k2n" "printf ' b  2\na   10\n  c 1\n' | g sort -b -k2n"
same "sort -s" "$SB/sort -s -k3,3 lines.txt" "g sort -s -k3,3 lines.txt"
same "sort 多文件" "$SB/sort dup.txt cjk.txt" "g sort dup.txt cjk.txt"
same "sort -c" "$SB/sort -c lines.txt >/dev/null 2>&1; echo \$?; g sort lines.txt | $SB/sort -c; echo \$?" "g sort -c lines.txt >/dev/null 2>&1; echo \$?; g sort lines.txt | g sort -c; echo \$?"
same "sort -o 同一文件" "cp lines.txt so1 && $SB/sort -o so1 so1 && cat so1" "cp lines.txt so2 && g sort -o so2 so2 && cat so2"
same "uniq" "$SB/uniq dup.txt" "g uniq dup.txt"
same "uniq -c" "$SB/uniq -c dup.txt" "g uniq -c dup.txt"
same "uniq -d" "$SB/uniq -d dup.txt" "g uniq -d dup.txt"
same "uniq -u" "$SB/uniq -u dup.txt" "g uniq -u dup.txt"
same "uniq -i" "$SB/uniq -i -c dup.txt" "g uniq -i -c dup.txt"
same "uniq -f -s" "printf 'x a\ny a\nz b\n' | $SB/uniq -f 1; printf 'xa\nya\nzb\n' | $SB/uniq -s 1" "printf 'x a\ny a\nz b\n' | g uniq -f 1; printf 'xa\nya\nzb\n' | g uniq -s 1"
same "sort | uniq -c 词频" "tr ' ' '\\n' < lines.txt | $SB/sort | $SB/uniq -c | $SB/sort -rn | head -5" "tr ' ' '\\n' < lines.txt | g sort | g uniq -c | g sort -rn | head -5"
same "cut -d -f" "$SB/cut -d: -f2 colon.txt" "g cut -d: -f2 colon.txt"
same "cut -f 列表" "$SB/cut -d: -f1,3- colon.txt" "g cut -d: -f1,3- colon.txt"
same "cut -f -2" "$SB/cut -d: -f-2 colon.txt" "g cut -d: -f-2 colon.txt"
same "cut -s" "$SB/cut -d: -s -f2 colon.txt" "g cut -d: -s -f2 colon.txt"
same "cut -c" "$SB/cut -c2-4 lines.txt" "g cut -c2-4 lines.txt"
same "cut -b" "$SB/cut -b1,3,5- lines.txt" "g cut -b1,3,5- lines.txt"
same "cut 制表符" "printf 'a\tb\tc\n' | $SB/cut -f2" "printf 'a\tb\tc\n' | g cut -f2"
same "cut --output-delimiter" "$SB/cut -d: -f1,2 --output-delimiter=' | ' colon.txt" "g cut -d: -f1,2 --output-delimiter=' | ' colon.txt"
expect "cut -c 按 UTF-8 字符" "$SB/cut -c1-2 cjk.txt" "中文
he
汉字"
same "cut 无效列表" "$SB/cut -f0 colon.txt" "g cut -f0 colon.txt"
same "tr 替换" "echo 'hello world' | $SB/tr a-z A-Z" "echo 'hello world' | g tr a-z A-Z"
same "tr 类" "echo 'Hello World 123' | $SB/tr '[:lower:]' '[:upper:]'" "echo 'Hello World 123' | g tr '[:lower:]' '[:upper:]'"
same "tr -d" "echo 'hello world' | $SB/tr -d 'lo'" "echo 'hello world' | g tr -d 'lo'"
same "tr -s" "echo 'aaa   bbb    c' | $SB/tr -s ' '" "echo 'aaa   bbb    c' | g tr -s ' '"
same "tr -cd" "echo 'a1b2-c3!' | $SB/tr -cd '[:digit:]\\n'" "echo 'a1b2-c3!' | g tr -cd '[:digit:]\\n'"
same "tr 集合2较短" "echo abcdef | $SB/tr a-f xy" "echo abcdef | g tr a-f xy"
same "tr -t" "echo abcdef | $SB/tr -t a-f xy" "echo abcdef | g tr -t a-f xy"
same "tr 转义" "printf 'a\tb\nc' | $SB/tr '\\t\\n' '_:'" "printf 'a\tb\nc' | g tr '\\t\\n' '_:'"
same "tr 八进制" "echo abc | $SB/tr 'a' '\\101'" "echo abc | g tr 'a' '\\101'"
same "tr -ds" "echo 'aabbccdd' | $SB/tr -ds a b" "echo 'aabbccdd' | g tr -ds a b"
same "tr -c 替换" "echo 'ab-cd ef' | $SB/tr -c 'a-z\\n' '_'" "echo 'ab-cd ef' | g tr -c 'a-z\\n' '_'"
same "tr [c*]" "echo abcdef | $SB/tr a-f '[x*]'; echo abcdef | $SB/tr a-f 'A[y*2]B'" "echo abcdef | g tr a-f '[x*]'; echo abcdef | g tr a-f 'A[y*2]B'"
same "tr rot13" "echo 'Hello' | $SB/tr 'A-Za-z' 'N-ZA-Mn-za-m'" "echo 'Hello' | g tr 'A-Za-z' 'N-ZA-Mn-za-m'"

# ---------------------------------------------------------------------------
section "seq test 杂项"
same "seq 5" "$SB/seq 5" "g seq 5"
same "seq 2 6" "$SB/seq 2 6" "g seq 2 6"
same "seq 1 3 20" "$SB/seq 1 3 20" "g seq 1 3 20"
same "seq 倒数" "$SB/seq 10 -3 -5" "g seq 10 -3 -5"
same "seq 负数" "$SB/seq -3 2" "g seq -3 2"
same "seq 小数" "$SB/seq 0 0.25 1.5; $SB/seq 1 0.5 3" "g seq 0 0.25 1.5; g seq 1 0.5 3"
same "seq -w" "$SB/seq -w 8 11; $SB/seq -w -3 3; $SB/seq -w 1 0.5 10" "g seq -w 8 11; g seq -w -3 3; g seq -w 1 0.5 10"
same "seq -s" "$SB/seq -s , 5; $SB/seq -s ' + ' 3" "g seq -s , 5; g seq -s ' + ' 3"
same "seq -f" "$SB/seq -f '%.2f' 1 3; $SB/seq -f 'n=%g' 2" "g seq -f '%.2f' 1 3; g seq -f 'n=%g' 2"
same "seq 空" "$SB/seq 5 1" "g seq 5 1"
same "seq 零增量" "$SB/seq 1 0 5" "g seq 1 0 5"
same "seq 无效" "$SB/seq abc" "g seq abc"
for e in "-e lines.txt" "-f lines.txt" "-f d" "-d d" "-d lines.txt" "-L d/link" "-h d/alpha" "-s lines.txt" "-s empty.txt" \
         "-x d/alpha" "-r lines.txt" "-w lines.txt" "-p d/fifo" "-c /dev/null" "-b /dev/null" "-e nope" \
         "-z ''" "-n ''" "-z x" "abc" "''" "a = a" "a = b" "a != b" "a == a" "1 -eq 1" "2 -gt 10" "-5 -lt 3" \
         "10 -ge 10" "3 -le 2" "1 -ne 2" "! -e nope" "-e lines.txt -a -d d" "-e nope -o -d d" "! a = b" \
         "( a = a )" "( a = b ) -o a = a" "lines.txt -nt d/beta" "d/beta -ot lines.txt" "hs1 -ef h1" \
         "-t 0" "x -a ''" "'' -o ''" "abc '<' abd" "abc '>' abd"; do
    same "test $e" "$SB/test $e; echo \$?" "g test $e; echo \$?"
done
same "test 错误" "$SB/test 1 -eq x; echo \$?; $SB/test a -zz b; echo \$?" "g test 1 -eq x; echo \$?; g test a -zz b; echo \$?"
same "[ ]" "$SB/[ 1 -eq 1 ]; echo \$?; $SB/[ 1 -eq 1; echo \$?" "g [ 1 -eq 1 ]; echo \$?; g [ 1 -eq 1; echo \$?"
same "true false" "$SB/true; echo \$?; $SB/false; echo \$?" "g true; echo \$?; g false; echo \$?"
same "yes" "$SB/yes | head -3; $SB/yes 你好 世界 | head -2" "g yes | head -3; g yes 你好 世界 | head -2"
same "tee" "echo hi | $SB/tee te1 te2; cat te1 te2" "echo hi | g tee te3 te4; cat te3 te4"
same "tee -a" "echo more | $SB/tee -a te1 >/dev/null; cat te1" "echo more | g tee -a te3 >/dev/null; cat te3"
same "env 运行命令" "$SB/env -i A=1 B=2 $SB/env" "\$GENV -i A=1 B=2 \$GENV"
same "env -u" "A=1 B=2 $SB/env -u A $SB/env | grep '^[AB]='" "A=1 B=2 \$GENV -u A \$GENV | grep '^[AB]='"
same "env 找不到命令" "$SB/env nosuchcmd" "g env nosuchcmd"
same "which" "PATH=$SB:/usr/bin $SB/which ls sh nosuch; echo \$?" "PATH=$SB:/usr/bin which ls sh nosuch; echo \$?"
same "which -a" "PATH=$SB:/usr/bin:/bin $SB/which -a ls" "PATH=$SB:/usr/bin:/bin which -a ls"
same "whoami" "$SB/whoami" "g whoami"
same "id -u -g -G -un" "$SB/id -u; $SB/id -g; $SB/id -G; $SB/id -un; $SB/id -gn" "g id -u; g id -g; g id -G; g id -un; g id -gn"
same "id 用户" "$SB/id -u root; $SB/id -G root" "g id -u root; g id -G root"
check "id 默认格式" "$SB/id | grep -q '^用户id=0(root) 组id=0(root) 组=0(root)'"
same "uname" "$SB/uname; $SB/uname -s -n -r -v -m" "g uname; g uname -s -n -r -v -m"
check "uname -a" "$SB/uname -a | grep -q \"^\$(uname -s) \$(uname -n) \$(uname -r) .* \$(uname -m) SketchOS\$\""
same "hostname" "$SB/hostname" "hostname"
same "sleep 小数" "s=\$(date +%s%N); $SB/sleep 0.3; e=\$(date +%s%N); [ \$(( (e - s) / 100000000 )) -ge 3 ] && [ \$(( (e - s) / 100000000 )) -lt 8 ] && echo ok" "echo ok"
same "sleep 后缀和相加" "s=\$(date +%s%N); $SB/sleep 0.1s 0.002m; e=\$(date +%s%N); [ \$(( (e - s) / 100000000 )) -ge 2 ] && echo ok" "echo ok"
same "sleep 无效" "$SB/sleep abc" "g sleep abc"
same "sync" "$SB/sync; echo \$?" "g sync; echo \$?"
expect "clear" "$SB/clear | od -An -c | tr -s ' '" " 033 [ H 033 [ 2 J 033 [ 3 J"

# ---------------------------------------------------------------------------
section "date cal"
same "date -d @ +格式" "$SB/date -u -d @1700000000 '+%Y-%m-%d %H:%M:%S %j %u %w %s %e %C %y %I %M %Z %z %%'" \
                    "g date -u -d @1700000000 '+%Y-%m-%d %H:%M:%S %j %u %w %s %e %C %y %I %M %Z %z %%'"
same "date 修饰符" "$SB/date -u -d @1700000000 '+%-d|%_m|%-H|%3N|%N|%:z|%k|%l|%10Y'" "g date -u -d @1700000000 '+%-d|%_m|%-H|%3N|%N|%:z|%k|%l|%10Y'"
same "date 时区" "TZ=CST-8 $SB/date -d @1700000000 '+%F %T %Z %z'" "TZ=CST-8 g date -d @1700000000 '+%F %T %Z %z'"
same "date -d 日期" "$SB/date -d '2026-10-23 13:55:00' +%s; $SB/date -d '2026-10-23' +%s" "g date -d '2026-10-23 13:55:00' +%s; g date -d '2026-10-23' +%s"
same "date 小数时间戳" "$SB/date -u -d @1.5 +%s.%N" "g date -u -d @1.5 +%s.%N"
expect "date 中文名称" "$SB/date -u -d @1700000000 '+%a %A %b %B %p'" "二 星期二 11月 十一月 下午"
expect "date 默认格式" "TZ=CST-8 $SB/date -d @1700000000" "2023年11月15日 星期三 06:13:20 CST"
check "date 当前时间" "[ \$(( \$($SB/date +%s) - \$(date +%s) )) -le 1 ]"
same "date -r 文件" "$SB/date -r d/beta +%s" "g date -r d/beta +%s"
same "date 无效日期" "$SB/date -d 'xyz'" "g date -d 'xyz' 2>/dev/null"
expect "cal 指定月份" "$SB/cal 2 2024" "      2024年2月
日 一 二 三 四 五 六
             1  2  3
 4  5  6  7  8  9 10
11 12 13 14 15 16 17
18 19 20 21 22 23 24
25 26 27 28 29"
check "cal 和 python 日历一致（2000~2030 年每个月）" "for y in 2000 2013 2024 2025 2026 2030; do for m in 1 2 3 4 5 6 7 8 9 10 11 12; do \
  a=\$($SB/cal \$m \$y | tail -n +3 | tr -s ' \n' ' ' | sed 's/^ //;s/ \$//'); \
  b=\$(python3 -c \"import calendar; calendar.setfirstweekday(6); print(' '.join(str(d) for w in calendar.monthcalendar(\$y,\$m) for d in w if d))\"); \
  [ \"\$a\" = \"\$b\" ] || { echo \$y-\$m; exit 1; }; done; done"
check "cal 整年" "$SB/cal 2026 | grep -q '十二月' && [ \$($SB/cal 2026 | wc -l) -ge 30 ]"
check "cal 无效" "! $SB/cal 13 2026 2>/dev/null"

# ---------------------------------------------------------------------------
section "hexdump xxd"
printf 'hello world\n\x00\x01\x02\xff%.0s' > hx.bin
head -c 100 /dev/zero >> hx.bin
printf 'tail' >> hx.bin
same "hexdump -C" "$SB/hexdump -C hx.bin" "hexdump -C hx.bin"
same "hexdump" "$SB/hexdump hx.bin" "hexdump hx.bin"
same "hexdump -C 奇数长度" "printf 'abc' | $SB/hexdump -C; printf 'abcdefghi' | $SB/hexdump -C" "printf 'abc' | hexdump -C; printf 'abcdefghi' | hexdump -C"
same "hexdump 奇数长度" "printf 'abc' | $SB/hexdump" "printf 'abc' | hexdump"
same "hexdump -v" "$SB/hexdump -C -v hx.bin" "hexdump -C -v hx.bin"
same "hexdump -n -s" "$SB/hexdump -C -n 20 -s 3 hx.bin" "hexdump -C -n 20 -s 3 hx.bin"
same "hexdump 空" "$SB/hexdump -C empty.txt" "hexdump -C empty.txt"
same "xxd" "$SB/xxd hx.bin" "xxd hx.bin"
same "xxd 短" "printf 'abc' | $SB/xxd" "printf 'abc' | xxd"
same "xxd -p" "$SB/xxd -p hx.bin" "xxd -p hx.bin"
same "xxd -l -s" "$SB/xxd -l 10 -s 2 hx.bin" "xxd -l 10 -s 2 hx.bin"
same "xxd -c -u" "$SB/xxd -c 8 -u hx.bin" "xxd -c 8 -u hx.bin"
same "xxd 中文" "$SB/xxd cjk.txt" "xxd cjk.txt"

# ---------------------------------------------------------------------------
section "du df mount"
same "du" "$SB/du d" "g du d"
same "du -s" "$SB/du -s d e" "g du -s d e"
same "du -a" "$SB/du -a d | sort -k2" "g du -a d | sort -k2"
same "du -h" "$SB/du -sh d lines.txt" "g du -sh d lines.txt"
same "du -c" "$SB/du -sc d e | sed 's/总用量/total/'" "g du -sc d e"
same "du -d 1" "$SB/du -d 1 d | sort -k2" "g du -d 1 d | sort -k2"
same "du 硬链接只算一次" "mkdir -p hl && head -c 100000 /dev/zero > hl/a && ln hl/a hl/b && $SB/du -s hl" "g du -s hl"
same "du -b" "$SB/du -sb d" "g du -sb d"
same "du 不存在" "$SB/du nope" "g du nope"

if [ "$(id -u)" = 0 ] && mkdir -p mnt1 && "$SB/mount" -t tmpfs -o size=2m,mode=755 sktest "$T/w/mnt1" 2>/dev/null; then
    check "mount 挂载 tmpfs" "grep -q ' $T/w/mnt1 tmpfs ' /proc/mounts && findmnt -n -o OPTIONS $T/w/mnt1 | grep -q 'size=2048k'"
    same "df 已挂载的 tmpfs" "$SB/df $T/w/mnt1 | tail -1 | awk '{print \$2, \$3, \$4, \$5}'" "g df -k $T/w/mnt1 | tail -1 | awk '{print \$2, \$3, \$4, \$5}'"
    head -c 700000 /dev/urandom > mnt1/fill
    same "df 写入文件后" "$SB/df $T/w/mnt1 | tail -1 | awk '{print \$2, \$3, \$4, \$5}'" "g df -k $T/w/mnt1 | tail -1 | awk '{print \$2, \$3, \$4, \$5}'"
    same "df -h" "$SB/df -h $T/w/mnt1 | tail -1 | awk '{print \$2, \$3, \$4, \$5}'" "g df -h $T/w/mnt1 | tail -1 | awk '{print \$2, \$3, \$4, \$5}'"
    check "df -T 显示类型" "$SB/df -T $T/w/mnt1 | tail -1 | grep -q tmpfs"
    check "mount 列表包含新挂载" "$SB/mount | grep -q '$T/w/mnt1'"
    check "mount -t 过滤" "$SB/mount -t tmpfs | grep -q '$T/w/mnt1' && ! $SB/mount -t proc | grep -q '$T/w/mnt1'"
    check "mount -o remount,ro" "$SB/mount -o remount,ro $T/w/mnt1 && grep ' $T/w/mnt1 ' /proc/mounts | grep -q ' ro,' && ! touch mnt1/x 2>/dev/null"
    check "umount 忙" "cd mnt1 && ! $SB/umount $T/w/mnt1 2>/dev/null"
    check "umount" "$SB/umount $T/w/mnt1 && ! grep -q ' $T/w/mnt1 ' /proc/mounts"
    check "mount --bind" "mkdir -p bsrc bdst && echo bound > bsrc/f && $SB/mount --bind bsrc bdst && [ \"\$(cat bdst/f)\" = bound ] && $SB/umount bdst"
    check "umount 未挂载" "! $SB/umount $T/w/mnt1 2>/dev/null"
else
    echo "  (不是 root 或无法挂载 tmpfs，跳过 mount/umount/df 的挂载测试)"
fi
check "df 列表含 /" "$SB/df | awk '\$NF==\"/\"' | grep -q ."
same "df / 数值" "$SB/df / | tail -1 | awk '{print \$2}'" "g df -k / | tail -1 | awk '{print \$2}'"
check "df -a 包含 proc" "$SB/df -a | grep -q '/proc'"
check "mount 无参数表格" "$SB/mount | head -1 | grep -q '挂载点' && $SB/mount | grep -q ' /proc '"
check "mount 错误提示" "$SB/mount -t nosuchfs x /nonexist 2>&1 | grep -q '^mount: '"

# ---------------------------------------------------------------------------
section "/proc 类工具（合理性检查）"
sleep 300 & SLEEPER=$!
expect "ps 表头" "$SB/ps | head -1" "  PID TTY          TIME CMD"
check "ps 包含自己" "$SB/ps | grep -q ' ps\$'"
check "ps 包含 sleep" "$SB/ps | grep -q \"^ *\$SLEEPER .* sleep\$\""
expect "ps aux 表头" "$SB/ps aux | head -1" "USER         PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND"
check "ps aux 有 sleep 300 的完整命令行" "$SB/ps aux | grep -q \"^root *\$SLEEPER .*sleep 300\$\""
check "ps aux 和 GNU ps 的进程数一致（±3）" "a=\$($SB/ps aux | wc -l); b=\$(ps aux | wc -l); [ \$((a - b)) -le 3 ] && [ \$((b - a)) -le 3 ]"
check "ps aux RSS 与 /proc 一致" "line=\$($SB/ps aux | awk -v p=\$SLEEPER '\$2==p'); rss=\$(echo \"\$line\" | awk '{print \$6}'); r2=\$(awk '/VmRSS/{print \$2}' /proc/\$SLEEPER/status); [ \"\$rss\" = \"\$r2\" ]"
check "ps -p" "$SB/ps -p \$SLEEPER | tail -1 | grep -q sleep && [ \$($SB/ps -p \$SLEEPER | wc -l) = 2 ]"
check "ps -f" "$SB/ps -f | head -1 | grep -q 'UID  *PID  *PPID'"
check "pstree 有树形字符" "$SB/pstree -p \$\$ | grep -q '───\\|─┬─'"
check "pstree 显示 sleep" "$SB/pstree -p \$\$ | grep -q \"sleep(\$SLEEPER)\""
check "pstree -A ASCII" "$SB/pstree -A -p \$\$ | grep -q -- '---'"
check "free 表头和数值" "$SB/free | head -1 | grep -q '总计' && t=\$($SB/free | awk 'NR==2{print \$2}'); t2=\$(awk '/MemTotal/{print \$2}' /proc/meminfo); [ \"\$t\" = \"\$t2\" ]"
check "free -m" "t=\$($SB/free -m | awk 'NR==2{print \$2}'); t2=\$(awk '/MemTotal/{print int(\$2/1024)}' /proc/meminfo); [ \"\$t\" = \"\$t2\" ]"
check "free -h" "$SB/free -h | awk 'NR==2{print \$2}' | grep -q '[KMG]'"
check "uptime" "$SB/uptime | grep -q '已运行' && $SB/uptime | grep -q '负载'"
check "uptime -p -s" "$SB/uptime -p | grep -q '^已运行' && $SB/uptime -s | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9:]{8}\$'"
check "uptime -s 与 GNU 一致（±1 秒）" "a=\$(date -d \"\$($SB/uptime -s)\" +%s); b=\$(date -d \"\$(uptime -s)\" +%s); [ \$((a-b)) -le 1 ] && [ \$((b-a)) -le 1 ]"
check "kill -l" "$SB/kill -l | grep -q 'KILL' && [ \"\$($SB/kill -l 9)\" = KILL ] && [ \"\$($SB/kill -l TERM)\" = 15 ]"
check "kill -0" "$SB/kill -0 \$SLEEPER"
check "kill -STOP / -CONT" "$SB/kill -STOP \$SLEEPER && sleep 0.2 && grep -q '^State:.*T' /proc/\$SLEEPER/status && $SB/kill -s CONT \$SLEEPER && sleep 0.2 && ! grep -q '^State:.*T' /proc/\$SLEEPER/status"
check "kill 不存在的进程" "! $SB/kill 999999 2>/dev/null && $SB/kill 999999 2>&1 | grep -q '没有那个进程'"
check "kill -9" "sleep 100 & p=\$!; $SB/kill -9 \$p; wait \$p; [ \$? = 137 ]"
check "kill 默认 TERM" "sleep 100 & p=\$!; $SB/kill \$p; wait \$p; [ \$? = 143 ]"
check "killall" "(exec -a skbox_victim sleep 100) & p=\$!; sleep 0.2; $SB/killall skbox_victim 2>/dev/null || $SB/killall sleep; wait \$p; [ \$? -ge 128 ]"
check "killall 没找到" "! $SB/killall nosuchprocname 2>/dev/null"
check "lsof 显示自己的 fd" "exec 7<lines.txt; $SB/lsof -p \$\$ | grep -q 'lines.txt'; r=\$?; exec 7<&-; [ \$r = 0 ]"
check "lsof 文件过滤" "exec 7<lines.txt; $SB/lsof \$T/w/lines.txt | grep -q \"\$\$\"; r=\$?; exec 7<&-; [ \$r = 0 ]"
check "lsof 有 cwd/txt" "$SB/lsof -p \$\$ | grep -q ' cwd ' && $SB/lsof -p \$\$ | grep -q ' txt '"
check "dmesg（WSL 可能不允许读）" "$SB/dmesg >/dev/null 2>&1 || $SB/dmesg 2>&1 | grep -q 'dmesg: '"
check "lscpu" "$SB/lscpu | grep -q '架构' && $SB/lscpu | grep -q \"\$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //' | cut -c1-20)\""
check "lspci（WSL 里可能没有 PCI）" "$SB/lspci >/dev/null 2>&1; [ \$? -le 1 ]"
check "lsinput（WSL 里可能没有输入设备）" "$SB/lsinput >/dev/null 2>&1; [ \$? -le 1 ]"
check "sysinfo" "$SB/sysinfo | grep -q '内核' && $SB/sysinfo | grep -q \"\$(uname -r)\""
check "svc 没有 sinit 时" "! SKBOX_SVC_STATUS=/nonexistent $SB/svc 2>/dev/null"
mkdir -p svcdir
printf '# name\tstate\tpid\trestarts\tcommand\nserial\trunning\t45\t0\t/bin/sksh -l\ndesktop\tfailed\t-\t5\t/bin/sketchd\nsetup\tdone\t-\t0\t/bin/true\nconsole\tdisabled\t-\t0\t/bin/sksh -l\n' > svcdir/status
check "svc 列表" "SKBOX_SVC_STATUS=svcdir/status $SB/svc | grep -q 'serial' && SKBOX_SVC_STATUS=svcdir/status $SB/svc | grep -q '运行中' && SKBOX_SVC_STATUS=svcdir/status $SB/svc | grep -q '失败'"
check "svc status 单个" "SKBOX_SVC_STATUS=svcdir/status $SB/svc status desktop | grep -q '重启次数: 5'"
check "svc 未知服务" "! SKBOX_SVC_STATUS=svcdir/status $SB/svc status nosuch 2>/dev/null"
check "top -b 批处理模式" "$SB/top -b -n 2 -d 0.3 | grep -q 'PID USER'"
check "top -b 有任务和内存行" "out=\$($SB/top -b -n 1); echo \"\$out\" | grep -q '任务' && echo \"\$out\" | grep -q '内存'"
kill $SLEEPER 2>/dev/null

# ---------------------------------------------------------------------------
section "交互式工具（伪终端）"
if command -v python3 >/dev/null; then
    check "top / more 伪终端测试" "python3 '$ROOT/tests/skbox/pty_tests.py' '$SB'"
fi

echo
echo "通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -gt 0 ]; then
    printf '  失败: %s\n' "${FAILED_NAMES[@]}"
    exit 1
fi
exit 0
