//
// Created by l on 2026/11/2.
//

#ifndef HELLO_WORLD_QMC5883_H
#define HELLO_WORLD_QMC5883_H
#include <cstdint>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void QMC5883_Write(uint8_t reg_addr, uint8_t data) ;
esp_err_t QMC5883_Read(uint8_t reg_addr, uint8_t *data, size_t len);
void QMC5883_Init() ;
uint8_t QMC5883_GetID();
void QMC5883_GetData(int16_t *x, int16_t *y, int16_t *z);

#ifdef	__cplusplus
}
#endif


#endif //HELLO_WORLD_QMC5883_H
