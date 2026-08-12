#!/bin/bash
# reproduce_watchdog.sh — 稳定复现"看门狗触发但恢复无效、重复旧数据"问题
#
# 原理：用可控方式让 ESP32 广播停止（等价控制器失联/卡死），
# 观察看门狗是否触发、恢复后数据是否真正恢复。不需要等 30 分钟。
#
# 用法：在 Pi 上 sudo bash ./reproduce_watchdog.sh
# 提示拔/插 ESP32 电源时照做。

set -u
LOG=isk_gateway.log
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"   # 以脚本所在目录为准，避免 sudo 下 ~ 解析成 /root

STOP_S=25      # 广播停止观察时长
RECOVER_S=25   # 恢复观察时长

# 退出时杀掉网关，避免留孤儿进程
cleanup() { sudo pkill -9 isk_gateway 2>/dev/null; }
trap cleanup EXIT

echo "=== [1/5] 清环境 ==="
sudo pkill -9 isk_gateway 2>/dev/null
rm -f "$LOG"      # 清空日志，保证判断准确
sleep 2

echo "=== [2/5] 启动网关（后台），等待数据（最多 20s） ==="
sudo ./isk_gateway > /dev/null 2>&1 &
sleep 3
for _ in $(seq 1 17); do
    if grep -q "MQTT 已发布" "$LOG"; then break; fi
    sleep 1
done
if ! grep -q "MQTT 已发布" "$LOG"; then
    echo "!! 20s 内无数据，网关日志（诊断用）："
    tail -20 "$LOG"
    exit 1
fi
echo "OK：数据在流（已发布 $(grep -c 'MQTT 已发布' "$LOG") 条）"

echo ""
echo "=== [3/5] 现在【拔掉 ESP32/STM32 电源】让广播停止 ==="
echo "      按回车继续（等待 $STOP_S 秒让看门狗触发）"
read -r _
sleep "$STOP_S"

echo "--- 看门狗是否触发：---"
grep -E "数据 .*无更新|重新发现|重启扫描" "$LOG" | tail -5
echo "看门狗触发次数（本次应 ≥1）：$(grep -c '数据 .*无更新' "$LOG")"

echo ""
echo "=== [4/5] 现在【重新接上 ESP32/STM32 电源】 ==="
echo "      按回车继续（等待 $RECOVER_S 秒观察是否恢复）"
read -r _
sleep "$RECOVER_S"

echo "--- 恢复后是否收到新数据：---"
tail -30 "$LOG" | grep -E "MQTT 已发布|数据 .*无更新|重新发现|重启扫描" | tail -10

echo ""
echo "=== [5/5] 判定 ==="
NEW_PUB=$(tail -10 "$LOG" | grep -c "MQTT 已发布")
LAST=$(tail -1 "$LOG")
echo "最后一条日志：$LAST"
if [ "$NEW_PUB" -ge 1 ]; then
    echo "✅ 恢复后收到了新数据 → 恢复逻辑正常"
else
    echo "❌ 恢复后没有新数据（可能仍卡死循环）→ 复现 bug，看上面的日志判断卡在哪"
fi