/*
 * help.c — 内建命令 help：中文帮助（内建命令一览、语法速查、快捷键）
 */
#include "sksh.h"

#include <string.h>

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    const char *detail;     /* 可以为 NULL */
} help_entry;

static const help_entry entries[] = {
    { ".", ". 文件 [参数...]", "在当前 shell 里执行文件中的命令（同 source）",
      "  和直接运行脚本不同，source 不会 fork 子进程，所以脚本里的 cd、变量赋值、\n"
      "  alias 会影响当前 shell。文件名不含 / 时先在 PATH 里找，再找当前目录。" },
    { ":", ":", "什么也不做，返回 0", "  常用来写死循环：while :; do ...; done" },
    { "[", "[ 表达式 ]", "同 test，最后一个参数必须是 ]", NULL },
    { "alias", "alias [名字[=值] ...]", "定义或显示别名",
      "  alias ll='ls -l'   以后输入 ll 就相当于 ls -l\n"
      "  不带参数时列出所有别名。别名只在命令名的位置展开。" },
    { "bg", "bg [%作业]", "让已停止的作业在后台继续运行",
      "  先用 Ctrl-Z 停止前台命令，再用 bg 让它在后台接着跑。" },
    { "break", "break [n]", "跳出 for/while/until 循环（n 层）", NULL },
    { "cd", "cd [-L|-P] [目录]", "改变当前目录",
      "  cd        回到家目录 $HOME\n"
      "  cd -      回到上一次所在的目录（$OLDPWD）\n"
      "  cd ..     上一级目录\n"
      "  当前目录是进程的属性（内核里 task_struct 的 fs->pwd），所以 cd 必须是内建命令。" },
    { "command", "command [-vV] 命令 [参数...]", "执行命令（跳过同名函数），或查询命令是什么",
      "  command -v ls   打印 ls 的路径，找不到返回 1（脚本里常用来检测命令是否存在）" },
    { "continue", "continue [n]", "跳到循环的下一轮", NULL },
    { "echo", "echo [-neE] [参数...]", "输出参数",
      "  -n  不输出结尾的换行\n"
      "  -e  解释转义：\\n 换行 \\t 制表符 \\\\ 反斜杠 \\e ESC \\0nnn 八进制 \\xHH 十六进制 \\c 停止输出" },
    { "eval", "eval [参数...]", "把参数拼成一条命令再执行一次", NULL },
    { "exec", "exec [命令 [参数...]]", "用命令替换当前 shell 进程；不带命令时让重定向永久生效",
      "  exec ls      shell 进程直接变成 ls（不 fork），ls 结束后这个 shell 也就没了\n"
      "  exec >log    之后所有输出都写到 log 文件" },
    { "exit", "exit [n]", "退出 shell，退出码为 n（默认是上一条命令的 $?）", NULL },
    { "export", "export [-n] [名字[=值] ...]", "把变量标记为环境变量，传给之后启动的程序",
      "  export PATH=$PATH:/root/bin\n"
      "  不带参数时列出所有环境变量；-n 取消导出。" },
    { "false", "false", "返回 1（失败）", NULL },
    { "fg", "fg [%作业]", "把作业放到前台运行",
      "  shell 用 tcsetpgrp() 把终端交给这个作业的进程组，再发送 SIGCONT 让它继续。" },
    { "help", "help [命令 | 语法 | 快捷键 | 作业控制]", "显示帮助", NULL },
    { "history", "history [n] | history -c", "显示命令历史（最多 200 条，保存在 ~/.sksh_history）",
      "  按 ↑ ↓ 可以翻历史；history -c 清空。" },
    { "jobs", "jobs [-lprs]", "列出作业",
      "  -l 同时显示进程组号  -p 只显示进程组号  -r 只显示运行中的  -s 只显示已停止的\n"
      "  + 表示“当前作业”（fg/bg 不带参数时操作它），- 表示上一个作业。" },
    { "kill", "kill [-s 信号 | -信号] pid | %作业 ...   kill -l", "给进程或作业发送信号",
      "  kill %1         给 1 号作业发 SIGTERM（请求结束）\n"
      "  kill -9 1234    SIGKILL：强制杀死，进程无法捕获\n"
      "  kill -STOP %1   让作业停止，kill -CONT %1 继续\n"
      "  kill -l         列出所有信号" },
    { "local", "local 名字[=值] ...", "在函数里定义局部变量（函数返回时恢复原值）", NULL },
    { "logout", "logout", "退出登录 shell", NULL },
    { "printf", "printf [-v 变量] 格式 [参数...]", "按格式输出",
      "  格式和 C 语言的 printf 相同：%s %d %x %o %c %f %b(解释转义) %%\n"
      "  参数比格式多时会重复使用格式：printf '%s\\n' a b c 输出三行。" },
    { "pwd", "pwd [-LP]", "显示当前目录（-P 显示解析符号链接后的真实路径）", NULL },
    { "read", "read [-rs] [-p 提示] [-n 个数] [-d 分隔符] [变量...]", "从标准输入读一行，按 IFS 切分后赋给变量",
      "  read name          读一行到 name\n"
      "  read a b rest      第一个词给 a，第二个给 b，剩下的全给 rest\n"
      "  -r 不处理反斜杠   -s 不回显（输入密码）  -p 先显示提示\n"
      "  没有变量名时存到 REPLY。读到文件结束返回 1，所以可以写 while read line; do ...; done" },
    { "readonly", "readonly [名字[=值] ...]", "把变量设为只读", NULL },
    { "return", "return [n]", "从函数（或 source 的脚本）返回", NULL },
    { "set", "set [-efmnuvxC] [-o 选项] [--] [参数...]", "设置 shell 选项或位置参数",
      "  不带参数：列出所有变量\n"
      "  set -e   命令失败就退出      set -u   使用未定义的变量时报错\n"
      "  set -x   执行前打印命令      set -f   关闭文件名通配\n"
      "  set -C   > 不覆盖已有文件    set -o pipefail  管道里任何一个失败都算失败\n"
      "  set -- a b c   把位置参数设为 a b c（$1=a $2=b $3=c）\n"
      "  把 - 换成 + 表示关闭选项。" },
    { "shift", "shift [n]", "位置参数左移 n 位（$2 变成 $1……）", NULL },
    { "source", "source 文件 [参数...]", "在当前 shell 里执行文件中的命令", NULL },
    { "test", "test 表达式", "条件测试，成立返回 0",
      "  字符串：-z 串（为空） -n 串（非空） 串1 = 串2   串1 != 串2\n"
      "  整数：  a -eq b  -ne  -lt（小于） -le  -gt（大于） -ge\n"
      "  文件：  -e 存在 -f 普通文件 -d 目录 -r 可读 -w 可写 -x 可执行 -s 非空 -L 符号链接\n"
      "  组合：  ! 取反   表达式 -a 表达式（并且）  表达式 -o 表达式（或者）  ( 表达式 )\n"
      "  例子：  if [ -f /etc/motd ] && [ \"$USER\" = root ]; then ... fi" },
    { "times", "times", "显示 shell 和它的子进程用掉的 CPU 时间", NULL },
    { "trap", "trap [-lp] ['命令' | '' | -] 信号...", "收到信号时执行命令",
      "  trap 'echo 再见' EXIT      shell 退出时执行\n"
      "  trap 'echo 按了 Ctrl-C' INT\n"
      "  trap '' INT               忽略信号      trap - INT   恢复默认" },
    { "true", "true", "返回 0（成功）", NULL },
    { "type", "type [-tp] 名字...", "说明一个名字是别名、关键字、函数、内建命令还是外部程序", NULL },
    { "umask", "umask [-S] [模式]", "显示或设置新建文件的权限掩码",
      "  umask 022：新建文件默认权限 = 0666 & ~022 = 0644（rw-r--r--）" },
    { "unalias", "unalias [-a] 名字...", "删除别名（-a 删除全部）", NULL },
    { "unset", "unset [-vf] 名字...", "删除变量或函数", NULL },
    { "wait", "wait [pid | %作业 ...]", "等待后台作业结束，返回它的退出状态",
      "  不带参数时等待所有后台作业。" },
};

static const char syntax_help[] =
    "命令语法速查\n"
    "  命令 参数...           运行程序或内建命令\n"
    "  a ; b                 先后执行\n"
    "  a && b                a 成功才执行 b         a || b    a 失败才执行 b\n"
    "  a | b                 管道：a 的输出接到 b 的输入\n"
    "  a &                   在后台运行，立刻返回提示符\n"
    "  ( a; b )              在子 shell 里执行（变量、cd 不影响当前 shell）\n"
    "  { a; b; }             把多条命令当作一组\n"
    "  ! a                   结果取反\n"
    "\n"
    "引号\n"
    "  '...'                 单引号：里面所有字符都是普通字符\n"
    "  \"...\"                 双引号：保留空格，但 $变量 和 $(命令) 仍然展开\n"
    "  \\x                    反斜杠：让下一个字符失去特殊含义\n"
    "  # 注释                 # 开头的单词直到行尾都是注释\n"
    "\n"
    "变量与展开\n"
    "  名字=值                设置变量（= 两边不能有空格）\n"
    "  名字=值 命令            只对这一条命令生效的环境变量\n"
    "  $名字 ${名字}           取值；${名字:-默认值} ${名字:=默认值} ${名字:+替代} ${#名字}(长度)\n"
    "  ${名字%模式} ${名字#模式}  去掉后缀/前缀（%% ## 为最长匹配）\n"
    "  $? 上一条命令的退出码   $$ shell 的进程号      $! 最近的后台进程号\n"
    "  $0 脚本名  $1..$9 参数  $# 参数个数  \"$@\" 所有参数（各自独立）  $*\n"
    "  ~ 家目录                $(命令) 或 `命令`：命令替换     $((1+2*3))：算术\n"
    "  * ? [abc]             文件名通配（没有匹配时保持原样）\n"
    "\n"
    "重定向\n"
    "  < 文件   > 文件   >> 文件（追加）   2> 文件（标准错误）   2>&1   &> 文件（输出和错误）\n"
    "  n>&-（关闭）  <<结束标记（here-document）  <<< 字符串\n"
    "\n"
    "流程控制\n"
    "  if 条件; then 命令; elif 条件; then 命令; else 命令; fi\n"
    "  while 条件; do 命令; done          until 条件; do 命令; done\n"
    "  for 名字 in 单词...; do 命令; done\n"
    "  case 单词 in 模式1|模式2) 命令;; *) 命令;; esac\n"
    "  名字() { 命令; }                   定义函数，参数是 $1 $2 ...\n";

static const char keys_help[] =
    "行编辑快捷键\n"
    "  ← →  Ctrl-B Ctrl-F     左右移动光标        Alt-B Alt-F  按单词移动\n"
    "  Home End  Ctrl-A Ctrl-E 行首 / 行尾\n"
    "  Backspace  Delete       删除光标前 / 光标处的字符\n"
    "  Ctrl-U  Ctrl-K          删除到行首 / 删除到行尾\n"
    "  Ctrl-W                  删除光标前的一个单词       Ctrl-Y  粘贴刚删除的内容\n"
    "  ↑ ↓  Ctrl-P Ctrl-N      上一条 / 下一条历史命令\n"
    "  Tab                     补全命令名或文件名；按两次列出所有可能\n"
    "  Ctrl-L                  清屏\n"
    "  Ctrl-C                  放弃当前输入的这一行\n"
    "  Ctrl-D                  空行时退出 shell，否则删除光标处的字符\n";

static const char jobs_help[] =
    "作业控制\n"
    "  命令 &        在后台运行（显示 [作业号] 进程号）\n"
    "  Ctrl-Z        停止前台命令（终端给前台进程组发 SIGTSTP）\n"
    "  Ctrl-C        中断前台命令（SIGINT）\n"
    "  jobs          列出作业        fg %1   放回前台        bg %1   在后台继续\n"
    "  kill %1       结束作业        wait    等待后台作业结束\n"
    "  %1 %2 第几号作业   %+ 或 %% 当前作业   %- 上一个作业   %vi 命令以 vi 开头的作业\n"
    "\n"
    "原理：每条管道线是一个进程组。终端在同一时刻只有一个前台进程组，\n"
    "键盘产生的信号（Ctrl-C / Ctrl-Z）只发给前台进程组，后台进程读终端会被内核停住(SIGTTIN)。\n";

static const help_entry *find_entry(const char *name)
{
    for (size_t i = 0; i < SK_ARRAY_LEN(entries); i++)
        if (strcmp(entries[i].name, name) == 0)
            return &entries[i];
    return NULL;
}

const char *help_summary(const char *name)
{
    const help_entry *e = find_entry(name);
    return e ? e->summary : NULL;
}

void help_show_all(FILE *out)
{
    fprintf(out, "sksh %s —— SketchOS 的命令行 shell\n", SKSH_VERSION);
    fputs("输入 help 名字 查看某个内建命令的详细说明；help 语法、help 快捷键、help 作业控制 查看专题。\n\n", out);
    fputs("内建命令：\n", out);
    for (size_t i = 0; i < SK_ARRAY_LEN(entries); i++) {
        const help_entry *e = &entries[i];
        int w = str_width(e->usage, strlen(e->usage));
        fprintf(out, "  %s", e->usage);
        if (w < 40) {
            for (; w < 40; w++)
                fputc(' ', out);
        } else {
            fputs("\n  ", out);
            for (w = 0; w < 40; w++)
                fputc(' ', out);
        }
        fprintf(out, "%s\n", e->summary);
    }
    fputc('\n', out);
    fputs(syntax_help, out);
}

int help_show_one(FILE *out, const char *name)
{
    if (strcmp(name, "语法") == 0 || strcmp(name, "syntax") == 0) {
        fputs(syntax_help, out);
        return 0;
    }
    if (strcmp(name, "快捷键") == 0 || strcmp(name, "keys") == 0) {
        fputs(keys_help, out);
        return 0;
    }
    if (strcmp(name, "作业控制") == 0 || strcmp(name, "作业") == 0 || strcmp(name, "jobctl") == 0) {
        fputs(jobs_help, out);
        return 0;
    }
    const help_entry *e = find_entry(name);
    if (!e)
        return -1;
    fprintf(out, "%s\n    %s\n", e->usage, e->summary);
    if (e->detail)
        fprintf(out, "%s\n", e->detail);
    return 0;
}

int bi_help(int argc, char **argv)
{
    if (argc < 2) {
        help_show_all(stdout);
        return 0;
    }
    int st = 0;
    for (int i = 1; i < argc; i++) {
        if (help_show_one(stdout, argv[i]) < 0) {
            sh_error("help: 没有与 '%s' 匹配的帮助主题（试试 help 不带参数）", argv[i]);
            st = 1;
        }
    }
    return st;
}
