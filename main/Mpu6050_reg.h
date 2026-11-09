//
// Created by l on 2026/11/2.
//

#ifndef HELLO_WORLD_MPU6050_REG_H
#define HELLO_WORLD_MPU6050_REG_H


#ifdef __cplusplus
extern "C" {
#endif
#define MPU6050_SMPLRT_DIV		0x19
#define	MPU6050_CONFIG			0x1A
#define	MPU6050_GYRO_CONFIG		0x1B
#define	MPU6050_ACCEL_CONFIG	0x1C

#define	MPU6050_ACCEL_XOUT_H	0x3B
#define	MPU6050_ACCEL_XOUT_L	0x3C
#define	MPU6050_ACCEL_YOUT_H	0x3D
#define	MPU6050_ACCEL_YOUT_L	0x3E
#define	MPU6050_ACCEL_ZOUT_H	0x3F
#define	MPU6050_ACCEL_ZOUT_L	0x40
#define	MPU6050_TEMP_OUT_H		0x41
#define	MPU6050_TEMP_OUT_L		0x42
#define	MPU6050_GYRO_XOUT_H		0x43
#define	MPU6050_GYRO_XOUT_L		0x44
#define	MPU6050_GYRO_YOUT_H		0x45
#define	MPU6050_GYRO_YOUT_L		0x46
#define	MPU6050_GYRO_ZOUT_H		0x47
#define	MPU6050_GYRO_ZOUT_L		0x48

#define	MPU6050_PWR_MGMT_1		0x6B
#define	MPU6050_PWR_MGMT_2		0x6C
#define	MPU6050_WHO_AM_I		0x75

/* ---- 位域常量 ---- */
/* PWR_MGMT_1: 器件复位(写 1 触发, 硬件自动清零) */
#define MPU6050_PWR1_DEVICE_RESET	0x80
/* PWR_MGMT_1: 时钟源选择 PLL with X axis gyroscope reference (推荐, 比内部振荡器稳定) */
#define MPU6050_PWR1_CLKSEL_PLL_X	0x01
/* GYRO_CONFIG: 满量程 ±2000 dps, 灵敏度 16.4 LSB/(°/s) */
#define MPU6050_GYRO_FS_2000		0x18

/* ACCEL_CONFIG: AFS_SEL 位域在 [4:3]
 *   0=±2g  1=±4g  2=±8g  3=±16g
 * 本工程选 ±8g：2.5g 跌倒判据留 3 倍裕度，同时 4096 LSB/g 让静态 1g 的信噪比好一倍。
 * ±16g 只有在"冲击远超 5g 且明确不关心静态精度"时才用。 */
#define MPU6050_ACCEL_FS_8			0x10	/* 4096 LSB/g */
#define MPU6050_ACCEL_FS_16			0x18	/* 2048 LSB/g，备选 */

/* 灵敏度常数, 与上面的量程配置必须成对修改。
 * 只改量程不改常数 → 所有物理量整体差一个倍数，而且不会报任何错，只是"数据看着不对"。 */
#define MPU6050_GYRO_SENS_2000DPS	16.4f	/* LSB/(°/s) */
#define MPU6050_ACCEL_SENS_8G		4096.0f	/* LSB/g  ← 与 ACCEL_FS_8 配套 */
#define MPU6050_ACCEL_SENS_16G		2048.0f	/* LSB/g  ← 与 ACCEL_FS_16 配套 */

/* 7 位 I2C 地址 (AD0 接地): 0x68 */
#define MPU6050_7BIT_ADDRESS		0x68
#define MPU6050_ID_VALUE			0x68

#ifdef __cplusplus
}
#endif

#endif //HELLO_WORLD_MPU6050_REG_H
