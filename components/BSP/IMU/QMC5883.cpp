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
#include "Mpu6050.h"             // 总线归它所有：I2C_AddDevice / I2C_GetBus
#include "freertos/task.h"
#include "QMC5883_reg.h"

/* 注意原版写的是 "qmc5883_reg.h"（小写）。Windows 文件系统不区分大小写所以能编过，
 * 一上 Linux CI 就找不到文件。文件名大小写必须和磁盘一致。 */

#define QMC5883_ADDRESS         0x0D

/* 事务超时 20ms：与 MPU 一致，宁可丢一帧也不拖垮 50Hz 节拍 */
#define I2C_TIMEOUT_MS          20

static const char *TAG = "qmc5883";

static i2c_master_dev_handle_t s_dev = NULL;

/* 懒挂载：QMC 的句柄由总线所有者（Mpu6050.cpp）分配。
 * 这样保证"总线只被建一次"，两颗传感器不会各建一条总线互相打架。 */
static esp_err_t s_ensure_dev(void)
{
        if (s_dev != NULL) {
                return ESP_OK;
        }
        if (I2C_GetBus() == NULL) {
                ESP_LOGE(TAG, "I2C 总线还没初始化，不能挂载 QMC5883L");
                return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = I2C_AddDevice(QMC5883_ADDRESS, &s_dev);
        if (err != ESP_OK) {
                ESP_LOGE(TAG, "挂载 QMC5883L 失败: %s", esp_err_to_name(err));
                return err;
        }
        ESP_LOGI(TAG, "已挂载到 I2C 总线（7 位地址 0x%02X）", QMC5883_ADDRESS);
        return ESP_OK;
}

esp_err_t QMC5883_Write(uint8_t reg_addr, uint8_t data)
{
        esp_err_t err = s_ensure_dev();
        if (err != ESP_OK) {
                return err;
        }
        const uint8_t buf[2] = {reg_addr, data};
        return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t QMC5883_Read(uint8_t reg_addr, uint8_t *data, size_t len)
{
        esp_err_t err = s_ensure_dev();
        if (err != ESP_OK) {
                return err;
        }
        if (data == NULL || len == 0) {
                return ESP_ERR_INVALID_ARG;
        }
        return i2c_master_transmit_receive(s_dev, &reg_addr, 1, data, len, I2C_TIMEOUT_MS);
}

uint8_t QMC5883_GetID(void)
{
        uint8_t ID = 0;
        if (QMC5883_Read(QMC5883_WHO_AM_I, &ID, 1) != ESP_OK) {
                return 0;   /* 合法 chip id 是 0xFF，0 表示读取失败 */
        }
        return ID;
}

esp_err_t QMC5883_Init(void)
{
        esp_err_t err;

        err = s_ensure_dev();
        if (err != ESP_OK) {
                return err;
        }

        /* 1. 软复位（CTRL2 bit7）。热复位或上一轮配置被写坏时，寄存器可能停在
         *    半配置状态，不软复位直接写会让芯片带着旧参数跑。 */
        err = QMC5883_Write(QMC5883_CTRL2, QMC5883_CTRL2_SOFT_RST);
        if (err != ESP_OK) {
                return err;
        }
        /* 原版是 vTaskDelay(100)：那是 100 个 **tick**，不是 100ms。
         * 100Hz tick 下等于 1 秒，1000Hz 下才是 100ms —— 这种单位错不会报错，只会让人以为"卡住了"。
         * 软复位只需几 ms，这里给 20ms 足够。 */
        vTaskDelay(pdMS_TO_TICKS(20));

        /* 2. SET/RESET 周期。datasheet 明确推荐 0x01；
         *    不写这项会有明显温漂，静止时航向能漂 10° 以上。 */
        err = QMC5883_Write(QMC5883_SetReg, 0x01);
        if (err != ESP_OK) {
                return err;
        }

        /* 3. 打开数据指针回卷，这样一次事务能连状态寄存器一起读回来 */
        err = QMC5883_Write(QMC5883_CTRL2, QMC5883_CTRL2_ROL_PNT);
        if (err != ESP_OK) {
                return err;
        }

        /* 4. CTRL1 必须最后写：MODE 位一旦置为连续测量，芯片立刻按**当前**参数开始转换，
         *    如果这时上面几项还没配好，头几帧就是按默认参数采的脏数据。 */
        err = QMC5883_Write(QMC5883_ContrReg_1, QMC5883_CTRL1_50HZ_8G);
        if (err != ESP_OK) {
                return err;
        }

        /* 首次转换需要时间，等一下再读，避免拿到上电默认值 0 */
        vTaskDelay(pdMS_TO_TICKS(20));

        /* 5. 回读验证 MODE=01（连续测量）。
         *    软复位会把 CTRL1 清 0 → Standby，如果前面任何一句失败，CTRL1 就没写，
         *    芯片永远不测量，但每次事务照样 ACK —— 数据恒 0。
         *    和 MPU6050 的"停在 SLEEP 但照样 ACK"是同一类坑（mzx 分支修复）。 */
        uint8_t ctrl1 = 0;
        if (QMC5883_Read(QMC5883_ContrReg_1, &ctrl1, 1) != ESP_OK) {
                return ESP_FAIL;
        }
        if ((ctrl1 & 0x03) != 0x01) {
                ESP_LOGE(TAG, "CTRL1 回读 0x%02X，MODE!=01（仍 Standby），磁力计不会出数据", ctrl1);
                return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "初始化完成: ±8G / ODR 50Hz / OSR 512 / SET-RESET 已开 / 指针回卷已开");
        return ESP_OK;
}

esp_err_t QMC5883_GetData(int16_t *x, int16_t *y, int16_t *z)
{
        uint8_t buffer[7];

        /* 一次读 7 字节：0x00~0x05 是 XYZ（LSB 在前），0x06 是状态。
         * 借 CTRL2 的指针回卷，省掉一次单独的状态寄存器事务。 */
        esp_err_t err = QMC5883_Read(QMC5883_DATA_X_LSB, buffer, sizeof(buffer));
        if (err != ESP_OK) {
                return err;
        }

        /* LSB 在前！先拼 uint16 再转 int16，别让符号位参与移位 */
        *x = (int16_t)(((uint16_t)buffer[1] << 8) | buffer[0]);
        *y = (int16_t)(((uint16_t)buffer[3] << 8) | buffer[2]);
        *z = (int16_t)(((uint16_t)buffer[5] << 8) | buffer[4]);

        /* 全 0 = 芯片没在测量。真实地磁一定不为 0，三轴同时精确为 0 不可能。
         * 报 ESP_ERR_NOT_FINISHED 而不是总线错误：这不是线断了，是没数据。 */
        if (buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0 &&
            buffer[3] == 0 && buffer[4] == 0 && buffer[5] == 0) {
                return ESP_ERR_NOT_FINISHED;
        }

        /* 量程溢出说明铁磁物太近、数据已经削顶，这种值参与解算会把航向带偏，
         * 必须丢掉。用独立错误码和"总线读失败"区分：这不是故障，是环境。 */
        if (buffer[6] & QMC5883_ST_OVL) {
                return ESP_ERR_INVALID_RESPONSE;
        }
        return ESP_OK;
}
