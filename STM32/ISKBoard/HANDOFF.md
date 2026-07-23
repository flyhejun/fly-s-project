# ISKBoard 项目交接文档

> 最后更新：2026-07-23

## 我们在做什么

基于 **STM32L431RC + FreeRTOS + MPU6050** 的可穿戴跌倒检测原型系统。检测到跌倒后 LED 闪烁 + 蜂鸣器报警，支持按键取消。跌倒事件通过 USART2 → ESP32 BLE 上传至树莓派。

### 当前架构

```
MPU6050 (50Hz I2C)
    ↓
SensorTask ── imuQueue(20) ──▶ FallTask
  ├── ESP32_Init()                    ├── accel_sq + gyro_sq → FallDetect 状态机
  └── I2C → MPU6050                  ├── IMUBuf_PushData（256 帧环形缓存）
                                       └── FALL_CONFIRMED → osSemaphoreRelease(alarmSem)
                                                                  ↓
                                                          AlarmTask
                                                             ├── alarm_routine（LED+蜂鸣器 15s）
                                                             ├── FallDetect_Reset
                                                             ├── IMUBuf_GetPeak → Comm_PackNotify
                                                             ├── ESP32_Send（USART2 → ESP32 BLE）
                                                             └── IMUBuf_Reset
```

---

## 已完成模块

### 1. 基础设施
| 模块 | 文件 | 状态 |
|------|------|:--:|
| 软件 I2C 驱动 | `Core/User/bsp/bsp_soft_i2c.c/h` | ✓ DWT 精确延时, ~100kHz |
| MPU6050 驱动 | `Core/User/sensor/mpu6050.c/h` | ✓ ±2g(16384)/±250dps(131), 125Hz 内部采样 |
| printf 互斥 | `Core/Src/syscalls.c` | ✓ `_write()` 中加锁, 整段发送 |
| USART2 驱动 | `Core/Src/usart.c` | ✓ PA2/PA3, 115200, ESP32 通信 |
| ESP32 BLE | `Core/User/communication/esp32_ble.c/h` | ✓ AT 检测 + 数据发送 |
| 通用工具 | `Core/User/common/util.c/h` | ✓ isqrt, dump_hex |

### 2. 跌倒检测算法（独立模块）
| 文件 | API | 说明 |
|------|------|------|
| `Core/User/algorithm/fall_detect.h` | `FallDetect_Init/Process/GetState/Reset` | 零 RTOS 依赖 |
| `Core/User/algorithm/fall_detect.c` | 三段式 | NORMAL→FREE_FALL→IMPACT→MOTIONLESS |

**判断维度**：加速度平方和（`accel_sq`）+ **角速度平方和**（`gyro_sq`）

**双重计时器**（IMPACT 状态下）：`state_enter_tick`（静止计时，可重置）+ `impact_start_tick`（全局超时保护，不重置）

### 3. FreeRTOS 任务架构（全部代码在 `Core/Src/freertos.c`）

| 任务 | 函数 | 栈 | 优先级 | 周期 | 职责 |
|------|------|:--:|:--:|------|------|
| SensorTask | `sensorTask()` | 512B | Normal | **20ms(50Hz)** | I2C→MPU6050→`imuQueue` + `ESP32_Init()` |
| FallTask | `fallTask()` | **1024B** | Normal | 队列驱动 | 数据预处理→状态机→事件分发 |
| AlarmTask | `alarmTask()` | 1024B | Normal | 通知驱动 | 报警→`Comm_PackNotify`→`ESP32_Send`→`IMUBuf_Reset` |
| defaultTask | `StartDefaultTask()` | 512B | Normal | 1s idle | 空循环 |

**IPC 对象：**

| 对象 | 类型 | 容量 | 用途 |
|------|------|:--:|------|
| `imuQueue` | MessageQueue | 20 | SensorTask→FallTask，元素 `MPU_Raw_t`（12B） |
| `sensorSem` | BinarySemaphore | (1,0) | 按键 ISR→AlarmTask 轮询取消报警 |
| `alarmSem` | BinarySemaphore | **(1,0)** | 信号量通知报警，二值模式避免累积误触发 |
| `printfMutex` | Mutex | — | printf 原子性 |

### 4. IMU 环形缓存

| 文件 | 说明 |
|------|------|
| `Core/User/algorithm/imubuf.h` | `IMUBuf_PushData/Trigger/GetPeak/Reset` + `FallEvent_Data_t` + `IMU_Sample_t` |
| `Core/User/algorithm/imubuf.c` | 256 帧环形实现，24B/帧，约 6KB RAM。`PushData` 直接组装写入 |

**工作模式：**
- 正常态：持续写入，循环覆盖最旧帧
- 触发态：`IMUBuf_Trigger` 标记事件，再写入 150 帧（3s @ 50Hz）后冻结
- 冻结态：前 106 帧（≈2.1s）触发前 + 150 帧（≈3s）触发后 = 256 帧
- 复位态：`IMUBuf_Reset` 解锁，继续写入

**统一事件结构体**（在 `imubuf.h`）：

```c
typedef struct {
    uint32_t timestamp_ms;       // 预留，当前填 0（后期树莓派下发北京时间）
    uint8_t  event_type;         // 0=跌倒确认
    uint32_t max_accel_sq;       // 冲击加速度峰值
    uint32_t freefall_min_sq;    // 失重加速度谷值
    const IMU_Sample_t *samples; // 指向 256 帧环形缓冲区，零拷贝
} FallEvent_Data_t;
```

### 5. 报警系统

| 功能 | 说明 |
|------|------|
| LED 闪烁 | PC6 200ms Toggle |
| 蜂鸣器 | PA11 持续响（保持现状） |
| 按键取消 | PB13 EXTI 下降沿→`osSemaphoreRelease(sensorSem)` |
| 超时 | 15 秒自动停止 |
| 事件摘要 | `IMUBuf_GetPeak(&event)` 填充 `FallEvent_Data_t` |
| 数据发送 | `Comm_PackNotify` 打包 → `ESP32_Send`（USART2→ESP32 BLE） |

### 6. 通信协议

| 文件 | 说明 |
|------|------|
| `Core/User/common/comm_protocol.h` | 帧格式宏 + `Comm_PackNotify/Full` 声明 |
| `Core/User/common/comm_protocol.c` | 序列化实现（含 256 帧完整打包） |
| `Core/User/communication/esp32_ble.h` | `ESP32_Init/Send` 声明 |
| `Core/User/communication/esp32_ble.c` | ESP32 AT 检测 + `HAL_UART_Transmit` 发送 |

**帧格式：**
| Byte | 字段 | 大小 | 说明 |
|:--:|------|:--:|------|
| 0 | SOF | 1 | 帧头 0xAA |
| 1 | TYPE | 1 | 消息类型 |
| 2-3 | LEN | 2 | Payload 长度（小端序） |
| 4~n | PAYLOAD | LEN | 数据体 |
| n+1 | CRC | 1 | XOR 校验 |
| n+2 | EOF | 1 | 帧尾 0x55 |

---

## 阈值

| 参数 | 值 | 说明 |
|------|------|------|
| `freefall_threshold` | 70,000,000 | 约 0.51g |
| `impact_threshold` | 1,000,000,000 | 约 1.93g |
| `gyro_thr` | 200,000,000 | 约 108°/s 等效角速度 |
| `still_low/high` | 130M/450M | 约 0.7g~1.3g |
| `impact_window_ms` | 800 | 失重→冲击窗口 |
| `still_time_ms` | 2000 | 静止确认时长 |
| `impact_timeout_ms` | 5000 | IMPACT 全局超时 |
| `alarm_hold_ms` | 15000 | 报警锁定期 |

---

## 已修复的问题

| 问题 | 根因 | 修复 |
|------|------|------|
| `vTaskDelay` 在 AlarmTask "失效" | **不是 vTaskDelay 的 bug** —— 根因是 FallTask **栈溢出**（512B 栈+printf+嵌套切换），溢出后 HardFault 导致全系统锁死 | FallTask 栈 512B→1024B |
| `alarmSem` 未使用 | 历史遗留 | 替换 task notification，用 `osSemaphoreRelease/Acquire` |
| `MPU6050_RawData_t` 命名过长 | 设计 | 改为 `MPU_Raw_t` |
| 无用 enum 量程值 | 设计 | 替换为 4 行 `#define` |
| PC9 初始化 | CubeMX 遗留 | 删除 |
| 5 个未消费的 float 变量 | `FallDetect_Input_t` 中 gyro_dps/pitch/roll 无用字段 | 全部删除，换成 `gyro_sq` |
| `IMUBuf_Push` 前向声明缺失 | C89 隐式声明导致编译警告 | `PushData` 与 `Push` 合并为一个函数 |
| `alarmSem` 最大计数 5 | 原设计意图不明 | 改为 `osSemaphoreNew(1, 0, ...)` 二值模式 |
| I2C 无错误处理 | `I2C_ReadBytes` 为 void 无返回值 | 加入 NACK 检查，每步检测 ACK，失败 `return 1` |
| `isqrt`/`dump_hex` 散落在 freertos.c | 工具函数不应放在业务模块 | 移入 `Core/User/common/util.c/h` |
| `IMUBuf_Dump` 未被调用 | 调试函数不再需要 | 删除整函数及 `#include <stdio.h>` |

---

## 绝对不要踩的坑

| # | 坑 | 避法 |
|---|-----|------|
| 1 | **栈不够** — printf + 任务切换组合压栈容易溢出 | FallTask/涉及到 printf 的任务栈至少 1024B |
| 2 | **按键 PB13 必须 `GPIO_PULLUP`** | 原 `GPIO_NOPULL` 导致 EXTI 无效 |
| 3 | **`sensorSem` 初始值必须为 0** | `osSemaphoreNew(1,0,...)`，否则立即拿到信号量等于误按键 |
| 4 | **newlib-nano 不支持 `%f`** | 用 `isqrt()` + `%lu.%02lu` 打印 g 值，不要用 float printf |
| 5 | **CubeMX 文件有 USER CODE 区域** | 只在 `/* USER CODE BEGIN/END */` 内写代码，否则 CubeMX 重新生成时会覆盖 |
| 6 | **`SDA_Output()/SDA_Input()` 用 `HAL_GPIO_Init()` 开销大** | 后续优化可改直接操作 MODER 寄存器，现在不动 |
| 7 | **`osSemaphoreAcquire(阻塞超时)` 模式曾被认为不可靠** | 结论是栈溢出引起的误判，目前用 `osWaitForever` 正常工作 |
| 8 | **构建用项目根目录 `make`** | `make -j4`（有时 Git Bash CreateProcess 失败，用 `cmd //c make -j4`） |

---

## 源文件结构

```
Core/User/
├── bsp/
│   └── bsp_soft_i2c.c/h          ← 软件 I2C（PB10=SCL, PB11=SDA, ~100kHz）
├── sensor/
│   └── mpu6050.c/h               ← MPU6050 驱动（ReadID/Init/ReadAll）
├── algorithm/
│   ├── fall_detect.c/h           ← 跌倒检测状态机（零 RTOS 依赖）
│   └── imubuf.c/h                ← IMU 环形缓存 + FallEvent_Data_t（零 RTOS 依赖）
├── common/
│   ├── util.c/h                  ← 通用工具（isqrt, dump_hex）
│   └── comm_protocol.c/h         ← 通信协议帧打包
├── communication/
│   └── esp32_ble.c/h             ← ESP32 BLE 驱动（AT 检测 + 数据发送）

Core/Src/
├── freertos.c                    ← 3 个任务 + alarm_routine + isqrt + IPC
├── syscalls.c                    ← _write() + printfMutex
├── gpio.c                        ← PB13 EXTI（按键）、PA11（蜂鸣器）、PC6（LED）
├── usart.c                       ← USART1（printf）+ USART2（ESP32）
├── main.c                        ← HAL Init + osKernelStart
```

## 下一步任务

### 树莓派端 — 接收与解析

树莓派通过 BLE 接收 STM32 发送的二进制帧，需编写 Python 接收程序：

1. BLE 扫描连接 ESP32
2. 接收二进制帧，按 `comm_protocol.h` 的格式解析
3. 存入数据库（SQLite/InfluxDB）
4. Web 展示

**帧解析要点：**
- 找 SOF(0xAA) 和 EOF(0x55)
- 读 TYPE → 决定 Payload 长度
- 读 LEN → 确定 Payload 边界
- CRC 校验（TYPE+LEN+Payload 逐字节异或）
- TYPE=0x01: 即时通知（13B）
- TYPE=0x02: 完整事件（6144B, 256×24B 样本）

---

## 设计决策记录

| 决策 | 理由 |
|------|------|
| `FallDetect_Input_t` 不加姿态角 | 有按键取消机制，不需要精确姿态识别，减少复杂度 |
| `gyro_sq` 用整数平方和 | 和 `accel_sq` 一致的判断方式，避免 float 除法开销 |
| `IMUBuf_Push` 移除并合入 `PushData` | 减少 API 面，`PushData` 直接组装写入 |
| `MPU_Raw_t` 不改 `IMU_Raw_t` | 仅 MPU6050 用，保持前缀一致 |
| `FallEvent_Data_t` 替代 `IMUBuf_Peak_t` | 统一事件结构体，供报警/BLE/数据库复用 |
| `IMUBuf_GetPeak` 填充 `FallEvent_Data_t *` | 一次调用填完时间戳+峰值+samples指针，零拷贝 |
| alarmSem max=1 二值模式 | 防报警期间重复触发累积信号量 |
| I2C_ReadBytes 加入 NACK 检查 | I2C 总线偶发错误时跳过坏帧，避免状态机误判 |
| `isqrt()` 放入独立 util 模块 | 通用工具，避免散落在业务代码中 |
| 不加入姿态融合 | 当前阶段目标明确，不需要增加复杂度 |
