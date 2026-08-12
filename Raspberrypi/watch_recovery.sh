#!/bin/bash
# watch_recovery.sh — 自然断线恢复验证脚本
#
# 验收标准（用户定义）：自然断线后，网关必须自己重新扫描上并恢复数据。
# 用法：
#   终端1: sudo ./isk_gateway            （或已在跑）
#   终端2: sudo bash ./watch_recovery.sh （可任意目录，自动找日志）
# 也可显式指定日志路径：bash ./watch_recovery.sh /path/to/isk_gateway.log
set -u

# ---- 日志定位：脚本目录优先，找不到就在常见位置里找 ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="${1:-}"
if [ -z "$LOG" ]; then
    for c in \
        "$SCRIPT_DIR/isk_gateway.log" \
        "$PWD/isk_gateway.log" \
        "$HOME/isk_gateway.log" \
        "/root/isk_gateway.log" \
        ; do
        if [ -f "$c" ]; then
            LOG="$c"
            break
        fi
    done
fi
if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
    echo "找不到 isk_gateway.log —— 先确认网关在跑（sudo ./isk_gateway）"
    echo "或把日志路径作为参数传进来："
    echo "  sudo bash ./watch_recovery.sh /path/to/isk_gateway.log"
    exit 1
fi
echo "监视日志：$LOG"
echo "（正常收数据时每 10s 报一次心跳；Ctrl+C 退出不影响网关）"

# 恢复判定窗口：恢复链（Stop→HCI→bluetoothd）最坏 ~30s；超过即判控制器卡死，不空等运气
WINDOW_S=60

# ---- 主循环：每 2s 查一次看门狗日志，每 10s 报一次心跳 ----
HB=0
while true; do
    if grep -q "数据 .*ms 无更新" "$LOG"; then
        WARN_LINE=$(grep -n "数据 .*ms 无更新" "$LOG" | tail -1 | cut -d: -f1)
        echo ""
        echo "✓ 看门狗已触发（日志第 ${WARN_LINE} 行）："
        sed -n "${WARN_LINE}p" "$LOG"

        echo ""
        echo "等数据恢复（最多 ${WINDOW_S}s，超时即判控制器卡死）..."
        for i in $(seq 1 $((WINDOW_S / 5))); do
            if tail -n +"$WARN_LINE" "$LOG" | grep -q "MQTT 已发布"; then
                echo ""
                echo "✅ 恢复成功：看门狗之后重新收到数据"
                tail -n +"$WARN_LINE" "$LOG" | grep "MQTT 已发布" | tail -2
                echo "看门狗累计触发次数：$(grep -c '数据 .*ms 无更新' "$LOG")"
                exit 0
            fi
            if [ $((i % 3)) -eq 0 ]; then   # 每 15s 报一次恢复进度
                CNT=$(grep -c "数据 .*ms 无更新" "$LOG")
                REC=$(tail -n +"$WARN_LINE" "$LOG" | grep -E "HCI|bluetoothd|冷却|StartDiscovery|重新发现|缓存为空" | tail -1 | cut -c1-100)
                echo "[$(date +%H:%M:%S)] 仍在等待：看门狗共触发 ${CNT} 次，最近恢复动作: ${REC:-（无）}"
            fi
            sleep 5
        done

        echo ""
        echo "❌ ${WINDOW_S}s 内未恢复 —— 看门狗触发了但没扫回来。恢复过程日志："
        tail -n +"$WARN_LINE" "$LOG" | grep -E "无更新|HCI|bluetoothd|冷却|StartDiscovery|重新发现|缓存为空|代理" | tail -20
        exit 1
    fi

    if [ $((HB % 5)) -eq 0 ]; then   # 每 10s 报一次心跳
        LAST=$(grep "MQTT 已发布" "$LOG" | tail -1 | cut -c1-19)
        if [ -n "$LAST" ]; then
            echo "[$(date +%H:%M:%S)] 数据仍在更新（最后发布: $LAST）"
        else
            echo "[$(date +%H:%M:%S)] 日志里还没有 MQTT 发布记录"
        fi
    fi
    HB=$((HB + 1))
    sleep 2
done