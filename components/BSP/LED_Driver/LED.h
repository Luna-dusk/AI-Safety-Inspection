#ifndef __LED_H__
#define __LED_H__

#include <driver/gpio.h>

#define LED_GPIO_PIN GPIO_NUM_1
//LED状态枚举
enum LED_GPIO_State
{
    PIN_RESET,
    PIN_SET
};
//设置LED状态
#define LED(X) do {X?\
                    gpio_set_level(LED_GPIO_PIN, PIN_SET):\
                    gpio_set_level(LED_GPIO_PIN, PIN_RESET);} while(0)
//反转LED状态
#define LED_TOGGLE() do {gpio_set_level(LED_GPIO_PIN, !gpio_get_level(LED_GPIO_PIN));} while(0)
//LED初始化函数
/**
 * @brief  初始化状态指示灯(GPIO1)为输入输出模式，默认熄灭
 */
void LED_Init(void);

#endif