/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "bsp_soft_i2c.h"
#include "mpu6050.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* IMU 数据结构：任务间通过 Queue 传递 */
typedef struct
{
    int16_t ax;
    int16_t ay;
    int16_t az;

    int16_t gx;
    int16_t gy;
    int16_t gz;

    uint32_t timestamp;

} IMU_Data_t;

typedef enum {
    STATE_NORMAL = 0,       // 正常状态
    STATE_FREE_FALL,        // 疑似失重
    STATE_IMPACT,           // 检测到冲击
    STATE_MOTIONLESS,       // 冲击后静止（确认跌倒）
} FallState_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/*
 * 跌倒检测阈值
 *
 * 量程 ±2g，灵敏度 16384 LSB/g
 * 静止 1g → accel_sq ≈ 268,000,000
 *
 * 下面统一用"平方和"来比较，避免开根号
 */

/* 失重阈值: |a| < 0.5g → accel_sq < (0.5*16384)² ≈ 67M */
/* 加速度平方和阈值 (量程±2g, 1g²≈2.68亿) */
#define FREEFALL_THRESHOLD    70000000U   /* 失重: <0.5g  */
#define IMPACT_THRESHOLD     1000000000U   /* 冲击: >2g    */
#define STILL_LOW    130000000U   /* 静止下限 0.7g */
#define STILL_HIGH    450000000U   /* 静止上限 1.3g */

/* 时间阈值 (ms, 1tick=1ms) */
#define IMPACT_WINDOW    800     /* 失重→冲击 窗口 */
#define STILL_TIME    2000     /* 冲击→静止 确认 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
 
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

osThreadId_t SensorTaskHandle;
const osThreadAttr_t SensorTask_attributes = {
  .name = "SensorTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t FallTaskHandle;
const osThreadAttr_t FallTask_attributes = {
  .name = "FallTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};

osThreadId_t LedTaskHandle;
const osThreadAttr_t LedTask_attributes = {
  .name = "LedTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};


/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for imuQueue */
osMessageQueueId_t imuQueueHandle;
const osMessageQueueAttr_t imuQueue_attributes = {
  .name = "imuQueue"
};
/* Definitions for sensorSem */
osSemaphoreId_t sensorSemHandle;
const osSemaphoreAttr_t sensorSem_attributes = {
  .name = "sensorSem"
};
/* Definitions for printfMutex */
osMutexId_t printfMutexHandle;
const osMutexAttr_t printfMutex_attributes = {
  .name = "printfMutex"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void sensorTask(void *argument);
void fallTask(void *argument);
void ledTask(void *argument);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
 
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* creation of printfMutex */
  printfMutexHandle = osMutexNew(&printfMutex_attributes);
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* creation of sensorSem */
  sensorSemHandle = osSemaphoreNew(1, 1, &sensorSem_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of imuQueue */
  imuQueueHandle = osMessageQueueNew (5, sizeof(uint32_t), &imuQueue_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* Re-create imuQueue with correct item size */
  imuQueueHandle = osMessageQueueNew(5, sizeof(IMU_Data_t), &imuQueue_attributes);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */

  SensorTaskHandle = osThreadNew(sensorTask, NULL, &SensorTask_attributes);

  FallTaskHandle = osThreadNew(fallTask, NULL, &FallTask_attributes);

  LedTaskHandle = osThreadNew(ledTask, NULL, &LedTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  
  for(;;)
  {  
    osDelay(1000);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE BEGIN Header_sensorTask */
/**
* @brief Function implementing the SensorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_sensorTask */
void sensorTask(void *argument)
{
  /* USER CODE BEGIN sensorTask */
  /* Infinite loop */
  IMU_Data_t imu;
  MPU6050_RawData_t raw;  // MPU6050 原始数据
  uint8_t id;

  /* --- I2C & MPU6050 初始化 --- */
  I2C_Init();                          // 初始化 I2C 引脚

  id = MPU6050_ReadID();               // 读取芯片 ID 验证通信
  if (id == MPU6050_ADDR)              // 检查 ID 是否正确 (0x68)
  {
      printf("MPU6050 OK!\n");

      MPU6050_Init();                  // 配置量程、采样率等
      printf("MPU6050 Init done.\n");

      for(;;)
      {
          osSemaphoreAcquire(sensorSemHandle, osWaitForever);

          // 从 MPU6050 读取全部 6 轴数据
          MPU6050_ReadAll(&raw);

          // 映射到 IMU 数据结构（发送给 FallTask 处理）
          imu.ax = raw.ax_raw;
          imu.ay = raw.ay_raw;
          imu.az = raw.az_raw;
          imu.gx = raw.gx_raw;
          imu.gy = raw.gy_raw;
          imu.gz = raw.gz_raw;
          imu.timestamp = osKernelGetTickCount();

          osMessageQueuePut(imuQueueHandle, &imu, 0, 0);

          printf("sensorTask: Accel(%d,%d,%d) Gyro(%d,%d,%d)\n",
                 raw.ax_raw, raw.ay_raw, raw.az_raw,
                 raw.gx_raw, raw.gy_raw, raw.gz_raw);
      }
  }
  else
  {
      /*
       * ID 读对不上 —— 可能的原因：
       *   1. SDA/SCL 接反了
       *   2. MPU6050 未供电
       *   3. I2C 上拉电阻没接
       *   4. I2C 时序太快（Delay 循环不够）
       */
      printf("MPU6050 ERROR: Wrong ID!\n");

      for(;;)
      {
          osDelay(1000);  // 出错后挂起，不采样
      }
  }
  /* USER CODE END sensorTask */
}

/* USER CODE BEGIN Header_fallTask */
/**
* @brief Function implementing the FallTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_fallTask */
void fallTask(void *argument)
{
  /* USER CODE BEGIN fallTask */
  IMU_Data_t imu;
  uint32_t   accel_sq;          // 加速度平方和
  FallState_t state = STATE_NORMAL;  // 状态机当前状态
  TickType_t  state_enter_tick = 0; // 进入当前状态的时间戳
  TickType_t  now;

  for(;;)
  {
    if (osMessageQueueGet(imuQueueHandle, &imu, NULL, osWaitForever) == osOK)
    {
        now = osKernelGetTickCount();
        // 第一步：计算合加速度的平方和
        accel_sq = (uint32_t)imu.ax * imu.ax
                 + (uint32_t)imu.ay * imu.ay
                 + (uint32_t)imu.az * imu.az;
        /*
         *   每个 case 做两件事：
         *     1. 判断是否需要跳转到下一个状态
         *     2. 判断是否超时（跳回 NORMAL）
         */
        switch (state)
        {
            case STATE_NORMAL:
            /*
             * 失重条件：accel_sq < FREEFALL_THRESHOLD
             * 进入失重条件后，记录时间戳，便于后续判断持续时间
             */
              if (accel_sq < FREEFALL_THRESHOLD)
              {
                  state = STATE_FREE_FALL;
                  state_enter_tick = now;
                  printf("[FALL] FREE_FALL detected! sq=%lu\n", accel_sq);
              }
              break;

        /* ---------- FREE_FALL：等待冲击 ---------- */
            case STATE_FREE_FALL:
            /*
             * 冲击条件：accel_sq > IMPACT_THRESHOLD
             * 失重后必须短时间内（IMPACT_WINDOW）检测到冲击，
             * 否则只是普通的下蹲或跳跃
             */
              if (accel_sq>IMPACT_THRESHOLD && (now-state_enter_tick)<IMPACT_WINDOW)
              {
                  state = STATE_IMPACT;
                  state_enter_tick = now;
                  printf("[FALL] IMPACT! sq=%lu\n", accel_sq);                
              }
            else if ((now - state_enter_tick) > IMPACT_WINDOW)
            {
                state = STATE_NORMAL;
            }
            break;

        /* ---------- IMPACT：等待静止确认 ---------- */
        case STATE_IMPACT:
            /*
             * 静止条件：accel_sq 回到 1g 附近（STILL_LOW ~ HIGH）
             * 撞地后人弹起再静止有一个过程，所以要持续观察
             */
            if (accel_sq>STILL_LOW && accel_sq<STILL_HIGH)
            {
                if ((now - state_enter_tick) >= STILL_TIME)
                {
                    state = STATE_MOTIONLESS;
                    state_enter_tick = now;
                    printf("[FALL] *** MOTIONLESS —— FALL CONFIRMED! ***\n");
                }
            }
            else
            {
                // 还没静止，重置计时器
                state_enter_tick = now;
            }

            // IMPACT 超时保护：冲击后太久没静止，回 NORMAL
            if ((now - state_enter_tick)>5000 && state!=STATE_MOTIONLESS)
            {
                state = STATE_NORMAL;
                printf("[FALL] MOTIONLESS timeout, back to NORMAL\n");
            }
            break;

        /* ---------- MOTIONLESS：跌倒已确认 ---------- */
        case STATE_MOTIONLESS:
            /*
             * 跌倒确认后，打印报警，然后复位状态机。
             * 复位后可以继续检测下一次跌倒。
             */
            printf("[FALL] *** ALARM: FALL DETECTED! ***\n");

            // 可以在这里加 LED 闪烁报警
            // HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_6);

            // 复位状态机，准备检测下一次
            state = STATE_NORMAL;
            break;

        default:
            state = STATE_NORMAL;
            break;
        }

        // 每次都打印当前状态（调试用）
        printf("  state=%d, sq=%lu\n", (int)state, accel_sq);
    }
  }
  /* USER CODE END fallTask */
}
void ledTask(void *argument)
{

  for(;;)
  {
    // Toggle LED or perform some action
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_6); // Example: Toggle an LED on pin B0
    osDelay(500);
  }
}


/* USER CODE END Application */

