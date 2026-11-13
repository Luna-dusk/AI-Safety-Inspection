/*
 * FALL_DET：跌倒检测模块（FR-3）。
 *
 * 框架来源：mzx 分支 hello_world_main.c 的采集任务/零偏校准/姿态融合/恢复阶梯
 * （2026-11 提取，函数改 static 收进模块）；跌倒状态机为新增实现，
 * 判据按该分支注释预留的方案：冲击 2.5g + 姿态偏离基线保持 3s。
 *
 * ── 跌倒状态机 ─────────────────────────────────────────────────────
 *
 *   WATCH ──‖a‖>2.5g──► IMPACT ──姿态偏离>55°持续3s──► FALLEN（回调 FALL）
 *     ▲                    │                              │
 *     └──窗口内回到直立────┘              fall_det_cancel() │ 自行站起≥10s
 *     ▲                                                          │
 *     └──────────────────────────────────────────────────────────┘
 *
 *   - WATCH   ：逐帧监控。‖a‖<0.35g 记"自由落体证据"（冲击时的加分项，单出不足以判跌）
 *   - IMPACT  ：冲击后 5s 判定窗。倾角(相对基线)>55° 连续保持 3s → 确认；
 *               连续性被打断（挣扎/起身）重新计。窗内回到直立(<30°) → 误报回 WATCH
 *   - FALLEN  ：只回调一次。撤销靠 cancel()（"我没事"）或自行站起 10s 自动重新武装
 *
 *   为什么用"倾角相对基线"而不是绝对角：设备佩戴姿态人人不同（斜挎/口袋），
 *   开机静止期先采基线，判定的是"从平时姿态翻倒过去"，与佩戴方式解耦。
 *   佩戴中摘下放桌上会持续偏离 → 误报，靠 cancel()/告警侧"我没事"通道兜底
 *   （与需求场景 2 一致）；把 55°/3s 调严可进一步压误报，代价是漏报率上升。
 *
 * ── 时序约束 ───────────────────────────────────────────────────────
 *   采样 20ms/50Hz 三处联动（改任何一处其余两处必须同步）：
 *     Mpu6050.cpp 的 SMPLRT_DIV、QMC5883_reg.h 的 CTRL1 ODR、本文件 Sensor_DT。
 *   积分用 esp_timer 实测 dt，不用标称常量（丢拍/日志阻塞时按错误倍率累加角度）。
 *
 * ── 任务参数 ───────────────────────────────────────────────────────
 *   栈 4096 / 优先级 8 / 核 1（afe feed 也在核 1 但优先级更高，不冲突；
 *   ai_chat 12 > 本任务 8，语音轮不会被传感器饿死——语音是主链路）。
 */

#include "fall_dat.h"

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


#define FALL_DET_CSV_MODE 0     //采集数据时改 1
#define CSV_MARK_GPIO	GPIO_NUM_0

#if FALL_DET_CSV_MODE
#include  "driver/gpio.h"

static void csv_head(void) {
	printf("# t_us,ax,ay,az,gx,gy,gz,mx,my,mz,mag_ok\n");
}

static void csv_frame(int64_t t_us, int16_t ax, int16_t ay, int16_t az,
			  int16_t gx, int16_t gy, int16_t gz,
			  int16_t mx, int16_t my, int16_t mz, uint8_t mag_ok)
{
	printf("%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u\n",
		   (long long)t_us, ax, ay, az, gx, gy, gz, mx, my, mz,
		   (unsigned)mag_ok);
}

#endif

/* ===================== 参数（调参只动这里） ===================== */

#define Sensor_DT			0.02f		/* 20ms -> 50Hz（三处联动，见文件头） */
#define Sensor_PI			3.14159265f

/* ---- 跌倒判据（mzx 分支注释预留的方案）---- */
#define FALL_IMPACT_G		2.5f		/* 冲击阈值：‖a‖ 超过即进入判定窗 */
#define FALL_FREEFALL_G		0.35f		/* 自由落体证据阈值（跌倒前必有失重） */
#define FALL_ANGLE_DEG		55.0f		/* 姿态偏离基线超过此值 = 翻倒姿态 */
#define FALL_CONFIRM_MS		3000		/* 翻倒姿态连续保持时长 → 确认跌倒 */
#define FALL_CANCEL_DEG		30.0f		/* 偏离回到此值以内 = 直立（误报出口） */
#define FALL_WINDOW_MS		5000		/* 冲击后的判定窗总长 */
#define FALL_REARM_MS		10000		/* FALLEN 态自行站起这么久 → 自动重新武装 */

/* ---- 陀螺零偏校准 ---- */
#define MPU6050_CALI_SAMPLES	100		/* 100 帧 @20ms = 2s */
#define CALI_ACC_TOL_G		0.10f
#define CALI_GYRO_TOL_DPS	10.0f
#define CALI_MAX_WAIT_MS	10000
#define CALI_SIGMA_MAX_DPS	1.5f

/* ---- 姿态融合 ---- */
#define COMP_FILTER_ALPHA	0.98f		/* 互补滤波：陀螺为主，acc/磁修正漂移 */
#define QMC5883_LSB_PER_UT	30.0f		/* ±8G 量程 3000 LSB/G → 30 LSB/µT */
static float QMC5883_Declination = -4.8f;	/* 磁偏角：湖南；换地区要改 */

/* ---- 姿态可信度门控 ---- */
#define ACC_TRUST_TOL_G		0.25f		/* ‖a‖ 偏离 1g 超过它 → 冻结 acc 修正 */
#define MAG_NORM_MIN_UT		15.0f
#define MAG_NORM_MAX_UT		90.0f
#define MAG_BASE_FRAMES		50			/* 开机 1s 攒磁场模值基准 */

#define BASELINE_FRAMES		25			/* 开机 0.5s 攒姿态基线（须静止佩戴） */

/* ---- 恢复阶梯（单位：帧，20ms/帧）---- */
#define ERR_L1_FRAMES		3
#define ERR_L2_FRAMES		6
#define FAULT_RETRY_FRAMES	50			/* L3 降级后每 1s 重试一次 */
#define FAULT_REPORT_FRAMES	6000		/* 故障日志 120s 闸门 */

/* ---- 日志节奏 ---- */
#define SUMMARY_FRAMES		50			/* 1s 一行数据汇总（验机用，出厂可关） */
#define FALL_DET_SUMMARY_LOG	1		/* 0 = 关闭 1s 汇总与 10s 统计 */
#define STATS_FRAMES		500
#define DROP_TOLERANCE_MS	30

static const char *TAG = "fall_det";

/* ===================== 模块状态 ===================== */

/* 原始值（本帧），汇总日志要用 */
static int16_t s_ax, s_ay, s_az, s_gx, s_gy, s_gz, s_hx, s_hy, s_hz;

/* 陀螺零偏（°/s）与校准标志 */
static float s_bias_x, s_bias_y, s_bias_z;
static bool s_calibrated = false;

/* 磁场状态：0 正常 / 1 读失败 / 2 量程溢出（OVL 是环境干扰，不触发总线恢复） */
static uint8_t s_mag_status = 0;
static float s_mag_base_norm = 0.0f;
static float s_mag_norm_sum = 0.0f;
static uint32_t s_mag_base_cnt = 0;

/* 姿态基线（开机静止期采集）：跌倒判定的参考姿态 */
static float s_base_pitch = 0.0f, s_base_roll = 0.0f;
static uint32_t s_base_cnt = 0;
static float s_base_p_sum = 0.0f, s_base_r_sum = 0.0f;

/* 跌倒状态机 */
typedef enum { FS_CALI = 0, FS_WATCH, FS_IMPACT, FS_FALLEN } fall_state_t;
static fall_state_t s_state = FS_CALI;
static uint32_t s_impact_elapsed_ms = 0;	/* IMPACT 态：冲击后已过时间 */
static uint32_t s_posture_ms = 0;		/* IMPACT 态：翻倒姿态连续保持时长 */
static uint32_t s_rearm_ms = 0;			/* FALLEN 态：直立持续时间 */
static uint32_t s_ff_ms = 0;			/* WATCH 态：自由落体证据累积 */
static volatile bool s_cancel_req = false;	/* fall_det_cancel() 置位 */
static uint32_t s_fall_count = 0;
static bool s_fault_reported = false;		/* FAULT 事件只发一次，恢复后清零 */

/* 回调 */
static fall_det_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;

/* 运行统计 */
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

/* ===================== 工具 ===================== */

/* 环差：把任意角度归一到 [-180, 180]。yaw/roll 跨界融合必须走它，
 * 直接线性混合 +179° 与 −179° 会得到 ≈175°，滤波器随后"绕远路"爬回去 */
static float wrap180(float deg)
{
	return fmodf(deg + 540.0f, 360.0f) - 180.0f;
}

/* ===================== 陀螺零偏校准（移植自 mzx） ===================== */

/* 单帧静止判据：重力模值接近 1g 且三轴角速度都很小。
 * 只看均值不够——运动有正有负能互相抵消，均值会骗人。 */
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
 * 陀螺零偏校准。零偏只能来自陀螺三轴（不是加速度——那是重力分量）。
 * 三层保护：等静止（≤10s）→ 逐帧剔除非静止 → 标准差检验。
 * 失败不致命：零偏按 0 跑，倾角仍可用，航向会漂（跌倒判定不受影响）。
 */
static void calibrate_gyro(void)
{
	s_calibrated = false;

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
		ESP_LOGW(TAG, "等待静止超时（%d ms），跳过校准；航向会漂移，倾角不受影响",
			 CALI_MAX_WAIT_MS);
		return;
	}

	int64_t sum[3] = { 0, 0, 0 };
	int64_t sumsq[3] = { 0, 0, 0 };
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
		sum[0] += bX;
		sum[1] += bY;
		sum[2] += bZ;
		sumsq[0] += (int64_t)bX * bX;
		sumsq[1] += (int64_t)bY * bY;
		sumsq[2] += (int64_t)bZ * bZ;
		n++;
	}

	if (n < MPU6050_CALI_SAMPLES / 2) {
		ESP_LOGW(TAG, "校准有效样本不足（%u/%u）", (unsigned)n,
			 (unsigned)MPU6050_CALI_SAMPLES);
		return;
	}

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
		var[i] = (v > 0.0f) ? v : 0.0f;
	}

	const float sigma_dps = sqrtf((var[0] + var[1] + var[2]) / 3.0f) /
				MPU6050_GYRO_SENS_2000DPS;
	if (sigma_dps > CALI_SIGMA_MAX_DPS) {
		ESP_LOGW(TAG, "校准样本扰动过大（σ=%.2f °/s），作废", sigma_dps);
		return;
	}

	s_bias_x = mean_lsb[0] / MPU6050_GYRO_SENS_2000DPS;
	s_bias_y = mean_lsb[1] / MPU6050_GYRO_SENS_2000DPS;
	s_bias_z = mean_lsb[2] / MPU6050_GYRO_SENS_2000DPS;

	/* 真静止的零偏不可能超 1°/s；超了是器件/安装问题，清零并告警 */
	if (fabsf(s_bias_x) > 1.0f || fabsf(s_bias_y) > 1.0f || fabsf(s_bias_z) > 1.0f) {
		ESP_LOGW(TAG, "零偏异常（%.2f, %.2f, %.2f °/s），已清零",
			 s_bias_x, s_bias_y, s_bias_z);
		s_bias_x = s_bias_y = s_bias_z = 0.0f;
		return;
	}

	s_calibrated = true;
	ESP_LOGI(TAG, "零偏校准完成: (%.3f, %.3f, %.3f) °/s, σ=%.3f, n=%u",
		 s_bias_x, s_bias_y, s_bias_z, sigma_dps, (unsigned)n);
}

/* ===================== 姿态融合（移植自 mzx IMU_GetData） ===================== */

static float acc_pitch_of(int16_t AX_, int16_t AY_, int16_t AZ_)
{
	return atan2f(-(float)AX_, sqrtf((float)AY_ * (float)AY_ + (float)AZ_ * (float)AZ_))
	       * 180.0f / Sensor_PI;
}

static float acc_roll_of(int16_t AY_, int16_t AZ_)
{
	return atan2f((float)AY_, (float)AZ_) * 180.0f / Sensor_PI;
}

/*
 * 读一次传感器并做一帧姿态融合（互补滤波 + 磁力计倾角补偿）。
 * 返回值只反映 MPU6050 读取结果：总线健康与否 IMU 就能代表。
 * 磁力计失败只置 s_mag_status——OVL 溢出是物理干扰，驱动总线恢复没有意义。
 */
static esp_err_t attitude_step(float *pitch, float *roll, float *yaw, float dt)
{
	float angle_pitch = *pitch;
	float angle_roll = *roll;
	float angle_yaw = *yaw;

	esp_err_t err = Mpu6050_Get_Data(&s_ax, &s_ay, &s_az, &s_gx, &s_gy, &s_gz);
	if (err != ESP_OK) {
		return err;	/* 读失败不动姿态：脏数据比不更新更糟 */
	}

	err = QMC5883_GetData(&s_hx, &s_hy, &s_hz);
	if (err == ESP_OK) {
		s_mag_status = 0;
	} else if (err == ESP_ERR_INVALID_RESPONSE) {
		s_mag_status = 2;	/* OVL */
	} else {
		s_mag_status = 1;
	}

	/* 先除灵敏度（→物理单位）再减零偏；bias 存的是 °/s，顺序反了量纲就错 */
	const float groy_x_rate = (float)s_gx / MPU6050_GYRO_SENS_2000DPS - s_bias_x;
	const float groy_y_rate = (float)s_gy / MPU6050_GYRO_SENS_2000DPS - s_bias_y;
	const float groy_z_rate = (float)s_gz / MPU6050_GYRO_SENS_2000DPS - s_bias_z;

	const float acc_x = (float)s_ax / MPU6050_ACCEL_SENS_8G;
	const float acc_y = (float)s_ay / MPU6050_ACCEL_SENS_8G;
	const float acc_z = (float)s_az / MPU6050_ACCEL_SENS_8G;

	const float mag_x = (float)s_hx / QMC5883_LSB_PER_UT;
	const float mag_y = (float)s_hy / QMC5883_LSB_PER_UT;
	const float mag_z = (float)s_hz / QMC5883_LSB_PER_UT;

	/* 可信度门控：acc 偏离 1g → 冻结倾角修正（自由落体时方向向量是纯噪声）；
	 * mag 模值出界/畸变 → 冻结航向修正（铁门、喇叭磁铁） */
	const float acc_norm = sqrtf(acc_x * acc_x + acc_y * acc_y + acc_z * acc_z);
	const bool acc_trust = (fabsf(acc_norm - 1.0f) < ACC_TRUST_TOL_G);

	const float mag_norm = sqrtf(mag_x * mag_x + mag_y * mag_y + mag_z * mag_z);
	bool mag_trust = false;
	if (s_mag_status == 0) {
		if (s_mag_base_cnt < MAG_BASE_FRAMES) {
			s_mag_norm_sum += mag_norm;
			s_mag_base_cnt++;
			if (s_mag_base_cnt == MAG_BASE_FRAMES) {
				s_mag_base_norm = s_mag_norm_sum / (float)MAG_BASE_FRAMES;
				ESP_LOGI(TAG, "磁场基准: |m|=%.1f µT（正常 25~65µT）",
					 s_mag_base_norm);
			}
			mag_trust = true;
		} else {
			const float ratio = mag_norm / s_mag_base_norm;
			mag_trust = !((mag_norm < MAG_NORM_MIN_UT || mag_norm > MAG_NORM_MAX_UT) ||
				      (ratio > 1.4f || ratio < 0.6f));
		}
	}

	const float acc_pitch = acc_pitch_of(s_ax, s_ay, s_az);
	const float acc_roll = acc_roll_of(s_ay, s_az);

	const float pitch_pred = angle_pitch + groy_y_rate * dt;
	const float roll_pred = angle_roll + groy_x_rate * dt;
	const float yaw_pred = wrap180(angle_yaw + groy_z_rate * dt);

	if (acc_trust) {
		angle_pitch = COMP_FILTER_ALPHA * pitch_pred + (1.0f - COMP_FILTER_ALPHA) * acc_pitch;
		angle_roll = COMP_FILTER_ALPHA * roll_pred + (1.0f - COMP_FILTER_ALPHA) * acc_roll;
	} else {
		angle_pitch = pitch_pred;
		angle_roll = roll_pred;
	}

	/* 航向修正：磁力计倾角补偿投影到水平面，融合"误差"而不是直接混合角度 */
	if (mag_trust) {
		const float roll_rad = angle_roll * Sensor_PI / 180.0f;
		const float pitch_rad = angle_pitch * Sensor_PI / 180.0f;
		const float sp = sinf(pitch_rad), cp = cosf(pitch_rad);
		const float sr = sinf(roll_rad), cr = cosf(roll_rad);

		const float BX = mag_x * cp + mag_y * sr * sp + mag_z * cr * sp;
		const float BY = mag_y * cr - mag_z * sr;
		const float mag_yaw = atan2f(BY, BX) * 180.0f / Sensor_PI + QMC5883_Declination;

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

/* ===================== 恢复阶梯（移植自 mzx，加 FAULT 事件） ===================== */

static void notify(fall_det_event_t evt, float pitch, float roll)
{
	if (s_cb != NULL) {
		s_cb(evt, pitch, roll, s_cb_ctx);
	}
}

static esp_err_t sensors_reinit(void)
{
	esp_err_t err = I2C_Recover();
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "L2 总线复位失败: %s", esp_err_to_name(err));
	}
	err = Mpu6050_Init();	/* MPU 是总线探针：它活了总线才算通 */
	if (err != ESP_OK) {
		return err;
	}
	return QMC5883_Init();
}

/*
 * L0 等（新驱动超时自动复位硬件状态机）→ L1 复位总线 → L2 重初始化
 * → L3 降级 + 周期重试 + FAULT 事件。只有 IMU 失败驱动阶梯（同一根总线）。
 */
static void recover_step(uint32_t *err_run, float pitch, float roll)
{
	(*err_run)++;

	if (*err_run < ERR_L1_FRAMES) {
		return;
	}

	if (*err_run == ERR_L1_FRAMES) {
		s_stats.l1++;
		ESP_LOGW(TAG, "IMU 连续失败 %u 帧 → L1 复位总线", (unsigned)*err_run);
		if (I2C_Recover() == ESP_OK && Mpu6050_GetID() == MPU6050_ID_VALUE) {
			*err_run = 0;
		}
		return;
	}

	if (*err_run == ERR_L2_FRAMES) {
		s_stats.l2++;
		ESP_LOGW(TAG, "IMU 连续失败 %u 帧 → L2 重初始化", (unsigned)*err_run);
		if (sensors_reinit() == ESP_OK) {
			*err_run = 0;
		}
		return;
	}

	/* L3：接线/供电/器件问题。监护设备不能默默重试——家属必须知道 */
	if (*err_run == ERR_L2_FRAMES + 1) {
		s_stats.faults++;
		ESP_LOGE(TAG, "IMU 持续无响应 → L3 降级，每 %d ms 重试（累计故障 %u）",
			 FAULT_RETRY_FRAMES * 20, (unsigned)s_stats.faults);
		if (!s_fault_reported) {
			s_fault_reported = true;
			notify(FALL_DET_EVT_SENSOR_FAULT, pitch, roll);
		}
	}

	if ((*err_run % FAULT_RETRY_FRAMES) == 0) {
		if ((*err_run % FAULT_REPORT_FRAMES) == 0) {
			s_stats.faults++;
			ESP_LOGE(TAG, "传感器无响应 %u 帧（约 %u s）",
				 (unsigned)*err_run, (unsigned)(*err_run * 20 / 1000));
		}
		if (sensors_reinit() == ESP_OK) {
			ESP_LOGI(TAG, "降级状态重连成功，恢复采集");
			*err_run = 0;
		}
	}
}

/* ===================== 跌倒状态机 ===================== */

/* 姿态相对基线的偏离（°）：取俯仰/横滚里更大的那个。
 * roll 走环差（±180° 环形连续）；pitch ±90° 单调连续，直接作差 */
static float posture_deviation(float pitch, float roll)
{
	const float dp = pitch - s_base_pitch;
	const float dr = wrap180(roll - s_base_roll);
	const float adp = fabsf(dp), adr = fabsf(dr);
	return (adp > adr) ? adp : adr;
}

/*
 * 每帧喂一次跌倒状态机。
 * acc_norm / dt 由采集循环提供（attitude_step 已换算好物理单位）。
 */
static void fall_fsm(float pitch, float roll, float acc_norm, float dt_ms)
{
	const float dev = posture_deviation(pitch, roll);

	switch (s_state) {
	case FS_CALI:
		/* 基线采集期：只攒基线，不判定 */
		if (fabsf(acc_norm - 1.0f) < ACC_TRUST_TOL_G) {
			s_base_p_sum += pitch;
			s_base_r_sum += roll;
			s_base_cnt++;
			if (s_base_cnt >= BASELINE_FRAMES) {
				s_base_pitch = s_base_p_sum / (float)s_base_cnt;
				s_base_roll = s_base_r_sum / (float)s_base_cnt;
				s_state = FS_WATCH;
				ESP_LOGI(TAG, "基线就绪: pitch=%.1f° roll=%.1f° → 开始监控",
					 s_base_pitch, s_base_roll);
			}
		}
		break;

	case FS_WATCH:
		/* 自由落体证据：跌倒前必有失重段。只累积，不单独触发 */
		if (acc_norm < FALL_FREEFALL_G) {
			s_ff_ms += (uint32_t)dt_ms;
		} else if (acc_norm > 0.5f) {
			s_ff_ms = 0;
		}

		if (acc_norm > FALL_IMPACT_G) {
			s_impact_elapsed_ms = 0;
			s_posture_ms = 0;
			ESP_LOGW(TAG, "冲击 %.2fg（失重证据 %u ms）→ 进入 %d ms 判定窗",
				 acc_norm, (unsigned)s_ff_ms, FALL_WINDOW_MS);
			s_ff_ms = 0;
			s_state = FS_IMPACT;
		}
		break;

	case FS_IMPACT:
		s_impact_elapsed_ms += (uint32_t)dt_ms;

		if (dev > FALL_ANGLE_DEG) {
			s_posture_ms += (uint32_t)dt_ms;	/* 连续保持才累计 */
		} else if (dev < FALL_CANCEL_DEG) {
			/* 回到直立：误报出口（也覆盖"冲击后马上站起来"） */
			s_posture_ms = 0;
			if (s_impact_elapsed_ms > FALL_CONFIRM_MS) {
				ESP_LOGI(TAG, "冲击后恢复直立，判定未跌倒");
				s_state = FS_WATCH;
				break;
			}
		} else {
			s_posture_ms = 0;	/* 30°~55° 过渡带：保持中断，重新计 */
		}

		if (s_posture_ms >= FALL_CONFIRM_MS) {
			s_fall_count++;
			s_state = FS_FALLEN;
			s_rearm_ms = 0;
			ESP_LOGW(TAG, "跌倒确认 #%u: 偏离 %.0f°（pitch=%.1f roll=%.1f 基线 %.1f/%.1f）"
				 " 保持 %u ms",
				 (unsigned)s_fall_count, dev, pitch, roll,
				 s_base_pitch, s_base_roll, (unsigned)s_posture_ms);
			notify(FALL_DET_EVT_FALL, pitch, roll);
			break;
		}

		if (s_impact_elapsed_ms > FALL_WINDOW_MS) {
			ESP_LOGI(TAG, "判定窗超时未确认（偏离 %.0f°），回到监控", dev);
			s_state = FS_WATCH;
		}
		break;

	case FS_FALLEN:
		/* 撤销优先级最高（"我没事"语音/按键都走这里） */
		if (s_cancel_req) {
			s_cancel_req = false;
			ESP_LOGI(TAG, "跌倒告警被撤销（cancel），重新武装");
			s_state = FS_WATCH;
			break;
		}
		/* 自行站起足够久 → 自动重新武装（跌倒后自己爬起来的场景） */
		if (dev < FALL_CANCEL_DEG) {
			s_rearm_ms += (uint32_t)dt_ms;
			if (s_rearm_ms >= FALL_REARM_MS) {
				ESP_LOGI(TAG, "检测到自行站起，重新武装");
				s_state = FS_WATCH;
			}
		} else {
			s_rearm_ms = 0;
		}
		break;

	default:
		s_state = FS_WATCH;
		break;
	}
}

/* ===================== 采集任务 ===================== */

static void fall_det_task(void *pv)
{
	(void)pv;
#if FALL_DET_CSV_MODE
	/* ---- 采集模式第一步：静音 ----
 * 日志会撕碎 CSV 行，必须在任何输出之前关掉。
 * 注意 "*" 会把下面所有 ESP_LOGE/ESP_LOGW 一起静音（含 IMU 初始化失败），
 * 所以采数据时如果一颗传感器挂了，你只能从 mag_ok 列和 ‖a‖ 看异常。 */
	esp_log_level_set("*", ESP_LOG_NONE);

	/* 不关缓冲，数据会攒着不吐、时间戳和对数错位 */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* 按键脚：与 KEY 组件同配置，重复 config 无副作用 */
	gpio_config_t mk = {
		.pin_bit_mask = 1ULL << CSV_MARK_GPIO,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&mk);

	/* 等 USB 转串口枚举完成，否则头几行会丢 */
	vTaskDelay(pdMS_TO_TICKS(300));
	csv_head();

#endif



	ESP_LOGI(TAG, "任务启动: 50Hz, 优先级 8, 栈 4096, 核 1");

	/* ---- 初始化：总线 → 两颗传感器（失败不装死，恢复阶梯接手）---- */
	esp_err_t err = i2c_Init_Final();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "I2C 总线初始化失败: %s（持续重试）", esp_err_to_name(err));
	}
	if (Mpu6050_Init() != ESP_OK) {
		ESP_LOGE(TAG, "MPU6050 初始化失败，交由恢复阶梯处理");
	}
	if (QMC5883_Init() != ESP_OK) {
		ESP_LOGE(TAG, "QMC5883L 初始化失败，航向不可用");
	}

	/* ---- 校准（最长 ~12s；期间 FS_CALI 只攒基线）---- */
	calibrate_gyro();
	if (!s_calibrated) {
		ESP_LOGW(TAG, "零偏未校准（航向漂移，倾角不受影响）");
	}

	TickType_t last_wake = xTaskGetTickCount();
	const TickType_t period = pdMS_TO_TICKS(20);
	int64_t last_us = esp_timer_get_time();

	float pitch = 0.0f, roll = 0.0f, yaw = 0.0f;
	uint32_t err_run = 0;

	for (;;) {
		/* 绝对时刻排程：单帧耗时抖动不累积到周期上 */
		vTaskDelayUntil(&last_wake, period);

		const int64_t now_us = esp_timer_get_time();
		float dt = (float)(now_us - last_us) / 1000000.0f;
		last_us = now_us;
		s_stats.last_dt_ms = dt * 1000.0f;

		if (dt <= 0.0f || dt > 0.5f) {
			dt = Sensor_DT;	/* 时间戳异常退回标称周期 */
		} else if (dt * 1000.0f > (float)DROP_TOLERANCE_MS) {
			uint32_t missed = (uint32_t)(dt * 1000.0f / (Sensor_DT * 1000.0f) + 0.5f) - 1;
			if (missed == 0) {
				missed = 1;
			}
			s_stats.drops += missed;
			ESP_LOGW(TAG, "丢拍 %u 帧（间隔 %.1f ms），累计 %u",
				 (unsigned)missed, s_stats.last_dt_ms, (unsigned)s_stats.drops);
		}

		s_stats.frames++;

		err = attitude_step(&pitch, &roll, &yaw, dt);
		if (err != ESP_OK) {
			s_stats.imu_err++;
			recover_step(&err_run, pitch, roll);
			continue;	/* 本帧无有效姿态，不喂状态机 */
		}

		if (err_run > 0) {
			ESP_LOGI(TAG, "IMU 恢复，失败计数 %u 清零", (unsigned)err_run);
			err_run = 0;
			if (s_fault_reported) {
				s_fault_reported = false;
				notify(FALL_DET_EVT_SENSOR_RECOVER, pitch, roll);
			}
		}

		if (s_mag_status == 1) {
			s_stats.mag_err++;
		} else if (s_mag_status == 2) {
			s_stats.mag_ovl++;
		}

		/* 当前帧的物理量（状态机与日志共用） */
		const float acc_x = (float)s_ax / MPU6050_ACCEL_SENS_8G;
		const float acc_y = (float)s_ay / MPU6050_ACCEL_SENS_8G;
		const float acc_z = (float)s_az / MPU6050_ACCEL_SENS_8G;
		const float acc_norm = sqrtf(acc_x * acc_x + acc_y * acc_y + acc_z * acc_z);

		fall_fsm(pitch, roll, acc_norm, dt * 1000.0f);

#if FALL_DET_CSV_MODE
		/* 每帧都输出，磁力计失败帧也输出（mag_ok=0）。
		 * 丢帧会让离线分析误判采样率掉了，宁可标"这次没读到"也不留空洞。 */
		csv_frame(now_us, s_ax, s_ay, s_az, s_gx, s_gy, s_gz,
			  s_hx, s_hy, s_hz, (s_mag_status == 0) ? 1 : 0);

		/* 按键下降沿打标记：一次按下只打一行，按住不会连打 */
		const bool mark_lvl = (gpio_get_level(CSV_MARK_GPIO) == 0);
		if (mark_lvl && !s_mark_prev) {
			printf("MARK,%lld\n", (long long)now_us);
		}
		s_mark_prev = mark_lvl;
# else
		/* 1s 汇总：三个验收数字（静止 ‖a‖≈1g / ‖m‖ 25~65µT / 角度合理） */
		if ((s_stats.frames % SUMMARY_FRAMES) == 0) {
			const float gx = (float)s_gx / MPU6050_GYRO_SENS_2000DPS - s_bias_x;
			const float gy = (float)s_gy / MPU6050_GYRO_SENS_2000DPS - s_bias_y;
			const float gz = (float)s_gz / MPU6050_GYRO_SENS_2000DPS - s_bias_z;
			const float mx = (float)s_hx / QMC5883_LSB_PER_UT;
			const float my = (float)s_hy / QMC5883_LSB_PER_UT;
			const float mz = (float)s_hz / QMC5883_LSB_PER_UT;
			ESP_LOGI(TAG, "seq=%u |a|=%.3fg |w|=%.2f°/s |m|=%.1fµT | "
				      "p=%.1f r=%.1f y=%.1f | st=%d dev=%.0f° | %.1fms "
				      "err=%u/%u ovl=%u drop=%u L1=%u L2=%u fall=%u",
				 (unsigned)s_stats.frames, acc_norm,
				 sqrtf(gx * gx + gy * gy + gz * gz),
				 sqrtf(mx * mx + my * my + mz * mz),
				 pitch, roll, yaw, (int)s_state,
				 (s_state == FS_CALI) ? 0.0f : posture_deviation(pitch, roll),
				 s_stats.last_dt_ms,
				 (unsigned)s_stats.imu_err, (unsigned)s_stats.mag_err,
				 (unsigned)s_stats.mag_ovl, (unsigned)s_stats.drops,
				 (unsigned)s_stats.l1, (unsigned)s_stats.l2,
				 (unsigned)s_fall_count);
		}
		if ((s_stats.frames % STATS_FRAMES) == 0) {
			ESP_LOGI(TAG, "统计: 帧=%u drop=%u imu_err=%u mag_err=%u ovl=%u "
				      "L1=%u L2=%u 故障=%u 跌倒=%u",
				 (unsigned)s_stats.frames, (unsigned)s_stats.drops,
				 (unsigned)s_stats.imu_err, (unsigned)s_stats.mag_err,
				 (unsigned)s_stats.mag_ovl, (unsigned)s_stats.l1,
				 (unsigned)s_stats.l2, (unsigned)s_stats.faults,
				 (unsigned)s_fall_count);
		}
#endif
	}
}

/* ===================== 公共 API ===================== */

esp_err_t fall_det_start(fall_det_cb_t cb, void *ctx)
{
	s_cb = cb;
	s_cb_ctx = ctx;

	static bool s_task_up = false;
	if (s_task_up) {
		return ESP_OK;		/* 幂等：重复调用只更新回调 */
	}

	memset(&s_stats, 0, sizeof(s_stats));
	s_state = FS_CALI;

	if (xTaskCreatePinnedToCore(fall_det_task, "fall_det", 4096, NULL, 8, NULL, 1) != pdPASS) {
		ESP_LOGE(TAG, "任务创建失败（栈 4096 不足？）");
		return ESP_FAIL;
	}
	s_task_up = true;
	return ESP_OK;
}

void fall_det_cancel(void)
{
	s_cancel_req = true;	/* fall_det 任务下一帧生效，任何上下文调用都安全 */
}

bool fall_det_fall_active(void)
{
	return s_state == FS_FALLEN;
}
