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
#include "fall_detect.h"
#include "comm_protocol.h"
#include "imubuf.h"
#include "util.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

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
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t AlarmTaskHandle;
const osThreadAttr_t AlarmTask_attributes = {
  .name = "AlarmTask",
  .stack_size = 256 * 4,
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
/* Definitions for alarmSem */
osSemaphoreId_t alarmSemHandle;
const osSemaphoreAttr_t alarmSem_attributes = {
  .name = "alarmSem"
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
void alarmTask(void *argument);
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
  sensorSemHandle = osSemaphoreNew(1, 0, &sensorSem_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  alarmSemHandle = osSemaphoreNew(1, 0, &alarmSem_attributes);
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of imuQueue */
 
  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* Re-create imuQueue with correct item size */
  imuQueueHandle = osMessageQueueNew(20, sizeof(MPU_Raw_t), &imuQueue_attributes);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */

  SensorTaskHandle = osThreadNew(sensorTask, NULL, &SensorTask_attributes);

  FallTaskHandle = osThreadNew(fallTask, NULL, &FallTask_attributes);

  AlarmTaskHandle = osThreadNew(alarmTask, NULL, &AlarmTask_attributes);
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

/* 报警逻辑：LED + 蜂鸣器 */
static void alarm_routine(uint32_t tick_start)
{
    while ((osKernelGetTickCount() - tick_start) <= 15000)
     {
        if (osSemaphoreAcquire(sensorSemHandle, 0) == osOK) {
            /* 按键取消报警 */
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_SET);
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11, GPIO_PIN_RESET);
            return;
        }

        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_6);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11, GPIO_PIN_SET);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    /* 超时关闭 */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11, GPIO_PIN_RESET);
    return;
}

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
  MPU_Raw_t raw;  // MPU6050 原始数据
  uint8_t id;

  /* --- I2C & MPU6050 初始化 --- */
  I2C_Init();                          // 初始化 I2C 引脚

  id = MPU6050_ReadID();               // 读取芯片 ID 验证通信
  if (id == MPU6050_ADDR)              // 检查 ID 是否正确 (0x68)
  {
      printf("MPU6050 OK!\n");

      MPU6050_Init();                  // 配置量程、采样率等

      for(;;)
      {
          osDelay(20);  // 20ms 周期采集 (50Hz)
          if(MPU6050_ReadAll(&raw) == 0)
          {
            osMessageQueuePut(imuQueueHandle, &raw, 0, 0);
          }   
      }
  }
  else
  {
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
  MPU_Raw_t   raw;
  FallDetect_Input_t  input;
  FallEvent_t         event;
  uint32_t            mag;
  uint32_t            g_int, g_frac;
  /* ---- 初始化跌倒检测算法 ---- */
  const FallDetect_Config_t config = {
      .freefall_threshold     = 70000000U,
      .impact_threshold       = 1000000000U,
      .still_low              = 130000000U,
      .still_high             = 450000000U,
      .impact_window_ms       = 800,
      .still_time_ms          = 2000,
      .impact_timeout_ms      = 5000,
      .alarm_hold_ms          = 15000,   /* 报警后 15 秒不响应新跌倒 */
      .gyro_thr  = 200000000U, /* 约108°/s等效角速度 */
  };

  FallDetect_Init(&config);
  IMUBuf_Init();

  for (;;) 
  {
      if (osMessageQueueGet(imuQueueHandle, &raw, NULL, osWaitForever) == osOK) 
      {
          /* ---- 数据预处理 ---- */
          input.accel_sq = (uint32_t)raw.ax_raw * raw.ax_raw
                         + (uint32_t)raw.ay_raw * raw.ay_raw
                         + (uint32_t)raw.az_raw * raw.az_raw;
  
          input.gyro_sq   = (uint32_t)raw.gx_raw * raw.gx_raw
                          + (uint32_t)raw.gy_raw * raw.gy_raw
                          + (uint32_t)raw.gz_raw * raw.gz_raw;

          input.timestamp_ms = osKernelGetTickCount();
          /* ---- 调用跌倒算法 ---- */
          IMUBuf_PushData(input.timestamp_ms, &raw, input.accel_sq, input.gyro_sq);
          event = FallDetect_Process(&input);
        
          mag    = isqrt(input.accel_sq);
          g_int  = mag / 16384;
          g_frac = ((mag % 16384) * 100) / 16384;
          /* ---- 事件 → 动作 ---- */
          switch (event) 
          {
              case FALL_EVENT_FREEFALL:
                  printf("[FALL] FREE_FALL detected! g=%lu.%02lu\n", g_int, g_frac);
                  break;
              case FALL_EVENT_IMPACT:
                  printf("[FALL] IMPACT! g=%lu.%02lu\n\n", g_int, g_frac);
                  break;
              case FALL_EVENT_FALL_CONFIRMED:
                  printf("[FALL] *** ALARM: FALL DETECTED! ***\n");
                  IMUBuf_Trigger(0);
                  osSemaphoreRelease(alarmSemHandle);
                  break;
              case FALL_EVENT_TIMEOUT:
                  printf("[FALL] timeout, back to NORMAL\n");
                  break;
              default:
                  break;
          }
         
      }
  }
  /* USER CODE END fallTask */
}
 void alarmTask(void *argument)
{
  FallEvent_Data_t event;

  /* USER CODE BEGIN alarmTask */
  for (;;)
  {
      osSemaphoreAcquire(alarmSemHandle, osWaitForever);

      uint32_t time_start = osKernelGetTickCount();
      alarm_routine(time_start);
      FallDetect_Reset();
      IMUBuf_GetPeak(&event);

      /* 打包并打印帧内容（验证二进制帧格式） */
      uint8_t notify_buf[COMM_NOTIFY_FRAME_LEN];
      uint16_t len = Comm_PackNotify(notify_buf, &event);
      dump_hex(notify_buf, len);

      IMUBuf_Dump();
      IMUBuf_Reset();
  }
  /* USER CODE END alarmTask */
}


/* USER CODE END Application */

