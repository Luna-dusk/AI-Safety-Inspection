/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Mpu6050.h"
#include "Mpu6050_reg.h"
#include "QMC5883.h"
#include "QMC5883_reg.h"

/* ==================================================================
 * 一、参数
 * ================================================================== */

/* 采样周期 20ms → 50Hz。改这个值必须同步改三处，否则数据"看着正常但慢/快一倍"：
 *   1. Mpu6050.cpp 的 SMPLRT_DIV = 1000/周期ms − 1（20ms → 19）
 *   2. QMC5883_reg.h 的 CTRL1 ODR 位域（20ms → 50Hz）
 *   3. 这里
 * 注意 Sensor_DT 只是"标称周期"，真正参与积分的是 esp_timer 实测的 dt ——
 * 丢拍、日志阻塞、故障恢复都会让实际间隔偏离 20ms，用常量等于按错误倍率累加角度。 */
#define Sensor_DT			0.02f		/* 20ms -> 50Hz */
#define Sensor_PI			3.14159265f

/* 陀螺零偏校准采样帧数：100 帧 @20ms = 2s */
#define MPU6050_CALI_SAMPLES	100

/* 互补滤波系数：0.98 偏向陀螺积分（动态响应快），0.02 用于修正累积漂移。
 * 20ms 周期下时间常数 τ = dt·α/(1−α) ≈ 1s，与"姿态保持 3s"的跌倒判据不冲突。 */
#define COMP_FILTER_ALPHA	0.98f

/* QMC5883L ±8G 量程的灵敏度：3000 LSB/G，1 Gauss = 100 µT → 30 LSB/µT。
 * 这个常数错了不会报错，只会让所有磁场读数整体差一个倍数。
 * 参考 datasheet Table 2：±2G → 12000 LSB/G，±8G → 3000 LSB/G
 * （HMC5883L 才是 1090 LSB/G，别抄错型号） */
#define QMC5883_LSB_PER_UT	30.0f

/* 磁偏角：西安约 −3.5°，指北针读数加上它才是真北。换地方要重新填 */
static float QMC5883_Declination = -3.5f;

/* ---- 校准门控 ---- */
#define CALI_ACC_TOL_G		0.10f	/* ‖a‖ 偏离 1g 超过这个值 → 认为在动 */
#define CALI_GYRO_TOL_DPS	10.0f	/* 任一轴角速度超过这个值 → 认为在动 */
#define CALI_MAX_WAIT_MS	10000	/* 最多等 10s 静止 */
#define CALI_SIGMA_MAX_DPS	1.5f	/* 样本标准差超限 → 校准被污染 */

/* ---- 姿态可信度门控 ---- */
#define ACC_TRUST_TOL_G		0.25f	/* ‖a‖ 偏离 1g 超过它 → 有线性加速度，acc 不参与修正 */
#define MAG_NORM_MIN_UT		15.0f	/* 地表地磁 25~65µT，出界就是铁磁干扰 */
#define MAG_NORM_MAX_UT		90.0f
#define MAG_BASE_FRAMES		50		/* 开机前 1s 定磁场模值基准 */

/* ---- 恢复阶梯（单位：帧，20ms/帧） ---- */
/* 偶发单帧失败是正常的（WiFi 中断打断 I2C、从机正在更新数据）。
 * 连续 3 帧 = 60ms 才认为真出问题，单帧就复位总线会把总线打成筛子。 */
#define ERR_L1_FRAMES		3
#define ERR_L2_FRAMES		6
#define FAULT_RETRY_FRAMES	50		/* 降级后每 1s 重试一次 */
#define FAULT_REPORT_FRAMES	6000	/* 故障日志 120s 闸门，防刷爆串口 */

#define SUMMARY_FRAMES		50		/* 1s 一行数据汇总 */
#define STATS_FRAMES		500		/* 10s 一行运行统计 */
#define DROP_TOLERANCE_MS	30		/* 唤醒间隔超过 1.5 周期才算丢拍 */

static const char *TAG = "gw1";

/* ==================================================================
 * 二、全局量
 * ================================================================== */

/* 原始值，保留全局方便调试时在 watch 窗口里直接看 */
int16_t AX, AY, AZ, GX, GY, GZ, HX, HY, HZ;

/* 陀螺三轴零偏，单位 °/s（不是 LSB）。由 IMU_CalibrateGyro() 计算 */
static float GyroBiasX = 0.0f;
static float GyroBiasY = 0.0f;
static float GyroBiasZ = 0.0f;
static uint8_t GyroCalibrated = 0;

/* 磁场状态：0 正常 / 1 读失败 / 2 量程溢出（OVL）。
 * 溢出是环境干扰（喇叭磁铁、铁门），不是总线故障，绝不能触发总线恢复 */
static uint8_t MagStatus = 0;

/* 磁场模值基准（µT），开机前 50 帧求均值得到，用于判定"地磁是否被干扰" */
static float MagBaseNorm = 0.0f;
static float MagNormSum = 0.0f;
static uint32_t MagBaseCount = 0;

/* 运行统计，只在 sensor_task 里改，打印时直接读 */
static struct {
	uint32_t frames;
	uint32_t imu_err;
	uint32_t mag_err;
	uint32_t mag_ovl;
	uint32_t drops;
	uint32_t l1;
	uint32_t l2;
	uint32_t faults;
	float last_dt_ms;
} s_stats;

/* ==================================================================
 * 三、工具
 * ================================================================== */

/* 环差：把任意角度归一到 [-180, 180]。
 * +540 先把结果抬进正半轴再减 180，一次表达式覆盖 (-∞, +∞)，
 * 比 while 循环稳（循环在超大值上要跑很多次）。yaw 跨界融合必须走它。 */
static float wrap180(float deg)
{
	return fmodf(deg + 540.0f, 360.0f) - 180.0f;
}

/* ==================================================================
 * 四、陀螺零偏校准
 * ================================================================== */

/* 单帧静止判据：重力模值接近 1g 且三轴角速度都很小。
 * 只看均值是不够的 —— 运动有正有负能互相抵消，均值会骗人。 */
static bool imu_is_still(int16_t aX, int16_t aY, int16_t aZ,
                         int16_t bX, int16_t bY, int16_t bZ)
{
	const float ax = (float)aX / MPU6050_ACCEL_SENS_8G;
	const float ay = (float)aY / MPU6050_ACCEL_SENS_8G;
	const float az = (float)aZ / MPU6050_ACCEL_SENS_8G;
	const float norm = sqrtf(ax * ax + ay * ay + az * az);
	if (fabsf(norm - 1.0f) > CALI_ACC_TOL_G) {
		return false;
	}

	const float gx = fabsf((float)bX) / MPU6050_GYRO_SENS_2000DPS;
	const float gy = fabsf((float)bY) / MPU6050_GYRO_SENS_2000DPS;
	const float gz = fabsf((float)bZ) / MPU6050_GYRO_SENS_2000DPS;
	return (gx < CALI_GYRO_TOL_DPS && gy < CALI_GYRO_TOL_DPS && gz < CALI_GYRO_TOL_DPS);
}

/*
 * 陀螺零偏校准。
 *
 * 原版这里累加的是 aX/aY/aZ（加速度），算出来的"零偏"是重力分量
 * （Z 轴 ≈ 4096 甚至 16384），后面每次积分都按这个偏移跑，姿态必崩。
 * 零偏只能来自陀螺三轴 —— 这是本函数唯一要记住的事。
 *
 * 三层保护：
 *   1. 先等静止（最长 10s），不等静止直接采，均值里全是运动角速度；
 *   2. 逐帧判静止，被拿起就丢该帧（不是整轮作废，容错但仍要求足够样本）；
 *   3. 标准差检验，σ 大说明"静止"被破坏了，此时算出来的不是零偏而是运动。
 */
void IMU_CalibrateGyro(void)
{
	GyroCalibrated = 0;

	/* ---- 阶段一：等静止 ---- */
	bool still = false;
	const uint32_t max_attempts = CALI_MAX_WAIT_MS / 20;
	for (uint32_t i = 0; i < max_attempts; i++) {
		int16_t aX, aY, aZ, bX, bY, bZ;
		if (Mpu6050_Get_Data(&aX, &aY, &aZ, &bX, &bY, &bZ) == ESP_OK &&
		    imu_is_still(aX, aY, aZ, bX, bY, bZ)) {
			still = true;
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
	if (!still) {
		ESP_LOGW(TAG, "等待静止超时（%d ms），跳过本次校准；零偏按 0 处理，"
			      "航向会持续漂移，请静止后重新上电", CALI_MAX_WAIT_MS);
		return;
	}

	/* ---- 阶段二：采样 ----
	 * 一轮同时累加 sum 与 sum²，均值与标准差来自同一批数据。
	 * 分成两轮采两次既多花 2 秒，统计口径还不一致。 */
	int64_t sum[3] = {0, 0, 0};
	int64_t sumsq[3] = {0, 0, 0};
	uint32_t n = 0;

	for (uint32_t i = 0; i < MPU6050_CALI_SAMPLES; i++) {
		vTaskDelay(pdMS_TO_TICKS(20));

		int16_t aX, aY, aZ, bX, bY, bZ;
		if (Mpu6050_Get_Data(&aX, &aY, &aZ, &bX, &bY, &bZ) != ESP_OK) {
			continue;
		}
		if (!imu_is_still(aX, aY, aZ, bX, bY, bZ)) {
			continue;
		}
		/* 只累加陀螺（bX/bY/bZ），不是加速度 */
		sum[0] += bX;
		sum[1] += bY;
		sum[2] += bZ;
		sumsq[0] += (int64_t)bX * bX;
		sumsq[1] += (int64_t)bY * bY;
		sumsq[2] += (int64_t)bZ * bZ;
		n++;
	}

	if (n < MPU6050_CALI_SAMPLES / 2) {
		ESP_LOGW(TAG, "有效样本不足（%u/%u），校准失败", (unsigned)n,
			 (unsigned)MPU6050_CALI_SAMPLES);
		return;
	}

	/* ---- 阶段三：均值 → °/s，并用 sum/sum² 反推方差做质量检验 ---- */
	const float mean_lsb[3] = {
	    (float)sum[0] / (float)n,
	    (float)sum[1] / (float)n,
	    (float)sum[2] / (float)n,
	};

	float var[3];
	for (int i = 0; i < 3; i++) {
		const float mean = mean_lsb[i];
		const float ms = (float)sumsq[i] / (float)n;
		const float v = ms - mean * mean;
		var[i] = (v > 0.0f) ? v : 0.0f;	/* 浮点误差可能算出微小负值 */
	}

	const float sigma_dps = sqrtf((var[0] + var[1] + var[2]) / 3.0f) /
				MPU6050_GYRO_SENS_2000DPS;
	if (sigma_dps > CALI_SIGMA_MAX_DPS) {
		ESP_LOGW(TAG, "校准样本标准差过大（%.2f °/s），判定为被扰动，本次校准作废",
			 sigma_dps);
		return;
	}

	/* 存 °/s 而不是 LSB：后续换算一律"先除灵敏度再减 bias"，
	 * 顺序反了（先减 LSB 再除）得到的量纲是错的，且不会报错 */
	GyroBiasX = mean_lsb[0] / MPU6050_GYRO_SENS_2000DPS;
	GyroBiasY = mean_lsb[1] / MPU6050_GYRO_SENS_2000DPS;
	GyroBiasZ = mean_lsb[2] / MPU6050_GYRO_SENS_2000DPS;

	/* 合法性检查：真静止的零偏不可能超过 1°/s。超了说明器件或安装有问题，
	 * 与其带着错误的零偏跑，不如清零并告警 */
	if (fabsf(GyroBiasX) > 1.0f || fabsf(GyroBiasY) > 1.0f || fabsf(GyroBiasZ) > 1.0f) {
		ESP_LOGW(TAG, "零偏超出合理范围（%.2f, %.2f, %.2f °/s），已清零，"
			      "请检查安装与振动",
			 GyroBiasX, GyroBiasY, GyroBiasZ);
		GyroBiasX = GyroBiasY = GyroBiasZ = 0.0f;
		return;
	}

	GyroCalibrated = 1;
	ESP_LOGI(TAG, "零偏校准完成: bias=(%.3f, %.3f, %.3f) °/s, σ=%.3f °/s, n=%u",
		 GyroBiasX, GyroBiasY, GyroBiasZ, sigma_dps, (unsigned)n);
}

uint8_t IMU_IsCalibrated(void)
{
	return GyroCalibrated;
}

/* ==================================================================
 * 五、姿态解算
 * ================================================================== */

float IMU_GetPitch(float *pitch, int AX, int AY, int AZ)
{
	/* 绕 Y 轴旋转为俯仰, 由 X 轴重力分量与 YZ 平面合力求解。
	 * 分子前的负号来自 MPU6050 的坐标系（Z 向上时 +X 倾斜为低头） */
	*pitch = atan2f(-(float)AX, sqrtf((float)AY * (float)AY + (float)AZ * (float)AZ))
		 * 180.0f / Sensor_PI;
	return *pitch;
}

float IMU_GetRoll(float *roll, int AX, int AY, int AZ)
{
	/* 绕 X 轴旋转为横滚, 由 Y/Z 轴重力分量求解, X 轴分量不参与计算 */
	(void)AX;
	*roll = atan2f((float)AY, (float)AZ) * 180.0f / Sensor_PI;
	return *roll;
}

/*
 * 读一次传感器并做一帧姿态融合。
 *
 * 返回值只反映 **MPU6050 的读取结果**：总线健康与否，IMU 就能代表。
 * 磁力计失败只置 MagStatus，因为 OVL 溢出是物理干扰不是故障，
 * 让它去驱动总线恢复只会对同一根好总线做无意义的复位。
 *
 * dt 由调用方传入实测值，函数内部不再用常量。
 */
esp_err_t IMU_GetData(float *pitch, float *roll, float *yaw, float dt)
{
	float angle_pitch = *pitch;
	float angle_roll = *roll;
	float angle_yaw = *yaw;

	/* ---- 1. 读原始值 ---- */
	esp_err_t err = Mpu6050_Get_Data(&AX, &AY, &AZ, &GX, &GY, &GZ);
	if (err != ESP_OK) {
		return err;	/* 读失败不动姿态：拿脏数据凑合比不更新更糟 */
	}

	err = QMC5883_GetData(&HX, &HY, &HZ);
	if (err == ESP_OK) {
		MagStatus = 0;
	} else if (err == ESP_ERR_INVALID_RESPONSE) {
		MagStatus = 2;	/* OVL：量程溢出，数据已削顶 */
	} else {
		MagStatus = 1;	/* 总线读失败 */
	}

	/* ---- 2. 单位换算 ---- */
	/* 先除灵敏度（→物理单位）再减零偏。顺序不能反：bias 存的是 °/s */
	const float groy_x_rate = (float)GX / MPU6050_GYRO_SENS_2000DPS - GyroBiasX;
	const float groy_y_rate = (float)GY / MPU6050_GYRO_SENS_2000DPS - GyroBiasY;
	const float groy_z_rate = (float)GZ / MPU6050_GYRO_SENS_2000DPS - GyroBiasZ;

	const float acc_x = (float)AX / MPU6050_ACCEL_SENS_8G;
	const float acc_y = (float)AY / MPU6050_ACCEL_SENS_8G;
	const float acc_z = (float)AZ / MPU6050_ACCEL_SENS_8G;

	const float mag_x = (float)HX / QMC5883_LSB_PER_UT;
	const float mag_y = (float)HY / QMC5883_LSB_PER_UT;
	const float mag_z = (float)HZ / QMC5883_LSB_PER_UT;

	/* ---- 3. 可信度门控 ----
	 * 这是整个工程最重要的两个开关：
	 *   acc 不可信 → 冻结倾角修正，只靠陀螺积分。自由落体时 ‖a‖→0，
	 *                方向向量是纯噪声，混进修正项会污染跌倒判据的起点；
	 *   mag 不可信 → 冻结航向修正。走过铁门、靠近喇叭时航向会突然甩 90°。 */
	const float acc_norm = sqrtf(acc_x * acc_x + acc_y * acc_y + acc_z * acc_z);
	const bool acc_trust = (fabsf(acc_norm - 1.0f) < ACC_TRUST_TOL_G);

	const float mag_norm = sqrtf(mag_x * mag_x + mag_y * mag_y + mag_z * mag_z);
	bool mag_trust = false;
	if (MagStatus == 0) {
		if (MagBaseCount < MAG_BASE_FRAMES) {
			/* 开机阶段先攒基准，还没资格判干扰（没有比较对象） */
			MagNormSum += mag_norm;
			MagBaseCount++;
			if (MagBaseCount == MAG_BASE_FRAMES) {
				MagBaseNorm = MagNormSum / (float)MAG_BASE_FRAMES;
				ESP_LOGI(TAG, "磁场基准建立: |m|=%.1f µT（正常范围 25~65µT）",
					 MagBaseNorm);
			}
			mag_trust = true;
		} else {
			/* 绝对出界 + 相对畸变（偏离基准 >40%）双判据 */
			const float ratio = mag_norm / MagBaseNorm;
			mag_trust = !((mag_norm < MAG_NORM_MIN_UT || mag_norm > MAG_NORM_MAX_UT) ||
				      (ratio > 1.4f || ratio < 0.6f));
		}
	}

	/* ---- 4. 加速度单独算出的倾角（静置时可信） ---- */
	float acc_pitch, acc_roll;
	IMU_GetPitch(&acc_pitch, AX, AY, AZ);
	IMU_GetRoll(&acc_roll, AX, AY, AZ);

	/* ---- 5. 陀螺预测：三轴积分到欧拉角 ----
	 * 这一步只在水平附近严格成立（欧拉角速度与机体角速度存在运动学耦合），
	 * 跌倒判据用的是"相对基线的无状态倾角"，不依赖这个近似，所以可以接受 */
	const float pitch_pred = angle_pitch + groy_y_rate * dt;
	const float roll_pred = angle_roll + groy_x_rate * dt;
	const float yaw_pred = wrap180(angle_yaw + groy_z_rate * dt);

	/* ---- 6. 加速度修正 ---- */
	if (acc_trust) {
		angle_pitch = COMP_FILTER_ALPHA * pitch_pred + (1.0f - COMP_FILTER_ALPHA) * acc_pitch;
		angle_roll = COMP_FILTER_ALPHA * roll_pred + (1.0f - COMP_FILTER_ALPHA) * acc_roll;
	} else {
		angle_pitch = pitch_pred;
		angle_roll = roll_pred;
	}

	/* ---- 7. 航向修正：磁力计倾角补偿 + 环差融合 ----
	 * 倾角补偿：把机体坐标系磁场投影到水平面，投影完再 atan2 才不受倾斜影响
	 *   Bx = Mx·cp + My·sr·sp + Mz·cr·sp
	 *   By = My·cr − Mz·sr
	 */
	if (mag_trust) {
		const float roll_rad = angle_roll * Sensor_PI / 180.0f;
		const float pitch_rad = angle_pitch * Sensor_PI / 180.0f;
		const float sp = sinf(pitch_rad), cp = cosf(pitch_rad);
		const float sr = sinf(roll_rad), cr = cosf(roll_rad);

		const float BX = mag_x * cp + mag_y * sr * sp + mag_z * cr * sp;
		const float BY = mag_y * cr - mag_z * sr;
		const float mag_yaw = atan2f(BY, BX) * 180.0f / Sensor_PI + QMC5883_Declination;

		/* 必须融合"误差"而不是直接线性混合两个角度：
		 * 直接混合时 +179° 与 −179° 会算出 ≈175°，滤波器随后"绕远路"爬回去，
		 * 表现为过正北时罗盘突然反向转一整圈。
		 * 误差本身已被归一到 [-180,180]，跨界无害。 */
		const float err = wrap180(mag_yaw - yaw_pred);
		angle_yaw = wrap180(yaw_pred + (1.0f - COMP_FILTER_ALPHA) * err);
	} else {
		angle_yaw = yaw_pred;
	}

	*pitch = angle_pitch;
	*roll = angle_roll;
	*yaw = angle_yaw;
	return ESP_OK;
}

/* ==================================================================
 * 六、恢复阶梯
 * ================================================================== */

/* L2：整套重初始化。注意不碰零偏 —— 器件软复位不会改变它自己的物理零偏，
 * 而重新校准要用户静止 2s，故障恢复里做不到也没必要。 */
static esp_err_t sensors_reinit(void)
{
	esp_err_t err = I2C_Recover();
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "L2 总线复位失败: %s", esp_err_to_name(err));
	}

	/* MPU 先 init：它是总线的"探针"，它活了才说明总线真的通了 */
	err = Mpu6050_Init();
	if (err != ESP_OK) {
		return err;
	}
	return QMC5883_Init();
}

/*
 * 恢复阶梯：L0 什么都不做 → L1 复位总线 → L2 重初始化 → L3 上报故障并周期性重试
 *
 * L0 不在代码里：新驱动事务超时时会在内部自动复位硬件状态机，下一帧还能用。
 * 所以这里第一级动作是"等"，不是"修"。
 *
 * 只有 IMU 失败才驱动阶梯：两颗挂同一根总线，IMU 能读就说明总线是好的，
 * 两颗各爬一次阶梯等于对同一根坏总线做双倍动作。
 */
static void recover_step(uint32_t *err_run)
{
	(*err_run)++;

	if (*err_run < ERR_L1_FRAMES) {
		return;		/* 偶发单帧失败，不动作 */
	}

	if (*err_run == ERR_L1_FRAMES) {
		s_stats.l1++;
		ESP_LOGW(TAG, "IMU 连续失败 %u 帧 → L1 复位总线并探测", (unsigned)*err_run);
		if (I2C_Recover() == ESP_OK && Mpu6050_GetID() == MPU6050_ID_VALUE) {
			ESP_LOGI(TAG, "L1 恢复成功（累计 %u 次）", (unsigned)s_stats.l1);
			*err_run = 0;
		}
		return;
	}

	if (*err_run == ERR_L2_FRAMES) {
		s_stats.l2++;
		ESP_LOGW(TAG, "IMU 连续失败 %u 帧 → L2 传感器软复位并重写配置",
			 (unsigned)*err_run);
		if (sensors_reinit() == ESP_OK) {
			ESP_LOGI(TAG, "L2 恢复成功（零偏保持不变，累计 %u 次）",
				 (unsigned)s_stats.l2);
			*err_run = 0;
		}
		return;
	}

	/* L3：到这里已经不是"抖动"了，是接线/供电/器件本身的问题。
	 * 监护设备不能默默重试 —— 传感器死了，家属必须知道。 */
	if (*err_run == ERR_L2_FRAMES + 1) {
		s_stats.faults++;
		ESP_LOGE(TAG, "IMU 持续无响应 → L3 进入降级，每 %d ms 重试一次，"
			      "请检查 SDA/SCL 与供电（累计故障 %u 次）",
			 FAULT_RETRY_FRAMES * 20, (unsigned)s_stats.faults);
	}

	if ((*err_run % FAULT_RETRY_FRAMES) == 0) {
		if ((*err_run % FAULT_REPORT_FRAMES) == 0) {
			s_stats.faults++;
			ESP_LOGE(TAG, "传感器持续无响应 %u 帧（约 %u s），累计故障 %u 次",
				 (unsigned)*err_run, (unsigned)(*err_run * 20 / 1000),
				 (unsigned)s_stats.faults);
		}
		if (sensors_reinit() == ESP_OK) {
			ESP_LOGI(TAG, "降级状态下重连成功，恢复 50Hz 采集");
			*err_run = 0;
		}
	}
}

/* ==================================================================
 * 七、采集任务
 * ================================================================== */

/* 一秒一行的汇总。三个验收数字直接印在日志里：
 * 静止 ‖a‖≈1.000g、‖ω‖≈0、‖m‖ 在 25~65µT。这三个不对，后面算法都不用看。 */
static void log_summary(float pitch, float roll, float yaw, float acc_norm,
			float gyro_norm, float mag_norm)
{
	ESP_LOGI(TAG, "seq=%u |a|=%.3fg |w|=%.2f°/s |m|=%.1fµT | "
		      "pitch=%.1f° roll=%.1f° yaw=%.1f° | 周期=%.1fms "
		      "imu_err=%u mag_err=%u ovl=%u 丢拍=%u L1=%u L2=%u",
		 (unsigned)s_stats.frames, acc_norm, gyro_norm, mag_norm,
		 pitch, roll, yaw, s_stats.last_dt_ms, (unsigned)s_stats.imu_err,
		 (unsigned)s_stats.mag_err, (unsigned)s_stats.mag_ovl,
		 (unsigned)s_stats.drops, (unsigned)s_stats.l1, (unsigned)s_stats.l2);
}

void sensor_task(void *pvParameter)
{
	(void)pvParameter;

	/* 第一步就要把这行打出来：任务没起来 和 任务起来了但数据不对，是两回事 */
	ESP_LOGI(TAG, "采集任务启动: %d ms / 50Hz, 优先级 8, 栈 4096, 核心 1",
		 (int)(Sensor_DT * 1000));

	TickType_t last_wake = xTaskGetTickCount();
	const TickType_t period = pdMS_TO_TICKS(20);
	int64_t last_us = esp_timer_get_time();

	float angle_pitch = 0.0f;
	float angle_roll = 0.0f;
	float angle_yaw = 0.0f;
	uint32_t err_run = 0;

	for (;;) {
		/* vTaskDelayUntil 按"绝对唤醒时刻"排程，单帧耗时的抖动不会累积到下一帧；
		 * vTaskDelay(20) 的实际周期是 20ms + 循环耗时 + 调度抖动，误差会累积 */
		vTaskDelayUntil(&last_wake, period);

		const int64_t now_us = esp_timer_get_time();
		float dt = (float)(now_us - last_us) / 1000000.0f;
		last_us = now_us;
		s_stats.last_dt_ms = dt * 1000.0f;

		if (dt <= 0.0f || dt > 0.5f) {
			dt = Sensor_DT;	/* 时间戳异常（溢出/长时间挂起），退回标称周期 */
		} else if (dt * 1000.0f > (float)DROP_TOLERANCE_MS) {
			uint32_t missed = (uint32_t)(dt * 1000.0f / (Sensor_DT * 1000.0f) + 0.5f) - 1;
			if (missed == 0) {
				missed = 1;
			}
			s_stats.drops += missed;
			ESP_LOGW(TAG, "丢拍 %u 帧（实际间隔 %.1f ms），累计 %u",
				 (unsigned)missed, s_stats.last_dt_ms,
				 (unsigned)s_stats.drops);
		}

		s_stats.frames++;

		esp_err_t err = IMU_GetData(&angle_pitch, &angle_roll, &angle_yaw, dt);
		if (err != ESP_OK) {
			s_stats.imu_err++;
			recover_step(&err_run);
			continue;	/* 本帧没有有效姿态，不打印汇总 */
		}

		if (err_run > 0) {
			ESP_LOGI(TAG, "IMU 已恢复，连续失败计数从 %u 清零", (unsigned)err_run);
			err_run = 0;
		}

		if (MagStatus == 1) {
			s_stats.mag_err++;
		} else if (MagStatus == 2) {
			s_stats.mag_ovl++;
		}

		/* 汇总日志：计算三个验收量 */
		if ((s_stats.frames % SUMMARY_FRAMES) == 0) {
			const float ax = (float)AX / MPU6050_ACCEL_SENS_8G;
			const float ay = (float)AY / MPU6050_ACCEL_SENS_8G;
			const float az = (float)AZ / MPU6050_ACCEL_SENS_8G;
			const float gx = (float)GX / MPU6050_GYRO_SENS_2000DPS - GyroBiasX;
			const float gy = (float)GY / MPU6050_GYRO_SENS_2000DPS - GyroBiasY;
			const float gz = (float)GZ / MPU6050_GYRO_SENS_2000DPS - GyroBiasZ;
			const float mx = (float)HX / QMC5883_LSB_PER_UT;
			const float my = (float)HY / QMC5883_LSB_PER_UT;
			const float mz = (float)HZ / QMC5883_LSB_PER_UT;

			log_summary(angle_pitch, angle_roll, angle_yaw,
				    sqrtf(ax * ax + ay * ay + az * az),
				    sqrtf(gx * gx + gy * gy + gz * gz),
				    sqrtf(mx * mx + my * my + mz * mz));
		}

		/* 10s 一行的运行统计：丢拍/错误/恢复次数是"系统是否健康"的唯一证据 */
		if ((s_stats.frames % STATS_FRAMES) == 0) {
			ESP_LOGI(TAG, "运行统计: 帧=%u 丢拍=%u imu_err=%u mag_err=%u ovl=%u "
				      "L1=%u L2=%u 故障=%u",
				 (unsigned)s_stats.frames, (unsigned)s_stats.drops,
				 (unsigned)s_stats.imu_err, (unsigned)s_stats.mag_err,
				 (unsigned)s_stats.mag_ovl, (unsigned)s_stats.l1,
				 (unsigned)s_stats.l2, (unsigned)s_stats.faults);
		}
	}
}

/* ==================================================================
 * 八、入口
 * ================================================================== */

void app_main(void)
{
	esp_log_level_set("*", ESP_LOG_INFO);
	memset(&s_stats, 0, sizeof(s_stats));

	ESP_LOGI(TAG, "GW-1 守望者启动 | I2C SDA=IO8 SCL=IO9 400kHz | 采集 50Hz");

	/* 顺序不能乱：总线 → 两颗传感器 → 校准 → 起任务。
	 * 校准必须在任务启动前做，否则会和 50Hz 循环抢总线。 */
	esp_err_t err = i2c_Init_Final();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "I2C 总线初始化失败: %s，系统停机", esp_err_to_name(err));
		for (;;) {
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}

	/* 传感器初始化失败不算致命：日志告警后继续，采集任务里的恢复阶梯会一直重试。
	 * 可穿戴设备不能因为一颗片子虚焊就整机装死。 */
	if (Mpu6050_Init() != ESP_OK) {
		ESP_LOGE(TAG, "MPU6050 初始化失败，交由恢复阶梯处理");
	}

	if (QMC5883_Init() != ESP_OK) {
		ESP_LOGE(TAG, "QMC5883L 初始化失败，航向功能不可用");
	} else {
		const uint8_t id = QMC5883_GetID();
		if (id != 0xFF) {
			ESP_LOGW(TAG, "QMC5883L chip id 异常: 0x%02X（期望 0xFF）", id);
		} else {
			ESP_LOGI(TAG, "QMC5883L 在线，chip id=0xFF");
		}
	}

	IMU_CalibrateGyro();
	if (!IMU_IsCalibrated()) {
		ESP_LOGW(TAG, "零偏未校准，航向会持续漂移；倾角仍然可用");
	}

	if (xTaskCreatePinnedToCore(sensor_task, "sensor_task", 4096, NULL, 8, NULL, 1) != pdPASS) {
		ESP_LOGE(TAG, "采集任务创建失败（栈 4096 不足？），系统停机");
		for (;;) {
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}

	ESP_LOGI(TAG, "初始化完毕，进入监控");
	/* app_main 返回后其任务被删除，采集任务继续跑。这里退出即可。 */
}
