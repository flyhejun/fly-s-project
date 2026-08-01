# ISKBoard 项目交接文档

> 最后更新：2026-07-25

## 我们在做什么

基于 **STM32L431RC + FreeRTOS + MPU6050** 的可穿戴跌倒检测原型系统。检测到跌倒后 LED 闪烁 + 蜂鸣器报警，支持按键取消。跌倒事件通过 USART2 → ESP32 BLE 上传至树莓派。

**ESP32 端需自行开发固件**（central 模式，主动连接树莓派 BLE）。STM32 通过 UART2 与 ESP32 通信，接收 BLE 连接状态推送 (`+BLECONN`/`+BLEDISCONN`)，已实现断开暂存 + 重连补发。

### 当前架构

```
MPU6050 (50Hz I2C)
    ↓
SensorTask ── imuQueue(20) ──▶ FallTask
  └── I2C → MPU6050             ├── accel_sq + gyro_sq → 状态机
                                ├── s_event = {ts, accel_sq, gyro_sq}
                                └── FALL_CONFIRMED → s_event.type=1 → alarmSem
                                                                        ↓
                                                                AlarmTask
                                                                   ├── alarm_routine（LED+蜂鸣器 15s）
                                                                   ├── FallDetect_Reset
                                                                   ├── Comm_PackNotify(&s_event)
                                                                   └── osMessageQueuePut(commEventQueue)
                                                                        ↓
                                                                CommunicationTask
                                                                   ├── osMessageQueueGet → ESP32_Send
                                                                   │   ├── BLE 已连 → UART2 发出
                                                                   │   └── BLE 断开 → pending 暂存
                                                                   ├── ESP32_CheckPending（重连补发）
                                                                   └── UART2 RX 中断 ← ESP32 状态
                                                                        (+BLECONN / +BLEDISCONN)
                                                              USART2 (PA2/PA3, 115200)
                                                                        ↓
                                                                 ESP32（需自行开发）
                                                            central 模式主动连树莓派
```

---

## 已完成模块

### 1. 基础设施
| 模块 | 文件 | 状态 |
|------|------|:--:|
| 软件 I2C 驱动 | `Core/User/bsp/bsp_soft_i2c.c/h` | ✓ DWT 精确延时, ~100kHz, `SOFT_I2C_` 前缀 |
| MPU6050 驱动 | `Core/User/sensor/mpu6050.c/h` | ✓ ±2g(16384)/±250dps(131), 125Hz 内部采样 |
| printf 互斥 | `Core/Src/syscalls.c` | ✓ `_write()` 中加锁, 整段发送 |
| USART2 驱动 | `Core/Src/usart.c` | ✓ PA2/PA3, 115200, IT 中断接收 |
| ESP32 UART 驱动 | `Core/User/communication/esp32_uart.c/h` | ✓ BLE 状态跟踪 + pending 重发 + RX 行解析 |
| 通用工具 | `Core/User/common/util.c/h` | ✓ isqrt |

### 2. 跌倒检测算法（独立模块）
| 文件 | API | 说明 |
|------|------|------|
| `Core/User/algorithm/fall_detect.h` | `FallDetect_Init/Process/GetState/Reset/GetAccelSq/GetGyroSq` | 内置 accel_sq/gyro_sq 计算 |
| `Core/User/algorithm/fall_detect.c` | 三段式 | NORMAL→FREE_FALL→IMPACT→MOTIONLESS |

**关键设计：**
- `FallDetect_Process` 接受 `MPU_Raw_t *` + 时间戳，内部计算平方和
- 通过 `FallDetect_GetAccelSq()` / `FallDetect_GetGyroSq()` 获取最近一帧值
- 默认阈值通过 `FALL_DETECT_DEFAULT_CONFIG` 宏提供，`freertos.c` 一行引用
- 双重计时器（IMPACT 下）：`state_enter_tick`（静止计时）+ `impact_start_tick`（全局超时）

### 3. FreeRTOS 任务架构（全部代码在 `Core/Src/freertos.c`）

| 任务 | 函数 | 栈 | 优先级 | 驱动方式 | 职责 |
|------|------|:--:|:--:|:--------:|------|
| SensorTask | `sensorTask()` | 512B | Normal | 20ms 定时 | I2C→MPU6050→`imuQueue` |
| FallTask | `fallTask()` | **1024B** | Normal | 队列驱动 | `FallDetect_Process` → `s_event` → 事件分发 |
| AlarmTask | `alarmTask()` | 512B | Normal | 信号量驱动 | 报警 15s → `Comm_PackNotify(&s_event)` → `commEventQueue` |
| CommunicationTask | `commTask()` | 512B | Normal | 队列驱动 | UART2 收发 + BLE 状态 + pending 补发 |
| defaultTask | `StartDefaultTask()` | 512B | Normal | 1s idle | 空循环 |

**IPC 对象：**

| 对象 | 类型 | 容量 | 用途 |
|------|------|:--:|------|
| `imuQueue` | MessageQueue | 20 | SensorTask→FallTask，元素 `MPU_Raw_t`（12B） |
| `sensorSem` | BinarySemaphore | (1,0) | 按键 ISR→AlarmTask 轮询取消报警 |
| `alarmSem` | BinarySemaphore | (1,0) | 信号量通知报警，二值模式避免累积误触发 |
| `commEventQueue` | MessageQueue | **3** | AlarmTask→CommunicationTask，元素 `CommEvent_t` |
| `printfMutex` | Mutex | — | printf 原子性 |

### 4. 事件数据暂存

IMUBuf 模块（256 帧环形缓存）已被删除。数据流简化为：

```
freertos.c 全局变量：
  static FallEvent_Data_t s_event;

fallTask 每帧写入：
  s_event.timestamp_ms = ts;
  s_event.accel_sq     = accel_sq;
  s_event.gyro_sq      = gyro_sq;

FALL_CONFIRMED 时标记：
  s_event.event_type = 1;

alarmTask 直接读取：
  Comm_PackNotify(msg.data, &s_event);
```

**`FallEvent_Data_t`**（在 `comm_protocol.h` 中定义）：

```c
typedef struct {
    uint32_t timestamp_ms;
    uint8_t  event_type;
    uint32_t accel_sq;
    uint32_t gyro_sq;
} FallEvent_Data_t;
```

### 5. 报警系统

| 功能 | 说明 |
|------|------|
| LED 闪烁 | PC6 200ms Toggle |
| 蜂鸣器 | PA11 持续响（保持现状） |
| 按键取消 | PB13 EXTI 下降沿→`osSemaphoreRelease(sensorSem)` |
| 超时 | 15 秒自动停止 |
| 数据发送 | `Comm_PackNotify(&s_event)` → `commEventQueue` → CommunicationTask → `ESP32_Send` |

### 6. 通信协议

| 文件 | 说明 |
|------|------|
| `Core/User/common/comm_protocol.h` | 帧格式宏 + `FallEvent_Data_t` + `Comm_PackNotify` 声明 |
| `Core/User/common/comm_protocol.c` | 序列化实现 |
| `Core/User/communication/esp32_uart.h` | `ESP32_Send/CheckPending/RX_Char` + `g_ble_connected` |
| `Core/User/communication/esp32_uart.c` | 发送重试 + pending 暂存 + RX 行解析（无 AT 检测） |

**帧格式：**
| Byte | 字段 | 大小 | 说明 |
|:--:|------|:--:|------|
| 0 | SOF | 1 | 帧头 0xAA |
| 1 | TYPE | 1 | 消息类型：0x01 EVENT_NOTIFY |
| 2-3 | LEN | 2 | Payload 长度（小端序）= 13 |
| 4-7 | timestamp_ms | 4 | 事件时间戳 |
| 8 | event_type | 1 | 事件类型 |
| 9-12 | accel_sq | 4 | 加速度平方和 |
| 13-16 | gyro_sq | 4 | 角速度平方和 |
| 17 | CRC | 1 | XOR 校验（TYPE+LEN+Payload）|
| 18 | EOF | 1 | 帧尾 0x55 |

整帧 19 字节，仅支持 `EVENT_NOTIFY`（已删去 256 帧完整打包）。

**ESP32 通信细节：**

ESP32 需通过 UART2 推送 BLE 状态给 STM32：

| ESP32 推送 | 含义 | STM32 行为 |
|------------|------|-----------|
| `+BLECONN` | BLE 已连接 | `g_ble_connected = 1`，自动补发 pending |
| `+BLEDISCONN` | BLE 已断开 | `g_ble_connected = 0`，数据暂存 pending |

`ESP32_Send` 行为：
- BLE 已连接 → 直接 UART2 发送，最多 3 次重试
- BLE 断开 → 暂存到 pending 槽，重连后 `ESP32_CheckPending` 自动补发

**ESP32 无需 AT 测试**（`ESP32_Init` 已删除），STM32 上电后直接等待事件驱动。

---

## 阈值

| 参数 | 值 | 说明 |
|------|------|------|
| `freefall_threshold` | 70,000,000 | 约 0.51g |
| `impact_threshold` | 1,000,000,000 | 约 1.93g |
| `gyro_threshold` | 200,000,000 | 约 108°/s 等效角速度 |
| `still_low/high` | 130M/450M | 约 0.7g~1.3g |
| `impact_window_ms` | 800 | 失重→冲击窗口 |
| `still_time_ms` | 2000 | 静止确认时长 |
| `impact_timeout_ms` | 5000 | IMPACT 全局超时 |
| `alarm_hold_ms` | 15000 | 报警锁定期 |

---

## 已修复的问题

| 问题 | 根因 | 修复 |
|------|------|------|
| vTaskDelay 在 AlarmTask "失效" | FallTask 栈溢出（512B+printf），HardFault 锁死 | FallTask 栈 512B→1024B |
| alarmSem 未使用 | 历史遗留 | 替换 task notification，用 `osSemaphoreRelease/Acquire` |
| MPU6050_RawData_t 命名过长 | 设计 | 改为 `MPU_Raw_t` |
| 无用 enum 量程值 | 设计 | 替换为 4 行 `#define` |
| PC9 初始化 | CubeMX 遗留 | 删除 |
| 5 个未消费的 float 变量 | `FallDetect_Input_t` 中 gyro_dps/pitch/roll | 删除，换 `gyro_sq` |
| IMUBuf_Push 前向声明 | C89 隐式声明 | `PushData` 与 `Push` 合并 |
| alarmSem 最大计数 5 | 原设计意图不明 | 改为 `osSemaphoreNew(1, 0, ...)` 二值模式 |
| I2C 无错误处理 | `I2C_ReadBytes` 为 void | 加入 NACK 检查，失败 `return 1` |
| isqrt/dump_hex 散落在 freertos.c | 工具函数不应在业务模块 | 移入 `Core/User/common/util.c/h` |
| dump_hex 死代码 | 调试后未清理 | 删除 |
| 无独立通信任务 | alarmTask 直接发 UART | 新增 CommunicationTask + commEventQueue |
| 数据发送无重试 | `ESP32_Send` 发完不管 | 3 次重试 + pending 暂存 |
| I2C_ 函数可能冲突 | 和硬件 I2C 同名 | 加 `SOFT_I2C_` 前缀 |
| gyro_thr 命名不一致 | 缩写 | 改为 `gyro_threshold` |
| IMUBuf 256 帧环形缓存过大 | 仅需触发帧值，6KB 浪费 | 删除整个模块，改 `static FallEvent_Data_t s_event` |
| FallDetect_Input_t 多余 | 调用方需手动计算平方和 | `FallDetect_Process` 内部计算，暴露 `GetAccelSq/GetGyroSq` |
| Config 散落在 fallTask | 结构体手动填充 | `FALL_DETECT_DEFAULT_CONFIG` 宏 |
| Comm_PackFull 未使用 | 需求取消 | 删除函数及 EVENT_FULL 相关代码 |
| ESP32_Init 冗余 | 不跑 AT 固件 | 删除 |
| FallEvent_Data_t 含无关字段 | freefall_min_sq, samples, trigger_index | 简化至 4 字段 |

---

## 绝对不要踩的坑

| # | 坑 | 避法 |
|---|-----|------|
| 1 | 栈不够 — printf + 任务切换组合压栈容易溢出 | FallTask 栈至少 1024B |
| 2 | 按键 PB13 必须 `GPIO_PULLUP` | `NOPULL` 导致 EXTI 无效 |
| 3 | `sensorSem` 初始值必须为 0 | `osSemaphoreNew(1,0,...)` |
| 4 | newlib-nano 不支持 `%f` | `isqrt()` + `%lu.%02lu` 打印 g 值 |
| 5 | CubeMX 文件有 USER CODE 区域 | 只在 `/* USER CODE BEGIN/END */` 内写 |
| 6 | `SDA_Output()` 用 `HAL_GPIO_Init()` 开销大 | 后续可改直接操作 MODER 寄存器 |
| 7 | osSemaphoreAcquire 曾被认为不可靠 | 栈溢出误判，已用 `osWaitForever` |
| 8 | 构建用项目根目录 `make` | `make -j4`（超时用 `cmd //c make -j4`） |

---

## 源文件结构

```
Core/User/
├── bsp/
│   └── bsp_soft_i2c.c/h          ← 软件 I2C（PB10=SCL, PB11=SDA, ~100kHz）
├── sensor/
│   └── mpu6050.c/h               ← MPU6050 驱动
├── algorithm/
│   └── fall_detect.c/h           ← 三段式跌倒检测状态机
├── common/
│   ├── util.c/h                  ← isqrt
│   └── comm_protocol.c/h         ← 帧协议 + FallEvent_Data_t
├── communication/
│   └── esp32_uart.c/h            ← ESP32 UART 驱动（pending + 状态跟踪）

Core/Src/
├── freertos.c                    ← 5 个任务 + IPC + s_event 暂存
├── syscalls.c                    ← printf 互斥
├── gpio.c                        ← PB13 EXTI、PA11 蜂鸣器、PC6 LED
├── usart.c                       ← USART1(printf) + USART2(ESP32, IT)
├── main.c                        ← HAL Init + KernelStart
```

## 下一步任务

### 1. ESP32 端固件开发（待完成）

ESP32 作为 BLE central，主动连接树莓派：
1. 上电初始化 BLE central 模式
2. 扫描并连接树莓派指定的 BLE MAC
3. 循环：UART 收到 STM32 数据帧 → BLE Notify 转发给树莓派
4. 连接/断开时 UART 推送 `+BLECONN` / `+BLEDISCONN`

### 2. 树莓派端 — 接收与解析

解析二进制帧，帧格式详看第 6 节。

### 3. 报警增强（待定）

- 超时时改为多一盏灯闪烁 + 蜂鸣器变调（需确认硬件引脚和 PWM 支持）

---

## 设计决策记录

| 决策 | 理由 |
|------|------|
| FallDetect_Process 接受 MPU_Raw_t | 内部计算平方和，简化调用方 |
| 删除 IMUBuf 环形缓存 | 仅需触发帧值，s_event 静态变量足矣 |
| 删除 EVENT_FULL / Comm_PackFull | 树莓派只需 NOTIFY 特征值 |
| 删除 ESP32_Init | ESP32 跑自定义固件，无需 AT 检测 |
| 仅支持 EVENT_NOTIFY（19B 帧） | 足够传递跌倒事件特征值 |
| pending 只存一帧（覆盖策略） | 新事件比旧事件重要 |
| CommunicationTask | 独立管理 UART2，不阻塞报警 |
| UART2 RX 中断 + 行解析 | 轻量接收 BLE 状态 |
| SOFT_I2C_ 函数前缀 | 避免和 HAL 硬件 I2C 冲突 |
| FALL_DETECT_DEFAULT_CONFIG 宏 | 一行引用，不散落配置 |
| alarmSem 二值模式 | 防报警期间重复触发累积信号量 |
| I2C NACK 检查 | 偶发错误时跳过坏帧 |
| 不加入姿态融合 | 当前阶段目标明确 |
