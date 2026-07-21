# ISKBoard 项目交接文档

## 我们在做什么

基于 **STM32L431RC + FreeRTOS + MPU6050** 的嵌入式跌倒检测系统。通过 MPU6050 六轴 IMU 采集加速度数据，运行三段式跌倒模型（失重→冲击→静止），检测到跌倒后 LED 闪烁 + 蜂鸣器报警，支持按键取消报警。

## 已完成

### 1. 基础设施
| 模块 | 文件 | 状态 |
|------|------|:--:|
| 软件 I2C 驱动 | `Core/User/bsp/bsp_soft_i2c.c/h` | ✓ DWT 精确延时, ~100kHz |
| MPU6050 驱动 | `Core/User/sensor/mpu6050.c/h` | ✓ ±2g/±250dps, 125Hz 配置, ReadAll |
| printf 互斥 | `Core/Src/syscalls.c` | ✓ `_write()` 中加锁, `HAL_UART_Transmit` 整段发送 |
| gpio.c PB13 上拉 | `Core/Src/gpio.c` | ✓ `GPIO_PULLUP`(之前是 NOPULL,按键无效) |
| VS Code IntelliSense | `.vscode/c_cpp_properties.json` | ✓ 新创建 |

### 2. 跌倒检测算法（独立模块）
| 文件 | 说明 |
|------|------|
| `Core/User/algorithm/fall_detect.h` | API: `FallDetect_Init`, `FallDetect_Process`, `FallDetect_GetState`, `FallDetect_Reset` |
| `Core/User/algorithm/fall_detect.c` | 三段式状态机: NORMAL→FREE_FALL→IMPACT→MOTIONLESS |

- **独立于 RTOS**，纯 C 算法，可在 PC 上单元测试
- **双重计时器修复**: `state_enter_tick`(静止计时,可重置) + `impact_start_tick`(超时保护,不重置)
- `FallDetect_Input_t` 已预留 `gyro_x_dps/y/z`(float) 和 `pitch/roll`

### 3. FreeRTOS 任务架构（代码在 `Core/Src/freertos.c`）
| 任务 | 函数 | 优先级 | 周期 | 职责 |
|------|------|:--:|------|------|
| SensorTask | `sensorTask()` | Normal | 20ms(50Hz) | I2C→MPU6050→`imuQueue` |
| FallTask | `fallTask()` | Normal | 队列驱动 | accel_sq 计算→`FallDetect_Process()`→事件分发 |
| AlarmTask | `alarmTask()` | Normal | 通知驱动 | `xTaskNotifyGive` 唤醒→`alarm_routine()`→LED+蜂鸣器 |
| defaultTask | `StartDefaultTask()` | Normal | 1s idle | 空循环 |

**IPC 对象:**

| 对象 | 类型 | 容量 | 用途 |
|------|------|:--:|------|
| `imuQueue` | MessageQueue | 20 | SensorTask→FallTask, 元素类型 `MPU6050_RawData_t` |
| `alarmSem` | BinarySemaphore | (5,0) | 原计划用信号量唤醒报警,当前**未使用** |
| `sensorSem` | BinarySemaphore | (1,0) | 按键取消报警(非阻塞轮询 `osOK`) |
| `printfMutex` | Mutex | — | printf 原子性 |

**数据流:**

```
MPU6050→I2C→SensorTask→imuQueue→FallTask→xTaskNotifyGive→AlarmTask→alarm_routine()
               20ms             accel_sq 计算            LED+蜂鸣器
                                isqrt→g 值              按键取消
                                事件分发                 10s 超时
```

### 4. 已改动的其他文件
- **Makefile**: 新增 `Core/User/algorithm/fall_detect.c` 源文件 + `-ICore/User/algorithm` include 路径
- **CMakeLists.txt**: 同样新增（保持两套构建系统一致）
- **gpio.c**: PB13 `GPIO_PULLUP`, PA11 蜂鸣器推挽输出
- **main.h**: `BUZZER_Pin/BUZZER_GPIO_Port` 宏

## 当前卡在哪里

**核心问题：AlarmTask 中 `vTaskDelay`/`osDelay` 不工作。**

### 现象
- alarmTask 里 `osDelay(200)` 或 `vTaskDelay(pdMS_TO_TICKS(200))` → **任务永远不回调度**
- alarmTask 里 `HAL_Delay(200)`(TIM7 硬件定时器) → **正常工作**
- sensorTask 里 `osDelay(20)` → **正常工作**
- fallTask 里 `printf` → **正常工作**
- GPIO 操作(WritePin/TogglePin) → 正常
- 信号量非阻塞获取(`osSemaphoreAcquire(sensorSemHandle, 0)`) → 正常
- 任务通知(`ulTaskNotifyTake`) → 正常(能收到通知进入 alarm_routine)

### 已排除的原因
- 不是栈溢出(栈从 512→1024 字节,无变化)
- 不是 FreeRTOS 全局配置问题(sensorTask 的 osDelay 正常)
- 不是阻塞等信号量机制问题(改用 task notification 后 vTaskDelay 依然失效)
- 不是优先级问题(所有任务都是 Normal)
- 不是 `configUSE_TASK_NOTIFICATIONS`(默认开启)
- 极简测试：`for(;;){TogglePin; vTaskDelay(500);}` → **正常闪烁**,但一旦**先被阻塞唤醒后再调用 vTaskDelay** 就失效

### 当前工作状态(有`HAL_Delay`的版本)

- 数据采集：传感器 50Hz → 队列 → fallTask **正常**
- 跌倒检测：三段式状态机 **正常**(自由落体→冲击→静止→确认)
- 报警触发：`xTaskNotifyGive(AlarmTaskHandle)` → `ulTaskNotifyTake` 唤醒 **正常**
- `alarm_routine()`：用 `HAL_Delay(200)` 替代 vTaskDelay **正常**
- 按键取消：`sensorSem` 非阻塞获取 `osOK` **正常**
- 报警结束：LED 灭、蜂鸣器停 **正常**

### 尚未验证
- ~~报警结束后 fallTask 是否继续打印数据？~~ 用户说"没有采集数据"
- fallTask 是否在报警锁定期(`alarm_hold_ms=30000`)后被阻塞

## 绝对不要踩的坑

| # | 坑 | 避法 |
|---|-----|------|
| 1 | **不要在 AlarmTask 里用 `osDelay`/`vTaskDelay`** | 用 `HAL_Delay` 代替,或别在"被阻塞唤醒后"调用延时函数 |
| 2 | **`osSemaphoreAcquire(sem, 超时值)` 阻塞模式不可靠** | 改用 `timeout=0` 非阻塞 + `HAL_Delay` 主动延时 |
| 3 | **PB13 必须 `GPIO_PULLUP`** | 原 `GPIO_NOPULL` 导致按键无效 |
| 4 | **`sensorSem` 初始值必须为 0** | `osSemaphoreNew(1,0,...)`,若初始=1 会立即拿到信号量等于误按键 |
| 5 | **`IMU_Data_t` 已删除,队列用 `MPU6050_RawData_t`** | 不要重新引入冗余结构体 |
| 6 | **newlib-nano 不支持 `%f`** | 用 `isqrt()` 整数开根 + `%lu.%02lu` 打印 g 值,不要用 float printf |
| 7 | **`SDA_Output()/SDA_Input()` 用 `HAL_GPIO_Init()` 开销大** | 后续优化改直接操作 MODER 寄存器,现在不动 |
| 8 | **构建用项目根目录 `make`,不用 CMake** | `make -j4`(有时 Git Bash CreateProcess 失败,用 `cmd //c make -j4`) |
| 9 | **CubeMX 文件有 USER CODE 区域** | 只在 `/* USER CODE BEGIN/END */` 标记内写代码 |
| 10 | **`impact_threshold`=1,000,000,000(~1.93g) 偏高** | 手接板测试无法触发 IMPACT,需降到 ~450,000,000(1.3g) |

## 下一步计划

1. **P0: 排查报警后 fallTask 停止打印的原因**
   - 检查 `alarm_hold_ms=30000` 是否导致 MOTIONLESS 锁死后 fallTask 的 printf 卡住
   - 在 fallTask 循环入口加 printf 确认队列是否还能收到数据
   - 确认 sensorTask 是否还在跑

2. **P0: 补全 alarm_routine 逻辑**
   - 蜂鸣器 4 秒响/1 秒停周期(当前一直响)
   - LED 改为报告警周期闪烁而非简单 Toggle

3. **P1: 根除 vTaskDelay 失效原因**
   - 用最小复现用例定位:为什么"被唤醒后"vTaskDelay 就废了

4. **P2: 通信层**
   - 创建 `comm.c/h` 抽象层(先用 UART 打印,未来切 BLE)
   - 结构化数据帧格式

5. **P3: gyro+姿态融合**
   - 互补滤波/Mahony 算法

## 关键文件速查

| 文件 | 关键内容 |
|------|----------|
| `Core/Src/freertos.c` | 3 个任务 + `alarm_routine()`, `isqrt()`, IPC 对象 |
| `Core/User/algorithm/fall_detect.c` | 状态机 4 状态 + 双重计时器 |
| `Core/User/algorithm/fall_detect.h` | `FallDetect_Input_t`, `FallDetect_Config_t`, API |
| `Core/User/bsp/bsp_soft_i2c.c` | `I2C_Delay()`(DWT), `I2C_ReadBytes()` |
| `Core/User/sensor/mpu6050.c` | `MPU6050_ReadAll()` |
| `Core/Src/syscalls.c` | `_write()` + `printfMutex` |
| `Core/Src/gpio.c` | PB13 EXTI, PA11 蜂鸣器, PC6 LED |
| `Core/Inc/FreeRTOSConfig.h` | `TICK_RATE_HZ=1000`, heap_4, NO FPU |
| `Makefile` | C_SOURCES, C_INCLUDES, `-specs=nano.specs` |
| `.vscode/c_cpp_properties.json` | IntelliSense include 路径 |
