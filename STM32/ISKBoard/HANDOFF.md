# ISKBoard 项目交接文档

> 最后更新：2026-07-22

## 我们在做什么

基于 **STM32L431RC + FreeRTOS + MPU6050** 的可穿戴跌倒检测原型系统。检测到跌倒后 LED 闪烁 + 蜂鸣器报警，支持按键取消。未来通过 BLE（STM32 UART2 → ESP32 → 树莓派）上传事件数据。

### 当前架构

```
MPU6050 (50Hz I2C)
    ↓
SensorTask ── imuQueue(20) ──▶ FallTask
                                    ├── accel_sq + gyro_sq → FallDetect 状态机
                                    ├── IMUBuf_PushData（256 帧环形缓存）
                                    └── FALL_CONFIRMED → osSemaphoreRelease(alarmSem)
                                                              ↓
                                                      AlarmTask
                                                         ├── alarm_routine（LED+蜂鸣器 15s）
                                                         ├── FallDetect_Reset
                                                         ├── IMUBuf_GetPeak → FallEvent_Data_t（统一事件）
                                                         ├── IMUBuf_Dump（CSV 导出）
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

### 2. 跌倒检测算法（独立模块）
| 文件 | API | 说明 |
|------|------|------|
| `Core/User/algorithm/fall_detect.h` | `FallDetect_Init/Process/GetState/Reset` | 零 RTOS 依赖 |
| `Core/User/algorithm/fall_detect.c` | 三段式 | NORMAL→FREE_FALL→IMPACT→MOTIONLESS |

**判断维度**：加速度平方和（`accel_sq`）+ **角速度平方和**（`gyro_sq`，近期新增）

**双重计时器**（IMPACT 状态下）：`state_enter_tick`（静止计时，可重置）+ `impact_start_tick`（全局超时保护，不重置）

### 3. FreeRTOS 任务架构（全部代码在 `Core/Src/freertos.c`）

| 任务 | 函数 | 栈 | 优先级 | 周期 | 职责 |
|------|------|:--:|:--:|------|------|
| SensorTask | `sensorTask()` | 512B | Normal | **20ms(50Hz)** | I2C→MPU6050→`imuQueue` |
| FallTask | `fallTask()` | **1024B** | Normal | 队列驱动 | 数据预处理→状态机→事件分发 |
| AlarmTask | `alarmTask()` | 1024B | Normal | 通知驱动 | `osSemaphoreAcquire` 阻塞→报警→特征打印→缓存导出 |
| defaultTask | `StartDefaultTask()` | 512B | Normal | 1s idle | 空循环 |

**IPC 对象：**

| 对象 | 类型 | 容量 | 用途 |
|------|------|:--:|------|
| `imuQueue` | MessageQueue | 20 | SensorTask→FallTask，元素 `MPU_Raw_t`（12B） |
| `sensorSem` | BinarySemaphore | (1,0) | 按键 ISR→AlarmTask 轮询取消报警 |
| `alarmSem` | BinarySemaphore | (5,0) | **已启用** `osSemaphoreRelease`→`osSemaphoreAcquire`，替换了旧的 task notification |
| `printfMutex` | Mutex | — | printf 原子性 |

### 4. IMU 环形缓存

| 文件 | 说明 |
|------|------|
| `Core/User/algorithm/imubuf.h` | `IMUBuf_PushData/Trigger/Dump/GetPeak/Reset` + `FallEvent_Data_t` + `IMU_Sample_t` |
| `Core/User/algorithm/imubuf.c` | 256 帧环形实现，24B/帧，约 6KB RAM。`PushData` 直接组装写入（已合并 `Push`） |

**工作模式：**
- 正常态：持续写入，循环覆盖最旧帧
- 触发态：`IMUBuf_Trigger` 标记事件，再写入 150 帧（3s @ 50Hz）后冻结
- 冻结态：前 106 帧（≈2.1s）触发前 + 150 帧（≈3s）触发后 = 256 帧
- 复位态：`IMUBuf_Reset` 解锁，继续写入

**统一事件结构体**（在 `imubuf.h`，替代了旧的 `IMUBuf_Peak_t`）：

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
| 事件摘要 | `IMUBuf_GetPeak(&event)` 填充 `FallEvent_Data_t`，打印 `freefall_min_sq` 和 `max_accel_sq` |
| CSV 导出 | `IMUBuf_Dump` 输出 256 行 `ts,accel_sq,gyro_sq` |

---

## 阈值

| 参数 | 值 | 说明 |
|------|------|------|
| `freefall_threshold` | 70,000,000 | 约 0.51g |
| `impact_threshold` | 1,000,000,000 | 约 1.93g |
| `gyro_thr` | 200,000,000 | 约 108°/s 等效角速度（估算，待调） |
| `still_low/high` | 130M/450M | 约 0.7g~1.3g |
| `impact_window_ms` | 800 | 失重→冲击窗口 |
| `still_time_ms` | 2000 | 静止确认时长 |
| `impact_timeout_ms` | 5000 | IMPACT 全局超时 |
| `alarm_hold_ms` | 15000 | 报警锁定期 |

---

## 已修复的问题

| 问题 | 根因 | 修复 |
|------|------|------|
| `vTaskDelay` 在 AlarmTask "失效" | **不是 vTaskDelay 的 bug** —— 根因是 FallTask **栈溢出**（512B 栈+printf+嵌套切换），溢出后 HardFault 导致全系统锁死，看起来像"vTaskDelay 不返回" | FallTask 栈 512B→1024B，vTaskDelay 恢复 |
| `alarmSem` 未使用 | 历史遗留 | 替换 task notification，用 `osSemaphoreRelease/Acquire` |
| `MPU6050_RawData_t` 命名过长 | 设计 | 改为 `MPU_Raw_t` |
| 无用 enum 量程值 | 设计 | 替换为 4 行 `#define` |
| PC9 初始化 | CubeMX 遗留 | 删除 |
| 5 个未消费的 float 变量 | `FallDetect_Input_t` 中 gyro_dps/pitch/roll 赋值但不使用 | 全部删除，换成 `gyro_sq` |
| `IMUBuf_Push` 前向声明缺失 | `PushData` 调用了定义在后面的 `Push`，C89 隐式声明导致编译警告 | `PushData` 与 `Push` 合并为一个函数，消除问题 |

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
│   └── bsp_soft_i2c.c/h      ← 软件 I2C（PB10=SCL, PB11=SDA, ~100kHz）
├── sensor/
│   └── mpu6050.c/h           ← MPU6050 驱动（ReadID/Init/ReadAll）
├── algorithm/
│   ├── fall_detect.c/h       ← 跌倒检测状态机（零 RTOS 依赖）
│   └── imubuf.c/h            ← IMU 环形缓存 + FallEvent_Data_t（零 RTOS 依赖）
└── comm/
    └── comm_protocol.c/h     ← 通信协议帧打包（进行中）

Core/Src/
├── freertos.c                ← 3 个任务 + alarm_routine + isqrt + IPC
├── syscalls.c                ← _write() + printfMutex
├── gpio.c                    ← PB13 EXTI（按键）、PA11（蜂鸣器）、PC6（LED）
├── main.c                    ← HAL Init + osKernelStart
```

---

## 下一步任务

### 优先级 P2 — 通信协议定义（进行中）

**状态**：帧格式已确定，`comm_protocol.h/c` 编写中。

定义 STM32 → 树莓派的二进制帧格式：

**帧格式概况：**
| Byte | 字段 | 大小 | 说明 |
|:--:|------|:--:|------|
| 0 | SOF | 1 | 帧头 0xAA |
| 1 | TYPE | 1 | 消息类型 |
| 2-3 | LEN | 2 | Payload 长度（小端序） |
| 4~n | PAYLOAD | LEN | 数据体 |
| n+1 | CRC | 1 | XOR 校验（TYPE+LEN+PAYLOAD 逐字节异或） |
| n+2 | EOF | 1 | 帧尾 0x55 |

**消息类型：**
- `0x01` EVENT_NOTIFY — 跌倒确认立即发送（Payload 13B = `timestamp_ms[4B预留]+event_type[1B]+max_accel_sq[4B]+freefall_min_sq[4B]`，整帧 19B）
- `0x02` EVENT_FULL — 报警结束后发送完整数据（Payload 6157B = 13B 特征值 + 256×24B 样本，整帧 6163B）

**预期产出文件：**
```
Core/User/comm/comm_protocol.h    ← 帧格式宏 + 打包函数声明
Core/User/comm/comm_protocol.c    ← 序列化实现
Core/User/comm/esp32_ble.c/h      ← UART2 驱动 + AT 指令（需硬件）
```

### 优先级 P3 — 阈值调优

工具链已有：`IMUBuf_Dump` 输出 CSV 可在 PC 端画波形。采集数据验证 `gyro_thr=200M` 是否合理。

### 优先级 P4 — ESP32 + BLE 对接

硬件就位后完成 UART2 → ESP32 AT 初始化序列 + 透传数据发送。

---

## 设计决策记录

| 决策 | 理由 |
|------|------|
| `FallDetect_Input_t` 不加姿态角 | 有按键取消机制，不需要精确姿态识别，减少复杂度 |
| `gyro_sq` 用整数平方和 | 和 `accel_sq` 一致的判断方式，避免 float 除法开销 |
| `IMUBuf_Push` 改为 static | ~~外部统一走 `IMUBuf_PushData`，减少 API 面~~ 已合并且删除 `Push`，`PushData` 直接组装写入 |
| `MPU_Raw_t` 不改 `IMU_Raw_t` | 仅 MPU6050 用，保持前缀一致 |
| `FallEvent_Data_t` 替代 `IMUBuf_Peak_t` | 统一事件结构体，供报警/BLE/数据库复用，避免各模块独立捞数据 |
| `IMUBuf_GetPeak` 填充 `FallEvent_Data_t *` | 一次调用填完时间戳+峰值+samples指针，零拷贝 |
| alarmSem 替换 task notification | 用已有的信号量，减少 CMSIS API 混用 |
| 不加入姿态融合 | 当前阶段目标明确，不需要增加复杂度 |
