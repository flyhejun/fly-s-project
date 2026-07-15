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
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
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
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};

osThreadId_t LedTaskHandle;
const osThreadAttr_t LedTask_attributes = {
  .name = "LedTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t TriggleTaskHandle;
const osThreadAttr_t TriggleTask_attributes = {
  .name = "TriggleTask",
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

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void sensorTask(void *argument);
void fallTask(void *argument);
void ledTask(void *argument);
void triggleTask(void *argument);
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
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* creation of sensorSem */
  sensorSemHandle = osSemaphoreNew(1, 0, &sensorSem_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of imuQueue */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */

  SensorTaskHandle = osThreadNew(sensorTask, NULL, &SensorTask_attributes);

  FallTaskHandle = osThreadNew(fallTask, NULL, &FallTask_attributes);

  LedTaskHandle = osThreadNew(ledTask, NULL, &LedTask_attributes);

  TriggleTaskHandle = osThreadNew(triggleTask, NULL, &TriggleTask_attributes);
  if(TriggleTaskHandle == NULL)
{
    printf("Trigger create failed\r\n");
}
else
{
    printf("Trigger create success\r\n");
}

  imuQueueHandle = osMessageQueueNew (5, sizeof(IMU_Data_t), &imuQueue_attributes);
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
    osDelay(1);
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

  for(;;)
  {

    osSemaphoreAcquire(sensorSemHandle, osWaitForever);
    // Simulate reading IMU data
    imu.ax = 100; // Example value
    imu.ay = 200; // Example value
    imu.az = 300; // Example value
    imu.gx = 400; // Example value
    imu.gy = 500; // Example value
    imu.gz = 600; // Example value

    imu.timestamp++;

    // Send the IMU data to the queue
    osMessageQueuePut(imuQueueHandle, &imu, 0, 0);
    
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
  /* Infinite loop */
  IMU_Data_t imu;

  for(;;)
  {
    if (osMessageQueueGet(imuQueueHandle, &imu, NULL, osWaitForever) == osOK)
    {
      // Process the received IMU data
      printf("Received IMU data:\n");
      printf("  Accel: (%d, %d, %d)\n", imu.ax, imu.ay, imu.az);
      printf("  Gyro: (%d, %d, %d)\n", imu.gx, imu.gy, imu.gz);
    }

    osDelay(1000);
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

void triggleTask(void *argument)
{
  for(;;)
  {
    // Simulate triggering the sensor task
   
    osSemaphoreRelease(sensorSemHandle);
    osDelay(1000); // Trigger every 1 second
  }
}
/* USER CODE END Application */

