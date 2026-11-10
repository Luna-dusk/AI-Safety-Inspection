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

/* ---- I2C 总线：全工程唯一的初始化与所有者 ----
 * MPU6050 / QMC5883L 挂同一条总线，两个驱动各自 i2c_new_master_bus() 会重复占用
 * 硬件资源并互相打架。所以总线归这里，QMC 通过 I2C_AddDevice 挂上来。
 *
 * 注意原版这里写的是 `static esp_err_t i2c_Init_Final();` —— 头文件声明成 static
 * 会变成"每个翻译单元各自一个内部函数"，而 .cpp 里的定义不是 static，
 * 链接时 main.c 找不到符号。去掉 static 即可。 */
esp_err_t i2c_Init_Final(void);

/* 取总线句柄；未初始化返回 NULL */
i2c_master_bus_handle_t I2C_GetBus(void);

/* 挂一个从机，返回该从机的 device handle（驱动自己保存） */
esp_err_t I2C_AddDevice(uint8_t addr_7bit, i2c_master_dev_handle_t *out_dev);

/* 总线恢复：硬件状态机复位 + 清总线。
 * 只在事务间隙调用，不要在事务中途调。 */
esp_err_t I2C_Recover(void);

/* 写寄存器。原版返回 void，配置序列写失败完全看不见 —— 改成返回错误码 */
esp_err_t Mpu6050_Write(uint8_t reg, uint8_t data);

esp_err_t Mpu6050_Read(uint8_t reg, uint8_t *data, size_t n);

esp_err_t Mpu6050_Init(void);

/* 读 14 字节原始值。原版返回 void、失败直接 return，上层无法知道这一帧是坏的 */
esp_err_t Mpu6050_Get_Data(int16_t *Accx, int16_t *Accy, int16_t *Accz,
                           int16_t *Gyrox, int16_t *Gyroy, int16_t *Gyroz);

uint8_t Mpu6050_GetID(void);

#ifdef __cplusplus
}
#endif


#endif //HELLO_WORLD_MPU6050_H
