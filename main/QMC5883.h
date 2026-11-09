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

/* 写寄存器，返回错误码（原来是 void，配置写失败完全不可见） */
esp_err_t QMC5883_Write(uint8_t reg_addr, uint8_t data);

esp_err_t QMC5883_Read(uint8_t reg_addr, uint8_t *data, size_t len);

esp_err_t QMC5883_Init(void);

uint8_t QMC5883_GetID(void);

/* 读三轴磁场（LSB）。量程溢出（OVL）时返回 ESP_ERR_INVALID_RESPONSE，
 * 与"总线读失败"区分开：前者是环境干扰该丢帧，后者才该走总线恢复。 */
esp_err_t QMC5883_GetData(int16_t *x, int16_t *y, int16_t *z);

#ifdef	__cplusplus
}
#endif


#endif //HELLO_WORLD_QMC5883_H
