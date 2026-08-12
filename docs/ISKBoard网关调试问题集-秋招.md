# ISKBoard 网关调试问题集（秋招准备）

> 一个真实的端到端 IoT 系统调试案例：STM32 采集 → ESP32 BLE 广播 → 树莓派 3 网关（BlueZ 扫描）→ MQTT 云。
> 本文把一次完整的调试经历整理成可讲的 case，覆盖**现象 → 排查 → 根因 → 修复 → 验证**全流程，
> 并提炼出面试可讲的**方法论**和**技术知识点**。

---

## 0. 项目一句话背景

穿戴式跌倒检测系统，端到端链路：

```
MPU6050(50Hz) ─▶ STM32L431 ─▶ ESP32(AT固件, BLE广播) ─▶ 树莓派3(BlueZ被动扫描)
                                                          │
                                                  MQTT 云 (fall_detection/pi01/*)
```

- 上行：STM32 每 500ms 发一帧实时数据 → ESP32 写入 BLE 广播包 ManufacturerData → Pi 扫描解析 → MQTT
- 下行：云 → MQTT cmd → Pi → GATT 写 ESP32 特征值 0xC302 → STM32 执行指令（如亮 LED）

---

## 1. 问题一：网关运行 30 分钟后数据静默停止（最经典的"隐性故障"）

### 1.1 现象

- 网关正常跑 30 分钟，MQTT 持续有数据
- 30 分钟节点数据**永久停流**，没有任何报错日志
- 重启网关又能好一阵，然后又停

### 1.2 排查过程（顺序很重要）

| 步骤 | 动作 | 结论 |
|------|------|------|
| 1 | 看日志 | 没有任何 ERROR，日志像是"正常停止了" |
| 2 | `pgrep -af isk_gateway` | 看到 3 个 PID（1 真 + 2 个残留 sudo 壳），**排查后确认不是根因** |
| 3 | 手动 `sudo hcitool lescan` | 能扫到 ESP32 → **无线电链路是好的** |
| 4 | `bluetoothctl show` | `Powered: yes` 但 `PowerState: off` → **BlueZ/控制器状态错乱** |
| 5 | 读代码 `poll_manufacturer_data` | **找到根因** |

### 1.3 根因：健康检查是"内容存在"型，不是"数据新鲜"型

轮询逻辑（精简）：

```c
props = g_dbus_proxy_get_cached_property(dev_proxy, "ManufacturerData");
if (props) {
    fail_cnt = 0;          // ← 只要缓存里有数据就认为"正常"，清零失败计数
    process_mfg_data(props);
} else {
    if (++fail_cnt >= 8)   // ← 只有缓存为空才累计失败
        self_heal();
}
```

**关键洞察**：`poll` 读的是 **BlueZ 缓存的** ManufacturerData，不是实时的。

当 Pi 控制器卡死、新广播收不到时：
- BlueZ 的 Device1 缓存里**还留着最后一帧旧数据**
- 于是 `props` 永远非空 → `fail_cnt` 每次都被清零 → **自愈永不触发**
- 结果：数据早停了，但 poll 以为设备正常，**日志一片祥和地死着**

**这类 bug 的通用形态**：用"有没有缓存数据"判断死活，无法区分"设备真的活着但数据相同"和"设备已死但缓存残留"。

### 1.4 修复方案（滚动序号 + 新鲜度看门狗）

核心难点：设备静止时帧内容相同，Pi 有"静止去重"，**靠"内容变了没"判断不了死活**。
解法：让**每次广播在字节上可区分** —— STM32 在广播包末尾追加 1 字节滚动序号。

- **STM32**（`ESP32_Send`）：每帧 `ad[ad_len++] = s_adv_seq++;`
- **Pi**（`process_mfg_data`）：剥离末尾序号 → 序号变化 → 更新存活时间 `g_last_seq_ms`；去重仍按帧内容（不刷 MQTT）
- **Pi**（`poll`）：`now - g_last_seq_ms > 15s` → 判定失联 → 触发自愈

```c
/* poll 里 */
if (now_ms - g_last_seq_ms > FRESH_WATCHDOG_MS) {
    if (++fail_cnt >= 8) {
        g_dev_path[0] = '\0';
        restart_discovery(g_dev_conn);   // 真正重启扫描
    }
}
```

设计要点：
- **广播预算**：31B 上限，加 1B 序号后实时帧 14B→总 28B，仍安全
- **序号 ≠ 去重**：去重按帧内容，序号只当"存活信号"，静止时不刷 MQTT
- **看门狗阈值**：15s = 30 次漏收（2Hz），确凿卡死，不会误触发

### 1.5 面试讲法

> 我遇到一个"数据 30 分钟后就静默停流"的 bug。排查发现是**健康检查只看缓存里有没有数据，没看数据是否新鲜**。控制器卡死后缓存残留最后一帧，导致自愈永不触发。我加了"滚动序号 + 15 秒新鲜度看门狗"解决，既保留静止去重，又能确凿检测失联。

**加分点**：能主动讲出"这类隐性故障的特征是日志祥和但数据停流，健康检查必须看新鲜度而不是存在性"。

---

## 2. 问题二：看门狗触发但恢复无效（重复播报最后一条数据）

### 2.1 现象

数据停流 → 看门狗触发 → **每 4 秒重复发布同一条旧数据**（accel_sq 完全一样）→ 插回电源才恢复。

### 2.2 根因：恢复逻辑只在缓存里打转，从不重启扫描

```c
if (++fail_cnt >= 8) {
    g_dev_path[0] = '\0';
    if (!check_existing_devices(g_dev_conn))   // ← 只在 BlueZ 缓存里重找设备
        restart_discovery(g_dev_conn);
}
```

`check_existing_devices` 找到的是**陈旧的缓存对象**（控制器卡死，新广播到不了）：
- 重看它 → 读到冻结帧 + 冻结序号
- 冻结序号 → `g_last_seq_ms` 不更新 → 15s 后看门狗又触发 → **死循环刷旧数据**
- 从没真正 `restart_discovery`（重启扫描）

### 2.3 修复：看门狗触发 → 直接重启扫描

```c
LOG_WARN("[POLL] 数据 %dms 无更新，重启扫描");
restart_discovery(g_dev_conn);   // 直接升级链，不再先查缓存
```

### 2.4 最小复现测试（写测试而非盲改）

这一步很重要 —— 用户要求先复现确认根因再改。写了个 `reproduce_watchdog.sh`：**拔 ESP32 电源模拟广播停止 → 观察看门狗 → 插回观察恢复**，不需要等 30 分钟。

```
Buggy 版本：看门狗触发 → 每 4s 重复旧帧 392188944（卡死循环）
修复版本：看门狗触发 → 重启扫描 → 静默等待 → 数据恢复（值变化）
```

**方法论**：硬件相关的偶发 bug，用可控方式（拔电源）把"失联"变成可重复的测试输入，才能稳定复现、验证修复。

---

## 3. 问题三：下行 GATT 连不上 ESP32

### 3.1 现象

- 云下发 cmd → Pi 收到 → Connect ESP32 失败
- 错误演进：`le-connection-abort-by-local` → `Timeout` → `Operation already in progress` → `NotReady: Resource Not Ready` → `Network is down`

### 3.2 关键定位过程

| 测试 | 结果 | 结论 |
|------|------|------|
| 手机连 ESP32 | ✅ 能连 | ESP32 可连接，广播参数正确（`AT+BLEADVPARAM=160,160,0,0,7` = 100ms 可连接、全信道） |
| `bluetoothctl connect` | ❌ `le-connection-abort-by-local` | Pi 本地中止 |
| 停止扫描后再连 | ❌ 一样 | 不是扫描冲突 |
| `hcitool lecc`（原始 HCI） | ❌ Timeout | **是无线电/控制器层问题，与软件无关** |

### 3.3 根因与软件缓解

**根因**：Pi 3 内置 BCM43438 蓝牙射频弱（比手机差 ~20dB），对 ESP32 的窄连接窗口成功率低。这是**硬件限制**，纯软件无法根治。

**软件缓解**（能做多少做多少）：
1. **GATT 写入前暂停扫描**：Broadcom 控制器扫描 + 连接不可并存，先 `StopDiscovery`
2. **Connect 多次重试**：4×10s + 1s 间隔，摊开连接窗口
3. **Connect 成功判断**：查 `Connected` 属性，失败直接放弃，不再白等 ServicesResolved
4. **扫描暂停标志** `g_scan_suspended`：下行期间 poll 跳过自愈，防止两个模块打架

**面试价值**：能讲清"硬件限制 vs 软件可解"的边界判断 —— 哪些问题值得花代码解决，哪些只能缓解。

---

## 4. 问题四：Pi 3 控制器本身不稳定（硬件层面，贯穿全程）

### 4.1 症状演进（一个比一个重）

```
InProgress: Operation already in progress   （扫描卡死，控制器还活着）
Busy: Failed                                 （拒绝 D-Bus 下电）
NotReady: Resource Not Ready                 （控制器没就绪）
Network is down（ENETDOWN）                  （内核层面控制器 DOWN）
Set scan parameters failed: Input/output error（连原始扫描都失败）
```

### 4.2 恢复手段对比（实测）

| 手段 | 方式 | 实测效果 |
|------|------|---------|
| D-Bus `Powered off/on` | `Set` Adapter1.Powered | ❌ 从不成功（先签名 bug，后 Busy） |
| `hciconfig hci0 down/up` | 内核 HCI 层 | ✅ 有效但重 |
| `systemctl restart bluetooth` | 重启 daemon | ✅ 有效（最可靠） |
| `hcitool cmd 0x03 0x0003`（HCI_Reset） | 原始 HCI 命令 | ✅ 有效（轻量，但有时打不开设备） |
| `hcitool lescan` | 原始扫描 | ✅ 能救活（用户手动手段） |

### 4.3 设计成"升级链 + 冷却"

```
StartDiscovery → 失败 → StopDiscovery
  → hcitool HCI_Reset（轻，~3s）
  → systemctl restart bluetooth（重，~20s）
深度复位加 5 分钟冷却，防反复触发锤控制器崩溃
```

**面试讲法**：这是个"恢复策略"设计题 —— 从最轻到最重逐级升级、重手段加冷却限频，既保证恢复能力又不伤系统。

---

## 5. 调试中发现的一组 C/GDBus 内存管理 bug（面试加分点）

这组 bug 全是真实踩过的坑，面试常考：

### 5.1 GDBus `Set` 方法签名：`(ssv)` 不是 `((ssv))`

```c
// ❌ 错误：把 (ssv) 再包一层
params = g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(FALSE));
g_dbus_proxy_call_sync(props, "Set", g_variant_new_tuple(&params, 1), ...);
// → Method "Set" with signature "(ssv)" doesn't exist

// ✅ 正确：Properties.Set 参数签名是 ssv（三个参数），(ssv) 本身就是参数元组
g_dbus_proxy_call_sync(props, "Set", params, ...);
```

**知识点**：D-Bus 方法参数元组 —— 一个 dict 参数是 `(a{sv})`（要包一层），三个标量参数是 `(ssv)`（直接是元组）。

### 5.2 `g_error_free` 不置 NULL → 二次使用 &error → 段错误

```c
if (error) {
    g_error_free(error);      // 释放后 error 仍是悬垂指针（非 NULL）
}
...调用 g_dbus_proxy_call_sync(..., &error);   // GLib 断言 *error==NULL 失败 → 崩溃
// ✅ 用 g_clear_error(&error)（释放 + 置 NULL）
```

### 5.3 `g_dbus_proxy_call_sync` 会消费 parameters 参数

```c
g_dbus_proxy_call_sync(props, "Set", params, ...);
// 传完后不要 g_variant_unref(params)—— 已被消费，再 unref 是双重释放
// （曾出现 g_atomic_ref_count_dec: assertion 'old_value > 0' failed）
```

### 5.4 `system()` 的两个坑

```c
// 坑1：PATH 问题 —— hcitool 不在默认 PATH，要全路径
system("hcitool ...");        // command not found
system("/usr/bin/hcitool ...");  // ✅

// 坑2：timeout 杀 systemctl → 返回非零 → 误判"重启失败"
system("timeout 10 systemctl restart bluetooth");   // 超10s被kill → 124
// ✅ 不依赖 system() 返回码，执行后固定等待再无条件重试
```

### 5.5 进程残留（"3 个网关进程"其实是误判）

`pgrep` 显示 3 个 `isk_gateway`，排查后发现是：**1 个真网关 + 2 个残留的 `sudo` 壳**（SecureCRT 多 tab 启动，旧网关退出后 sudo 还挂着）。不是程序 fork 出的子进程。
用 PID 文件锁防重复启动（fcntl 写锁，进程退出内核自动释放）。

---

## 6. 方法论总结（面试怎么讲）

### 6.1 系统性调试流程（本 case 全程遵循）

```
1. 先复现（最小、可控、稳定）—— 拔 ESP32 电源制造"失联"，不等 30 分钟
2. 确认 bug 真实存在（有复现证据，不是猜）
3. 定位根因（读代码 + 日志 + 实测交叉验证）
4. 修复（一处一处改，不批量）
5. 验证（同一复现流程跑修复版，对比行为）
```

### 6.2 关键设计模式（能主动讲出原理）

| 模式 | 用途 | 本项目体现 |
|------|------|-----------|
| **新鲜度看门狗** | 检测"假活"（缓存残留） | 滚动序号 + 15s 阈值 |
| **滚动序号** | 不破坏去重的前提下做存活信号 | 广播末尾 1B 序号 |
| **升级链 + 冷却** | 恢复策略：轻→重逐级，重手段限频 | Stop→HCI→bluetoothd，5min 冷却 |
| **模块间互斥标志** | 两个子系统打架时协调 | `g_scan_suspended`（下行 vs 看门狗） |
| **最小复现** | 偶发 bug 转可控测试 | `reproduce_watchdog.sh` |

### 6.3 一句"面试自述"

> 我在一个穿戴设备网关项目里做了端到端调试，解决了一串问题：一个"数据 30 分钟静默停流"的隐性故障（缓存残留导致健康检查失效，用滚动序号+新鲜度看门狗解决）、一个"看门狗触发但恢复无效"的循环刷旧数据 bug（恢复逻辑从不重启扫描，改为直接重启）、以及一整套 Pi 3 蓝牙控制器的硬件不稳问题（设计成 Stop/HCI/bluetoothd 三级升级链 + 冷却）。调试中还踩了一组 GDBus 引用计数/参数消费的坑，都定位到了根因。

---

## 7. 项目技术栈速查（问答备用）

| 领域 | 技术点 |
|------|--------|
| MCU | STM32L431, FreeRTOS, CMSIS-RTOS V2, HAL, 软件 I2C |
| 传感器 | MPU6050, 50Hz, 平方和阈值状态机 |
| BLE | AD 结构, 31B 广播预算, adv interval, GATT, ESP-AT |
| Linux/BT | BlueZ D-Bus (ObjectManager/Adapter1/Device1/GattCharacteristic1), hcitool, hciconfig, bluetoothctl |
| C/GDBus | GVariant 引用计数, floating ref, g_clear_error, 参数消费 |
| MQTT | mosquitto, 主题设计 (data/alert/status/cmd) |
| 构建 | STM32 Makefile (arm-none-eabi-gcc), Pi gcc + 静态库 |
| 调试 | 日志分级, 看门狗自愈, 复现脚本, 升级链 |

---

*本文由真实调试过程整理，所有 bug 均有复现日志与修复验证。*