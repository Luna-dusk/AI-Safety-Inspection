//
// Created by l on 2026/11/1.
//
#include <stdio.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"   // pdMS_TO_TICKS 来自这里
#include "Mpu6050_reg.h"
#include "driver/i2c.h"
#include "freertos/task.h"

#define I2C_SDA_GPIO 8
#define I2C_SCL_GPIO 9
#define I2C_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000

#define MPU6050_ADDRESS 0x68
#define MPU6050_Write_ADDRESS 0XD0
#define MPU6050_Read_ADDRESS 0XD1

#define I2C_MASTER_RX_BUF_DISABLE 0
#define I2C_MASTER_TX_BUF_DISABLE 0

typedef enum {
        #define I2C_MASTER_ACK 0x0
        #define I2C_MASTER_NACK 0x01
}i2c_master_ack_type_t;


static esp_err_t i2c_Init_Final() {

        i2c_config_t conf{
                .mode = I2C_MODE_MASTER,
                .sda_io_num = I2C_SDA_GPIO,
                .scl_io_num = I2C_SCL_GPIO,
                .sda_pullup_en = GPIO_PULLUP_ENABLE,
                .scl_pullup_en = GPIO_PULLUP_ENABLE,
                .master.clk_speed = I2C_MASTER_FREQ_HZ,
                .clk_flags = 0,
        };
        //配置I2C的各个参数，包括了I2C的主从模式、I2C引脚、是否上拉、时钟频率以及I2C时钟源

        esp_err_t ear = i2c_param_config(I2C_NUM, &conf);
        if (ear != ESP_OK) {
                return ear;
        }
        //初始化配置好的I2C引脚

        return i2c_driver_install(I2C_NUM, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
        //下载I2C的驱动
}
//一次性配置I2C的GPIO

void Mpu6050_Write(uint8_t reg, uint8_t data) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();

        i2c_master_start(cmd);
        //启动标志位
        i2c_master_write_byte(cmd,MPU6050_Write_ADDRESS,true);
        //写入MPU6050的写地址，给ACK应答
        i2c_master_write_byte(cmd, reg, true);
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
//MPU6050的写逻辑

esp_err_t Mpu6050_Read( uint8_t reg, uint8_t *data,size_t n) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();

        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, MPU6050_Write_ADDRESS,true);
        i2c_master_write_byte(cmd, reg, true);

        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, MPU6050_Read_ADDRESS,true);

        for (size_t i = 0; i < n - 1; i++) {
                i2c_master_read_byte(cmd, &data[i], static_cast<i2c_ack_type_t>(I2C_MASTER_ACK));
        }
        i2c_master_read_byte(cmd, &data[n-1], static_cast<i2c_ack_type_t>(I2C_MASTER_NACK));
        //i2c读连续多地址数据的逻辑

        i2c_master_stop(cmd);

        esp_err_t err = i2c_master_cmd_begin(I2C_NUM, cmd,pdMS_TO_TICKS(1000) );

        i2c_cmd_link_delete(cmd);

        return err;
}
//MPU6050的读逻辑

void Mpu6050_Init () {
        Mpu6050_Write(MPU6050_PWR_MGMT_1, MPU6050_PWR1_DEVICE_RESET);
        vTaskDelay(pdMS_TO_TICKS(100));

        /* ---- 3. 器件配置 ---- */
        /* 唤醒并选择 X 轴陀螺 PLL 作为时钟源(比内部 RC 振荡器稳定得多) */
        Mpu6050_Write(MPU6050_PWR_MGMT_1, MPU6050_PWR1_CLKSEL_PLL_X);
        /* 六轴全部使能(不进入休眠) */
        Mpu6050_Write(MPU6050_PWR_MGMT_2, 0x00);
        /* 采样率分频: 输出率 = 1kHz / (1 + 9) = 100Hz, 与 TIM2 的 100Hz 对齐 */
        Mpu6050_Write(MPU6050_SMPLRT_DIV, 19);
        /* 数字低通滤波 DLPF_CFG = 3, 带宽 44Hz(陀螺) / 42Hz(加速度), 抑制高频噪声 */
        Mpu6050_Write(MPU6050_CONFIG, 0x03);
        /* 陀螺满量程 ±2000dps */
        Mpu6050_Write(MPU6050_GYRO_CONFIG, MPU6050_GYRO_FS_2000);
        /* 加速度满量程 ±16g */
        Mpu6050_Write(MPU6050_ACCEL_CONFIG, MPU6050_ACCEL_FS_16);

        vTaskDelay(pdMS_TO_TICKS(100));

}

void Mpu6050_Get_Data (int16_t *Accx, int16_t *Accy, int16_t *Accz, int16_t *Gyrox, int16_t *Gyroy, int16_t *Gyroz) {
       uint8_t buffer[14];

        esp_err_t err =  Mpu6050_Read(MPU6050_ACCEL_XOUT_H, buffer,14 );

        if (err != ESP_OK) {
                return ;
        }

        *Accx = (int16_t)(((uint16_t)buffer[0] << 8) | buffer[1]);
        *Accy = (int16_t)(((uint16_t)buffer[2] << 8) | buffer[3]);
        *Accz = (int16_t)(((uint16_t)buffer[4] << 8) | buffer[5]);

        /* buffer[6]、buffer[7] 为温度, 本工程未使用 */

        *Gyrox = (int16_t)(((uint16_t)buffer[8]  << 8) | buffer[9]);
        *Gyroy = (int16_t)(((uint16_t)buffer[10] << 8) | buffer[11]);
        *Gyroz = (int16_t)(((uint16_t)buffer[12] << 8) | buffer[13]);

}

uint8_t Mpu6050_GetID () {
        uint8_t ID = 0;
        Mpu6050_Read(MPU6050_WHO_AM_I, &ID , 1);
        return ID;
}