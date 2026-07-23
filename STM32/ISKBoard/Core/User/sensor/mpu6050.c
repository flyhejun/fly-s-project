#include "mpu6050.h"
#include "bsp_soft_i2c.h"


/* ==================== 读取芯片 ID ==================== */

/*
 * WHO_AM_I 寄存器 (0x75) 是 MPU6050 的"身份证"
 * 出厂固化了 0x68，读不出来就说明 I2C 通信有问题
 */
uint8_t MPU6050_ReadID(void)
{
    uint8_t id;
    I2C_ReadBytes(MPU6050_ADDR, MPU6050_REG_WHO_AM_I, &id, 1);
    return id;
}


/* ==================== MPU6050 初始化 ==================== */

/*
 * 上电后 MPU6050 默认处于 SLEEP 模式，需要唤醒并配置。
 *
 * 初始化步骤：
 *   1. 写 PWR_MGMT_1 (0x6B) = 0x00 → 退出睡眠，选择内部 8MHz 时钟
 *   2. 写 SMPLRT_DIV (0x19) = 0x07 → 采样率 = 1kHz / (1+7) = 125Hz
 *   3. 写 CONFIG (0x1A) = 0x00 → 低通滤波 256Hz（保持原始数据）
 *   4. 写 GYRO_CONFIG (0x1B) = 0x00 → 陀螺仪 ±250°/s
 *   5. 写 ACCEL_CONFIG (0x1C) = 0x00 → 加速度计 ±2g
 */
void MPU6050_Init(void)
{
    /*
     * 1. 唤醒 MPU6050
     *    PWR_MGMT_1 bit6 (SLEEP) = 0 → 退出睡眠
     */
    I2C_WriteOneByte(MPU6050_ADDR, MPU6050_REG_PWR_MGMT_1, 0x00);
    HAL_Delay(100);  // 等待芯片从睡眠唤醒（数据手册建议）
    /*
     * 2. 配置采样率
     *    公式: 采样率 = 1kHz / (1 + SMPLRT_DIV) = 1k / 8 = 125Hz
     */
    I2C_WriteOneByte(MPU6050_ADDR, MPU6050_REG_SAMPLE_RATE_DIV, 0x07);
    /*
     * 3. 低通滤波器配置
     *    DLPF_CFG = 0 → 加速度带宽 260Hz, 陀螺仪带宽 256Hz
     */
    I2C_WriteOneByte(MPU6050_ADDR, MPU6050_REG_DLPF_CONFIG, 0x00);
    /*
     * 4. 陀螺仪量程：±250°/s
     *    灵敏度最高 (131 LSB/°/s)，跌倒检测用得到精细的角速度数据
     */
    I2C_WriteOneByte(MPU6050_ADDR, MPU6050_REG_GYRO_CONFIG, MPU6050_GYRO_RANGE_250DPS);

    /*
     * 5. 加速度计量程：±2g
     *    灵敏度最高 (16384 LSB/g)
     *    如果后续发现数据截顶（值卡在 ±32767），再扩大到 ±4g
     */
    I2C_WriteOneByte(MPU6050_ADDR, MPU6050_REG_ACCEL_CONFIG, MPU6050_ACCEL_RANGE_2G);
}


/* ==================== 读取全部 6 轴数据 ==================== */

/*
 * MPU6050 数据寄存器布局（大端序）：
 *
 *   0x3B: ACCEL_XOUT[15:8]  高字节
 *   0x3C: ACCEL_XOUT[7:0]   低字节
 *   0x3D: ACCEL_YOUT[15:8]
 *   0x3E: ACCEL_YOUT[7:0]
 *   0x3F: ACCEL_ZOUT[15:8]
 *   0x40: ACCEL_ZOUT[7:0]
 *   0x41: TEMP_OUT[15:8]    ← 温度，跳过
 *   0x42: TEMP_OUT[7:0]     ← 温度，跳过
 *   0x43: GYRO_XOUT[15:8]
 *   0x44: GYRO_XOUT[7:0]
 *   0x45: GYRO_YOUT[15:8]
 *   0x46: GYRO_YOUT[7:0]
 *   0x47: GYRO_ZOUT[15:8]
 *   0x48: GYRO_ZOUT[7:0]
 *
 * 一次 I2C 读 14 字节 → 效率最高
 */
uint8_t MPU6050_ReadAll(MPU_Raw_t *data)
{
    uint8_t buf[14];

    /* 从 0x3B 连续读 14 字节 */
    if(I2C_ReadBytes(MPU6050_ADDR, MPU6050_REG_ACCEL_DATA_START, buf, 14) != 0)
    {
        return 1;
    }

    /*
     * 拼接：高字节 << 8 | 低字节
     *
     * buf 索引:
     *   [0][1] = AX  [2][3] = AY  [4][5] = AZ
     *   [6][7] = 温度（跳过）
     *   [8][9] = GX  [10][11] = GY  [12][13] = GZ
     */
    data->ax_raw = (int16_t)((buf[0] << 8) | buf[1]);
    data->ay_raw = (int16_t)((buf[2] << 8) | buf[3]);
    data->az_raw = (int16_t)((buf[4] << 8) | buf[5]);

    /* buf[6] buf[7] 是温度，不取 */

    data->gx_raw = (int16_t)((buf[8]  << 8) | buf[9]);
    data->gy_raw = (int16_t)((buf[10] << 8) | buf[11]);
    data->gz_raw = (int16_t)((buf[12] << 8) | buf[13]);

    return 0;
}