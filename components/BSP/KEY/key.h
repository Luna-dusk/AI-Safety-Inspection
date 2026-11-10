#ifndef __KEY_H
#define __KEY_H

#include "driver/gpio.h"
#include <stdbool.h>

#define KEY_GPIO_PIN      GPIO_NUM_2
#define BOOT_GPIO_PIN     GPIO_NUM_0
#define KEY_ACTIVE_LEVEL  0
#define KEY_DEBOUNCE_MS   30
#define KEY_BOOT_LONG_PRESS_MS 3000  /* BOOT 长按阈值：进入配网模式 */

/**
 * @brief  初始化两个按键(GPIO2 / GPIO0)并启动 20 ms 扫描任务
 */
void key_init(void);
/**
 * @brief  读取并清除一次按键唤醒请求（读取即消费）
 * @return true=本次取到了未处理的按键请求
 * @note   两个键共用同一个标志，无法区分键值与按住时长。
 */
bool key_consume_wake_request(void);
/**
 * @brief  读取并清除一次 BOOT 长按请求（读取即消费）
 * @return true=本次取到了未处理的长按请求
 * @note   扫描任务检测到 BOOT 稳定按住 >= KEY_BOOT_LONG_PRESS_MS 时置位，
 *         并已同时调用 wifi_provision_request() 进入配网；本接口仅供
 *         其它模块旁路观察/自测，不影响配网触发。
 */
bool key_consume_boot_long_press(void);

#endif
