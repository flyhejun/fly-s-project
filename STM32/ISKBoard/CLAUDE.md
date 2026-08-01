# CLAUDE.md — ISKBoard 项目指南

## 教学风格（最高优先级）

**每次只改一个部分，先讲解再动手。**

- 不要一次性批量修改多个不相关的代码块
- 每个改动前，先说清楚：改什么、为什么、怎么改
- 让用户可以跟着每一步理解代码演进的过程
- 用户说"确认"或"可以"之后再动手
- 复杂任务先出方案，等用户确认后再逐步执行

反例（禁止）：把 3 个文件的 5 处改动一次性全部做完
正例：先改 FallTask 优先级 → 解释为什么 → 确认 → 再改队列深度 → ...

**每次任务必须留一部分给用户自己写，不能全部代劳。**

- 先讲解原理和思路，然后指明"我来改 A 部分，你改 B 部分"
- B 部分应有明确的学习价值（新语法、新思路、关键逻辑）
- 对于纯体力活（如多处重复修改、格式化调整），可以全部代劳
- 对于涉及新概念、新模式的改动，必须留一块让用户动手
- 每次指明用户写什么后，要说清楚：涉及哪个文件、大概几行、写什么内容

反例（禁止）：5 个改动全部做完，用户只是看着
正例："isqrt 函数我来加，g 值的计算和 printf 格式你来写，公式是 mag / 16384"

## 项目概览

| 项目 | 说明 |
|------|------|
| **硬件** | STM32L431RC (Cortex-M4F, 256KB Flash, 64KB RAM) @ 80MHz |
| **RTOS** | FreeRTOS V10.3.1，通过 CMSIS-RTOS V2 API 封装 |
| **传感器** | MPU6050 (加速度计 + 陀螺仪)，通过软件 I2C 连接 |
| **功能** | 基于 IMU 的三段式跌倒检测（失重 → 冲击 → 静止） |
| **构建** | `make -j8`（项目根目录的 Makefile，非 CMake） |
| **烧录** | OpenOCD + ST-Link，或 STM32_Programmer_CLI 通过 UART |
| **工具链** | `arm-none-eabi-gcc` |

## 目录结构

```
ISKBoard/
├── Makefile                    ← 真正的构建文件（不是 CMake）
├── STM32L431xx_FLASH.ld        ← 链接脚本
├── startup_stm32l431xx.s       ← 启动汇编
├── Core/
│   ├── Inc/                    ← HAL 配置、FreeRTOSConfig.h、main.h、gpio.h
│   ├── Src/                    ← main.c、freertos.c、gpio.c、usart.c、stm32l4xx_it.c 等
│   └── User/
│       ├── bsp/
│       │   ├── bsp_soft_i2c.c  ← 软件 I2C（PB10=SCL, PB11=SDA，开漏+上拉）
│       │   └── bsp_soft_i2c.h
│       ├── sensor/
│       │   ├── mpu6050.c       ← MPU6050 驱动（ReadID/Init/ReadAll）
│       │   └── mpu6050.h
│       └── algorithm/
│           ├── fall_detect.c   ← 跌倒检测状态机（零 RTOS 依赖，可单独测试）
│           └── fall_detect.h
├── Drivers/                    ← CMSIS + STM32L4 HAL
├── Middlewares/                ← FreeRTOS 内核 + CMSIS-RTOS V2 封装
└── build/                      ← 构建产物（.o、.elf、.hex、.bin、.map）
```

## 构建系统

**使用项目根目录的 `Makefile`**（不是 CMake，尽管 `cmake/` 目录也存在）。

- 新增 `.c` 文件 → 加到 `Makefile` 的 `C_SOURCES` 列表
- 新增 include 路径 → 加到 `Makefile` 的 `C_INCLUDES` 和 `AS_INCLUDES`
- 编译命令：在项目根目录执行 `make -j8`

## FreeRTOS 任务架构

```
SensorTask (Normal)  ──imuQueue(20)──▶  FallTask (Normal)
      │                                      │
  10ms 周期                                预处理 accel_sq
  I2C → MPU6050                            FallDetect_Process()
  入队 IMU 数据                             event → printf/LED

LedTask (Normal)           defaultTask (Normal)
  500ms 翻转 PC6            空循环 1s delay
```

### IPC 对象

| 对象 | 类型 | 用途 |
|------|------|------|
| `imuQueue` | MessageQueue × 20 | SensorTask → FallTask，元素类型 `IMU_Data_t` |
| `sensorSem` | BinarySemaphore | 保留未用（原按键触发模式遗留） |
| `printfMutex` | Mutex | `_write()` 中使用，保证 printf 原子性 |

### 已知问题（待修）

- `imuQueue` 创建了两次：先用 `sizeof(uint32_t)` 创建（CubeMX 生成），再立即用 `sizeof(IMU_Data_t)` 重建（USER CODE）。第一次创建浪费资源。
- `IMU_Data_t` 的 `timestamp` 字段在重构中被移除（linter 修改），队列数据结构与 fallTask 使用需保持一致。

## 跌倒检测算法

### 模块边界

```
freertos.c (RTOS 层)          fall_detect.c (算法层)
─────────────────────         ─────────────────────
任务循环                       状态机逻辑
队列收发                       阈值比较
事件→动作 分发                计时器管理
                              零外部依赖
```

### 三段式模型

```
NORMAL ──(accel_sq < 70M)──▶ FREE_FALL
                                 │
                  800ms 内冲击?   │  超时 → NORMAL
                                 ▼
                             IMPACT
                                 │
                  持续静止 2s?    │  5s 超时 → NORMAL
                                 ▼
                           MOTIONLESS
                            (跌倒确认)
```

### 关键设计决策

- **加速度平方和** 作为判断依据（避免开根号）
- **双重计时器**（IMPACT 状态）：`state_enter_tick` 用于静止持续计时（可重置），`impact_start_tick` 用于全局超时（不重置）—— 修复了原版超时永不触发的 bug
- `FallDetect_Process()` 输入 `FallDetect_Input_t`，输出 `FallEvent_t`，纯数据驱动
- `FallDetect_Input_t` 已预留 gyro 原始值 + pitch/roll 字段

### 可配置阈值

| 参数 | 默认值 | 语义 |
|------|--------|------|
| `freefall_threshold` | 70,000,000 | 加速度平方和 < 此值 = 失重 (~0.5g) |
| `impact_threshold` | 1,000,000,000 | 加速度平方和 > 此值 = 冲击 (~2g) |
| `still_low` | 130,000,000 | 静止下限 (~0.7g) |
| `still_high` | 450,000,000 | 静止上限 (~1.3g) |
| `impact_window_ms` | 800 | 失重→冲击最大间隔 |
| `still_time_ms` | 2000 | 持续静止确认时间 |
| `impact_timeout_ms` | 5000 | IMPACT 全局超时 |

## 软件 I2C

- SCL = PB10, SDA = PB11（开漏 + 上拉）
- `I2C_Delay()` 使用 DWT 周期计数器（`DWT->CYCCNT`），精度 ±12.5ns @ 80MHz
- `I2C_DELAY_US = 5`（≈100kHz SCL），在 `bsp_soft_i2c.h` 中定义
- `SDA_Output()` / `SDA_Input()` 用 `HAL_GPIO_Init()` 切换方向（开销大，后续可优化）

## 代码规范

- 文件编码 UTF-8
- CubeMX 生成的文件（freertos.c、main.c、gpio.c 等）有 `USER CODE BEGIN/END` 标记，自定义代码只写在标记区域内
- 注释语言：模块头用英文，行内注释用中文
- 外设初始化在 CubeMX 生成的 `MX_*_Init()` 中，应用逻辑在 freertos.c 的 task 函数中
