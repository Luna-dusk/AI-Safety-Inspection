//
// Created by l on 2026/11/2.
//

#ifndef HELLO_WORLD_QMC5883_H
#define HELLO_WORLD_QMC5883_H

/* 原来这里是 <cstdint> —— 那是 C++ 标准库头，C 文件 include 会直接
 * "fatal error: cstdint: No such file or directory"。C/C++ 通用头必须是 <stdint.h> */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  写 QMC5883L 单个寄存器
 *
 * @param reg_addr 寄存器地址
 * @param data     要写的值
 * @return ESP_OK 事务成功；其它为 I2C 错误（器件不在线时在这里就失败）
 *
 * @note 首次调用会懒挂载从机（内部走 I2C_AddDevice），所以不需要单独的
 *       "add device"步骤；但总线必须已由 i2c_Init_Final() 建好。
 * @note 原版返回 void，配置写失败完全不可见。
 */
esp_err_t QMC5883_Write(uint8_t reg_addr, uint8_t data);

/**
 * @brief  从指定寄存器起连续读 len 字节
 *
 * @param reg_addr 起始寄存器地址
 * @param data     输出缓冲（调用方保证 ≥ len 字节）
 * @param len      读取长度
 * @return ESP_OK 成功；其它为 I2C 错误
 *
 * @note 因为 CTRL2 打开了指针回卷（ROL_PNT），从 0x00 一次读 7 字节可以
 *       顺带把状态寄存器 0x06 读回来，省掉一次独立事务。
 */
esp_err_t QMC5883_Read(uint8_t reg_addr, uint8_t *data, size_t len);

/**
 * @brief  初始化 QMC5883L：软复位 → SET/RESET → 指针回卷 → CTRL1 → 回读验证
 *
 * 写入顺序不能改，每一步都有理由：
 *   1. 软复位（CTRL2 bit7）会把 CTRL1 清 0，芯片回到 Standby —— 所以 CTRL1
 *      必须最后写；
 *   2. SET/RESET 周期（0x0B = 0x01）必须在 CTRL1 之前。不写这项会有明显温漂，
 *      静止时航向能漂 10° 以上；
 *   3. CTRL1 一旦置 MODE=连续测量，芯片立刻按**当前**参数开始转换，此时再改
 *      其它寄存器，头几帧就是按旧参数采的脏数据。
 *
 * 末尾回读 CTRL1 确认 (ctrl1 & 0x03) == 0x01。这一步是必须的：如果前面任何
 * 一次写失败，CTRL1 就没写，芯片永远停在 Standby，数据寄存器恒输出 0，
 * 但 I2C 照样 ACK —— 和 MPU6050 的"假在线"是同一类坑，日志上完全看不出来。
 *
 * 配置结果：±8G（30 LSB/µT）/ ODR 50Hz / OSR 512 / 连续测量 / 指针回卷。
 *
 * @return ESP_OK                  已进入连续测量
 *         ESP_ERR_INVALID_STATE   CTRL1 回读 MODE != 01（仍在 Standby）
 *         其它                    I2C 错误
 */
esp_err_t QMC5883_Init(void);

/**
 * @brief  读 WHO_AM_I 寄存器（0x0D）
 *
 * @return 器件 ID（QMC5883L = 0xFF）；**读失败返回 0**。
 *         0 不是合法 ID，上层据此判"器件无应答"。
 */
uint8_t QMC5883_GetID(void);

/**
 * @brief  读三轴磁场原始值
 *
 * @param[out] x/y/z 原始 LSB
 *
 * @return ESP_OK                  正常
 *         ESP_ERR_INVALID_RESPONSE 量程溢出（OVL）：铁磁物太近、数据已削顶。
 *                                  属于环境干扰，该丢帧，但**不该**触发总线恢复
 *         ESP_ERR_NOT_FINISHED     三轴全 0：芯片没在测量（Standby / 未正确配置）。
 *                                  不是总线故障，但也没有可用数据
 *         其它                     I2C 总线错误
 *
 * @note **字节序是 LSB 在前**（0x00 = X 低字节），与 MPU6050 的 MSB 在前相反。
 *       抄错顺序不会报错，只会得到 256 倍量级的乱数。
 * @note 换算：±8G 量程 → 3000 LSB/G → 30 LSB/µT。
 *       用 10.24 是 HMC5883L 的值（型号抄错），会让读数小 3 倍且不报错。
 * @note 状态寄存器 bit0 = DRDY、bit1 = OVL、bit2 = DOR。目前只处理 OVL；
 *       不查 DRDY 意味着 50Hz 读取与 50Hz ODR 相位漂移时可能读到重复值。
 */
esp_err_t QMC5883_GetData(int16_t *x, int16_t *y, int16_t *z);

#ifdef	__cplusplus
}
#endif


#endif //HELLO_WORLD_QMC5883_H
