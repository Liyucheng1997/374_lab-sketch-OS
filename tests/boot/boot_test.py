#!/usr/bin/env python3
"""
SketchOS 开机测试：检查 1 号进程 sinit 的行为（在 WSL 里运行）

    python3 tests/boot/boot_test.py                  跑全部测试
    python3 tests/boot/boot_test.py -k main,halt     只跑某几个场景
    python3 tests/boot/boot_test.py --list           列出所有场景
    python3 tests/boot/boot_test.py --real-shell     同时用真正的 sksh/skbox 跑一遍 "real" 场景

场景（--list 可以看到）：cli、unit 直接在 WSL 里运行 sinit / signals.c 的单元测试；
其余场景各启动一次 QEMU：main（完整检查 + SIGUSR2 关机）、nogui、halt、noacpi、fallback、
nothing（含 Ctrl+Alt+Del）、noconsole，以及需要 --real-shell 的 real。

流程：
  1. 编译 sinit（--real-shell 时还有 sksh、skbox）到 <工作目录>/build
  2. 用 sk-cc 编译测试探针 helpers/sktest.c 和替身 shell helpers/minish.c
     （sinit 的测试不依赖正在并行开发的 sksh；minish 作为 /bin/sksh 放进测试根文件系统）
  3. 用 scripts/initramfs.sh 组装基础根文件系统，再为每个场景复制一份、修改、打包
  4. 在 QEMU 里启动每个场景，通过串口执行命令、检查输出（tools/qemu_harness.py）

工作目录默认 $SKETCHOS_WORK/boottest（$SKETCHOS_WORK 默认 ~/sketchos-work），可用 --work 指定。
串口日志保存在 <工作目录>/logs/<场景>.log，测试失败时可以打开看。
"""
import argparse
import codecs
import gzip
import os
import re
import shutil
import stat
import subprocess
import sys
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from qemu_harness import VM  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get("SKETCHOS_WORK", os.path.expanduser("~/sketchos-work"))
SKCC = os.path.join(WORK, "toolchain", "bin", "sk-cc")
GEN_CPIO = os.path.join(WORK, "toolchain", "bin", "gen_init_cpio")
KERNEL = os.path.join(ROOT, "out", "bzImage")

PROMPT = r"root@sketchos:[^\r\n]*# "
TTYS0_NR = (4 << 8) | 64      # /dev/ttyS0 的设备号 4:64 → /proc/<pid>/stat 里的 tty_nr
TTY1_NR = (4 << 8) | 1        # /dev/tty1 = 4:1

# 测试用服务表：覆盖 respawn/once/wait、cond、delay、log、引号、找不到程序、崩溃、顽固进程
TEST_CONF = """\
# 测试用服务表（由 tests/boot/boot_test.py 生成）
serial    respawn  /dev/ttyS0                                  /bin/sksh -l
sleeper   respawn  -                                           /bin/sktest sleep 100000
crashy    respawn  -                                           /bin/sktest crash 3
setup     wait     -          log=/var/log/order.log           /bin/sktest stamp setup 800
after     once     -          log=/var/log/order.log           /bin/sktest stamp after 0
gui       respawn  /dev/tty1  cond=gui log=/var/log/gui.log    /bin/sktest info-sleep
nogui     respawn  /dev/tty2  cond=nogui log=/var/log/nogui.log /bin/sktest info-sleep
late      respawn  -          delay=3000                       /bin/sktest sleep 100000
missing   respawn  -                                           /bin/does-not-exist
quoted    once     -          log=/var/log/quoted.log          /bin/sktest args "hello world" 'x y' plain
stubborn  respawn  -                                           /bin/sktest ignore-term
failonce  once     -                                           /bin/sktest crash 9
"""

# 故意写错的配置（sinit --check 测试）：第几行出什么错是固定的
BAD_CONF = (
    "# 坏配置\r\n"                                                    # 1 注释（CRLF 也要能处理）
    "good     respawn  /dev/ttyS0   /bin/sksh -l\r\n"                 # 2 正确
    "bad      respwan  -            /bin/true\n"                      # 3 策略拼错
    "bad!name once     -            /bin/true\n"                      # 4 名字非法
    "nocmd    once     -            cond=gui\n"                       # 5 缺命令
    "badcond  once     -            cond=maybe /bin/true\n"           # 6 cond 值不对
    "baddelay once     -            delay=abc /bin/true\n"            # 7 delay 不是数字
    "good     once     -            /bin/true\n"                      # 8 名字重复
    "warnopt  once     -            colour=red /bin/true\n"           # 9 未知选项：只警告
    "badtty   once     tty1         /bin/true\n"                      # 10 终端不是路径
    "quoted   once     -  log=/var/log/q.log  /bin/echo \"a b\" 'c d'\n"  # 11 正确
)


# ======================================================================
# 结果记录
# ======================================================================

class Results:
    def __init__(self):
        self.items = []          # (场景, 是否通过, 描述, 细节)
        self.current = "?"

    def check(self, cond, desc, detail=""):
        self.items.append((self.current, bool(cond), desc, detail))
        mark = "\033[32m  通过\033[0m" if cond else "\033[31m  失败\033[0m"
        print("%s  %s%s" % (mark, desc, "" if cond or not detail else "\n        " + str(detail)[:600]),
              flush=True)
        return bool(cond)

    def summary(self):
        by = {}
        for sc, ok, _, _ in self.items:
            p, f = by.get(sc, (0, 0))
            by[sc] = (p + ok, f + (not ok))
        print("\n========== 汇总 ==========")
        for sc, (p, f) in by.items():
            print("  %-10s 通过 %3d   失败 %3d" % (sc, p, f))
        fails = [i for i in self.items if not i[1]]
        if fails:
            print("\n失败的检查：")
            for sc, _, desc, _ in fails:
                print("  [%s] %s" % (sc, desc))
        total_p = sum(1 for i in self.items if i[1])
        print("\n共 %d 项检查，通过 %d，失败 %d" % (len(self.items), total_p, len(fails)))
        return not fails


R = Results()


# ======================================================================
# 构建与打包
# ======================================================================

def run(cmd, **kw):
    print("$", " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def build_userspace(tdir, targets):
    """编译用户态程序。等价于 USERSPACE_BUILD=... scripts/userspace.sh <targets>；
    直接调用 make，并且在 Makefile 带 Windows 换行符时用去掉 \\r 的副本（防御性处理）。"""
    ub = os.path.join(tdir, "build")
    mk = os.path.join(ROOT, "userspace", "Makefile")
    args = ["make", "-C", os.path.join(ROOT, "userspace"), "CC=" + SKCC, "BUILD=" + ub,
            "-j%d" % (os.cpu_count() or 4)]
    with open(mk, "rb") as f:
        data = f.read()
    if b"\r" in data:
        lf = os.path.join(tdir, "Makefile.lf")
        with open(lf, "wb") as f:
            f.write(data.replace(b"\r\n", b"\n"))
        args += ["-f", lf]
    run(args + targets, stdout=subprocess.DEVNULL)
    return ub


def build_helpers(tdir):
    out = os.path.join(tdir, "helpers")
    os.makedirs(out, exist_ok=True)
    common = os.path.join(ROOT, "userspace", "common")
    for name in ("sktest", "minish"):
        run([SKCC, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
             "-Wno-sign-compare", "-I", common, "-o", os.path.join(out, name),
             os.path.join(HERE, "helpers", name + ".c"), os.path.join(common, "skutil.c"), "-lm"])
    return out


def make_base_stage(tdir, ub, helpers):
    """用正式的 scripts/initramfs.sh 组装根文件系统（rootfs/ + 编译好的程序），再放进测试探针"""
    base = os.path.join(tdir, "base")
    env = dict(os.environ, USERSPACE_BUILD=ub)
    run(["bash", os.path.join(ROOT, "scripts", "initramfs.sh"), "--stage", base,
         "--out", os.path.join(tdir, "base.cpio.gz")], env=env, stdout=subprocess.DEVNULL)
    shutil.copy2(os.path.join(helpers, "sktest"), os.path.join(base, "bin", "sktest"))
    return base


def pack(stage, out, console_nodes=True):
    """把暂存目录打包成 initramfs（和 scripts/initramfs.sh 同样的办法：内核自带的 gen_init_cpio）"""
    lines = []
    if console_nodes:
        lines += ["nod /dev/console 0600 0 0 c 5 1", "nod /dev/null 0666 0 0 c 1 3"]
    entries = []
    for dirpath, dirnames, filenames in os.walk(stage):
        for n in dirnames + filenames:
            entries.append(os.path.join(dirpath, n))
    for p in sorted(entries):
        rel = "/" + os.path.relpath(p, stage)
        if console_nodes and rel in ("/dev/console", "/dev/null"):
            continue
        st = os.lstat(p)
        if stat.S_ISLNK(st.st_mode):
            lines.append("slink %s %s 0777 0 0" % (rel, os.readlink(p)))
        elif stat.S_ISDIR(st.st_mode):
            mode = "1777" if rel == "/tmp" else "%o" % (st.st_mode & 0o7777)
            lines.append("dir %s %s 0 0" % (rel, mode))
        elif stat.S_ISREG(st.st_mode):
            lines.append("file %s %s %o 0 0" % (rel, p, st.st_mode & 0o7777))
    spec = out + ".spec"
    with open(spec, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    data = subprocess.run([GEN_CPIO, spec], check=True, stdout=subprocess.PIPE).stdout
    with gzip.open(out, "wb", compresslevel=6) as f:
        f.write(data)
    return out


class SafeVM(VM):
    """qemu_harness.VM 把每次 recv() 到的字节单独解码；串口数据是一点一点到的，
    一个汉字（3 字节）常常被拆在两次 recv 里，于是变成乱码 "��"，中文输出的匹配就会失败。
    这里换成“增量解码器”：不完整的字符先留着，等后面的字节到了再拼起来解码。"""

    def _read_serial(self):
        if not hasattr(self, "_buf_lock") or not hasattr(self, "_log"):
            return super()._read_serial()      # 万一 harness 内部结构变了，退回原实现
        dec = codecs.getincrementaldecoder("utf-8")(errors="replace")
        while not self._closed:
            try:
                data = self.serial.recv(4096)
            except OSError:
                break
            if not data:
                break
            text = dec.decode(data)
            with self._buf_lock:
                self._buf += text
            try:
                self._log.write(text)
                self._log.flush()
            except Exception:
                pass


class Scenario:
    """一个测试场景：基于基础根文件系统做些修改，打包，启动 QEMU"""

    def __init__(self, ctx, name, real_shell=False):
        self.ctx = ctx
        self.name = name
        self.stage = os.path.join(ctx.tdir, "stage-" + name)
        shutil.rmtree(self.stage, ignore_errors=True)
        shutil.copytree(ctx.base, self.stage, symlinks=True)
        if not real_shell:
            # 替身 shell minish 覆盖（或补上）/bin/sksh：sinit 的测试不依赖真正 sksh 的输出格式
            dst = os.path.join(self.stage, "bin", "sksh")
            if os.path.lexists(dst):
                os.remove(dst)
            shutil.copy2(os.path.join(ctx.helpers, "minish"), dst)
            sh_link = os.path.join(self.stage, "bin", "sh")
            if not os.path.lexists(sh_link):
                os.symlink("sksh", sh_link)

    def write(self, rel, text):
        p = os.path.join(self.stage, rel.lstrip("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="") as f:
            f.write(text)

    def remove(self, rel):
        p = os.path.join(self.stage, rel.lstrip("/"))
        if os.path.islink(p) or os.path.isfile(p):
            os.remove(p)
        elif os.path.isdir(p):
            shutil.rmtree(p)

    def boot(self, append="", console_nodes=True):
        initrd = pack(self.stage, os.path.join(self.ctx.tdir, self.name + ".cpio.gz"), console_nodes)
        log = os.path.join(self.ctx.tdir, "logs", self.name + ".log")
        os.makedirs(os.path.dirname(log), exist_ok=True)
        vm = SafeVM(kernel=KERNEL, initrd=initrd, extra_cmdline=append, log_path=log)
        vm.start()
        return vm


# ======================================================================
# 小工具
# ======================================================================

def parse_status(text):
    """解析 /run/sinit/status：返回 (表头, {服务名: {...}})"""
    header, rows = None, {}
    for line in text.replace("\r", "").split("\n"):
        if line.startswith("#"):
            header = line
        elif "\t" in line:
            f = line.split("\t")
            if len(f) >= 5:
                rows[f[0]] = {"state": f[1], "pid": int(f[2]), "restarts": int(f[3]), "cmd": f[4]}
    return header, rows


def status(vm):
    return parse_status(vm.run_serial("sktest status"))


def wait_until(fn, timeout, interval=0.5):
    deadline = time.time() + timeout
    while True:
        v = fn()
        if v or time.time() > deadline:
            return v
        time.sleep(interval)


def wait_exit(vm, timeout):
    try:
        vm.proc.wait(timeout=timeout)
        return True
    except subprocess.TimeoutExpired:
        return False


def boot_to_prompt(vm, timeout=90):
    vm.wait_serial(r"sinit: SketchOS [0-9.]+ starting", timeout)
    vm.wait_serial(r"sinit: services started", timeout)
    vm.wait_serial(PROMPT, timeout)
    time.sleep(0.3)


def info_dict(text):
    d = {"env": {}}
    for line in text.replace("\r", "").split("\n"):
        for m in re.finditer(r"(\w+)=(\S+)", line) if line.startswith(("info ", "parent ")) else []:
            d[m.group(1)] = m.group(2)
        if line.startswith("env ") and "=" in line:
            k, _, v = line[4:].partition("=")
            d["env"][k] = v
        m = re.match(r"fd(\d)=(.*)", line)
        if m:
            d["fd" + m.group(1)] = m.group(2)
    return d


# ======================================================================
# 场景
# ======================================================================

def sc_cli(ctx):
    """不启动 QEMU：在 WSL 里直接运行 sinit（它不是 1 号进程时是个命令行工具）"""
    sinit = os.path.join(ctx.ub, "bin", "sinit")

    def sinit_run(*args):
        r = subprocess.run([sinit] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")

    code, out, _ = sinit_run("--version")
    R.check(code == 0 and re.search(r"sinit \(SketchOS\) [0-9.]+", out), "sinit --version", out)
    code, out, _ = sinit_run("--help")
    R.check(code == 0 and "sinit --check" in out and "poweroff" in out, "sinit --help", out)
    code, out, err = sinit_run()
    R.check(code == 1 and "不是 1 号进程" in err, "不带参数运行：提示不是 1 号进程，退出码 1", err)

    code, out, err = sinit_run("--check", os.path.join(ROOT, "rootfs", "etc", "sketch", "services.conf"))
    R.check(code == 0 and "3 个服务，0 处错误" in out, "--check 正式的 services.conf：3 个服务 0 错误", out + err)
    for name in ("serial", "desktop", "console"):
        R.check(re.search(r"^\s+%s\s" % name, out, re.M), "--check 列出服务 " + name)
    R.check(re.search(r"desktop\s+respawn\s+/dev/tty1\s+cond=gui log=/var/log/sketchd.log\s+/bin/sketchd", out),
            "--check 正确解析 desktop 的选项", out)

    test_conf = os.path.join(ctx.tdir, "test.conf")
    with open(test_conf, "w", encoding="utf-8") as f:
        f.write(TEST_CONF)
    code, out, err = sinit_run("--check", test_conf)
    R.check(code == 0 and "12 个服务，0 处错误" in out, "--check 测试服务表：12 个服务", out + err)
    R.check('/bin/sktest args "hello world" "x y" plain' in out, "--check 引号参数被正确切分", out)

    bad = os.path.join(ctx.tdir, "bad.conf")
    with open(bad, "w", encoding="utf-8", newline="") as f:
        f.write(BAD_CONF)
    code, out, err = sinit_run("--check", bad)
    R.check(code == 1, "--check 坏配置：退出码 1", "code=%d" % code)
    R.check("3 个服务，7 处错误" in out, "--check 坏配置：3 个有效服务（含只有警告的一行）、7 处错误", out + err)
    for line, word in ((3, "respwan"), (4, "bad!name"), (5, "缺少要运行的命令"), (6, "maybe"),
                       (7, "abc"), (8, "重复"), (9, "colour=red"), (10, "tty1")):
        R.check(re.search(r"bad\.conf:%d: .*%s" % (line, re.escape(word)), err),
                "--check 坏配置第 %d 行报告 %s" % (line, word), err)
    R.check(re.search(r"quoted\s+once\s+-\s+log=/var/log/q.log\s+/bin/echo \"a b\" \"c d\"", out),
            "--check 坏配置里正确的行仍被解析（含 CRLF、引号）", out)

    code, out, err = sinit_run("--check", "/nonexistent/services.conf")
    R.check(code == 2 and "打不开" in err, "--check 不存在的文件：退出码 2", err)

    # 安全检查：在 WSL 里 1 号进程不是 sinit，sinit reboot 必须拒绝发信号
    code, out, err = sinit_run("reboot")
    R.check(code == 1 and "不发送信号" in err, "sinit reboot 在非 SketchOS 环境里拒绝发送信号", err)
    code, out, err = sinit_run("bogus")
    R.check(code == 1 and "不认识的参数" in err, "未知参数报错", err)


def sc_unit(ctx):
    """不启动 QEMU：signals.c 的单元测试（用 seccomp 禁用 signalfd，测试“自管道”后备路径）"""
    out = os.path.join(ctx.tdir, "helpers", "sigtest")
    us = os.path.join(ROOT, "userspace")
    run([SKCC, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare",
         "-I", os.path.join(us, "common"), "-I", os.path.join(us, "init"), "-o", out,
         os.path.join(HERE, "helpers", "sigtest.c"), os.path.join(us, "init", "signals.c")])
    r = subprocess.run([out], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    text = r.stdout.decode("utf-8", "replace")
    n = 0
    for line in text.splitlines():
        if line.startswith(("ok ", "FAIL ")):
            n += 1
            R.check(line.startswith("ok "), line.split(" ", 1)[1].strip())
    # signalfd 模式 7 项 + 自管道模式 6 项（自管道不知道发送者，少一项）
    R.check(r.returncode == 0 and n == 13, "sigtest 全部通过（%d 项）" % n, text[-1500:])


def sc_main(ctx):
    """图形模式（默认命令行）下的完整检查，最后用 SIGUSR2 关机"""
    s = Scenario(ctx, "main")
    s.write("/etc/sketch/services.conf", TEST_CONF)
    vm = s.boot()
    try:
        boot_to_prompt(vm)
        t_prompt = time.time()
        out = vm.serial_output()

        # ---- 里程碑和横幅 ----
        R.check(re.search(r"sinit: SketchOS 1\.1 starting", out), "里程碑 'sinit: SketchOS 1.1 starting'")
        R.check(out.find("sinit: SketchOS") < out.find("sinit: services started"), "starting 在 services started 之前")
        R.check("手绘操作系统 1.1" in out, "横幅：手绘操作系统 1.1")
        R.check("|____/|_|\\_\\___|" in out, "横幅：SketchOS 字样")
        R.check(re.search(r"内核\s+Linux 6\.12\.\S+ \(i686\)", out), "横幅：内核版本和架构")
        R.check(re.search(r"时间\s+20\d\d-\d\d-\d\d \d\d:\d\d:\d\d（时区 CST-8）", out), "横幅：时间和时区")
        R.check(re.search(r"内核用了 \d+\.\d\d 秒", out), "横幅：开机用时")
        R.check(out.count("sinit: SketchOS 1.1 starting") == 1, "里程碑只出现一次（没有重复输出）")
        R.check("[sinit]" not in out, "sinit 的普通日志没有刷到串口上")

        # ---- 刚开机时的服务状态（late 还在 delay 中）----
        header, st = status(vm)
        R.check(header == "# name\tstate\tpid\trestarts\tcommand", "状态文件表头", repr(header))
        R.check(st.get("late", {}).get("state") == "stopped" and st["late"]["pid"] == 0,
                "delay=3000 的服务开机后先处于 stopped", st.get("late"))
        R.check(time.time() - t_prompt < 3, "（状态检查足够早，delay 测试有效）")
        for name, want in (("serial", "running"), ("sleeper", "running"), ("setup", "done"),
                           ("after", "done"), ("gui", "running"), ("nogui", "disabled"),
                           ("missing", "failed"), ("quoted", "done"), ("stubborn", "running"),
                           ("failonce", "failed")):
            R.check(st.get(name, {}).get("state") == want, "服务 %s 的状态是 %s" % (name, want), st.get(name))
        R.check(st.get("serial", {}).get("cmd") == "/bin/sksh -l", "状态文件里的命令列", st.get("serial"))
        R.check(st.get("quoted", {}).get("cmd") == '/bin/sktest args "hello world" "x y" plain',
                "状态文件里带空格的参数加了引号", st.get("quoted"))
        R.check(re.search(r"服务 missing 无法启动：找不到可执行的程序 /bin/does-not-exist", out),
                "找不到程序时在控制台报错")
        R.check("服务 failonce 结束了，退出状态 9" in vm.serial_output(), "once 服务失败时报告退出状态")

        # ---- 挂载 ----
        mounts = vm.run_serial("sktest cat /proc/mounts")
        for pat, desc in ((r"^proc /proc proc rw,nosuid,nodev,noexec", "proc 挂在 /proc (nosuid,nodev,noexec)"),
                          (r"^sysfs /sys sysfs rw,nosuid,nodev,noexec", "sysfs 挂在 /sys"),
                          (r"^devtmpfs /dev devtmpfs rw,nosuid", "devtmpfs 挂在 /dev"),
                          (r"^devpts /dev/pts devpts \S*gid=5,mode=620,ptmxmode=666", "devpts (gid=5,mode=620,ptmxmode=666)"),
                          (r"^tmpfs /dev/shm tmpfs rw,nosuid,nodev", "tmpfs 挂在 /dev/shm"),
                          (r"^tmpfs /tmp tmpfs rw,nosuid,nodev", "tmpfs 挂在 /tmp"),
                          (r"^tmpfs /run tmpfs rw,nosuid,nodev\S*mode=755", "tmpfs 挂在 /run (mode=755)")):
            R.check(re.search(pat, mounts, re.M), desc, mounts)
        R.check(len(re.findall(r"^devtmpfs /dev ", mounts, re.M)) == 1, "/dev 没有被重复挂载")
        for path, want in (("/tmp", "1777"), ("/dev/shm", "1777"), ("/run", "0755"),
                           ("/run/sketch", "0755"), ("/run/sinit", "0755"), ("/var/log", "0755")):
            o = vm.run_serial("sktest mode " + path)
            R.check("mode %s=%s" % (path, want) in o, "%s 权限 %s" % (path, want), o)
        for link, target in (("/dev/fd", "/proc/self/fd"), ("/dev/stdin", "/proc/self/fd/0"),
                             ("/dev/stdout", "/proc/self/fd/1"), ("/dev/stderr", "/proc/self/fd/2")):
            o = vm.run_serial("sktest readlink " + link)
            R.check("link %s -> %s" % (link, target) in o, "符号链接 %s -> %s" % (link, target), o)

        # ---- 主机名、回环网卡 ----
        o = vm.run_serial("sktest hostname")
        R.check("hostname=sketchos" in o, "主机名来自 /etc/hostname", o)
        o = vm.run_serial("sktest cat /sys/class/net/lo/flags")
        m = re.search(r"0x([0-9a-f]+)", o)
        R.check(m and int(m.group(1), 16) & 1, "回环网卡 lo 已启用 (IFF_UP)", o)

        # ---- wait 策略的顺序 ----
        o = vm.run_serial("sktest cat /var/log/order.log")
        ss = re.search(r"setup start ([\d.]+)", o)
        se = re.search(r"setup end ([\d.]+)", o)
        af = re.search(r"after start ([\d.]+)", o)
        R.check(ss and se and af and float(se.group(1)) - float(ss.group(1)) >= 0.75
                and float(af.group(1)) >= float(se.group(1)),
                "wait 服务结束之后才启动后面的服务", o)
        R.check("==== [" in o and "sinit: 启动服务 setup" in o, "log= 文件里有启动分隔行", o)
        o = vm.run_serial("sktest cat /var/log/quoted.log")
        R.check("argv[1]=hello world" in o and "argv[2]=x y" in o and "argv[3]=plain" in o,
                "引号参数原样传给程序", o)

        # ---- 服务进程的运行环境 ----
        o = vm.run_serial("sktest cat /var/log/gui.log")
        d = info_dict(o)
        gpid = st.get("gui", {}).get("pid")
        R.check(d.get("pid") == str(gpid) and d.get("sid") == d.get("pid") and d.get("pgrp") == d.get("pid"),
                "服务是新会话和进程组的首领 (setsid)", o)
        R.check(d.get("tty_nr") == str(TTY1_NR), "服务的控制终端是 /dev/tty1", o)
        R.check(d.get("sigblk") == "0000000000000000" and d.get("sigign") == "0000000000000000",
                "服务的信号屏蔽字和忽略集合都被清空", o)
        R.check(d.get("ppid") == "1", "服务的父进程是 1 号进程", o)
        R.check(d.get("cwd") == "/root" and d.get("umask") == "0022", "工作目录 /root、umask 0022", o)
        R.check(d.get("fd0") == "/dev/tty1" and d.get("fd1") == "/var/log/gui.log" and d.get("fd2") == "/var/log/gui.log",
                "stdin 是终端，stdout/stderr 被 log= 重定向", o)
        R.check(d.get("openfds") == "0,1,2", "服务只继承了 0/1/2，sinit 自己的文件描述符没有泄漏", o)
        want_env = {"PATH": "/bin:/sbin", "HOME": "/root", "USER": "root", "SHELL": "/bin/sksh",
                    "TERM": "linux", "TZ": "CST-8", "LANG": "C.UTF-8"}
        R.check(all(d["env"].get(k) == v for k, v in want_env.items()), "服务的环境变量 (docs 4.5)", d["env"])
        R.check(not any(k.startswith("sketch") or k == "BOOT_IMAGE" for k in d["env"]),
                "内核塞给 init 的杂项环境变量没有传给服务", d["env"])

        o = vm.run_serial("sktest parentinfo")
        d = info_dict(o)
        R.check(d.get("sid") == d.get("pid") and d.get("tty_nr") == str(TTYS0_NR) and d.get("ppid") == "1",
                "串口 shell 是会话首领，控制终端 ttyS0", o)
        # 检查一个不在 fork 中途的服务（shell 此刻正在 fork，musl 会临时屏蔽所有信号，不能拿它判断）
        o = vm.run_serial("sktest ids sleeper")
        d = info_dict(o)
        R.check(d.get("sigblk") == "0000000000000000" and d.get("sigign") == "0000000000000000"
                and d.get("tty_nr") == "0" and d.get("sid") == d.get("pid"),
                "没有终端的服务 (-)：信号设置干净、没有控制终端、自成一个会话", o)

        o = vm.run_serial("sktest cat /proc/1/status")
        R.check(re.search(r"^Name:\s+sinit", o, re.M), "1 号进程的名字是 sinit", o[:200])
        o = vm.run_serial("sktest readlink /proc/1/fd/0")
        R.check("/dev/null" in o, "sinit 自己的 0/1/2 指向 /dev/null", o)

        # ---- 崩溃太频繁的服务：重启 5 次后放弃 ----
        try:
            vm.wait_serial(r"service crashy exited \(status 3\), giving up", 20)
            gave_up = True
        except Exception:
            gave_up = False
        out = vm.serial_output()
        n_resp = len(re.findall(r"sinit: service crashy exited \(status 3\), respawning", out))
        R.check(gave_up, "崩溃的服务最终被放弃 (giving up)")
        R.check(n_resp == 5, "放弃前重启了 5 次 (respawning ×5)", "实际 %d 次" % n_resp)
        header, st = status(vm)
        R.check(st.get("crashy", {}).get("state") == "failed" and st["crashy"]["restarts"] == 5,
                "crashy 状态 failed，restarts=5", st.get("crashy"))

        # ---- delay：3 秒后 late 启动 ----
        st2 = wait_until(lambda: (lambda s: s if s.get("late", {}).get("state") == "running" else None)(status(vm)[1]), 8)
        R.check(st2 and st2["late"]["pid"] > 0, "delay=3000 的服务到时间后启动", st2 and st2.get("late"))

        # ---- 杀掉服务，sinit 重启它 ----
        old = st["sleeper"]["pid"]
        mark = len(vm.serial_output())
        vm.run_serial("sktest kill TERM sleeper")
        try:
            vm.wait_serial(r"sinit: service sleeper exited \(status 143\), respawning", 10, start=mark)
            ok = True
        except Exception:
            ok = False
        R.check(ok, "杀掉 sleeper 后打印 'service sleeper exited (status 143), respawning'")
        time.sleep(0.3)
        _, st = status(vm)
        R.check(st["sleeper"]["state"] == "running" and st["sleeper"]["pid"] not in (0, old)
                and st["sleeper"]["restarts"] == 1, "sleeper 以新的 pid 重新运行，restarts=1", st["sleeper"])

        # 串口 shell 自己被杀掉也会重启
        spid = st["serial"]["pid"]
        mark = len(vm.serial_output())
        vm.serial_write("sktest kill KILL %d\n" % spid)
        vm.wait_serial(r"service serial exited \(status 137\), respawning", 10, start=mark)
        vm.wait_serial(PROMPT, 10, start=mark)
        time.sleep(0.3)
        _, st = status(vm)
        R.check(st["serial"]["pid"] != spid and st["serial"]["state"] == "running", "串口 shell 被杀后自动重启", st["serial"])

        # ---- 孤儿进程：双重 fork 后被过继给 1 号进程，退出后必须被回收 ----
        o = vm.run_serial("sktest orphans 5 1", timeout=20)
        orphans = re.findall(r"orphan (\d+) ppid=(\d+)", o)
        R.check(len(orphans) == 5 and all(p == "1" for _, p in orphans), "5 个孙进程都被过继给 1 号进程", o)
        time.sleep(2.5)
        o = vm.run_serial("sktest zombies")
        R.check("zombies=0" in o, "孤儿退出后没有僵尸进程残留", o)
        gone = all("=no" in vm.run_serial("sktest exists " + pid) for pid, _ in orphans)
        R.check(gone, "孤儿进程都已经被彻底回收（/proc 里不存在）")

        # ---- SIGHUP：重读服务表，只启动新增的服务 ----
        before = st
        vm.run_serial("sktest append /etc/sketch/services.conf 'added respawn - /bin/sktest sleep 100000'")
        vm.run_serial("sktest append /etc/sketch/services.conf 'sleeper once - /bin/sktest true'")
        mark = len(vm.serial_output())
        vm.run_serial("sktest kill HUP 1")
        try:
            vm.wait_serial(r"重新读取了服务表：新增 1 个服务", 10, start=mark)
            ok = True
        except Exception:
            ok = False
        R.check(ok, "SIGHUP 后报告新增 1 个服务")
        time.sleep(0.5)
        _, st = status(vm)
        R.check(st.get("added", {}).get("state") == "running", "新增的服务已经启动", st.get("added"))
        R.check(st["sleeper"]["pid"] == before["sleeper"]["pid"] and st["serial"]["pid"] == before["serial"]["pid"],
                "已有的服务没有受影响（同名的新行被忽略）", (before.get("sleeper"), st.get("sleeper")))
        R.check(st.get("crashy", {}).get("state") == "failed", "已放弃的服务保持 failed")

        # ---- 关机：SIGUSR2 ----
        mark = len(vm.serial_output())
        vm.serial_write("sktest kill USR2 1\n")
        exited = wait_exit(vm, 30)
        out = vm.serial_output()[mark:]
        R.check(exited, "SIGUSR2 之后 QEMU 退出（ACPI 关机）", out[-1500:])
        R.check(re.search(r"sinit: 收到 SIGUSR2（来自 pid \d+( sktest|，它已经退出了)）", out),
                "关机提示里说明了信号来源", out[-1500:])
        R.check("正在关机" in out and "通知所有进程退出 (SIGTERM)" in out, "关机流程：先发 SIGTERM", out[-1500:])
        R.check(re.search(r"还有 \d+ 个进程没有退出，强制结束 \(SIGKILL\)", out), "顽固进程被 SIGKILL 强制结束", out[-1500:])
        R.check("再见！正在关闭电源" in out, "关机前的告别语", out[-1500:])
        R.check("respawning" not in out, "关机过程中不再重启任何服务", out[-1500:])
        R.check("Kernel panic" not in vm.serial_output(), "全程没有内核 panic")
    finally:
        vm.stop()


def sc_nogui(ctx):
    """sketch.nogui + sketch.tz：cond 条件和时区参数；SIGTERM 重启（-no-reboot 时 QEMU 退出）"""
    s = Scenario(ctx, "nogui")
    s.write("/etc/sketch/services.conf", TEST_CONF)
    vm = s.boot(append="sketch.nogui sketch.tz=JST-9")
    try:
        boot_to_prompt(vm)
        _, st = status(vm)
        R.check(st.get("gui", {}).get("state") == "disabled", "nogui 模式下 cond=gui 的服务是 disabled", st.get("gui"))
        R.check(st.get("nogui", {}).get("state") == "running", "nogui 模式下 cond=nogui 的服务在运行", st.get("nogui"))
        o = vm.run_serial("sktest cat /var/log/nogui.log")
        d = info_dict(o)
        R.check(d["env"].get("TZ") == "JST-9", "sketch.tz=JST-9 覆盖默认时区", d["env"])
        R.check(d.get("tty_nr") == str((4 << 8) | 2), "nogui 服务的控制终端是 /dev/tty2", o)
        R.check("（时区 JST-9）" in vm.serial_output(), "横幅显示命令行指定的时区")

        mark = len(vm.serial_output())
        vm.serial_write("sktest kill TERM 1\n")
        exited = wait_exit(vm, 30)
        out = vm.serial_output()[mark:]
        R.check(exited, "SIGTERM 之后系统重启（-no-reboot 让 QEMU 退出）", out[-1500:])
        R.check("正在重启" in out and "再见！正在重启" in out, "重启提示", out[-1500:])
        R.check(re.search(r"reboot: Restarting system", out), "内核执行了重启", out[-1500:])
    finally:
        vm.stop()


def sc_halt(ctx):
    """sinit halt（SIGUSR1）停机：提示可以关窗口，内核停机，但 QEMU 不退出"""
    s = Scenario(ctx, "halt")
    s.write("/etc/sketch/services.conf", "serial respawn /dev/ttyS0 /bin/sksh -l\n")
    vm = s.boot()
    try:
        boot_to_prompt(vm)
        o = vm.run_serial("sinit --version")
        R.check(re.search(r"sinit \(SketchOS\) 0\.1", o), "系统里可以运行 sinit --version", o)
        mark = len(vm.serial_output())
        vm.serial_write("sinit halt\n")
        try:
            vm.wait_serial(r"System halted", 20, start=mark)
            halted = True
        except Exception:
            halted = False
        out = vm.serial_output()[mark:]
        R.check("请求 1 号进程停机（发送 SIGUSR1）" in out and "收到 SIGUSR1（来自 pid" in out,
                "sinit halt 命令给 1 号进程发了 SIGUSR1", out[-1500:])
        R.check(halted, "内核打印 System halted", out[-1500:])
        R.check("正在停机" in out and "系统已停止，可以关闭窗口了" in out, "停机提示", out[-1500:])
        time.sleep(2)
        R.check(vm.proc.poll() is None, "停机后 QEMU 仍在运行（停机不是断电）")
        R.check("Kernel panic" not in vm.serial_output(), "没有内核 panic")
    finally:
        vm.stop()


def sc_noacpi(ctx):
    """没有 ACPI（像网页版的 v86）：SIGPWR/关机请求不能断电，sinit 提示可以关窗口并继续待命"""
    s = Scenario(ctx, "noacpi")
    s.write("/etc/sketch/services.conf", "serial respawn /dev/ttyS0 /bin/sksh -l\n")
    vm = s.boot(append="acpi=off")
    try:
        boot_to_prompt(vm)
        o = vm.run_serial("sktest mode /sys/firmware/acpi")
        R.check("No such file" in o, "（确认 acpi=off 时没有 /sys/firmware/acpi）", o)
        mark = len(vm.serial_output())
        vm.serial_write("sktest kill PWR 1\n")
        try:
            vm.wait_serial(r"系统已停止，可以关闭窗口了", 20, start=mark)
            ok = True
        except Exception:
            ok = False
        out = vm.serial_output()[mark:]
        R.check(ok, "没有 ACPI 时提示“系统已停止，可以关闭窗口了”", out[-1500:])
        R.check("收到 SIGPWR" in out and "正在关机" in out, "SIGPWR 也当作关机请求", out[-1500:])
        R.check("没有 ACPI" in out, "说明了为什么不能断电", out[-1500:])
        time.sleep(3)
        out = vm.serial_output()
        R.check(vm.proc.poll() is None and "Kernel panic" not in out and "Attempted to kill init" not in out,
                "sinit 停在原地待命，内核没有 panic")
    finally:
        vm.stop()


def sc_fallback(ctx):
    """没有 services.conf：在 /dev/console 上启动后备 shell"""
    s = Scenario(ctx, "fallback")
    s.remove("/etc/sketch/services.conf")
    vm = s.boot()
    try:
        boot_to_prompt(vm)
        out = vm.serial_output()
        R.check(re.search(r"打不开 /etc/sketch/services.conf .*启动后备 shell", out), "提示没有服务表、启动后备 shell")
        o = vm.run_serial("echo hello-fallback")
        R.check("hello-fallback" in o, "后备 shell 可以执行命令", o)
        _, st = status(vm)
        R.check(st.get("fallback", {}).get("state") == "running" and "/dev/console" not in st["fallback"]["cmd"]
                and st["fallback"]["cmd"] == "/bin/sksh -l", "状态文件里有 fallback 服务", st)
        o = vm.run_serial("sktest parentinfo")
        d = info_dict(o)
        R.check(d.get("tty_nr") == str(TTYS0_NR) and d.get("sid") == d.get("pid"),
                "后备 shell 通过 /dev/console 拿到了控制终端 ttyS0", o)
        mark = len(vm.serial_output())
        vm.serial_write("sktest kill TERM 1\n")
        R.check(wait_exit(vm, 30), "后备模式下也能正常重启", vm.serial_output()[mark:][-800:])
    finally:
        vm.stop()


def sc_nothing(ctx):
    """没有服务表，也没有任何 shell：报错后继续空转回收进程；Ctrl+Alt+Del 仍然能重启"""
    s = Scenario(ctx, "nothing")
    for p in ("/etc/sketch/services.conf", "/bin/sksh", "/bin/sh"):
        s.remove(p)
    vm = s.boot()
    try:
        vm.wait_serial(r"sinit: SketchOS [0-9.]+ starting", 60)
        vm.wait_serial(r"没有可以运行的程序", 30)
        time.sleep(3)
        out = vm.serial_output()
        R.check("找不到可以运行的 shell" in out, "报告找不到 shell")
        R.check(vm.proc.poll() is None and "Kernel panic" not in out, "sinit 没有退出，内核没有 panic")
        # Ctrl+Alt+Del：sinit 用 reboot(RB_DISABLE_CAD) 把它变成发给 1 号进程的 SIGINT
        mark = len(out)
        vm.key("ctrl-alt-delete")
        exited = wait_exit(vm, 30)
        out = vm.serial_output()[mark:]
        R.check(exited, "按 Ctrl+Alt+Del 后系统重启", out[-1500:])
        R.check("收到 SIGINT（来自内核：有人按了 Ctrl+Alt+Del）" in out, "Ctrl+Alt+Del 变成了给 sinit 的 SIGINT", out[-1500:])
        R.check("所有进程都已退出" in out, "重启前的关机流程照常执行", out[-1500:])
    finally:
        vm.stop()


def sc_noconsole(ctx):
    """1 号进程启动时 0/1/2 都没打开、根目录里也没有 /dev/console 和 /dev/null：sinit 要自己补上"""
    # 内核总会先解压一个内置的小 initramfs（里面有 /dev/console），所以光是打包时不放设备节点，
    # 内核仍能打开初始控制台。这里用一个小程序当真正的第一个 1 号进程：它关掉 0/1/2、
    # 删掉 /dev/console 和 /dev/null，再 execve 成 sinit（pid 仍然是 1）。
    s = Scenario(ctx, "noconsole")
    s.write("/etc/sketch/services.conf", "serial respawn /dev/ttyS0 /bin/sksh -l\n")
    os.symlink("sktest", os.path.join(s.stage, "bin", "closefds-init"))
    vm = s.boot(append="rdinit=/bin/closefds-init", console_nodes=False)
    try:
        boot_to_prompt(vm)
        for fd in (0, 1, 2):
            o = vm.run_serial("sktest readlink /proc/1/fd/%d" % fd)
            R.check("-> /dev/null" in o, "sinit 的 %d 号描述符被补成了 /dev/null" % fd, o)
        o = vm.run_serial("sktest cat /proc/1/cmdline")
        R.check("sinit" in o, "1 号进程确实是 execve 过来的 sinit", o)
        o = vm.run_serial("echo still-works")
        R.check("still-works" in o, "串口 shell 正常工作", o)
        vm.serial_write("sktest kill USR2 1\n")
        R.check(wait_exit(vm, 30), "可以正常关机")
    finally:
        vm.stop()


def sc_real(ctx):
    """用正式的根文件系统（rootfs/ 原样）和真正的 sksh/skbox 启动（需要 --real-shell）"""
    if not ctx.real_shell:
        print("  （跳过：需要 --real-shell）")
        return
    if not os.path.exists(os.path.join(ctx.base, "bin", "sksh")):
        R.check(False, "real 场景需要真正的 sksh，但它没有编译成功")
        return
    s = Scenario(ctx, "real", real_shell=True)
    vm = s.boot(append="sketch.nogui")
    try:
        vm.wait_serial(r"sinit: services started", 90)
        vm.wait_serial(r"# ", 30)
        time.sleep(1)
        out = vm.serial_output()
        R.check("欢迎来到 SketchOS" in out, "登录 shell 显示了 /etc/motd", out[-2000:])
        o = vm.run_serial("cat /run/sinit/status")
        _, st = parse_status(o)
        R.check(st.get("serial", {}).get("state") == "running", "serial 服务在运行", o)
        R.check(st.get("console", {}).get("state") == "running", "console 服务在 tty1 上运行", o)
        R.check(st.get("desktop", {}).get("state") == "disabled", "nogui 时 desktop 是 disabled", o)
        o = vm.run_serial("sh /root/示例/1-hello.sh", timeout=30)
        R.check("你好，SketchOS 的新朋友！" in o, "示例脚本 1-hello.sh 可以运行", o)
        for script in ("2-pipes.sh", "3-if.sh", "4-loops.sh", "5-sysinfo.sh"):
            o = vm.run_serial("sh /root/示例/%s" % script, timeout=30)
            R.check("not found" not in o and "语法错误" not in o and "syntax" not in o.lower(),
                    "示例脚本 %s 运行无语法错误" % script, o[-800:])
        vm.serial_write("sinit poweroff\n")
        R.check(wait_exit(vm, 30), "sinit poweroff 关机")
    finally:
        vm.stop()


SCENARIOS = [
    ("cli", sc_cli), ("unit", sc_unit), ("main", sc_main), ("nogui", sc_nogui), ("halt", sc_halt), ("noacpi", sc_noacpi),
    ("fallback", sc_fallback), ("nothing", sc_nothing), ("noconsole", sc_noconsole), ("real", sc_real),
]


class Ctx:
    pass


def main():
    ap = argparse.ArgumentParser(description="sinit 开机测试")
    ap.add_argument("-k", "--only", help="只跑这些场景（逗号分隔）")
    ap.add_argument("--list", action="store_true", help="列出场景")
    ap.add_argument("--work", default=os.environ.get("BOOTTEST_DIR", os.path.join(WORK, "boottest")),
                    help="工作目录")
    ap.add_argument("--real-shell", action="store_true", help="编译并使用真正的 sksh/skbox（real 场景）")
    args = ap.parse_args()

    if args.list:
        for name, fn in SCENARIOS:
            print("  %-10s %s" % (name, (fn.__doc__ or "").strip().split("\n")[0]))
        return 0
    only = set(args.only.split(",")) if args.only else None
    for need in (SKCC, GEN_CPIO, KERNEL):
        if not os.path.exists(need):
            print("缺少 %s：请先运行 scripts/toolchain.sh 和 scripts/kernel.sh" % need)
            return 2

    ctx = Ctx()
    ctx.tdir = os.path.abspath(args.work)
    ctx.real_shell = args.real_shell
    os.makedirs(ctx.tdir, exist_ok=True)
    targets = ["sinit"] + (["sksh", "skbox"] if args.real_shell else [])
    ctx.ub = build_userspace(ctx.tdir, targets)
    ctx.helpers = build_helpers(ctx.tdir)
    ctx.base = make_base_stage(ctx.tdir, ctx.ub, ctx.helpers)

    for name, fn in SCENARIOS:
        if only and name not in only:
            continue
        print("\n=== 场景 %s：%s ===" % (name, (fn.__doc__ or "").strip().split("\n")[0]), flush=True)
        R.current = name
        t0 = time.time()
        try:
            fn(ctx)
        except Exception as e:
            R.check(False, "场景 %s 执行出错: %s" % (name, e), traceback.format_exc())
        print("  （用时 %.1f 秒）" % (time.time() - t0))
    return 0 if R.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
