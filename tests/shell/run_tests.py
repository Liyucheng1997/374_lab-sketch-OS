#!/usr/bin/env python3
"""
tests/shell/run_tests.py — sksh 的非交互测试（在 WSL 里直接运行 i386 静态程序）

每个用例是一段 shell 脚本，用 `sksh -c 脚本`（或写成脚本文件、或从标准输入喂进去）运行，
检查标准输出、退出码，必要时检查标准错误。标了 bash=True 的用例会用同样的方式交给
/bin/bash 运行，要求两者的标准输出和退出码完全一致（错误信息是中文的，不比较 stderr）。

用法：
    python3 tests/shell/run_tests.py                       # 默认测试 $WORK/agents/shell/build/bin/sksh
    python3 tests/shell/run_tests.py --sksh 路径 -k 关键字 -v
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

DEFAULT_SKSH = os.path.expanduser("~/sketchos-work/agents/shell/build/bin/sksh")


class Case:
    def __init__(self, name, script, out=None, rc=0, err=None, bash=False, stdin=None,
                 mode="c", args=(), files=None, env=None, timeout=20, out_re=None, no_err=False):
        self.name = name
        self.script = script
        self.out = out          # 期望的 stdout（字符串，精确比较）
        self.out_re = out_re    # 或者：stdout 要匹配的正则
        self.rc = rc
        self.err = err          # stderr 要匹配的正则
        self.no_err = no_err    # stderr 必须为空
        self.bash = bash
        self.stdin = stdin
        self.mode = mode        # c / file / stdin
        self.args = list(args)
        self.files = files or {}
        self.env = env or {}
        self.timeout = timeout


C = Case
CASES = []


def add(*a, **k):
    CASES.append(Case(*a, **k))


# ---------------------------------------------------------------- 基础与引号
add("echo 基本", "echo hello world", bash=True)
add("相邻引号拼接(测试框架依赖)", "uname -s; echo __SKDO''NE_123__", bash=True)
add("单引号", "echo 'a  b   $HOME \\n'", bash=True)
add("双引号", 'x=val; echo "a  b $x \\$x \\" \\\\ \\a"', bash=True)
add("反斜杠", "echo a\\ b \\$x \\\\ \\'q\\'", bash=True)
add("空参数", 'printf "[%s]" "" a "" \'\'; echo', bash=True)
add("混合拼接", "x=1; echo a\"b$x\"'c$x'd", bash=True)
add("注释", "echo a # 注释\n# 整行注释\necho b#不是注释", bash=True)
add("ANSI-C 引号", "echo $'a\\tb\\n'c$'\\x41\\u4f60'", bash=True)
add("续行", "echo a \\\nb", bash=True)
add("引号里的换行", "echo 'a\nb'; echo \"c\nd\"", bash=True)
add("中文参数", "echo 你好 世界; x=汉字; echo ${#x}", out="你好 世界\n2\n")
add("$\"...\"", 'echo $"hi there"', bash=True)
add("美元符号字面", "echo $ a$ '$' \"$\"", bash=True)

# ---------------------------------------------------------------- 变量与参数展开
add("赋值与引用", "a=1 b=2; echo $a$b ${a}x", bash=True)
add("默认值", "unset u; e=; echo ${u:-d1} ${u-d2} ${e:-d3} [${e-d4}]", bash=True)
add("赋默认值", "unset u; echo ${u:=set}; echo $u; e=; echo ${e:=x}$e", bash=True)
add("替代值", "s=v; e=; echo [${s:+alt}] [${e:+alt}] [${e+alt}] [${u+alt}]", bash=True)
add("长度", 'x=hello; echo ${#x}; set -- a b c; echo ${#}', bash=True)
add("去后缀/前缀", "f=dir/sub/file.tar.gz; echo ${f%.*} ${f%%.*} ${f#*/} ${f##*/}", bash=True)
add("模式里引号", 'f="a*b*c"; echo "${f%"*c"}" ${f#a\\*}', bash=True)
add("去前缀(汉字)", "x=你好世界; echo ${x#你} ${x%界}", out="好世界 你好世\n")
add(":? 未设置报错", "echo ${nosuch:?没有这个变量}; echo 不应执行", rc=1, err="nosuch: 没有这个变量", out="")
add(":? 有值不报错", "v=1; echo ${v:?x}", out="1\n")
add("操作数里的展开", 'unset u; d=def; echo "${u:-$d and $(echo cmd)}"', bash=True)
add("操作数分词", 'unset u; for w in ${u:-a b c}; do echo "<$w>"; done', bash=True)
add("双引号操作数不分词", 'unset u; for w in "${u:-a b c}"; do echo "<$w>"; done', bash=True)
add("双引号里单引号", "unset u; echo \"${u:-'q'}\"", bash=True)
add("$?", "true; echo $?; false; echo $?; (exit 7); echo $?", bash=True)
add("$$ 与子 shell", 'a=$$; b=$(echo $$); [ "$a" = "$b" ] && echo same', bash=True)
add("$# $@ $*", 'set -- "a b" c ""; echo $#; for x in "$@"; do echo "[$x]"; done; for x in $*; do echo "<$x>"; done', bash=True)
add('"$*" 与 IFS', 'set -- a b c; echo "$*"; IFS=:; echo "$*"; IFS=; echo "$*"', bash=True)
add('"$@" 无参数', 'set --; for x in "$@"; do echo no; done; echo "[$@]"; printf "%s." "$@" x; echo', bash=True)
add('"$@" 前后缀', 'set -- 1 2 3; for x in "a$@b"; do echo "$x"; done', bash=True)
add("$0 与 -c 参数", "echo $0 $1 $2", mode="c", args=["myname", "p1", "p2"], out="myname p1 p2\n")
add("${10}", "set -- 1 2 3 4 5 6 7 8 9 10 11; echo $10 ${10} ${11}", bash=True)
add("前缀赋值只影响一条命令", 'X=outer; X=inner sh -c \'echo $X\'; echo $X', bash=True)
add("前缀赋值给内建命令", 'IFS=: read a b <<EOF\nx:y\nEOF\necho "$a|$b|$IFS|"', out="x|y||\n")
add("export 给子进程", "export EXPV=exported; NOEX=no; sh -c 'echo [$EXPV][$NOEX]'", bash=True)
add("export 带赋值不分词", 'v="a b"; export E=$v; sh -c \'echo "$E"\'', bash=True)
add("unset", "x=1; unset x; echo [${x-unset}]; f() { :; }; unset -f f; type f >/dev/null 2>&1 || echo nofunc", bash=True)
add("readonly", "readonly R=1; R=2; echo $R", out="1\n", err="只读")
add("RANDOM 是数字", 'r=$RANDOM; case $r in *[!0-9]*) echo bad;; *) echo ok;; esac', out="ok\n")
add("LINENO", "echo $LINENO\necho $LINENO", out="1\n2\n")
add("set -u", "set -u; echo ${undefined_var}; echo after", rc=1, err="未绑定", out="")
add("set -u 默认值可用", "set -u; echo ${undefined_var:-ok}", out="ok\n")

# ---------------------------------------------------------------- 分词
add("IFS 默认分词", 'x="  a   b	c  "; for w in $x; do echo "[$w]"; done', bash=True)
add("IFS=:", 'IFS=:; x="a::b:"; for w in $x; do echo "[$w]"; done', bash=True)
add("IFS 混合", 'IFS=" :"; x="a : b:c :d"; for w in $x; do echo "[$w]"; done', bash=True)
add("IFS 空", 'IFS=; x="a b"; for w in $x; do echo "[$w]"; done', bash=True)
add("命令替换分词", 'for w in $(printf "1 2\\n3"); do echo "<$w>"; done', bash=True)
add("空变量不产生参数", 'e=; printf "[%s]" $e a $e; echo', bash=True)
add('""$e 产生空参数', 'e=; set -- ""$e; echo $#', bash=True)

# ---------------------------------------------------------------- 文件名展开
GLOB_FILES = {"a.txt": "", "b.txt": "", "c.log": "", ".hidden.txt": "", "dir/x.txt": "", "ab": ""}
add("* 通配", "echo *.txt", files=GLOB_FILES, bash=True)
add("? [..] 通配", "echo ?.txt [ab]* [!a]*", files=GLOB_FILES, bash=True)
add("没有匹配保持原样", "echo *.nomatch 'z*'", files=GLOB_FILES, bash=True)
add("引号里不通配", 'echo "*.txt" \\*.txt', files=GLOB_FILES, bash=True)
add("变量里的通配符", 'p="*.log"; echo $p "$p"', files=GLOB_FILES, bash=True)
add("目录通配", "echo */*.txt", files=GLOB_FILES, bash=True)
add("隐藏文件", "echo .h*", files=GLOB_FILES, bash=True)
add("set -f 关闭通配", "set -f; echo *.txt; set +f; echo *.log", files=GLOB_FILES, bash=True)
add("for 里的通配", "for f in *.txt; do echo f=$f; done", files=GLOB_FILES, bash=True)

# ---------------------------------------------------------------- 波浪号
add("~", 'echo ~ ~/x "~" \'~\' a~', out=None, out_re=r"^(\S+) \1/x ~ ~ a~\n$")
add("~ 等于 HOME", '[ ~ = "$HOME" ] && echo yes', out="yes\n")
add("赋值里的 ~", 'p=~/a:~/b; [ "$p" = "$HOME/a:$HOME/b" ] && echo ok', out="ok\n")
add("~root", "echo ~root", out="/root\n")

# ---------------------------------------------------------------- 命令替换
add("$( ) 嵌套", "echo $(echo a $(echo b $(echo c)))", bash=True)
add("反引号", "echo `echo bq` \"`echo in dq`\"", bash=True)
add("反引号里转义", "echo `echo \\`echo inner\\``", bash=True)
add("去掉末尾换行", 'x=$(printf "a\\n\\n\\n"); echo "[$x]"', bash=True)
add("命令替换的状态", "x=$(false); echo $?; y=$(true); echo $?", bash=True)
add("命令替换里的引号和括号", 'echo "$(echo "a)b" \')\' )"', bash=True)
add("命令替换里的 case", 'echo $(case x in x) echo yes;; esac)', bash=True)
add("命令替换跨多行", "x=$(echo a\necho b)\necho $x", bash=True)
add("$((( 是子shell", "echo $( (echo sub) )", bash=True)
add("命令替换变量不影响外面", 'x=1; y=$(x=2; echo $x); echo $x $y', bash=True)

# ---------------------------------------------------------------- 算术
add("算术优先级", "echo $((1+2*3)) $(((1+2)*3)) $((7/2)) $((7%3)) $((-7/2)) $((2**10))", bash=True)
add("算术比较与逻辑", "echo $((3>2)) $((3<2)) $((2==2)) $((2!=2)) $((1&&0)) $((0||5)) $((!0))", bash=True)
add("算术变量", "a=5; b=3; echo $((a*b)) $(($a+$b)) $((a-b*2))", bash=True)
add("算术赋值", "i=1; echo $((i+=2)) $i $((i++)) $i $((++i)) $((i--)) $i", bash=True)
add("三元与位运算", "echo $((5>3?10:20)) $((6&3)) $((6|3)) $((6^3)) $((1<<4)) $((256>>2)) $((~0))", bash=True)
add("进制", "echo $((0x1F)) $((010)) $((2#1010)) $((16#ff))", bash=True)
add("未设置变量为 0", "unset z; echo $((z+1))", bash=True)
add("变量值是表达式", "e='2+3'; echo $((e*2))", bash=True)
add("除以零", "echo $((1/0)); echo 不执行", rc=1, err="除以零", out="")
add("除以零后下一行继续", "echo $((1/0))\necho 下一行", out="下一行\n", err="除以零")
add("算术语法错误", "echo $((1+))", rc=1, err="算术表达式错误")
add("逗号与括号", "echo $(( (1, 2) + 3 ))", bash=True)
add("算术里的命令替换", "echo $(( $(echo 4) * 5 ))", bash=True)

# ---------------------------------------------------------------- 管道
add("管道", "printf 'c\\nb\\na\\n' | sort | head -2", bash=True)
add("管道状态是最后一个", "false | true; echo $?; true | false; echo $?", bash=True)
add("! 取反", "! false; echo $?; ! true; echo $?", bash=True)
add("pipefail", "set -o pipefail; false | true; echo $?", bash=True)
add("管道里的内建命令", "echo abc | read x; echo [$x]; echo xyz | { read y; echo [$y]; }", bash=True)
add("长管道", "echo x | cat | cat | cat | cat | cat | cat | cat | tr x y", bash=True)
add("管道跨行", "echo a |\ncat", bash=True)
add("管道 + 大量数据", "seq 1 100000 | wc -l", bash=True)

# ---------------------------------------------------------------- 重定向
add("> >> <", "echo one > f; echo two >> f; cat < f; wc -l < f", bash=True)
add("2> 与 2>&1", "ls /nonexist 2> e; [ -s e ] && echo err-saved; ls /nonexist > o 2>&1; [ -s o ] && echo both", bash=True)
add("&>", "{ echo out; echo err >&2; } &> both; sort both", bash=True)
add(">&2", "echo to-stderr >&2", out="", err="to-stderr")
add("n> 任意描述符", "exec 3> f3; echo via3 >&3; exec 3>&-; cat f3", bash=True)
add("<> 和 n<", "echo data > f; exec 4< f; read l <&4; echo $l", bash=True)
add("关闭 >&-", "echo closed >&- ; echo $?", out="1\n", err="写入错误")
add("here-doc", "x=V; cat <<EOF\nline $x\n  $(echo cmd)\nEOF", bash=True)
add("here-doc 引号定界符", "x=V; cat <<'EOF'\nline $x `no`\nEOF", bash=True)
add("here-doc <<-", "cat <<-EOF\n\t\ttabbed\n\tEOF", bash=True)
add("here-doc 两个", "cat <<A; cat <<B\na1\nA\nb1\nB", bash=True)
add("here-doc 反斜杠", "cat <<EOF\na\\$x \\\\ \\n \"q\"\nEOF", bash=True)
add("here-string", "cat <<< 'hello here'; x=1; tr 1 2 <<< $x", bash=True)
add("复合命令重定向", "{ echo a; echo b; } > f; cat f; for i in 1 2; do echo $i; done > g; cat g", bash=True)
add("while read < file", "printf 'l1\\nl2\\n' > f; while read line; do echo \"got $line\"; done < f", bash=True)
add("重定向文件不存在", "cat < /nonexist; echo $?", out="1\n", err="没有那个文件或目录")
add("noclobber", "echo a > f; set -C; echo b > f; echo $?; echo c >| f; cat f", out="1\nc\n", err="不能覆盖")
add("内建命令重定向后恢复", "echo to-file > f; echo to-stdout; cat f", bash=True)
add("重定向顺序", "ls /nonexist 2>&1 > /dev/null | wc -l", bash=True)
add("模糊重定向", 'f="a b"; echo x > $f; echo $?', out="1\n", err="模糊的重定向")

# ---------------------------------------------------------------- 列表与分组
add("; && ||", "true && echo a; false && echo b; false || echo c; true || echo d", bash=True)
add("&& || 链", "false && echo 1 || echo 2 && echo 3", bash=True)
add("{ } 分组", "{ echo a; echo b; } | wc -l", bash=True)
add("( ) 子 shell 隔离", "x=1; (x=2; cd /; echo $x $PWD); echo $x; [ \"$PWD\" != / ] && echo notroot", bash=True)
add("子 shell 退出码", "(exit 3); echo $?", bash=True)
add("后台与 wait", "sleep 0.1 & p=$!; wait $p; echo $?; (exit 5) & wait $!; echo $?", bash=True)
add("后台作业不阻塞", "sleep 2 & echo immediately; kill $! 2>/dev/null; wait", out="immediately\n")
add("wait 全部", "for i in 1 2 3; do (sleep 0.1; echo job$i) & done; wait; echo all", out_re=r"^(job[123]\n){3}all\n$")
add("非交互后台 stdin 是 /dev/null", "cat & wait; echo done", out="done\n", stdin="should-not-be-read\n")

# ---------------------------------------------------------------- 流程控制
add("if elif else", "for v in 1 2 3; do if [ $v = 1 ]; then echo one; elif [ $v = 2 ]; then echo two; else echo other; fi; done", bash=True)
add("if 多行", "if true\nthen\n  echo yes\nfi", bash=True)
add("while", "i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done", bash=True)
add("until", "i=0; until [ $i -ge 3 ]; do echo $i; i=$((i+1)); done", bash=True)
add("for 无 in", "set -- x y; for a; do echo $a; done", bash=True)
add("for 空列表", "for a in; do echo no; done; echo done", bash=True)
add("case", "for w in apple banana cherry 'x y' Z; do case $w in a*|b*) echo AB:$w;; 'x y') echo space;; [A-Z]) echo upper;; *) echo other:$w;; esac; done", bash=True)
add("case 空分支与 (", "case x in (y) echo y;; (x) ;; esac; echo after", bash=True)
add("case 变量模式", 'p="a*"; case abc in $p) echo glob;; esac; case abc in "$p") echo literal;; *) echo nomatch;; esac', bash=True)
add("break continue", "for i in 1 2 3 4 5; do [ $i = 2 ] && continue; [ $i = 4 ] && break; echo $i; done", bash=True)
add("break 2", "for i in 1 2; do for j in a b; do [ $j = b ] && break 2; echo $i$j; done; done; echo end", bash=True)
add("continue 2", "for i in 1 2; do for j in a b; do [ $j = b ] && continue 2; echo $i$j; done; echo never; done", bash=True)
add("循环状态", "for i in 1; do false; done; echo $?; while false; do :; done; echo $?", bash=True)
add("嵌套 if 在管道里", "echo x | if read v; then echo got $v; fi", bash=True)

# ---------------------------------------------------------------- 函数
add("函数参数与返回", "f() { echo \"$# [$1] [$2]\"; return 4; }; f a 'b c'; echo $?", bash=True)
add("函数递归", "fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1-1))) )); fi; }; fact 10", bash=True)
add("local", "x=g; f() { local x=l; echo in:$x; }; f; echo out:$x", bash=True)
add("local 无值", "x=g; f() { local x; echo [${x-unset}]; x=1; }; f; echo $x", bash=True)
add("函数修改全局", "f() { y=set; }; f; echo $y", bash=True)
add("function 关键字", "function g { echo kw; }; g; function h() { echo kw2; }; h", bash=True)
add("函数位置参数恢复", "set -- outer; f() { echo $1; }; f inner; echo $1", bash=True)
add("函数重定向", "f() { echo body; } > out; f; cat out", bash=True)
add("函数里 return 跳出循环", "f() { for i in 1 2 3; do [ $i = 2 ] && return 9; echo $i; done; }; f; echo $?", bash=True)
add("无限递归不崩溃", "f() { f; }; f; echo survived", out=None, out_re=r"(survived\n)?$", err="函数调用层数过多", rc=None)

# ---------------------------------------------------------------- 内建命令
add("cd 与 pwd", "mkdir -p d/e; cd d/e; pwd | sed 's,.*/,,'; cd ..; basename $(pwd); cd -; cd; [ \"$PWD\" = \"$HOME\" ] && echo home",
    out_re=r"^e\nd\n.*/d/e\nhome\n$")
add("cd 不存在", "cd /nonexist; echo $?", out="1\n", err="cd: /nonexist: 没有那个文件或目录")
add("cd .. 逻辑路径", "mkdir -p real/sub; ln -s real/sub link; cd link; cd ..; [ \"$PWD\" = \"$HOME\" ] && echo logical; cd -P link; cd ..; basename $PWD",
    out="logical\nreal\n")
add("echo -n -e", "echo -n a; echo -e 'b\\tc\\nd'; echo -e 'x\\cy'; echo; echo -E 'raw\\n'; echo -ne 'z\\n'", bash=True)
add("echo 不认识的选项", "echo -x -n; echo --", bash=True)
add("printf", "printf '%s-%d-%05.1f-%x-%o-%c-%%\\n' str 42 3.14159 255 8 abc", bash=True)
add("printf 重复格式", "printf '%s=%s\\n' a 1 b 2 c", bash=True)
add("printf %b 和宽度", "printf '[%5s][%-5s][%b]\\n' ab cd 'e\\tf'", bash=True)
add("printf -v", "printf -v v '%03d' 7; echo $v", bash=True)
add("printf 字符编码", "printf '%d\\n' \"'A\"", bash=True)
add("read 多个变量", "echo 'a b c d' | { read x y z; echo \"$x|$y|$z\"; }", bash=True)
add("read -r 与反斜杠", "printf 'a\\\\ b c\\n' | { read x y; echo \"[$x][$y]\"; }; printf 'a\\\\ b\\n' | { read -r x y; echo \"[$x][$y]\"; }", bash=True)
add("read REPLY 与 EOF", "printf 'line' | { read; echo \"$? [$REPLY]\"; }", bash=True)
add("read 不多读 stdin", "", mode="stdin", stdin="read x\nhello-data\necho got=$x\n", out="got=hello-data\n")
add("read IFS", "echo 'a:b:c' | { IFS=: read p q; echo \"$p $q\"; }", bash=True)
add("read -p 非终端不显示提示", "echo v | { read -p 'prompt> ' x; echo $x; }", bash=True)
add("test 字符串", "[ abc = abc ] && echo eq; [ abc != abd ] && echo ne; [ -z '' ] && echo z; [ -n x ] && echo n; [ x ] && echo nonempty; [ '' ] || echo empty", bash=True)
add("test 整数", "[ 3 -eq 3 ] && echo eq; [ 2 -lt 3 ] && echo lt; [ 3 -ge 3 ] && echo ge; [ 4 -gt 3 ] && echo gt; [ 1 -ne 2 ] && echo ne; [ -1 -le 0 ] && echo le", bash=True)
add("test 文件", "touch f; mkdir d; [ -e f ] && echo e; [ -f f ] && echo f; [ -d d ] && echo d; [ -f d ] || echo notf; [ -s f ] || echo empty; echo x > f; [ -s f ] && echo nonempty; [ -r f -a -w f ] && echo rw; [ -x f ] || echo noexec", bash=True)
add("test ! -a -o ( )", "[ ! -e /nonexist ] && echo neg; [ 1 = 1 -a 2 = 2 ] && echo and; [ 1 = 2 -o 2 = 2 ] && echo or; [ \\( 1 = 2 \\) -o \\( 3 = 3 \\) ] && echo paren", bash=True)
add("test 特殊参数个数", "[ -n ] && echo a; [ = ] && echo b; [ ! ] && echo c; [ ! = ] || echo d; test; echo $?", bash=True)
add("test 错误", "[ 1 -eq x ]; echo $?; [ a; echo $?", out="2\n2\n", err="需要整数表达式")
add("true false :", "true; echo $?; false; echo $?; :; echo $?", bash=True)
add("shift", "set -- a b c d; shift; echo $@; shift 2; echo $@; shift 5; echo $?", out="b c d\nd\n1\n", err="移位次数超出范围")
add("set -- 与 set 列出", "set -- p q; echo $1$2; x_test_var='a b'; set | grep '^x_test_var='", out="pq\nx_test_var='a b'\n")
add("set -e", "set -e; true; false; echo 不应执行", rc=1, out="")
add("set -e 条件里不退出", "set -e; if false; then :; fi; false || true; ! true; f() { false; }; f && echo no; echo survived", bash=True)
add("set -x", "set -x; echo traced", out="traced\n", err=r"\+ echo traced")
add("set -o 列表", "set -o | grep -c errexit", out="1\n")
add("alias", "alias hi='echo hello'\nhi there\nalias hi\nunalias hi\nhi 2>/dev/null || echo gone", out="hello there\nalias hi='echo hello'\ngone\n")
add("alias 递归不死循环", "alias ls='ls -d'\nls /", out="/\n")
add("alias 结尾空格", "alias pre='echo '\nalias word=expanded\npre word", out="expanded\n")
add("alias 在同一行不生效", "alias zz='echo z'; zz 2>/dev/null; echo $?", out="127\n")
add("source 与 .", "echo 'sv=sourced; echo in-src $1' > s.sh; . ./s.sh arg; echo $sv; source ./s.sh", bash=True)
add("source 里 return", "printf 'echo a\\nreturn 3\\necho b\\n' > r.sh; . ./r.sh; echo $?", bash=True)
add("source 不存在", ". ./nonexist.sh; echo $?", out="1\n", err="没有那个文件或目录")
add("eval", "cmd='echo evaluated $((1+1))'; eval $cmd; eval 'x=5'; echo $x", bash=True)
add("type", "type cd; type if; type ls; f() { :; }; type f | head -1; type nonexist; echo $?",
    out_re=r"^cd 是 shell 内建命令\nif 是 shell 关键字\nls 是 /\S+/ls\nf 是函数\n1\n$", err="type: nonexist: 未找到")
add("type -t", "alias a1=x; f() { :; }; type -t a1 if f cd ls", out="alias\nkeyword\nfunction\nbuiltin\nfile\n")
add("command -v", "command -v cd; command -v ls; command -v nonexist; echo $?", out_re=r"^cd\n/\S+/ls\n1\n$")
add("command 跳过函数", "echo() { printf 'func\\n'; }; echo hi; command echo real", out="func\nreal\n")
add("umask", "umask 027; umask; umask -S; umask u=rwx,g=rx,o=; umask", out="0027\nu=rwx,g=rx,o=\n0027\n")
add("exec 重定向", "exec > out; echo into-file; exec >&2; cat out >&2", out="", err="into-file")
add("exec 替换进程", "exec echo replaced; echo never", bash=True)
add("exec 不存在的命令", "exec nonexist_cmd; echo never", rc=127, out="", err="未找到命令")
add("times", "times | wc -l", out="2\n")
add("help", "help | head -1; help cd | head -1", out_re=r"^sksh .*shell\ncd \[-L\|-P\] \[目录\]\n$")
add("help 主题", "help 语法 | head -1", out="命令语法速查\n")
add("help 未知", "help nosuch; echo $?", out="1\n", err="没有与")
add("history 非交互", "history; echo $?", out="0\n")
add("kill -l", "kill -l | head -1; kill -l 9; kill -l 130; kill -l TERM", out_re=r"^ 1\) SIGHUP.*\nKILL\nINT\n15\n$")
add("kill 进程", "sleep 10 & kill $!; wait $!; echo $?", bash=True)
add("kill -9", "sleep 10 & kill -9 $!; wait $!; echo $?", bash=True)
add("trap EXIT", "trap 'echo bye' EXIT; echo main", bash=True)
add("trap 信号", "trap 'echo got USR1' USR1; kill -USR1 $$; echo after", bash=True)
add("trap 列出与重置", "trap 'echo x' INT; trap -p; trap - INT; trap -p | wc -l", out="trap -- 'echo x' INT\n0\n")
add("trap 子 shell 不继承", "trap 'echo trapped' EXIT; (echo sub); echo main", bash=True)
add("jobs fg 非交互报错", "fg; echo $?", out="1\n", err="没有作业控制")
add("jobs 非交互", "sleep 0.3 & jobs | grep -c sleep; wait", out="1\n")
add("logout 非登录", "logout; echo $?", out="1\n", err="不是登录 shell")
add("exit 码", "exit 42", rc=42, out="")
add("exit 默认上一条", "false; exit", rc=1, out="")
add("exit 非数字", "exit abc", rc=2, err="需要数字参数")
add("break 在循环外", "break; echo $?", out="1\n", err="只能在")
add("return 在函数外", "return 2; echo $?", out="1\n", err="只能在函数")

# ---------------------------------------------------------------- 语法错误与错误处理
add("语法错误 fi", "echo before\nfi\necho after", rc=2, out="before\n", err=r"^sksh: 语法错误: 意外的符号 'fi'")
add("语法错误 缺 fi", "if true; then echo x", rc=2, out="", err=r"sksh: 语法错误: 意外的文件结束")
add("语法错误 ;;", "echo a ;; echo b", rc=2, out="", err=r"sksh: 语法错误: 意外的符号 ';;'")
add("语法错误 |", "| echo x", rc=2, err=r"sksh: 语法错误: 意外的符号 '\|'")
add("语法错误 未闭合", "echo $(echo", rc=2, err=r"sksh: 语法错误")
add("语法错误 重定向", "echo >", rc=2, err=r"sksh: 语法错误: 重定向 '>' 后面缺少文件名")
add("语法错误 错误的替换", "echo ${a:x}", rc=2, err=r"sksh: 语法错误: 错误的替换")
add("语法错误 空分组", "{ }", rc=2, err=r"sksh: 语法错误")
add("语法错误 带行号(脚本)", "echo ok\n\nwhile true; do\ndone\n", mode="file", rc=2, out="ok\n", err=r"语法错误: 意外的符号 'done'（\S+ 第 4 行）")
add("未找到命令", "nonexistent_cmd_xyz arg; echo $?", out="127\n", err="sksh: nonexistent_cmd_xyz: 未找到命令")
add("权限不够", "echo 'echo hi' > noexec.sh; chmod -x noexec.sh; ./noexec.sh; echo $?", out="126\n", err="权限不够")
add("是一个目录", "mkdir dd; ./dd; echo $?", out="126\n", err="是一个目录")
add("没有 #! 的脚本当 shell 脚本运行", "printf 'echo noshebang $1\\n' > ns.sh; chmod +x ns.sh; ./ns.sh arg", out="noshebang arg\n")
add("脚本错误带行号", "true\nnonexist_xyz\n", mode="file", rc=127, err=r"sksh: \S+: 第 2 行: nonexist_xyz: 未找到命令")

# ---------------------------------------------------------------- 脚本模式
add("脚本参数", 'echo "$0" | sed "s,.*/,,"; echo $# "$1" "$2"', mode="file", args=["a b", "c"], out="script.sh\n2 a b c\n")
add("#! 脚本由内核执行", "", mode="shebang", args=["x"], out="shebang x\n")
add("从 stdin 读脚本", "", mode="stdin", stdin="echo from-stdin\nx=1\necho $x\n", out="from-stdin\n1\n")
add("脚本退出码是最后一条", "true\nfalse\n", mode="file", rc=1)
add("-s 读 stdin 带参数", "", mode="stdin", stdin='echo "$1"\n', args=["-s", "argx"], out="argx\n")
add("--version", "", mode="raw", args=["--version"], out_re=r"^sksh ")

# ---------------------------------------------------------------- 健壮性
add("超长单词 200KB", "x=$(head -c 200000 /dev/zero | tr '\\0' a); echo ${#x}", out="200000\n")
add("大量参数 20000", "set -- $(seq 1 20000); echo $#; printf '%s\\n' \"$@\" | tail -1", out="20000\n20000\n")
add("深层嵌套命令替换", "echo " + "$(echo " * 30 + "deep" + ")" * 30, out="deep\n")
add("大 here-doc 1MB", "cat <<EOF | wc -c\n" + ("x" * 999 + "\n") * 1000 + "EOF\n", mode="file", out="1000000\n")
add("大 here-doc 展开", "v=val\ncat <<EOF | tail -1\n" + ("line $v\n" * 20000) + "EOF\n", mode="file", out="line val\n")
add("万行脚本", "".join("i=$((i+1))\n" for _ in range(10000)) + "echo $i\n", mode="file", out="10000\n")
add("没有泄漏的文件描述符", "ls /proc/self/fd | sort -n | tr '\\n' ' '", out="0 1 2 3 ")
add("脚本 fd 不泄漏给子进程", "ls /proc/self/fd | sort -n | tr '\\n' ' '\n", mode="file", out="0 1 2 3 ")
add("内建命令重定向后 fd 不泄漏", "echo x > f; exec 5<f; ls /proc/self/fd | sort -n | tr '\\n' ' '", out="0 1 2 3 5 ")
add("深层括号嵌套", "echo " + "(" * 0 + "$((" + "(" * 50 + "1" + ")" * 50 + "))", out="1\n")
add("大量函数调用", "f() { :; }; i=0; while [ $i -lt 2000 ]; do f; i=$((i+1)); done; echo $i", out="2000\n")
add("空命令和空行", "\n\n   \n;\n", rc=2, err="语法错误")
add("只有注释", "# 只是注释\n", out="")
add("NUL 字节被忽略", "printf 'echo a\\0b\\n' > z.sh; . ./z.sh", out="ab\n")
add("嵌套太深报错不崩溃", "echo " + "$(" * 400 + ")" * 400, rc=2, err="嵌套太深")

# ---------------------------------------------------------------- 登录 shell
add("登录 shell 执行 profile 和 rc", "", mode="login",
    files={".skshrc": "echo rc-ran; alias rcalias='echo from-rc'\n"},
    out_re=r"rc-ran\n(.*\n)*from-rc\ndone\n")


def run_one(sksh, case, bash_mode=False, verbose=False):
    tmp = tempfile.mkdtemp(prefix="sksh-test-")
    try:
        home = tmp
        for rel, content in case.files.items():
            p = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as f:
                f.write(content)
        env = {
            "PATH": "/usr/local/bin:/usr/bin:/bin",
            "HOME": home,
            "LC_ALL": "C.UTF-8",
            "LANG": "C.UTF-8",
            "TERM": "dumb",
            "USER": "root",
        }
        env.update(case.env)
        shell = "/bin/bash" if bash_mode else sksh
        stdin_data = case.stdin
        if case.mode == "c":
            argv = [shell, "-c", case.script] + case.args
        elif case.mode == "file":
            sp = os.path.join(tmp, "script.sh")
            with open(sp, "w", encoding="utf-8") as f:
                f.write(case.script)
            argv = [shell, sp] + case.args
        elif case.mode == "stdin":
            argv = [shell] + case.args
        elif case.mode == "shebang":
            sp = os.path.join(tmp, "sb.sh")
            with open(sp, "w", encoding="utf-8") as f:
                f.write("#!%s\necho shebang \"$1\"\n" % shell)
            os.chmod(sp, 0o755)
            argv = [sp] + case.args
        elif case.mode == "login":
            # 登录 + 强制交互（stdin 是管道）：执行 /etc/profile 和 ~/.skshrc 之后再读命令
            argv = [shell, "-l", "-i"]
            stdin_data = "rcalias\necho done\n"
        else:
            argv = [shell] + case.args
        if stdin_data is None:
            stdin_data = ""
        p = subprocess.run(argv, input=stdin_data.encode(), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, cwd=tmp, env=env, timeout=case.timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return "TIMEOUT", "", ""
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sksh", default=os.environ.get("SKSH", DEFAULT_SKSH))
    ap.add_argument("-k", help="只运行名字包含这个关键字的用例")
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()
    if not os.path.exists(args.sksh):
        print("找不到 sksh: %s（先编译）" % args.sksh)
        sys.exit(2)
    passed = failed = 0
    failures = []
    for case in CASES:
        if args.k and args.k not in case.name:
            continue
        rc, out, err = run_one(args.sksh, case)
        problems = []
        if case.bash:
            brc, bout, berr = run_one(args.sksh, case, bash_mode=True)
            if (rc, out) != (brc, bout):
                problems.append("与 bash 不一致：\n  sksh: rc=%r out=%r\n  bash: rc=%r out=%r\n  sksh stderr=%r" %
                                (rc, out, brc, bout, err))
        else:
            if case.out is not None and out != case.out:
                problems.append("stdout 不对：期望 %r，实际 %r" % (case.out, out))
            if case.out_re is not None and not re.search(case.out_re, out):
                problems.append("stdout 不匹配 /%s/：实际 %r" % (case.out_re, out))
            if case.rc is not None and rc != case.rc:
                problems.append("退出码：期望 %r，实际 %r" % (case.rc, rc))
        if case.err is not None and not re.search(case.err, err, re.M):
            problems.append("stderr 不匹配 /%s/：实际 %r" % (case.err, err))
        if case.no_err and err:
            problems.append("stderr 应该为空：%r" % err)
        if "段错误" in err or "core" in err or rc in (-11, -6, 139, 134):
            problems.append("崩溃了！rc=%r err=%r" % (rc, err))
        if problems:
            failed += 1
            failures.append((case.name, problems))
            print("✗ %s" % case.name)
            for pr in problems:
                print("    " + pr.replace("\n", "\n    "))
        else:
            passed += 1
            if args.v:
                print("✓ %s" % case.name)
    print("\n通过 %d，失败 %d（共 %d）" % (passed, failed, passed + failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
