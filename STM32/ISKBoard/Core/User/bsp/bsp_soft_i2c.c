#include "bsp_soft_i2c.h"



#define SDA_HIGH() \
HAL_GPIO_WritePin(SOFT_I2C_SDA_PORT,SOFT_I2C_SDA_PIN,GPIO_PIN_SET)


#define SDA_LOW() \
HAL_GPIO_WritePin(SOFT_I2C_SDA_PORT,SOFT_I2C_SDA_PIN,GPIO_PIN_RESET)



#define SCL_HIGH() \
HAL_GPIO_WritePin(SOFT_I2C_SCL_PORT,SOFT_I2C_SCL_PIN,GPIO_PIN_SET)


#define SCL_LOW() \
HAL_GPIO_WritePin(SOFT_I2C_SCL_PORT,SOFT_I2C_SCL_PIN,GPIO_PIN_RESET)



static void I2C_Delay(void)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = I2C_DELAY_US * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < cycles) 
    {
        /* 空循环等待 */
    }
}

void I2C_Init(void)
{
    SDA_HIGH();
    SCL_HIGH();

    /* 使能 DWT 周期计数器（用于 I2C 精确延时） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void I2C_Start(void)
{

    SDA_HIGH();
    SCL_HIGH();

    I2C_Delay();

    SDA_LOW();
    I2C_Delay();

    SCL_LOW();
    I2C_Delay();
}

void I2C_Stop(void)
{
    SDA_LOW();
    I2C_Delay();

    SCL_HIGH();
    I2C_Delay();

    SDA_HIGH();
    I2C_Delay();
}

static void SDA_Output(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin = SOFT_I2C_SDA_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

    HAL_GPIO_Init(SOFT_I2C_SDA_PORT, &GPIO_InitStruct);
}


static void SDA_Input(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin = SOFT_I2C_SDA_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;

    HAL_GPIO_Init(SOFT_I2C_SDA_PORT, &GPIO_InitStruct);
}

void I2C_SendByte(uint8_t data)
{
    uint8_t i;

    SDA_Output();

    for (i = 0; i < 8; i++)
    {
        if (data & 0x80)
        {
            SDA_HIGH();
        }
        else
        {
            SDA_LOW();
        }

        I2C_Delay();

        SCL_HIGH();
        I2C_Delay();

        SCL_LOW();
        I2C_Delay();

        data <<= 1;
    }
}

uint8_t I2C_ReadByte(uint8_t ack)
{
    uint8_t i;
    uint8_t data = 0;

    SDA_Input();

    for (i = 0; i < 8; i++)
    {
        data <<= 1;  // 先左移，腾出最低位接收新数据

        SCL_HIGH();
        I2C_Delay();

        if (HAL_GPIO_ReadPin(SOFT_I2C_SDA_PORT, SOFT_I2C_SDA_PIN))
        {
            data |= 0x01;  // 读取 SDA 电平，存入最低位
        }

        SCL_LOW();
        I2C_Delay();
    }

    if (ack)
    {
        I2C_SendAck();
    }
    else
    {
        I2C_SendNack();
    }

    return data;
}

uint8_t I2C_WaitAck(void)
{
    uint8_t ack;

    SDA_Input();

    SCL_HIGH();
    I2C_Delay();

    ack = HAL_GPIO_ReadPin(SOFT_I2C_SDA_PORT, SOFT_I2C_SDA_PIN);

    SCL_LOW();
    I2C_Delay();

    SDA_Output();

    return ack;
}

void I2C_SendAck(void)
{
    SDA_Output();
    SDA_LOW();
    I2C_Delay();

    SCL_HIGH();
    I2C_Delay();

    SCL_LOW();
    I2C_Delay();
}

void I2C_SendNack(void)
{
    SDA_Output();
    SDA_HIGH();
    I2C_Delay();

    SCL_HIGH();
    I2C_Delay();

    SCL_LOW();
    I2C_Delay();
}

/*
 * I2C 向设备指定寄存器写入 1 字节
 *
 * 时序: Start → 设备地址(W) → 寄存器地址 → 数据 → Stop
 *                     ↓              ↓          ↓
 *                 0xD0(写)      要写的寄存器   要写的值
 */
void I2C_WriteOneByte(uint8_t dev_addr, uint8_t reg_addr, uint8_t data)
{
    I2C_Start();

    I2C_SendByte(dev_addr << 1);        // 设备地址 + 写位(bit0=0)
    I2C_WaitAck();

    I2C_SendByte(reg_addr);              // 寄存器地址
    I2C_WaitAck();

    I2C_SendByte(data);                  // 数据
    I2C_WaitAck();

    I2C_Stop();
}

/*
 * I2C 从设备指定寄存器连续读取多个字节
 *
 * 时序: Start → 设备地址(W) → 起始寄存器地址 →
 *       Restart → 设备地址(R) → [读1 ACK] → [读2 ACK] → ... → [读N NACK] → Stop
 *
 * ACK = 继续读下一字节, NACK = 读完停止
 */
void I2C_ReadBytes(uint8_t dev_addr, uint8_t reg_addr, uint8_t *buf, uint8_t len)
{
    uint8_t i;

    I2C_Start();

    I2C_SendByte(dev_addr << 1);        // 设备地址 + 写位
    I2C_WaitAck();

    I2C_SendByte(reg_addr);              // 起始寄存器地址
    I2C_WaitAck();

    I2C_Start();                          // 重复开始信号

    I2C_SendByte((dev_addr << 1) | 1);   // 设备地址 + 读位(bit0=1)
    I2C_WaitAck();

    for (i = 0; i < len; i++)
    {
        /*
         * 前 len-1 个字节: 读完后回 ACK，告诉从机"继续发"
         * 最后 1 个字节:    读完后回 NACK，告诉从机"发完了，停"
         */
        buf[i] = I2C_ReadByte(i < (len - 1) ? 1 : 0);
    }

    I2C_Stop();
}