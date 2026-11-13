/**
 * @brief 扬声器诊断模块（无仪表版）——开机自动播一组"有意义的测试音"
 *
 * 场景：功放 VIN 改成 5V 之后要验证响度 / 排查"完全没声"。手上没有万用表、
 * 没有示波器时，唯一可靠的判据就是"耳朵能听到什么"，所以本模块的设计原则是：
 * 每一种故障模式，都能被某一段声音区分出来。
 *
 * 三步诊断（顺序固定，间隔 400ms 静音）：
 *   [1] 1kHz 满幅正弦 2.5s，volume=1.00
 *       → 最容易被听到/被手机录音捕捉；完全无声 = 功放没点亮或喇叭断路，
 *         与音源、音量、网络全无关。
 *   [2] 语音（spk_test.pcm，峰值 -1dBFS）按当前业务音量播 1 遍
 *       → 判断"响度够不够、清不清楚"，这才是奶奶实际听到的音量。
 *   [3] 100Hz → 8kHz 线性扫频 2.5s，volume=1.00
 *       → 低频段发"噗噗"破音 = 喇叭超行程（5V 常见）；某段完全没声 =
 *         喇叭/腔体频响缺陷；全场失真 = 电源带不动 5V 峰值。
 *
 * 播放期间按 BOOT 键（GPIO0）随时中止。
 * 编译期开关：把 SPK_DIAG_ON_BOOT 改成 0，整个模块从固件里消失（无运行期开销）。
 *
 * 数据通路：全部走 i2s_spk_write()，与业务（AI_CHAT 播放 TTS）同一条通路、
 * 同一个数字音量增益，并且共用 i2s.c 里的 s_spk_lock，不会和 TTS 互相插断。
 */
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "i2s.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define SPK_DIAG_ON_BOOT     1      /* ← 1=开机自测，0=关闭 */
#define SPK_DIAG_DELAY_MS    3000   /* 等启动日志落完再出声 */
#define SPK_DIAG_GAP_MS      400    /* 每步之间的静音间隔 */
#define SPK_DIAG_TONE_MS     2500   /* 单频/扫频时长 */
#define SPK_DIAG_FADE_FRAMES 320    /* 20ms 淡入淡出 = 16kHz/50，防首尾咔哒声 */
#define SPK_DIAG_FULL_SCALE  32000  /* 满幅但留 0.2dB 余量，避免 int16 溢出 */

static const char *TAG = "SPK_DIAG";

/* EMBED_FILES 生成的符号（components/BSP/CMakeLists.txt） */
extern const uint8_t spk_test_pcm_start[] asm("_binary_spk_test_pcm_start");
extern const uint8_t spk_test_pcm_end[]   asm("_binary_spk_test_pcm_end");

static int16_t s_tone_buf[SPK_BLOCK_FRAMES];

static size_t clip_samples(void)
{
    return (size_t)(spk_test_pcm_end - spk_test_pcm_start) / sizeof(int16_t);
}

/* BOOT 键低电平有效；已由 key_init 配成输入上拉 */
static bool boot_pressed(void)
{
    return gpio_get_level(BTN_BOOT_PIN) == 0;
}

/**
 * @brief  生成并播一段（可扫频的）正弦，带 20ms 淡入淡出
 *
 * @param  f0  起始频率 Hz；f1 == f0 时为定频
 * @param  f1  结束频率 Hz
 * @param  ms  时长毫秒
 * @return ESP_OK / I2S 写入错误码
 *
 * 相位用 double 累加、频率随时间线性插值：定频时相位每周期正好闭环，
 * 扫频时也不会因为整数截断出现"台阶感"。音量走 i2s_spk_write 的数字增益。
 */
static esp_err_t play_sine(float f0, float f1, uint32_t ms)
{
    const size_t total = (size_t)((uint64_t)I2S_SAMPLE_RATE * ms / 1000);
    const double two_pi = 6.283185307179586;
    double phase = 0.0;
    size_t done = 0;

    while (done < total) {
        size_t n = total - done;
        if (n > SPK_BLOCK_FRAMES) n = SPK_BLOCK_FRAMES;

        for (size_t i = 0; i < n; ++i) {
            const size_t idx = done + i;
            const double f = (double)f0 + ((double)f1 - (double)f0) * ((double)idx / (double)total);
            phase += two_pi * f / (double)I2S_SAMPLE_RATE;
            if (phase > two_pi) phase -= two_pi;

            double amp = (double)SPK_DIAG_FULL_SCALE;
            if (idx < SPK_DIAG_FADE_FRAMES)                      /* 淡入 */
                amp *= (double)idx / SPK_DIAG_FADE_FRAMES;
            else if (idx + SPK_DIAG_FADE_FRAMES >= total)         /* 淡出 */
                amp *= (double)(total - idx - 1) / SPK_DIAG_FADE_FRAMES;

            s_tone_buf[i] = (int16_t)(sin(phase) * amp);
        }

        esp_err_t err = i2s_spk_write(s_tone_buf, n);
        if (err != ESP_OK) return err;
        done += n;
    }
    return ESP_OK;
}

static esp_err_t play_clip(void)
{
    return i2s_spk_write((const int16_t *)spk_test_pcm_start, clip_samples());
}

/* 一步诊断：打日志 → 播 → 打结果。返回 false 表示该中止（BOOT 按下或出错） */
static bool run_step(int step, int total, const char *what, float vol,
                     esp_err_t (*fn)(void), size_t samples)
{
    if (boot_pressed()) return false;

    i2s_spk_set_volume(vol);
    ESP_LOGI(TAG, "[%d/%d] %s  volume=%.2f", step, total, what, (double)vol);

    esp_err_t err = fn();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%d/%d] I2S 写入失败: %s（DMA/驱动层问题，不是功放）",
                 step, total, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "[%d/%d] 完成 samples=%u (%.2f s)",
             step, total, (unsigned)samples, (double)samples / I2S_SAMPLE_RATE);

    i2s_spk_write_silence(6);    /* 推空 TX DMA，让尾巴播完再进下一步 */
    vTaskDelay(pdMS_TO_TICKS(SPK_DIAG_GAP_MS));
    return true;
}

static esp_err_t step_tone(void)  { return play_sine(1000.0f, 1000.0f, SPK_DIAG_TONE_MS); }
static esp_err_t step_sweep(void) { return play_sine(100.0f, 8000.0f, SPK_DIAG_TONE_MS); }

static void diag_task(void *arg)
{
    (void)arg;
    const size_t samples = clip_samples();
    const float vol0 = i2s_spk_get_volume();
    const int total = 3;

    ESP_LOGW(TAG, "扬声器诊断开始：3 步，业务音量 %.2f，按 BOOT 可中止", (double)vol0);
    vTaskDelay(pdMS_TO_TICKS(SPK_DIAG_DELAY_MS));

    bool ok = true;
    ok = ok && run_step(1, total, "1kHz 满幅正弦（完全无声=功放没点亮/喇叭断路）",
                        1.0f, step_tone, (size_t)((uint64_t)I2S_SAMPLE_RATE * SPK_DIAG_TONE_MS / 1000));
    ok = ok && run_step(2, total, "语音（业务音量，判断响度与清晰度）",
                        vol0, play_clip, samples);
    ok = ok && run_step(3, total, "100Hz-8kHz 扫频（低频破音=喇叭超行程）",
                        1.0f, step_sweep, (size_t)((uint64_t)I2S_SAMPLE_RATE * SPK_DIAG_TONE_MS / 1000));

    i2s_spk_set_volume(vol0);
    i2s_spk_write_silence(6);

    if (ok) {
        ESP_LOGI(TAG, "诊断结束，音量已恢复 %.2f", (double)vol0);
        ESP_LOGI(TAG, "全无声 → 查 VIN=5V / SD=VIN(5V) / 喇叭接 SPK± 且线圈 4-8Ω");
        ESP_LOGI(TAG, "有声但第3步低频破音 → 喇叭功率到顶，5V 已压满它");
    } else if (boot_pressed()) {
        ESP_LOGI(TAG, "诊断被 BOOT 中止，音量已恢复 %.2f", (double)vol0);
    }
    vTaskDelete(NULL);
}

void spk_selftest_start(void)
{
#if SPK_DIAG_ON_BOOT
    /* 核 0 + 优先级 5：低于 ai_chat(12) 和 fall_det(8)，绝不抢它们的 CPU */
    if (xTaskCreate(diag_task, "spk_diag", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create spk_diag task failed");
    }
#endif
}
