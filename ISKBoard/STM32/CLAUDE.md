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

可穿戴跌倒检测原型，端到端链路：**STM32 采集 → ESP32 BLE 广播 → 树莓派网关 → MQTT 云**。

| 项目 | 说明 |
|------|------|
| **固件** | STM32L431RC (Cortex-M4F, 256KB Flash, 64KB RAM) @ 80MHz，FreeRTOS V10.3.1（CMSIS-RTOS V2 封装） |
| **传感器** | MPU6050 (加速度计 + 陀螺仪)，软件 I2C 连接，50Hz 读取 |
| **无线链路** | USART2 → ESP32（AT 固件，BLE Server）→ 广播包 → 树莓派被动扫描 |
| **网关** | 树莓派（GLib D-Bus 扫 BLE + mosquitto MQTT），帧 → JSON → 云端 |
| **构建** | STM32：`make -j8`（ISKBoard/STM32 的 Makefile，非 CMake）；网关：`make`（Raspberrypi） |
| **烧录** | OpenOCD + ST-Link，或 STM32_Programmer_CLI 通过 UART |
| **工具链** | STM32：`arm-none-eabi-gcc`；网关：gcc + libglib2.0-dev + openssl |

**仓库**：git 根 `d:\fly project`（多子项目混合仓库）。多根工作区 `fly-s-project.code-workspace` 包含两个根：`ISKBoard/STM32` + `Raspberrypi`。原 `ISKBoard/ESP32` 已删除，不纳入。

## 目录结构

```
d:\fly project/
├── fly-s-project.code-workspace   ← VSCode 多根工作区
├── ISKBoard/STM32/                ← 固件子项目
│   ├── Makefile                   ← 真正的构建文件（不是 CMake）
│   ├── STM32L431xx_FLASH.ld       ← 链接脚本
│   ├── startup_stm32l431xx.s      ← 启动汇编
│   ├── Core/
│   │   ├── Inc/                   ← HAL 配置、FreeRTOSConfig.h、main.h、gpio.h、usart.h
│   │   ├── Src/                   ← main.c、freertos.c、gpio.c、usart.c、syscalls.c 等
│   │   └── User/
│   │       ├── bsp/               ← bsp_soft_i2c.c/h（软件 I2C）
│   │       ├── sensor/            ← mpu6050.c/h
│   │       ├── algorithm/         ← fall_detect.c/h（状态机，零 RTOS 依赖）
│   │       ├── common/            ← comm_protocol.c/h、aes128.c/h、util.c/h
│   │       └── communication/     ← esp32_uart.c/h（ESP32 UART + BLE 广播）
│   ├── Drivers/                   ← CMSIS + STM32L4 HAL
│   ├── Middlewares/               ← FreeRTOS 内核 + CMSIS-RTOS V2 封装
│   └── build/                     ← 构建产物（.o、.elf、.hex、.bin、.map）
└── Raspberrypi/                   ← 树莓派网关子项目
    ├── Makefile                   ← gcc 构建（target: isk_gateway）
    ├── include/                   ← comm_parse.h / crypto.h / log.h / mqtt_publish.h
    ├── src/
    │   ├── main.c                 ← GLib 主循环 + MQTT 初始化
    │   ├── ble_central.c          ← BlueZ D-Bus 被动扫描广播包
    │   ├── comm_parse.c           ← 帧解析（CRC-8 校验 + AES 解密）
    │   ├── crypto.c               ← openssl EVP AES-128-CTR
    │   ├── mqtt_publish.c         ← 帧 → JSON → MQTT
    │   └── log.c                  ← 日志（文件 + stderr）
    └── third_party/
        ├── cjson/                 ← cJSON 静态库（include + lib）
        └── mosquitto/             ← libmosquitto 静态库（include + lib）
```

## 构建系统

**STM32（固件）**：使用 `ISKBoard/STM32/Makefile`（不是 CMake，尽管 `cmake/` 目录也存在）。
- 新增 `.c` 文件 → 加到 `Makefile` 的 `C_SOURCES` 列表
- 新增 include 路径 → 加到 `Makefile` 的 `C_INCLUDES` 和 `AS_INCLUDES`
- 编译命令：在 `ISKBoard/STM32` 下执行 `make -j8`

**树莓派（网关）**：使用 `Raspberrypi/Makefile`。
- 依赖：`sudo apt install libglib2.0-dev`；cjson / mosquitto 静态库自带在 `third_party/`
- 链接 `-lssl -lcrypto`（openssl EVP 解密）
- 编译命令：在 `Raspberrypi` 下执行 `make`，产物 `isk_gateway`

## 通信链路总览

```
MPU6050 (50Hz, 软I2C PB10/PB11)
    │
SensorTask ──imuQueue(20)──▶ FallTask
                                 │  accel_sq/gyro_sq 阈值状态机
                                 ▼
                            FALL_CONFIRMED
                                 │  alarmSem
                                 ▼
AlarmTask ──Comm_PackNotify──▶ commEventQueue(3)
  （LED PC6 + 蜂鸣器 PA11，15s）│
                                 ▼
CommTask（100ms 节拍）
  ├── 事件帧 / 10Hz 实时帧 ──▶ ESP32_Send ──▶ USART2 ──▶ ESP32
  │                                                    │
  │                                    BLE Server：AT+BLEADVDATA 更新广播包
  │                                                    │
  │                                          BLE 广播包（31B 限制内）
  │                                                    ▼
  └── 下行指令 ◀── ESP32_RX_GetFrame ◀── USART2 ◀── ESP32 ◀── GATT Write (0xC302)
                                                     ▲
                                       树莓派（被动扫描 + 按需连接写指令）
                                                     │
                                          MQTT（121.40.252.238:1883）
                                                     │
                                        topics: data / alert / status / cmd
```

- **上行（实时数据）走广播**：ESP32 广播包，树莓派免连接被动接收，天然支持 10Hz 吞吐。
- **下行（控制指令）走 GATT**：树莓派按需连接 ESP32，写可写特征值 `0xC302`，ESP32 透传 UART2 给 STM32。

## 通信协议

### 帧格式（comm_protocol.h）

```
┌──────┬──────┬──────────┬──────────┬──────┬──────┐
│ SOF  │ TYPE │   LEN    │ PAYLOAD  │ CRC  │ EOF  │
│ 1B   │ 1B   │ 2B (LE)  │ LEN 字节  │ 1B   │ 1B   │
│0xAA  │      │          │          │      │0x55  │
└──────┴──────┴──────────┴──────────┴──────┴──────┘
```

- **CRC-8/MAXIM**（poly 0x31）：对 SOF → PAYLOAD 末尾逐字节位反序计算，**在 AES 加密后**计算。
- **AES-128-CTR**：加密 PAYLOAD（`aes128.c` 纯软件实现），CTR 流模式密文与明文等长，不改变帧长度。加解密同一函数。固定 key/IV 两端硬编码一致。

### 消息类型

| 方向 | TYPE | 名称 | Payload | 帧长 |
|:--:|:--:|------|------|:--:|
| ↑ | 0x01 | EVENT_NOTIFY | date(6) + event_type(1) + accel_sq(4) + gyro_sq(4) | 21B |
| ↑ | 0x02 | REAL_TIME | date(6) + accel_sq(4) + gyro_sq(4)，10Hz | 20B |
| ↑ | 0x03 | STATUS_REPLY | state(1) | 7B |
| ↓ | 0x81 | SET_THRESHOLD | param_id(1) + value(4) | 11B |
| ↓ | 0x83 | ALARM_CANCEL | 空 | 6B |
| ↓ | 0x84 | TEST_LED | on/off(1) | 7B |
| ↓ | 0x85 | TEST_BUZZER | on/off(1) | 7B |
| ↓ | 0x86 | TIME_SYNC | year(2)+month+day+hour+minute | 12B |
| ↓ | 0x87 | QUERY_STATUS | 空 | 6B |

- `date` 为上位机 TIME_SYNC 周期下发、STM32 直接存储的年月日时分（不换算，无 RTC）。
- 下行可改阈值参数 ID：`0x01` freefall、`0x02` impact、`0x03` still_low、`0x04` still_high（时间与 gyro 阈值只读）。
- 两端实现需同步：STM32 `comm_protocol.c/h` ↔ 树莓派 `comm_parse.c/h`。

## BLE 广播机制（esp32_uart.c）

ESP32 跑 **AT 固件**，作为 BLE Server：
- `AT+BLEINIT=2`（Server）+ `AT+BLENAME=FallSensor` + GATT 服务（含可写特征值 `0xC302`）+ `AT+BLEADVPARAM=160,160,0,0,7`（可连接广播）+ `AT+BLEADVSTART`。
- **发送上行帧**：`AT+BLEADVDATA="hex"` 把帧写进广播包 ManufacturerData。Fire & Forget（不等待 OK），保证 10Hz 吞吐。
- **接收下行帧**：UART2 RX 中断双模式状态机 —— 行模式（默认，解析 `+BLEDISCONN`）＋ 帧模式（SOF 0xAA → 按 LEN 收整帧，SPSC 环形缓冲 4 深，无锁）。
- 断开后 `+BLEDISCONN` 置位标志，`ESP32_CheckAdvStatus()` 在任务里补发 `AT+BLEADVSTART` 恢复广播（不能放中断——HAL 阻塞）。

## FreeRTOS 任务架构（Core/Src/freertos.c）

```
SensorTask (Normal, 512B)  ──imuQueue(20)──▶  FallTask (Normal, 1024B)
  20ms 周期                                    队列驱动
  I2C → MPU6050 → imuQueue                    FallDetect_Process()
                                              事件分发 → printf / g_data_stream / alarmSem
                                                          │
AlarmTask (Normal, 512B)  ◀── alarmSem ────────┘
  alarm_routine: LED+蜂鸣器 15s（按键/下行可取消）
  → Comm_PackNotify → commEventQueue(3)
                                │
CommTask (Normal, 1024B)  ◀─────┘
  100ms 节拍：事件帧 + 10Hz 实时帧 → ESP32_Send
  下行：ESP32_RX_GetFrame → Comm_ParseCmd → 指令分发
  ESP32_Init_BLE() / ESP32_CheckAdvStatus()

defaultTask (Normal, 512B)     空循环 1s delay
```

### IPC 对象

| 对象 | 类型 | 容量 | 用途 |
|------|------|:--:|------|
| `imuQueue` | MessageQueue | 20 × `MPU_Raw_t`(12B) | SensorTask → FallTask |
| `commEventQueue` | MessageQueue | 3 × `CommEvent_t` | AlarmTask → CommTask（事件帧） |
| `alarmSem` | BinarySemaphore (1,0) | — | 跌倒确认 → AlarmTask 触发（二值防累积） |
| `sensorSem` | BinarySemaphore (1,0) | — | PB13 按键 EXTI 取消报警 |
| `printfMutex` | Mutex | — | `_write()` 中保证 printf 原子性 |

### 任务间全局状态（freertos.c USER CODE）

| 变量 | 类型 | 语义 |
|------|------|------|
| `s_event` | `static FallEvent_Data_t` | fallTask 逐帧写日期/accel/gyro，FALL_CONFIRMED 置 event_type，alarmTask 读取打包 |
| `g_data_stream` | `volatile uint8_t` | 失重停传，误报超时/报警结束恢复 |
| `g_alarm` | `volatile uint8_t` | 下行 ALARM_CANCEL 置位，alarm_routine 轮询取消 |
| `g_date_*` / `g_time_synced` | static | TIME_SYNC 直接存储的日期时间 |

## 已知问题（待修）

- **树莓派下行链路未实现**：`main.c` 的 `on_message` 收到 MQTT cmd 只打印不转发，未实现"MQTT 指令 → 协议帧 → BLE GATT Write(0xC302) → ESP32"。
- **`g_time_synced` 是死标志**：TIME_SYNC 置位 1，但没有任何读取方。
- **硬编码**：MQTT 账号密码（main.c）、AES key/IV（两端）、ESP32 MAC（ble_central.c）均写死在源码。
- **ESP32_Send Fire & Forget**：不校验 `AT+BLEADVDATA` 是否执行成功，广播包丢包无感知（10Hz 吞吐的取舍）。

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
- `FallDetect_Process(const MPU_Raw_t*, ts)` 内部计算平方和，输出 `FallEvent_t`，纯数据驱动
- 通过 `FallDetect_GetAccelSq()/GetGyroSq()` 取最近一帧值
- 默认阈值走 `FALL_DETECT_DEFAULT_CONFIG` 宏，`freertos.c` 一行引用

### 可配置阈值（fall_detect.h）

| 参数 | 默认值 | 语义 |
|------|--------|------|
| `freefall_threshold` | 70,000,000 | 加速度平方和 < 此值 = 失重 (~0.51g) |
| `impact_threshold` | 1,000,000,000 | 加速度平方和 > 此值 = 冲击 (~1.93g) |
| `still_low` | 130,000,000 | 静止下限 (~0.7g) |
| `still_high` | 450,000,000 | 静止上限 (~1.3g) |
| `gyro_threshold` | 200,000,000 | 角速度平方和阈值 (~108°/s) |
| `impact_window_ms` | 800 | 失重→冲击最大间隔 |
| `still_time_ms` | 2000 | 持续静止确认时间 |
| `impact_timeout_ms` | 5000 | IMPACT 全局超时 |
| `alarm_hold_ms` | 15000 | 报警锁定期（alarm_routine 用） |

- 运行时改阈值：`FallDetect_SetParam(FD_PARAM_*, value)`，仅 RAM 生效、掉电恢复默认；下行 `SET_THRESHOLD` 即走此接口（时间/gyro 只读）。

## 树莓派网关

- **架构**：GLib GMainLoop（BlueZ D-Bus BLE 扫描事件）+ `mosquitto_loop_start`（MQTT 后台线程）双事件循环并存。MQTT 断连自动重试，BLE 扫描失败不退出。
- **BLE 接收**（ble_central.c）：`StartDiscovery` 被动扫描，读 `ManufacturerData`（AD type 0xFF），匹配目标 MAC `58:8c:81:0e:4e:16`；订阅 `PropertiesChanged` 实时收新帧；`check_existing_devices` 兜底已缓存设备。
- **帧解析**（comm_parse.c）：SOF/EOF/LEN 校验 → **先 CRC-8 校验密文** → AES-128-CTR 解密（openssl EVP）→ 按类型解析。与 STM32 `comm_protocol.c` 同步。
- **MQTT 上传**（mqtt_publish.c）：`ParsedFrame_t` → JSON → 发布。

| Topic | 内容 |
|------|------|
| `fall_detection/pi01/data` | REAL_TIME 实时帧（date + accel_sq + gyro_sq） |
| `fall_detection/pi01/alert` | EVENT_NOTIFY 跌倒事件 |
| `fall_detection/pi01/status` | STATUS_REPLY 状态 + 上线 "online" |
| `fall_detection/pi01/cmd` | 下行指令（main.c 已订阅，转发未实现） |

- **MQTT 服务器**：`121.40.252.238:1883`，用户 `flyzzz`，client id `pi01`。
- 日志：`log_init("./isk_gateway.log")` 文件 + stderr，`LOG_INFO/WARN/ERROR`。

## 软件 I2C

- SCL = PB10, SDA = PB11（开漏 + 上拉）
- `I2C_Delay()` 使用 DWT 周期计数器（`DWT->CYCCNT`），精度 ±12.5ns @ 80MHz
- `I2C_DELAY_US = 5`（≈100kHz SCL），在 `bsp_soft_i2c.h` 中定义
- `SDA_Output()` / `SDA_Input()` 用 `HAL_GPIO_Init()` 切换方向（开销大，后续可优化）

## USART

- **USART1**（PA9/PA10, 115200）：printf 调试输出
- **USART2**（PA2/PA3, 115200）：接 ESP32，IT 中断接收 → `HAL_UART_RxCpltCallback` → `ESP32_RX_Char(rx_byte)` 逐字喂入解析状态机

## 代码规范

- 文件编码 UTF-8
- CubeMX 生成的文件（freertos.c、main.c、gpio.c 等）有 `USER CODE BEGIN/END` 标记，自定义代码只写在标记区域内
- 注释语言：模块头用英文，行内注释用中文
- 外设初始化在 CubeMX 生成的 `MX_*_Init()` 中，应用逻辑在 freertos.c 的 task 函数中