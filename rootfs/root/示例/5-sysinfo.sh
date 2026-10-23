#!/bin/sksh
# 5-sysinfo.sh — 把前面学的东西组合起来：做一个小小的“系统信息”报告
#
# 所有信息都来自 /proc 和 /run：内核把自己的状态做成了“文件”，读文件就是在问内核问题。
# 运行完以后，报告也会保存一份在 /tmp/系统报告.txt。

report=/tmp/系统报告.txt
echo "SketchOS 系统报告  $(date)" > $report

echo "== 内核 ==" >> $report
cat /proc/version >> $report

echo "== CPU ==" >> $report
grep "model name" /proc/cpuinfo >> $report

echo "== 内存（前 3 行） ==" >> $report
head -n 3 /proc/meminfo >> $report

echo "== 开机多久了（秒） ==" >> $report
cat /proc/uptime >> $report

echo "== sinit 照看的服务 ==" >> $report
if [ -f /run/sinit/status ]; then
    cat /run/sinit/status >> $report
else
    echo "（没有找到 /run/sinit/status）" >> $report
fi

echo "== 挂载的文件系统 ==" >> $report
cat /proc/mounts >> $report

cat $report
echo ""
echo "报告已保存到 $report，一共 $(wc -l < $report) 行"
