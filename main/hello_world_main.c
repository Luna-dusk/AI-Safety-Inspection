/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gptimer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Mpu6050.h"
#include "qmc5883.h"
#include "Mpu6050_reg.h"
#include <math.h>

/* 采样周期, 必须与 Timer.c 中 TIM2 的实际中断周期一致 */
#define Sensor_DT			0.02f		/* 20ms -> 50Hz */
#define Sensor_PI			3.14159265f

/* 陀螺零偏校准采样次数 */
#define MPU6050_CALI_SAMPLES	200

/* 互补滤波系数: 0.98 偏向陀螺积分(动态响应快), 0.02 用于修正累积漂移 */
#define COMP_FILTER_ALPHA	0.98f

int16_t AX, AY, AZ, GX, GY, GZ, HX, HY, HZ;

/* 陀螺三轴零偏(LSB), 由 MPU6050_CalibrateGyro() 计算 */
static float GyroBiasX = 0.0f;
static float GyroBiasY = 0.0f;
static float GyroBiasZ = 0.0f;
static uint8_t GyroCalibrated = 0;

static float QMC5883_Declination = -3.0f;	/* 磁偏角, 单位度, 按所在地区填写 */

static const char *TAG = "HELLO_WORLD";


void IMU_CalibrateGyro(void) {
    int32_t sumX = 0, sumY = 0, sumZ = 0;
    int16_t aX, aY, aZ , bX, bY, bZ;

    for (uint16_t i = 0; i < MPU6050_CALI_SAMPLES; i++) {
        Mpu6050_Get_Data(&aX, &aY, &aZ, &bX, &bY, &bZ);
        sumX += aX;
        sumY += aY;
        sumZ += aZ;

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    GyroBiasX = (float)sumX / (float)MPU6050_CALI_SAMPLES;
    GyroBiasY = (float)sumY / (float)MPU6050_CALI_SAMPLES;
    GyroBiasZ = (float)sumZ / (float)MPU6050_CALI_SAMPLES;


    GyroCalibrated = 1;
}

uint8_t IMU_IsCalibrated(void) {
    return GyroCalibrated;
}

float IMU_GetPitch(float *pitch, int AX, int AY, int AZ) {
    /* 绕 Y 轴旋转为俯仰, 由 X 轴重力分量与 YZ 平面合力求解 */
    *pitch = atan2f(-(float)AX, sqrtf((float)AY * (float)AY + (float)AZ * (float)AZ))
             * 180.0f / Sensor_PI;
    return *pitch;
}

float IMU_GetRoll(float *roll, int AX, int AY, int AZ) {

    /* 绕 X 轴旋转为横滚, 由 Y/Z 轴重力分量求解, X 轴分量不参与计算 */
    (void)AX;
    *roll = atan2f((float)AY, (float)AZ) * 180.0f / Sensor_PI;
    return *roll;

}


void IMU_GetData(float *pitch, float *roll, float *yaw) {
    float acc_pitch, acc_roll, mag_yaw;
    float angle_pitch, angle_roll, angle_yaw;
    float groy_x_rate, groy_y_rate, groy_z_rate;
    float BX, BY;
    float roll_rad, pitch_rad;

    angle_pitch = *pitch;
    angle_roll  = *roll;
    angle_yaw   = *yaw;

    /* 获取传感器原始数据 */
    Mpu6050_Get_Data(&AX, &AY, &AZ, &GX, &GY, &GZ);
    QMC5883_GetData(&HX, &HY, &HZ);

    /* 角速度换算为 °/s, 并扣除零偏 */
    groy_x_rate = ((float)GX - GyroBiasX) / MPU6050_GYRO_SENS_2000DPS;
    groy_y_rate = ((float)GY - GyroBiasY) / MPU6050_GYRO_SENS_2000DPS;
    groy_z_rate = ((float)GZ - GyroBiasZ) / MPU6050_GYRO_SENS_2000DPS;

    IMU_GetPitch(&acc_pitch, AX, AY, AZ);
    IMU_GetRoll(&acc_roll, AX, AY, AZ);

    /* ---- 磁力计倾角补偿: 把机体坐标系磁场投影到水平面 ---- */
    roll_rad  = angle_roll * Sensor_PI / 180.0f;
    pitch_rad = angle_pitch * Sensor_PI / 180.0f;

    BX = (float)HX * cosf(pitch_rad)
       + (float)HY * sinf(roll_rad) * sinf(pitch_rad)
       + (float)HZ * cosf(roll_rad) * sinf(pitch_rad);
    BY = (float)HY * cosf(roll_rad)
       - (float)HZ * sinf(roll_rad);

    mag_yaw = atan2f(BY, BX) * 180.0f / Sensor_PI;
    mag_yaw += QMC5883_Declination;	/* 补偿磁偏角, 得到真北朝向 */

    /* ---- 互补滤波: 陀螺积分提供短期精度, 加速度计/磁力计修正长期漂移 ---- */
    angle_pitch = COMP_FILTER_ALPHA * (angle_pitch + Sensor_DT * groy_y_rate)
                + (1.0f - COMP_FILTER_ALPHA) * acc_pitch;
    angle_roll  = COMP_FILTER_ALPHA * (angle_roll + Sensor_DT * groy_x_rate)
                + (1.0f - COMP_FILTER_ALPHA) * acc_roll;
    angle_yaw   = COMP_FILTER_ALPHA * (angle_yaw + Sensor_DT * groy_z_rate)
                + (1.0f - COMP_FILTER_ALPHA) * mag_yaw;

    /* 归一化到 [-180, 180] */
    while (angle_yaw > 180.0f)  angle_yaw -= 360.0f;
    while (angle_yaw < -180.0f) angle_yaw += 360.0f;

    *pitch = angle_pitch;
    *roll  = angle_roll;
    *yaw   = angle_yaw;

}


void Timer_Init() {
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t gptimer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT ,
        .direction = GPTIMER_COUNT_DOWN,
        .resolution_hz = 1*1000*1000,
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&gptimer_config , &gptimer));
}

static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    // 仅发送通知，不进行任何I2C或计算操作
    vTaskNotifyGiveFromISR(sensor_task_handle, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}


void sensor_task(void *pvParameter) {
    i2c_Init_Final();
    Mpu6050_Init();
    QMC5883_Init();





}








void app_main(void)
{



}
