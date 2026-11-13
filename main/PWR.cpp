//
// Created by l on 2026/11/12.
//

#include "PWR.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "PWR";

#define PWR_CTRL_GPIO		GPIO_NUM_40
#define PWR_KEY_GPIO		GPIO_NUM_18
#define PWR_KEY_PRESSED_LV	0		/* K1 按下 = 低电平 */

#define TICK_MS			10		/* 定时器节拍 */
#define KICK_PERIOD_MS		200		/* 打脉冲周期，必须 << 2200ms */
#define KICK_WIDTH_MS		10		/* 脉冲宽度：C19+R14 的 τ=2ms，10ms 足够充满 */

#define KEY_DEBOUNCE_TICKS	3		/* 30ms 消抖：机械抖动不会凑够 */
#define BOOT_GRACE_MS		5000		/* 开机保护期 */
#define LONG_PRESS_MS		3000		/* 长按判定 */
#define RELEASE_TIMEOUT_MS	10000		/* 长按后迟迟不松手 → 取消关机 */

typedef enum {
    PS_GRACE = 0,                       //上电保护
    PS_RUN,                             //正常监测
    PS_WAIT_RELEASE,                    //已判长按，等待松手断电
    PS_DOWN,                            //已断电，等待电容释放余量
} pwr_state_t ;
//上电状态

static esp_timer_handle_t s_timer = NULL;
static pwr_shutdown_cb_t s_cb = NULL;
static bool s_shutting_down = false;

static pwr_state_t s_st = PS_GRACE;
static volatile uint32_t s_pulse_left = 0;	/* 距收回当前脉冲还剩多少 ms */
static uint32_t s_kick_acc = 0;
static uint32_t s_grace_ms = 0;
static uint32_t s_hold_ms = 0;
static uint32_t s_release_wait_ms = 0;
static bool s_key_stable = false;
static uint8_t s_key_same = 0;

static void pwr_tick(void *arg) {
    (void) arg;

    if (s_pulse_left > 0) {
        s_pulse_left -= TICK_MS;
        if (s_pulse_left == 0) {
            gpio_set_level(PWR_CTRL_GPIO,0);
        }
    }else if (s_st != PS_DOWN) {
        s_kick_acc += TICK_MS;
        if (s_kick_acc >= KICK_WIDTH_MS) {
            s_kick_acc = 0;
            gpio_set_level(PWR_CTRL_GPIO,1);
            s_pulse_left = KICK_WIDTH_MS;
        }
    }
    const bool pressed = (gpio_get_level(PWR_KEY_GPIO) == PWR_KEY_PRESSED_LV);
    if (pressed != s_key_stable) {
        if (s_key_same ++ >= KEY_DEBOUNCE_TICKS) {
            s_key_stable = pressed;
            s_key_same = 0;
        }
    }else s_key_same = 0;

    switch (s_st) {
        case PS_GRACE:
            s_grace_ms += TICK_MS;
            if (s_grace_ms >= BOOT_GRACE_MS) {
                s_st = PS_RUN;
                s_grace_ms = 0;
                s_hold_ms = 0;
                ESP_LOGI(TAG, "电源键生效（%dms 开机保护期结束）", BOOT_GRACE_MS);
            }
            break;
        case PS_RUN:
            if (s_key_stable) {
                s_hold_ms += TICK_MS;
                if (s_hold_ms >= LONG_PRESS_MS) {
                    ESP_LOGW  (TAG, "长按 %ums → 请松手关机",(unsigned)s_hold_ms);
                    s_st = PS_WAIT_RELEASE;
                    s_hold_ms = 0;
                    s_release_wait_ms = 0;
                }
            }else s_hold_ms  = 0;
            break;

        case PS_WAIT_RELEASE:
            if (!s_key_stable) {
                ESP_LOGW(TAG, "已松手，停止维持脉冲，约 2.2s 后断电");
                s_shutting_down = true;
                s_pulse_left = 0;
                gpio_set_level(PWR_CTRL_GPIO,0);
                s_st = PS_DOWN;
                if (s_cb != NULL) {
                    s_cb();
                }
            }else {
                s_release_wait_ms += TICK_MS;
                if (s_release_wait_ms >= RELEASE_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "长按后 %us 未松手，取消关机",
                 RELEASE_TIMEOUT_MS / 1000);
                    s_st = PS_RUN;
                    s_release_wait_ms = 0;
                }
            }
            break;

        case PS_DOWN:
        default:

            break;
    }


}
esp_err_t pwr_init(){
        gpio_config_t ctrl = {
            .pin_bit_mask = 1ull << PWR_CTRL_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        ESP_RETURN_ON_ERROR (gpio_config(&ctrl) , TAG, "pwr_ctrl gpio config");

        gpio_set_level(PWR_CTRL_GPIO,1);
        s_pulse_left = KICK_WIDTH_MS;

        gpio_config_t KEY{
            .pin_bit_mask = 1ull << PWR_KEY_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        ESP_RETURN_ON_ERROR(gpio_config(&KEY),TAG,"key gpio is config");

        const esp_timer_create_args_t arg = {
            .callback = pwr_tick,
            .arg =  NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "PWR",
            /* 指定初始化器必须把字段补全，否则 -Wmissing-field-initializers 会报警 */
            .skip_unhandled_events = false,
        };

        ESP_RETURN_ON_ERROR(esp_timer_create(&arg, &s_timer), TAG, "timer is created");
        ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer,TICK_MS * 1000), TAG, "Timer is started");

        ESP_LOGI(TAG, "电源自锁已接管：脉冲 %dms / %dms（C18 撑持 τ=2.2s）",
        KICK_WIDTH_MS, KICK_PERIOD_MS);

        return ESP_OK;
}

void pwr_set_shutdown_cb(pwr_shutdown_cb_t cb)
{
    s_cb = cb;
}

bool pwr_is_shutting_down(void)
{
    return s_shutting_down;
}