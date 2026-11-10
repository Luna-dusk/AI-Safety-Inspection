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
#include "Mpu6050.h"
#include "freertos/task.h"

/* 为什么换掉 driver/i2c.h（legacy）：
 *   1. legacy 事务超时后硬件状态机停在坏状态，之后每次读都失败，永远；
 *      新驱动超时会在内部自动做 FSM 复位（i2c_master.c 里 s_i2c_hw_fsm_reset），下一帧还能用。
 *   2. 新驱动提供 i2c_master_bus_reset()，能处理"从机把 SDA 拉死"——这是总线恢复阶梯的 L1，
 *      legacy 完全没有对应 API。
 *   3. 顺带解决一个编译问题：legacy 的 i2c_config_t 要写 .master.clk_speed，
 *      而 C++ 不允许嵌套指定初始化器，原写法在 .cpp 里必定编不过。 */

#define I2C_SDA_GPIO 41
#define I2C_SCL_GPIO 42

/* 400kHz：100kHz 下两颗传感器 50Hz 就占掉约 13% 带宽，留给触摸的余量太少 */
#define I2C_MASTER_FREQ_HZ 400000

/* 事务超时 20ms ≈ 一个采样周期。卡死时宁可丢这一帧，也不能让 50Hz 节拍被拖垮
 * （legacy 原版用的是 1000ms，一次卡死直接丢 50 帧） */
#define I2C_TIMEOUT_MS 20

#define MPU6050_ADDRESS 0x68

static const char *TAG = "mpu6050";

/* 总线句柄与 MPU 自己的从机句柄。总线句柄全工程唯一，从机句柄每个器件一个 */
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* ------------------------------------------------------------------ */
/* I2C 总线                                                            */
/* ------------------------------------------------------------------ */

esp_err_t i2c_Init_Final(void)
{
        /* 幂等：QMC 和恢复流程都可能再次调用，重复建总线会直接报资源冲突 */
        if (s_bus != NULL) {
                return ESP_OK;
        }

        /* 注意这里不用 `i2c_master_bus_config_t cfg{ .i2c_port = ... }`：
         * C++ 的指定初始化器限制比 C 严，字段必须按声明顺序且不能嵌套，
         * 写错一个就报 "either all initializer clauses should be designated"。
         * 先零初始化再逐字段赋值，可读性和可移植性都更好。 */
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = I2C_NUM_0;
        bus_cfg.sda_io_num = (gpio_num_t)I2C_SDA_GPIO;
        bus_cfg.scl_io_num = (gpio_num_t)I2C_SCL_GPIO;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        /* 7 个时钟周期的毛刺过滤：传感器板引线长、又贴着喇叭，不滤会偶发 NACK */
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK) {
                ESP_LOGE(TAG, "创建 I2C 总线失败: %s", esp_err_to_name(err));
                s_bus = NULL;
                return err;
        }

        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = MPU6050_ADDRESS;
        dev_cfg.scl_speed_hz = I2C_MASTER_FREQ_HZ;

        err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
                ESP_LOGE(TAG, "挂载 MPU6050 失败: %s", esp_err_to_name(err));
                i2c_del_master_bus(s_bus);
                s_bus = NULL;
                s_dev = NULL;
                return err;
        }

        ESP_LOGI(TAG, "I2C 就绪: SDA=IO%d SCL=IO%d %dkHz", I2C_SDA_GPIO, I2C_SCL_GPIO,
                 I2C_MASTER_FREQ_HZ / 1000);
        return ESP_OK;
}

i2c_master_bus_handle_t I2C_GetBus(void)
{
        return s_bus;
}

esp_err_t I2C_AddDevice(uint8_t addr_7bit, i2c_master_dev_handle_t *out_dev)
{
        if (s_bus == NULL || out_dev == NULL) {
                return ESP_ERR_INVALID_STATE;
        }

        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = addr_7bit;
        dev_cfg.scl_speed_hz = I2C_MASTER_FREQ_HZ;

        return i2c_master_bus_add_device(s_bus, &dev_cfg, out_dev);
}

esp_err_t I2C_Recover(void)
{
        if (s_bus == NULL) {
                return ESP_ERR_INVALID_STATE;
        }
        return i2c_master_bus_reset(s_bus);
}

/* ------------------------------------------------------------------ */
/* MPU6050 读写                                                        */
/* ------------------------------------------------------------------ */

esp_err_t Mpu6050_Write(uint8_t reg, uint8_t data)
{
        if (s_dev == NULL) {
                return ESP_ERR_INVALID_STATE;
        }
        const uint8_t buf[2] = {reg, data};
        /* 新驱动一次事务搞定：起始 + 器件地址 + 寄存器 + 数据 + 停止，全在驱动内部完成，
         * 不像 legacy 每次都要 i2c_cmd_link_create() 再逐个拼命令、最后还要 delete */
        return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}
//MPU6050的写逻辑

esp_err_t Mpu6050_Read(uint8_t reg, uint8_t *data, size_t n)
{
        if (s_dev == NULL || data == NULL) {
                return ESP_ERR_INVALID_STATE;
        }
        /* 读：先写寄存器地址（不产生 STOP，靠重复起始位转读），再连续读 n 字节 */
        return i2c_master_transmit_receive(s_dev, &reg, 1, data, n, I2C_TIMEOUT_MS);
}
//MPU6050的读逻辑

uint8_t Mpu6050_GetID(void)
{
        uint8_t ID = 0;
        if (Mpu6050_Read(MPU6050_WHO_AM_I, &ID, 1) != ESP_OK) {
                return 0;   /* 读失败返回 0：0 不是合法 who_am_i，上层据此判故障 */
        }
        return ID;
}

esp_err_t Mpu6050_Init(void)
{
        if (s_dev == NULL) {
                ESP_LOGE(TAG, "初始化顺序错误：I2C 总线还没建");
                return ESP_ERR_INVALID_STATE;
        }

        esp_err_t err;

        /* 1. 器件复位。datasheet 要求复位后等 100ms，否则后续寄存器写入会被丢弃 */
        err = Mpu6050_Write(MPU6050_PWR_MGMT_1, MPU6050_PWR1_DEVICE_RESET);
        if (err != ESP_OK) {
                ESP_LOGE(TAG, "软复位失败: %s", esp_err_to_name(err));
                return err;
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        /* 2. 先确认器件在线，再动配置。
         *    不在线时写一堆配置只会得到一堆 NACK，日志还看不出根因。 */
        const uint8_t id = Mpu6050_GetID();
        if (id != MPU6050_ID_VALUE) {
                ESP_LOGE(TAG, "MPU6050 不在线: who_am_i=0x%02X（期望 0x%02X）", id,
                         MPU6050_ID_VALUE);
                return ESP_ERR_NOT_FOUND;
        }

        /* 3. 器件配置：每一步都检查，原版 void 写函数等于把错误全吞了 */
        /* 唤醒并选择 X 轴陀螺 PLL 作为时钟源（比内部 RC 振荡器稳定得多，
         * 内部 RC 的抖动会直接变成陀螺噪声） */
        if ((err = Mpu6050_Write(MPU6050_PWR_MGMT_1, MPU6050_PWR1_CLKSEL_PLL_X)) != ESP_OK) {
                return err;
        }
        /* 六轴全部使能（不进入休眠） */
        if ((err = Mpu6050_Write(MPU6050_PWR_MGMT_2, 0x00)) != ESP_OK) {
                return err;
        }
        /* 采样率分频：输出率 = 1kHz / (1 + 19) = 50Hz，与 sensor_task 的 20ms 严格对齐。
         * 注意 19 是十进制分频数，不是寄存器地址 0x19；写 0x19(=25) 会得到 38.5Hz 且不报错 */
        if ((err = Mpu6050_Write(MPU6050_SMPLRT_DIV, 19)) != ESP_OK) {
                return err;
        }
        /* DLPF_CFG = 3：陀螺带宽 44Hz / 加速度 42Hz。再低会把 2.5g 冲击峰平滑掉，
         * 跌倒检出率断崖下跌 */
        if ((err = Mpu6050_Write(MPU6050_CONFIG, 0x03)) != ESP_OK) {
                return err;
        }
        /* 陀螺满量程 ±2000dps：跌倒冲击时角速度可超 1000dps，宁可牺牲分辨率也不能饱和 */
        if ((err = Mpu6050_Write(MPU6050_GYRO_CONFIG, MPU6050_GYRO_FS_2000)) != ESP_OK) {
                return err;
        }
        /* 加速度满量程 ±8g（原版是 ±16g）：4096 LSB/g，2.5g 判据的噪声余量充足，
         * 换成 8g 后小信号的量化误差直接减半 */
        if ((err = Mpu6050_Write(MPU6050_ACCEL_CONFIG, MPU6050_ACCEL_FS_8)) != ESP_OK) {
                return err;
        }

        vTaskDelay(pdMS_TO_TICKS(50));

        ESP_LOGI(TAG, "初始化完成: acc ±8g / gyro ±2000dps / 50Hz / DLPF 44Hz");
        return ESP_OK;
}

esp_err_t Mpu6050_Get_Data(int16_t *Accx, int16_t *Accy, int16_t *Accz,
                           int16_t *Gyrox, int16_t *Gyroy, int16_t *Gyroz)
{
        uint8_t buffer[14];

        esp_err_t err = Mpu6050_Read(MPU6050_ACCEL_XOUT_H, buffer, 14);
        if (err != ESP_OK) {
                return err;
        }

        /* 14 字节布局：acc(6) + temp(2) + gyro(6)，全部 MSB 在前。
         * 先拼成 uint16 再转 int16：直接左移一个有符号变量会踩到符号位实现细节 */
        *Accx = (int16_t)(((uint16_t)buffer[0] << 8) | buffer[1]);
        *Accy = (int16_t)(((uint16_t)buffer[2] << 8) | buffer[3]);
        *Accz = (int16_t)(((uint16_t)buffer[4] << 8) | buffer[5]);

        /* buffer[6]、buffer[7] 为温度，本工程未使用 */

        *Gyrox = (int16_t)(((uint16_t)buffer[8] << 8) | buffer[9]);
        *Gyroy = (int16_t)(((uint16_t)buffer[10] << 8) | buffer[11]);
        *Gyroz = (int16_t)(((uint16_t)buffer[12] << 8) | buffer[13]);

        return ESP_OK;
}
