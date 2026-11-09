//
// Created by l on 2026/11/1.
//

#ifndef HELLO_WORLD_MPU6050_H
#define HELLO_WORLD_MPU6050_H

#ifdef __cplusplus
extern "C" {
#endif

static esp_err_t i2c_Init_Final();
void Mpu6050_Write(uint8_t reg, uint8_t data);
esp_err_t Mpu6050_Read( uint8_t reg, uint8_t *data,size_t n);
void Mpu6050_Init ();
void Mpu6050_Get_Data (int16_t *Accx, int16_t *Accy, int16_t *Accz, int16_t *Gyrox, int16_t *Gyroy, int16_t *Gyroz);
uint8_t Mpu6050_GetID ();

#ifdef __cplusplus
}
#endif


#endif //HELLO_WORLD_MPU6050_H
