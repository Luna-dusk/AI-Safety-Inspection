#ifndef FALL_DET_H
#define FALL_DET_H

/*
 * FALL_DET：跌倒检测（FR-3）—— MPU6050 + QMC5883L 50Hz 采集 + 姿态融合 + 跌倒状态机。
 *
 * 采集/校准/姿态融合/恢复阶梯移植自 mzx 分支 hello_world_main.c（2026-11 提取），
 * 跌倒判定状态机按该分支注释里预留的判据（2.5g 冲击 + 姿态保持）补全。
 *
 * 判定链（详见 fall_det.c 头部注释）：
 *   冲击(‖a‖>2.5g) → 5s 窗内姿态偏离基线 >55° 持续 3s → FALL 事件回调。
 *
 * 对外只有三件事：注册回调、撤销告警、查状态。
 * 回调跑在 fall_det 任务上下文（优先级 8，核 1）——只做轻活（置标志/投队列），
 * 别在里面播 I2S、发网络（同 local_cmd_reply_cb 的约束，拖死采集任务漏真跌倒）。
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FALL_DET_EVT_FALL = 0,       /**< 跌倒确认（一次跌倒只发一次，直到撤销/自行站起） */
    FALL_DET_EVT_SENSOR_FAULT,   /**< 传感器 L3 故障（持续无响应）——监护设备必须上报 */
    FALL_DET_EVT_SENSOR_RECOVER, /**< 传感器从 L3 故障中恢复 */
} fall_det_event_t;

/**
 * @brief  事件回调（fall_det 任务上下文，勿阻塞）
 * @param  evt   事件类型
 * @param  pitch 当前俯仰角（°，用于告警上报带姿态信息）
 * @param  roll  当前横滚角（°）
 * @param  ctx   注册时传入的用户指针
 */
typedef void (*fall_det_cb_t)(fall_det_event_t evt, float pitch, float roll, void *ctx);

/**
 * @brief  启动跌倒检测（幂等；重复调用只更新回调）
 *
 * 内部自建任务（栈 4096，优先级 8，核 1）完成：I2C 总线与两颗传感器初始化 →
 * 陀螺零偏校准（等静止，最长约 12s，期间不判跌倒）→ 姿态基线采集 → 50Hz 监控。
 * 传感器不在线不报错启动失败——恢复阶梯会持续重试并回调 FAULT 事件。
 *
 * @return ESP_OK 任务已创建；ESP_FAIL 任务创建失败（内存不足）
 */
esp_err_t fall_det_start(fall_det_cb_t cb, void *ctx);

/**
 * @brief  撤销当前跌倒告警（需求场景 2：老人说"我没事"）
 *
 * 语义：FALL 状态 → 回 WATCH 重新武装。跑在任何任务上下文都安全（置标志，
 * fall_det 任务在下一帧生效）。未处于 FALL 状态时是空操作。
 */
void fall_det_cancel(void);

/** @brief 当前是否处于"跌倒已确认"状态（FALL 事件已发、未被撤销） */
bool fall_det_fall_active(void);

#ifdef __cplusplus
}
#endif

#endif /* FALL_DET_H */
