#include "key.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_provision.h"

static const char *TAG = "KEY";

static volatile bool wake_request = false;
static volatile bool boot_long_press_request = false;

typedef struct {
    int pin;
    bool last_stable;
    bool last_raw;
    int64_t last_change_us;
} key_slot_t;

static key_slot_t keys[2];

/**
 * @brief  读取按键引脚原始电平，并换算成"是否处于按下状态"
 * @param  pin  GPIO 编号
 * @return true=按下，false=松开
 *
 * 按键一端接 GPIO、另一端接 GND，因此按下为低电平；
 * KEY_ACTIVE_LEVEL 定义了极性（当前为 0）。
 */
static bool key_raw_pressed(int pin)
{
    return gpio_get_level(pin) == KEY_ACTIVE_LEVEL;
}

/**
 * @brief  单键消抖 + "按下"边沿检测
 * @param  k  按键运行时状态（引脚、原始电平、稳定电平、最后跳变时刻）
 * @return true 仅表示"刚刚稳定按下"的这一拍；按住不放不会重复返回 true
 *
 * 分两步：
 *   1) 原始电平与上次不同 -> 记录跳变时刻并返回 false，开始消抖计时；
 *   2) 原始电平保持不变且已稳定超过 KEY_DEBOUNCE_MS -> 更新 last_stable，
 *      若这次是由"松开"变为"按下"，则返回 true。
 *
 * @note 由 20 ms 轮询任务调用，因此实际消抖窗口是 30~50 ms。
 * @note 只检测按下沿，不检测松开沿，也不支持长按判定。
 */
static bool key_update_edge(key_slot_t *k)
{
    bool raw = key_raw_pressed(k->pin);
    int64_t now = esp_timer_get_time();

    if (raw != k->last_raw) {
        k->last_raw = raw;
        k->last_change_us = now;
        return false;
    }
    if ((now - k->last_change_us) < (int64_t)KEY_DEBOUNCE_MS * 1000) {
        return false;
    }
    bool edge = (raw && !k->last_stable);
    k->last_stable = raw;
    return edge;
}

/**
 * @brief  按键扫描任务（20 ms 周期，优先级 10）
 * @param  arg 未使用
 *
 * 每 20 ms 轮询两个按键；任一键检测到"按下"边沿，就把共享的
 * wake_request 置为 true，由 afe_sr.c 的 fetch 任务取走并唤醒会话。
 *
 * 另检测 BOOT 键长按：稳定按住 >= KEY_BOOT_LONG_PRESS_MS 触发一次
 * 配网请求（wifi_provision_request），单次按住只触发一回。
 *
 * @note wake_request 两个按键共用、不带时长，行为与旧版完全一致；
 *       BOOT 长按是在此基础上的增量功能，不影响语音唤醒。
 */
static void key_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "key scan start GPIO%d+GPIO%d", keys[0].pin, keys[1].pin);
    int64_t boot_press_start = 0;   /* BOOT 稳定按住的起始时刻，0=未按 */
    bool boot_latched = false;      /* 本次按住已触发过长按，防重复 */
    while (1) {
        for (int i = 0; i < 2; i++) {
            if (key_update_edge(&keys[i])) {
                wake_request = true;
                ESP_LOGI(TAG, "key GPIO%d pressed", keys[i].pin);
            }
        }
        if (keys[1].last_stable) {
            int64_t now = esp_timer_get_time();
            if (boot_press_start == 0) {
                boot_press_start = now;
            }
            else if (!boot_latched &&
                     now - boot_press_start >= (int64_t)KEY_BOOT_LONG_PRESS_MS * 1000) {
                boot_latched = true;
                boot_long_press_request = true;
                ESP_LOGI(TAG, "BOOT long press -> wifi provisioning");
                wifi_provision_request();
            }
        }
        else {
            boot_press_start = 0;
            boot_latched = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * @brief  初始化按键 GPIO 并启动扫描任务
 *
 * 按键表固定为两项：KEY_GPIO_PIN(2) 与 BOOT_GPIO_PIN(0)。
 * 上拉/下拉按 KEY_ACTIVE_LEVEL 自动选择；初始状态直接采样当前电平，
 * 避免上电瞬间把"已经按住的键"误判成一次按键事件。
 *
 * @note 任务句柄用 static 变量保存但未导出，任务创建后无法被删除。
 */
void key_init(void)
{
    keys[0] = (key_slot_t){ .pin = KEY_GPIO_PIN };
    keys[1] = (key_slot_t){ .pin = BOOT_GPIO_PIN };

    for (int i = 0; i < 2; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << keys[i].pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = (KEY_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = (KEY_ACTIVE_LEVEL == 1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        keys[i].last_raw = key_raw_pressed(keys[i].pin);
        keys[i].last_stable = keys[i].last_raw;
        keys[i].last_change_us = esp_timer_get_time();
    }
    wake_request = false;

    static TaskHandle_t h = NULL;
    xTaskCreate(key_scan_task, "key_scan", 4096, NULL, 10, &h);

    ESP_LOGI(TAG, "keys: GPIO%d BOOT=%d (press->GND)", (int)KEY_GPIO_PIN, (int)BOOT_GPIO_PIN);
}

/**
 * @brief  读取并清除一次唤醒请求（读取即消费）
 * @return true=本次取到了未处理的按键请求
 *
 * 典型的"取走即清空"语义，配合 afe_sr.c 在 SR_IDLE 状态下轮询调用，
 * 实现"按一下键就能唤醒"。
 *
 * @note 只有 fetch 任务在消费，单读者所以没加锁；
 *       若将来出现第二个消费者，需要改成临界区或队列。
 */
bool key_consume_wake_request(void)
{
    if (wake_request) { wake_request = false; return true; }
    return false;
}

/**
 * @brief  读取并清除一次 BOOT 长按请求（读取即消费）
 *
 * 配网触发已由扫描任务直接调 wifi_provision_request() 完成，本接口
 * 仅供其它模块旁路观察/自测；单读者，未加锁，与 wake_request 同套路。
 */
bool key_consume_boot_long_press(void)
{
    if (boot_long_press_request) { boot_long_press_request = false; return true; }
    return false;
}
