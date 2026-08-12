#!/bin/bash
# watch_recovery.sh — 自然断线恢复验证脚本
#
# 验收标准（用户定义）：自然断线后，网关必须自己重新扫描上并恢复数据。
# 用法：
#   终端1: sudo ./isk_gateway            （或已在跑）
#   终端2: sudo bash ./watch_recovery.sh （同目录，盯 isk_gateway.log）
# 脚本会一直挂着，等看门狗触发 → 看数据是否恢复 → 给结论。
set -u
LOG=isk_gateway.log
[ -f "$LOG" ] || { echo "没有 $LOG —— 先启动网关（sudo ./isk_gateway）"; exit 1; }

echo "观察中：等待看门狗触发（自然卡死约 15s 后应有「数据 15000ms 无更新」）"
echo "（Ctrl+C 可退出，不影响网关）"
while ! grep -q "数据 .*ms 无更新" "$LOG"; do
    sleep 2
done

WARN_LINE=$(grep -n "数据 .*ms 无更新" "$LOG" | tail -1 | cut -d: -f1)
echo ""
echo "✓ 看门狗已触发（日志第 ${WARN_LINE} 行）："
sed -n "${WARN_LINE}p" "$LOG"

echo ""
echo "等数据恢复（最多 60s）..."
for i in $(seq 1 30); do
    if tail -n +"$WARN_LINE" "$LOG" | grep -q "MQTT 已发布"; then
        echo ""
        echo "✅ 恢复成功：看门狗之后重新收到数据"
        tail -n +"$WARN_LINE" "$LOG" | grep "MQTT 已发布" | tail -2
        echo "看门狗累计触发次数：$(grep -c '数据 .*ms 无更新' "$LOG")"
        exit 0
    fi
    sleep 2
done

echo ""
echo "❌ 60s 内未恢复 —— 看门狗触发了但没扫回来，日志尾部："
tail -15 "$LOG"
exit 1