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
#include "util.h"
#include "esp32_uart.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* CommunicationTask 队列元素 */
typedef struct {
    uint8_t  data[COMM_NOTIFY_FRAME_LEN];
    uint16_t len;
} CommEvent_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* 事件数据暂存（fallTask 写入，alarmTask 直接读取发送） */
static FallEvent_Data_t s_event;

/* 实时数据上传使能（失重停传，超时/报警结束恢复） */
volatile uint8_t g_data_stream = 1;

/* 报警取消标志（下行 ALARM_CANCEL 指令置位） */
volatile uint8_t g_alarm = 0;

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
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t CommTaskHandle;
const osThreadAttr_t CommTask_attributes = {
  .name = "CommTask",
  .stack_size = 512 * 4,   /* 2048B：send_at_cmd buf[256] + printf 嵌套 */
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t HeartbeatTaskHandle;
const osThreadAttr_t HeartbeatTask_attributes = {
  .name = "HeartbeatTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityRealtime,   /* 最高优先级：心跳永不饿死 */
};

/* USER CODE END Variables */
/* Definitions for imuQueue */
osMessageQueueId_t imuQueueHandle;
const osMessageQueueAttr_t imuQueue_attributes = {
  .name = "imuQueue"
};
/* Definitions for commEventQueue */
osMessageQueueId_t commEventQueueHandle;
const osMessageQueueAttr_t commEventQueue_attributes = {
  .name = "commEventQueue"
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
void commTask(void *argument);
void heartbeatTask(void *argument);
/* USER CODE END FunctionPrototypes */

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

  commEventQueueHandle = osMessageQueueNew(3, sizeof(CommEvent_t), &commEventQueue_attributes);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */

  SensorTaskHandle = osThreadNew(sensorTask, NULL, &SensorTask_attributes);

  FallTaskHandle = osThreadNew(fallTask, NULL, &FallTask_attributes);

  AlarmTaskHandle = osThreadNew(alarmTask, NULL, &AlarmTask_attributes);

  CommTaskHandle = osThreadNew(commTask, NULL, &CommTask_attributes);

  HeartbeatTaskHandle = osThreadNew(heartbeatTask, NULL, &HeartbeatTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}



/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* 报警逻辑：LED + 蜂鸣器 */
static void alarm_routine(uint32_t tick_start)
{
    g_alarm = 0;   /* 复位下行取消标志 */
    while ((osKernelGetTickCount() - tick_start) <= 15000)
    {
        if (osSemaphoreAcquire(sensorSemHandle, 0) == osOK || g_alarm)
        {
            /* 按键或下行指令取消报警 */
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
  MPU_Raw_t   raw;  // MPU6050 原始数据
  uint8_t     id;

  /* --- I2C & MPU6050 初始化 --- */
  SOFT_I2C_Init();                          // 初始化 I2C 引脚

  id = MPU6050_ReadID();               // 读取芯片 ID 验证通信
  if (id == MPU6050_ADDR)              // 检查 ID 是否正确 (0x68)
  {
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
      printf("MPU6050 ERROR: Wrong ID! (read 0x%02X)\n", id);
      for(;;)
      {
          osDelay(1000);  // 出错后挂起，不采样
      }
  }
  /* USER CODE END sensorTask */
}

/* USER CODE BEGIN Header_fallTask */

/* USER CODE END Header_fallTask */
void fallTask(void *argument)
{
  /* USER CODE BEGIN fallTask */
  MPU_Raw_t   raw;
  FallEvent_t event;
  uint32_t    ts, accel_sq, mag, g_int, g_frac;

  FallDetect_Init(&(FallDetect_Config_t)FALL_DETECT_DEFAULT_CONFIG);

  for (;;)
  {
      if (osMessageQueueGet(imuQueueHandle, &raw, NULL, osWaitForever) == osOK)
      {
          ts = osKernelGetTickCount();
          event = FallDetect_Process(&raw, ts);
          accel_sq = FallDetect_GetAccelSq();
          s_event.accel_sq    = accel_sq;
          s_event.gyro_sq     = FallDetect_GetGyroSq();

          mag    = isqrt(accel_sq);
          g_int  = mag / 16384;
          g_frac = ((mag % 16384) * 100) / 16384;
          /* ---- 事件 → 动作 ---- */
          switch (event)
          {
              case FALL_EVENT_FREEFALL:
                  g_data_stream = 0;   /* 失重 → 停止实时上传 */
                  printf("[FALL] FREE_FALL detected! g=%lu.%02lu\n", g_int, g_frac);
                  break;
              case FALL_EVENT_IMPACT:
                  printf("[FALL] IMPACT! g=%lu.%02lu\n\n", g_int, g_frac);
                  break;
              case FALL_EVENT_FALL_CONFIRMED:
                  printf("[FALL] *** ALARM: FALL DETECTED! ***\n");
                  s_event.event_type = 1;
                  osSemaphoreRelease(alarmSemHandle);
                  break;
              case FALL_EVENT_TIMEOUT:
                  g_data_stream = 1;   /* 误报超时 → 恢复实时上传 */
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
  CommEvent_t  msg;
  uint32_t     time_start;
  /* USER CODE BEGIN alarmTask */
  for (;;)
  {
      osSemaphoreAcquire(alarmSemHandle, osWaitForever);
      time_start = osKernelGetTickCount();
      alarm_routine(time_start);
      g_data_stream = 1;   /* 报警结束 → 恢复实时上传 */
      FallDetect_Reset();

      msg.len = Comm_PackNotify(msg.data, &s_event);
      osMessageQueuePut(commEventQueueHandle, &msg, 0, 0);
      s_event.event_type = 0;
  }
  /* USER CODE END alarmTask */
}


/* USER CODE BEGIN Header_commTask */
/**
* @brief  CommunicationTask：收 BLE 状态 + 发事件数据
*/
/* USER CODE END Header_commTask */
void commTask(void *argument)
{
  CommEvent_t       msg;
  uint8_t           tx_buf[COMM_NOTIFY_FRAME_LEN];  /* 最大帧（NOTIFY 15B） */
  uint16_t          tx_len;
  uint8_t           rx_buf[64];
  uint16_t          rx_len;
  Comm_Cmd_t        cmd;

  /* USER CODE BEGIN commTask */

  for (;;)
  {
    /* BLE 未就绪：推状态机 + 等就绪 */
    if (!g_ble_ready)
    {
        ESP32_Init_BLE_Step();
        osDelay(100);
        continue;
    }

    /* AT+BLEADVDATA 连续失败 → 触发重初始化 */
    if (g_ble_advdata_err)
    {
        static uint8_t s_err_cnt = 0;
        g_ble_advdata_err = 0;
        s_err_cnt++;
        if (s_err_cnt >= 5)
        {
            printf("[COMM] ADVDATA err x5, reinit BLE\n");
            s_err_cnt = 0;
            ESP32_Reset_BLE();   /* 重置状态机到 INIT，真正重初始化 */
            continue;
        }
    }

    /* 轮询接收下行指令（无中断，结构性避免风暴） */
    ESP32_RX_Poll();

    /* 100ms 节拍：等待事件帧最多 100ms（超时即到 10Hz 周期） */
    if (osMessageQueueGet(commEventQueueHandle, &msg, NULL, 100) == osOK)
    {
        /* 事件帧到达：本周期让位，不发实时帧，避免双 ADVDA */
        ESP32_Send(msg.data, msg.len);
    }
    else if (g_data_stream)
    {
        tx_len = Comm_PackRealTime(tx_buf, FallDetect_GetAccelSq(),
                                   FallDetect_GetGyroSq());
        ESP32_Send(tx_buf, tx_len);

        /* 诊断：每 100 帧（10 秒）打一次发送计数 */
        {
            static uint32_t s_send_cnt = 0;
            if (++s_send_cnt % 100 == 0)
                printf("[COMM] sent %lu frames, accel_sq=%lu\n",
                       s_send_cnt, FallDetect_GetAccelSq());
        }
    }

    /* 处理下行指令帧 */
    if (ESP32_RX_GetFrame(rx_buf, &rx_len))
    {
        if (Comm_ParseCmd(rx_buf, rx_len, &cmd))
        {
            switch (cmd.type)
            {
                case COMM_TYPE_SET_THRESHOLD:
                    FallDetect_SetParam(cmd.param_id, cmd.value);
                    printf("[CMD] SET_THRESHOLD id=%u val=%lu\n", cmd.param_id, cmd.value);
                    break;

                case COMM_TYPE_ALARM_CANCEL:
                    g_alarm = 1;
                    printf("[CMD] ALARM_CANCEL\n");
                    break;

                case COMM_TYPE_TEST_LED:
                    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6,
                                      cmd.value ? GPIO_PIN_RESET : GPIO_PIN_SET);
                    break;

                case COMM_TYPE_TEST_BUZZER:
                    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11,
                                      cmd.value ? GPIO_PIN_SET : GPIO_PIN_RESET);
                    break;

                case COMM_TYPE_CHECK_STATUS:
                    tx_len = Comm_PackStatusReply(tx_buf, (uint8_t)FallDetect_GetState());
                    ESP32_Send(tx_buf, tx_len);
                    break;

                default:
                    break;
            }
        }
    }

    ESP32_CheckAdvStatus();
  }
  /* USER CODE END commTask */
}

void heartbeatTask(void *argument)
{
  for(;;)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_9);
    osDelay(500);
  }
}

/* USER CODE END Application */

