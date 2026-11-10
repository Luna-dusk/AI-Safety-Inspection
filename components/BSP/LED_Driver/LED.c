#include "LED.h"

/**
 * @brief  初始化状态指示灯（GPIO1）
 *
 * 配置为 GPIO_MODE_INPUT_OUTPUT：
 *   - 输出侧用于点灯；
 *   - 保留输入能力是为了让 LED_TOGGLE() 能通过 gpio_get_level() 回读当前状态，
 *     否则反相操作会读到 0，导致灯永远点不亮。
 * 内部上拉使能，默认输出低电平（熄灭）。
 *
 * @note 若改为 LEDC/PWM 调光，LED_TOGGLE() 的回读方式要一并改掉。
 */
void LED_Init(void)
{
    // Initialize the GPIO pin for the LED
    // This is a placeholder implementation and should be replaced with actual hardware initialization code
    // For example, you might need to set the pin mode, configure pull-up/pull-down resistors, etc.
    gpio_config_t gpio_init_struct={0};
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;
    gpio_init_struct.pin_bit_mask = (1ULL << LED_GPIO_PIN);
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;

    gpio_config(&gpio_init_struct);

    LED(0); // Turn off the LED initially
}