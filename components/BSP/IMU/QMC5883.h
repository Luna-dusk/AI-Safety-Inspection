//
// Created by l on 2026/11/2.
//

#ifndef HELLO_WORLD_QMC5883_H
#define HELLO_WORLD_QMC5883_H

/* C/C++ 通用头只能用 <stdint.h>，<cstdint> 在 C 文件里会 fatal error */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 写寄存器，返回错误码（写失败必须让上层看得见） */
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
