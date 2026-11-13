//
// Created by l on 2026/11/1.
//

#ifndef HELLO_WORLD_MPU6050_H
#define HELLO_WORLD_MPU6050_H

/* 头文件必须自包含：原版靠"调用方先 include 过 stdint/esp_err"才能编过，
 * 一旦别人先 include 这个头就会报一堆类型未定义。 */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、I2C 总线：全工程唯一的初始化与所有者
 * ================================================================== */

/**
 * @brief  创建 I2C 主机总线，并把 MPU6050（0x68）挂上去
 *
 * 幂等：重复调用直接返回 ESP_OK，不会二次 i2c_new_master_bus()。
 * 两颗传感器挂同一条总线，两个驱动各自建总线会重复占用硬件资源并互相打架，
 * 所以总线一律由本模块建，QMC5883L 通过 I2C_AddDevice() 挂上来。
 *
 * @return ESP_OK   总线与 MPU 从机句柄均就绪
 *         ESP_FAIL 建总线或挂从机失败（句柄已回滚为 NULL，可重试）
 *
 * @note 必须在 Mpu6050_Init() / QMC5883_Init() 之前调用。
 * @note 配置：SDA=IO41 / SCL=IO42 / 400kHz / 硬件毛刺过滤 7 拍。
 * @note 原版这里写的是 `static esp_err_t i2c_Init_Final();` —— 头文件声明成
 *       static 会变成"每个翻译单元各自一个内部函数"，而 .cpp 里的定义不是
 *       static，链接时 main.c 找不到符号。去掉 static 即可。
 */
esp_err_t i2c_Init_Final(void);

/**
 * @brief  取总线句柄
 * @return 总线句柄；未初始化返回 NULL（QMC 驱动据此判断能不能挂设备）
 */
i2c_master_bus_handle_t I2C_GetBus(void);

/**
 * @brief  往已有总线上挂一个从机
 *
 * @param[in]  addr_7bit 7 位从机地址（QMC5883L = 0x0D）
 * @param[out] out_dev   返回的从机句柄，由调用方保存，不要释放
 *
 * @return ESP_OK                  挂载成功
 *         ESP_ERR_INVALID_STATE   总线还没建
 *         其它                    驱动层错误
 *
 * @note 这里只"挂设备"，不做在线探测 —— 器件不在也不会失败。
 *       真正的在线判断交给后续读写返回的 NACK。
 */
esp_err_t I2C_AddDevice(uint8_t addr_7bit, i2c_master_dev_handle_t *out_dev);

/**
 * @brief  总线恢复（硬件状态机复位 + 清总线）
 *
 * 恢复阶梯的 L1 动作：从机把 SDA 拉死、或某次事务超时后总线停在坏状态时调用。
 * 新驱动在事务超时时会自己复位一次状态机，所以 L1 属于"再加深一级"的兜底。
 *
 * @return ESP_OK                复位成功
 *         ESP_ERR_INVALID_STATE 总线还没建
 *
 * @note 只能在两次事务之间调用，不能在事务中途调用。
 */
esp_err_t I2C_Recover(void);

/* ==================================================================
 * 二、寄存器读写
 * ================================================================== */

/**
 * @brief  写 MPU6050 单个寄存器
 *
 * @param reg  寄存器地址
 * @param data 要写的值
 * @return ESP_OK 事务成功；其它为 I2C 错误（NACK / 超时）
 *
 * @note 返回值只代表"这次事务被 ACK"，**不代表寄存器内容真的生效了**。
 *       关键配置（PWR_MGMT_1 的休眠位）必须读回验证，见 Mpu6050_Init()。
 * @note 原版返回 void，配置序列写失败完全看不见。
 */
esp_err_t Mpu6050_Write(uint8_t reg, uint8_t data);

/**
 * @brief  从指定寄存器起连续读 n 字节
 *
 * @param reg  起始寄存器地址
 * @param data 输出缓冲（调用方保证 ≥ n 字节）
 * @param n    读取长度
 * @return ESP_OK 成功；其它为 I2C 错误
 *
 * @note 内部用"重复起始位"（写地址 → 不发 STOP → 转读），MPU6050 的连续读
 *       要求如此，不能拆成两次独立事务。
 */
esp_err_t Mpu6050_Read(uint8_t reg, uint8_t *data, size_t n);

/* ==================================================================
 * 三、器件初始化与数据读取
 * ================================================================== */

/**
 * @brief  初始化 MPU6050：软复位 → 探测 → 唤醒 → 配置 → 回读验证
 *
 * 四步，每一步都是踩过坑换来的：
 *   1. 软复位后等 150ms。datasheet 写 100ms，但克隆板实测更慢；复位未完成时
 *      后续写入会被静默丢弃，不会报错。
 *   2. who_am_i 最多试 3 次，且**不在这步中止**。复位刚结束时从机常首次不应答；
 *      另外市面大量 GY-521 实际贴的是 MPU6500(0x70) / MPU9250(0x71) /
 *      国产片(0x98)，ID 不是 0x68 但 0x3B~0x48 的寄存器布局完全兼容。
 *      在这里 return 的后果极隐蔽：芯片停在复位默认的 SLEEP 态，数据寄存器
 *      恒输出 0，而 I2C 每次事务照样 ACK —— 上层看到 imu_err=0 却 |a|=0.000g，
 *      比直接 NACK 难查十倍。
 *   3. 写入 PWR_MGMT_1 = 0x01（清 SLEEP + 选 X 轴陀螺 PLL 作时钟源）后，
 *      再配 SMPLRT_DIV / CONFIG / GYRO_CONFIG / ACCEL_CONFIG。
 *   4. 末尾回读 PWR_MGMT_1 确认 bit6(SLEEP) 已清。写完不算数，读到才算数。
 *
 * 最终配置：acc ±8g（4096 LSB/g）/ gyro ±2000dps（16.4 LSB/(°/s)）/
 *           采样率 50Hz（SMPLRT_DIV=19，十进制）/ DLPF 44Hz。
 *
 * @return ESP_OK                 已唤醒并配置完成
 *         ESP_ERR_NOT_FOUND      who_am_i 读不到（接线 / 供电 / AD0 电平）
 *         ESP_ERR_INVALID_STATE  写完仍处于 SLEEP（写保护或非原厂器件）
 *         其它                   I2C 错误
 *
 * @note 失败不等于致命：上层恢复阶梯会持续重试。可穿戴设备不能因为一颗片子
 *       虚焊就整机装死。
 */
esp_err_t Mpu6050_Init(void);

/**
 * @brief  读一帧 14 字节原始数据
 *
 * 布局：acc X/Y/Z（6 字节）+ 温度（2 字节，跳过）+ gyro X/Y/Z（6 字节），
 *       全部 MSB 在前。
 *
 * @param[out] Accx/Accy/Accz    加速度原始 LSB（÷4096 得 g）
 * @param[out] Gyrox/Gyroy/Gyroz 角速度原始 LSB（÷16.4 得 °/s）
 * @return ESP_OK 成功；其它为 I2C 错误
 *
 * @note 本函数只保证"事务成功"，不保证"数据有效"。芯片休眠时会返回全 0，
 *       上层用"六轴同时精确为 0"作为休眠判据（见 hello_world_main.c 的
 *       IMU_GetData）。没有这条判据，"通信正常但没有数据"永远不会触发恢复阶梯。
 */
esp_err_t Mpu6050_Get_Data(int16_t *Accx, int16_t *Accy, int16_t *Accz,
                           int16_t *Gyrox, int16_t *Gyroy, int16_t *Gyroz);

/**
 * @brief  读 WHO_AM_I 寄存器（0x75）
 *
 * @return 器件 ID（MPU6050 = 0x68）；**读失败返回 0**。
 *         0 不是任何合法 ID，上层据此可直接判"器件无应答"。
 */
uint8_t Mpu6050_GetID(void);

#ifdef __cplusplus
}
#endif


#endif //HELLO_WORLD_MPU6050_H
