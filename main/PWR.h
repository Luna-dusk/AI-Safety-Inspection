//
// Created by l on 2026/11/12.
//

#ifndef HELLO_WORLD_PWR_H
#define HELLO_WORLD_PWR_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern  "C" {
#endif

/**
 * @brief  启动电源自锁维持 + 电源键管理
 *
 * 内部立刻打一个脉冲完成接管（不能等定时器首次触发），之后交给 esp_timer
 * 每 200ms 维持一次。
 *
 * @return ESP_OK 定时器已启动、自锁已接管
 *         其它   定时器创建/启动失败 —— 此时 2.2s 后必然断电，
 *                调用方不要继续初始化假装开机成功
 *
 * @note **必须放在 app_main 的第一行业务代码之前**。K1 松手后主开关栅极
 *       只靠 C18(22µF) 撑 2.2s（τ = R11×C18 = 100k×22µF）；I2C/传感器/校准
 *       加起来几百毫秒，晚于这个窗口就会"上电→掉电→重启"死循环，而且掉得太快
 *       串口都抓不到。
 * @note 为什么用 esp_timer 而不是任务：维持自锁是"晚 2.2s 就整机断电"的硬实时
 *       动作，普通任务会被 ESP-SR 的 AFE 抢占几百毫秒。
 * @note 副作用：本设计**不支持 deep sleep / light sleep** —— 睡眠时定时器停摆，
 *       2.2s 后直接掉电。要低功耗必须改硬件（带使能的 PMIC 或双稳态触发器）。
 * @note 验证方式：USB 供电下这一行有没有生效**完全看不出来**（USB 的 5V 直接把
 *       输出轨拉起），必须拔掉 USB、只用电池测开关机。
 */
esp_err_t pwr_init(void);

/**
 * @brief  关机前回调
 *
 * 在"长按 3s 后松手"的那一刻触发，之后约 2.2s 真正断电。这段时间就是留给
 * 收尾的窗口。
 *
 * @note 跑在 esp_timer 上下文，必须毫秒级返回：不能阻塞、不能播 I2S、
 *       不能等网络。要做的重活请投队列交给别的任务。
 */
typedef void (*pwr_shutdown_cb_t)(void);

/**
 * @brief  注册关机前回调
 *
 * @param cb 回调函数；传 NULL 表示取消注册
 *
 * @note 只保存指针，不会立即调用。可以在 pwr_init() 之后任意时刻注册。
 */
void pwr_set_shutdown_cb(pwr_shutdown_cb_t cb);

/**
 * @brief  是否已进入关机流程
 *
 * @return true  已确认关机（脉冲已停，正在等 C18 放电）
 *         false 正常运行
 *
 * @note 其它模块据此停止产生新事件，避免断电前发出"半截"告警或写坏 NVS。
 */
bool pwr_is_shutting_down(void);


#ifdef __cplusplus
}
#endif

#endif //HELLO_WORLD_PWR_H
