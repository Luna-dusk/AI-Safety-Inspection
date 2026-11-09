//
// Created by l on 2026/11/2.
//

#include "QMC5883.h"
#include <stdio.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"   // pdMS_TO_TICKS 来自这里
#include "Mpu6050_reg.h"
#include "freertos/task.h"
#include "qmc5883_reg.h"
#include "driver/i2c.h"

#define I2C_NUM I2C_NUM_0

#define I2C_MASTER_RX_BUF_DISABLE 0
#define I2C_MASTER_TX_BUF_DISABLE 0

typedef enum {
#define I2C_MASTER_ACK 0x0
#define I2C_MASTER_NACK 0x01
}i2c_master_ack_type_t;

#define QMC5883_Write_ADDRESS 	0x1A
#define QMC5883_Read_ADDRESS 	0x1B
#define	QMC5883_ADDRESS         0x0D


void QMC5883_Write(uint8_t reg_addr, uint8_t data) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    //启动标志位
    i2c_master_write_byte(cmd,QMC5883_Write_ADDRESS,true);
    //写入MPU6050的写地址，给ACK应答
    i2c_master_write_byte(cmd, reg_addr, true);
    //指定要写入的寄存器的地址
    i2c_master_write_byte(cmd, data, true);
    //写入数据
    i2c_master_stop(cmd);
    //结束标志位
    i2c_master_cmd_begin(I2C_NUM, cmd, pdMS_TO_TICKS(1000));
    //正式发送这一帧
    i2c_cmd_link_delete(cmd);
    //释放资源
}


esp_err_t QMC5883_Read(uint8_t reg_addr, uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, QMC5883_Write_ADDRESS,true);
    i2c_master_write_byte(cmd, reg_addr, true);

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, QMC5883_Read_ADDRESS,true);

    for (size_t i = 0; i < len - 1; i++) {
        i2c_master_read_byte(cmd, &data[i], static_cast<i2c_ack_type_t>(I2C_MASTER_ACK));
    }
    i2c_master_read_byte(cmd, &data[len-1], static_cast<i2c_ack_type_t>(I2C_MASTER_NACK));
    //i2c读连续多地址数据的逻辑

    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(I2C_NUM, cmd,pdMS_TO_TICKS(1000) );

    i2c_cmd_link_delete(cmd);

    return err;

}



void QMC5883_Init() {
    vTaskDelay(100);

    QMC5883_Write(QMC5883_ContrReg_1,0x15);

    QMC5883_Write(0x0B,0x01);



}

uint8_t QMC5883_GetID() {
    uint8_t ID = 0;
    QMC5883_Read(QMC5883_WHO_AM_I, &ID, 1);
    return ID;
}

void QMC5883_GetData(int16_t *x, int16_t *y, int16_t *z) {
    uint8_t buffer[6];

    QMC5883_Read(QMC5883_X_H, buffer, 6);

    *x = (int16_t)(buffer[1] << 8) + buffer[0];
    *y = (int16_t)(buffer[3] << 8) + buffer[2];
    *z = (int16_t)(buffer[5] << 8) + buffer[4];
    
}