#include "i2s.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "DUAL_MIC";
static const char *const mode_names[] = {"LEFT", "RIGHT", "AVERAGE", "DIFFERENCE"};
/** 喇叭写锁：自测/回放/对话可能来自不同任务，串行化避免两路音频插断 */
static SemaphoreHandle_t s_spk_lock = NULL;

static inline void spk_lock(void)
{
    if (s_spk_lock != NULL) xSemaphoreTake(s_spk_lock, portMAX_DELAY);
}

static inline void spk_unlock(void)
{
    if (s_spk_lock != NULL) xSemaphoreGive(s_spk_lock);
}

typedef struct {
    uint32_t frames;
    int64_t sum[2];
    uint64_t square[2];
    int64_t cross;
    uint32_t peak[2];
    uint32_t clips[2];
    int32_t minimum[2];
    int32_t maximum[2];
    uint32_t identical;
} mic_stats_t;

/**
 * @brief  清零统计结构体，并把最小/最大值初始化为"反向极值"
 * @param  stats 待复位的统计上下文
 *
 * minimum 预置 INT16_MAX、maximum 预置 INT16_MIN，
 * 保证第一帧数据进来时一定会被替换，否则初值 0 会污染 min/max。
 */
static void reset_stats(mic_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    for (int ch = 0; ch < 2; ++ch) {
        stats->minimum[ch] = INT16_MAX;
        stats->maximum[ch] = INT16_MIN;
    }
}

/**
 * @brief  把一帧麦克风数据累加进统计上下文
 * @param  stats 统计上下文
 * @param  frame 一帧原始双麦数据（每声道 32 位，高 16 位有效）
 *
 * 逐声道累计：和、平方和、峰值、削顶次数(>=32760)、最小/最大值；
 * 另外统计左右内积（用于互相关）以及"两声道完全相同"的帧数。
 * 后者是判断两颗麦是否真的独立工作的关键证据。
 */
static void add_stats(mic_stats_t *stats, const mic_frame_t *frame)
{
    const int16_t pcm[2] = {dual_mic_pcm16(frame->left), dual_mic_pcm16(frame->right)};
    ++stats->frames;
    for (int ch = 0; ch < 2; ++ch) {
        int32_t value = pcm[ch];
        uint32_t magnitude = value < 0 ? (uint32_t)-value : (uint32_t)value;
        stats->sum[ch] += value;
        stats->square[ch] += (int64_t)value * value;
        if (magnitude > stats->peak[ch]) stats->peak[ch] = magnitude;
        if (magnitude >= 32760) ++stats->clips[ch];
        if (value < stats->minimum[ch]) stats->minimum[ch] = value;
        if (value > stats->maximum[ch]) stats->maximum[ch] = value;
    }
    stats->cross += (int64_t)pcm[0] * pcm[1];
    if (pcm[0] == pcm[1]) ++stats->identical;
}

/**
 * @brief  每秒输出一组双麦电平统计，用于接线与声学排查
 * @param  stats        统计上下文
 * @param  mode         当前混音模式（LEFT/RIGHT/AVERAGE/DIFFERENCE）
 * @param  recording    是否正在录音
 * @param  boot_level   BOOT 键当前原始电平
 * @param  boot_pressed 消抖后的按下状态
 * @param  held_ms      已按住时长（毫秒）
 * @param  boot_edges   本统计窗口内的按键跳变次数
 *
 * 计算均值、方差、RMS、左右互相关，并在两种异常下打 WARN：
 *   1) 某声道 min==max -> 该时隙恒为常数，检查供电、SD 或 L/R 接法；
 *   2) 全部帧左右相同   -> 两颗麦未被独立验证，可能是同一路信号复制。
 */
static void report_stats(const mic_stats_t *stats, dual_mic_mode_t mode, bool recording,
                         int boot_level, bool boot_pressed, uint32_t held_ms, uint32_t boot_edges)
{
    double count = stats->frames;
    double mean[2], variance[2], rms[2];
    for (int ch = 0; ch < 2; ++ch) {
        mean[ch] = stats->sum[ch] / count;
        variance[ch] = stats->square[ch] / count - mean[ch] * mean[ch];
        if (variance[ch] < 0) variance[ch] = 0;
        rms[ch] = sqrt(variance[ch]);
    }
    double covariance = stats->cross / count - mean[0] * mean[1];
    bool correlation_valid = variance[0] > 1 && variance[1] > 1;
    double correlation = correlation_valid ? covariance / sqrt(variance[0] * variance[1]) : 0;
    if (correlation > 1) correlation = 1;
    if (correlation < -1) correlation = -1;
    ESP_LOGI(TAG, "mode=%s record=%d play=0 | L rms=%.1f peak=%" PRIu32 " dc=%.1f clip=%" PRIu32
             " | R rms=%.1f peak=%" PRIu32 " dc=%.1f clip=%" PRIu32
             " | corr=%.3f valid=%d same=%.1f%% n=%" PRIu32
             " | boot=%d pressed=%d held_ms=%" PRIu32 " edges=%" PRIu32,
             mode_names[mode], (int)recording,
             rms[0], stats->peak[0], mean[0], stats->clips[0],
             rms[1], stats->peak[1], mean[1], stats->clips[1],
             correlation, (int)correlation_valid, 100.0 * stats->identical / count, stats->frames,
             boot_level, (int)boot_pressed, held_ms, boot_edges);
    for (int ch = 0; ch < 2; ++ch) {
        if (stats->minimum[ch] == stats->maximum[ch]) {
            ESP_LOGW(TAG, "%s slot is constant: check power, SD and L/R selection",
                     ch == 0 ? "LEFT" : "RIGHT");
        }
    }
    if (stats->identical == stats->frames) {
        ESP_LOGW(TAG, "slots are identical; independent microphones have NOT been verified");
    }
}

/**
 * @brief  初始化两路 I2S：I2S0 扬声器发送、I2S1 双麦接收
 *
 * I2S0（TX，接 MAX98357）：16 kHz / 16 bit / 立体声，同一单声道复制到左右时隙，
 *   DMA 4x128 帧，开 tx_desc_auto_clear 保证欠载时自动补零而不是循环旧数据。
 * I2S1（RX，接两颗 INMP441）：16 kHz / 32 bit 时隙（24 位数据左对齐在高位），
 *   BCLK = 1.024 MHz，DMA 8x128 帧。
 *
 * @note 两颗 INMP441 共用 BCLK/WS/SD：左麦 L/R 接地、右麦 L/R 接 3.3V，
 *       靠 L/R 脚选声道，接反会导致左右声道互换。
 * @note 本函数只装驱动、不读数据；实际采集在 i2s_loopback_task 或
 *       （规划中的）音频帧管线里进行，二者争用 I2S1，不能同时跑。
 */
void init_i2s(void)
{
    const i2s_config_t speaker = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 4,
        .dma_buf_len = SPK_BLOCK_FRAMES,
        .tx_desc_auto_clear = true,
    };
    const i2s_pin_config_t speaker_pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_SPK_BCK_PIN,
        .ws_io_num = I2S_SPK_WS_PIN,
        .data_out_num = I2S_SPK_DATAOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &speaker, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &speaker_pins));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
    s_spk_lock = xSemaphoreCreateMutex();   /* 必须在任何 i2s_spk_write 之前建好 */

    const i2s_config_t microphone = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .bits_per_chan = I2S_BITS_PER_CHAN_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = MIC_BLOCK_FRAMES,
    };
    const i2s_pin_config_t microphone_pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_MIC_BCK_PIN,
        .ws_io_num = I2S_MIC_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_DATAIN_PIN,
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_1, &microphone, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_1, &microphone_pins));
    ESP_LOGI(TAG, "RX: %d Hz, stereo 32-bit slots, BCLK=%d Hz; BCLK=%d WS=%d SD=%d, %u bytes/frame",
             I2S_SAMPLE_RATE, I2S_SAMPLE_RATE * 32 * 2,
             I2S_MIC_BCK_PIN, I2S_MIC_WS_PIN, I2S_MIC_DATAIN_PIN,
             (unsigned)sizeof(mic_frame_t));
}

/**
 * @brief  把 BOOT 键(GPIO0)配置为上拉输入
 *
 * 只在本模块的自测 demo 中使用；正式的按键管理见 KEY/key.c。
 *
 * @note 该引脚在 i2s.h 里叫 BTN_BOOT_PIN、在 key.h 里叫 BOOT_GPIO_PIN，
 *       同一个 GPIO0 被两处重复定义并各自初始化，改接线时要同步改两处。
 */
void boot_init(void)
{
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << BTN_BOOT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
}

/* ══════════ 音频帧管线：运行时音量 + 双麦接收 ══════════ */

/** 当前扬声器音量，0.0 ~ 1.0；初值取编译期默认 SPK_VOLUME */
static float s_spk_volume = SPK_VOLUME;
/** 双麦 DC 跟踪状态，跨调用保持（单声道混音路径） */
static dual_mic_filter_t s_mic_filter;
/** 双通道路径的 DC 跟踪状态，与 s_mic_filter 分开，避免两条路径互相污染 */
static dual_mic_filter_t s_mic_filter_stereo;
/** 自适应开关（当前没有 LMS 核，仅记录状态） */
static volatile bool s_mic_adapt = true;
/** receive_audio() 的整帧读取暂存区 */
static mic_frame_t s_mic_scratch[MIC_BLOCK_FRAMES];

/**
 * @brief  设置扬声器音量
 * @param  vol 目标音量，0.0 ~ 1.0，越界会被夹到边界
 *
 * 只改一个静态变量，不碰 DMA 或驱动；下一次 write_speaker() 生效。
 */
void i2s_spk_set_volume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_spk_volume = vol;
    ESP_LOGI(TAG, "speaker volume = %.2f", (double)s_spk_volume);
}

/**
 * @brief  读取当前扬声器音量
 * @return 0.0 ~ 1.0
 */
float i2s_spk_get_volume(void)
{
    return s_spk_volume;
}

/**
 * @brief  清空双麦 DC 估计
 *
 * 只重置滤波器状态，不改混音模式，也不动 I2S 驱动。
 */
void i2s_dual_mic_reset(void)
{
    memset(&s_mic_filter, 0, sizeof(s_mic_filter));
    memset(&s_mic_filter_stereo, 0, sizeof(s_mic_filter_stereo));
    ESP_LOGI(TAG, "dual-mic DC estimate reset");
}

/**
 * @brief  允许/禁止双麦自适应降噪
 * @param  on true = 允许，false = 冻结
 *
 * 当前实现只记录状态：本工程没有 LMS 自适应核（源工程里该核默认也是
 * I2S_NS_MU_Q16=0 的纯平均模式）。保留接口是为了让 AFE 的调用点不用改。
 */
void i2s_dual_mic_set_adapt(bool on)
{
    if (s_mic_adapt != on) {
        ESP_LOGI(TAG, "dual-mic adapt %s (no LMS core, flag only)", on ? "ON" : "OFF");
    }
    s_mic_adapt = on;
}

/**
 * @brief  从 I2S1 读取双麦数据，合成单声道 PCM16
 * @param  buf           输出缓冲区（int16_t 采样）
 * @param  len           buf 的字节长度，必须是 sizeof(int16_t) 的整数倍
 * @param  bytes_read    实际写入的字节数（可为 NULL）
 * @param  ticks_to_wait 每块读取的等待上限
 * @return ESP_OK 成功（可能是部分读取）；ESP_ERR_INVALID_ARG 参数非法；其它为驱动错误
 *
 * 按 mic_frame_t 整帧读取，每帧经 dual_mic_process() 混成单声道并做 DC 跟踪；
 * 混音模式取 I2S_INITIAL_MODE（当前=只取左麦）。
 *
 * @note 语音主链路已改用 receive_audio_stereo()（双麦要保留两路给 AFE 做 BSS），
 *       本函数只剩单麦/自测用途；在"MM"配置下调它会白白丢掉第二颗麦的信息。
 * @note 循环补齐：一次 i2s_read 可能只够一部分，凑满 want 个采样才返回。
 * @note 超时或短读时返回已拿到的部分，此时 bytes_read < len 是正常情况，
 *       调用方要按 bytes_read 而不是 len 处理。
 * @note 与 i2s_loopback_task 争用 I2S1，两者不能同时运行。
 */
esp_err_t receive_audio(int16_t *buf, size_t len, size_t *bytes_read, TickType_t ticks_to_wait)
{
    if (buf == NULL || len == 0 || (len % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_read != NULL) *bytes_read = 0;

    const size_t want = len / sizeof(int16_t);   /* 需要的采样点数 */
    size_t got = 0;

    while (got < want) {
        size_t chunk = want - got;
        if (chunk > MIC_BLOCK_FRAMES) chunk = MIC_BLOCK_FRAMES;

        size_t received = 0;
        esp_err_t err = i2s_read(I2S_NUM_1, (uint8_t *)s_mic_scratch,
                                 chunk * sizeof(mic_frame_t),
                                 &received, ticks_to_wait);
        if (err != ESP_OK) return err;

        size_t frames = received / sizeof(mic_frame_t);
        if (frames == 0) break;              /* 超时：把已拿到的交出去 */

        for (size_t i = 0; i < frames; ++i) {
            buf[got + i] = dual_mic_process(&s_mic_filter,
                                            s_mic_scratch[i].left,
                                            s_mic_scratch[i].right,
                                            I2S_INITIAL_MODE);
        }
        got += frames;
        if (frames < chunk) break;           /* 短读：不再等，直接返回 */
    }

    if (bytes_read != NULL) *bytes_read = got * sizeof(int16_t);
    return ESP_OK;
}

/**
 * @brief  从 I2S1 读取双麦数据，输出双通道交错的 PCM16（esp-sr 双麦 AFE 专用）
 * @param  buf           输出缓冲区，容量 >= frames * 2 个 int16_t
 * @param  frames        需要的帧数（一帧 = 左 + 右各一个采样）
 * @param  frames_read   实际写出的帧数（可为 NULL）
 * @param  ticks_to_wait 每块读取的等待上限
 * @return ESP_OK 成功（可能是部分读取）；ESP_ERR_INVALID_ARG 参数非法；其它为驱动错误
 *
 * 与 receive_audio() 的区别只有一处：不混成单声道，两路各自去直流后原样交错输出。
 * 这样 esp-sr AFE 拿到的是严格同步的两路麦克风信号，才能跑 SE(BSS) 做盲源分离
 * ——目标声源与其它方向的干扰在空间上可分，BSS 把它们分开。注意官方 S3 的双麦
 * 流水线是 "SE(BSS) -> VAD -> WakeNet"，**不含 NS**（双通道时 BSS 顶掉 NS），
 * 所以这里拿到的是空间分离，不是稳态噪声抑制。
 * 任何在本函数里做差分/自适应的"土办法"都会把目标人声一起削掉，源工程
 * （ai_client）已经踩过这个坑。
 *
 * @note 循环补齐：一次 i2s_read 可能只够一部分，凑满 frames 帧才返回。
 * @note 超时或短读时返回已拿到的部分，此时 frames_read < frames 是正常情况。
 * @note 与 receive_audio() 共用 s_mic_scratch，二者不能并发（当前只有 afe_feed 会调）。
 */
esp_err_t receive_audio_stereo(int16_t *buf, size_t frames, size_t *frames_read,
                               TickType_t ticks_to_wait)
{
    if (buf == NULL || frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frames_read != NULL) *frames_read = 0;

    size_t got = 0;
    while (got < frames) {
        size_t chunk = frames - got;
        if (chunk > MIC_BLOCK_FRAMES) chunk = MIC_BLOCK_FRAMES;

        size_t received = 0;
        esp_err_t err = i2s_read(I2S_NUM_1, (uint8_t *)s_mic_scratch,
                                 chunk * sizeof(mic_frame_t),
                                 &received, ticks_to_wait);
        if (err != ESP_OK) return err;

        size_t n = received / sizeof(mic_frame_t);
        if (n == 0) break;                   /* 超时：把已拿到的交出去 */

        for (size_t i = 0; i < n; ++i) {
            dual_mic_process_stereo(&s_mic_filter_stereo,
                                    s_mic_scratch[i].left,
                                    s_mic_scratch[i].right,
                                    &buf[(got + i) * 2]);
        }
        got += n;
        if (n < chunk) break;                /* 短读：不再等，直接返回 */
    }

    if (frames_read != NULL) *frames_read = got;
    return ESP_OK;
}

/**
 * @brief  向 I2S0 写出指定帧数，处理短写
 * @param  frames 待输出的 PCM16 双声道帧数组
 * @param  count  帧数（不是字节数）
 * @return ESP_OK 全部写完；其它为驱动返回的错误；写不动时返回 ESP_ERR_TIMEOUT
 *
 * 对外只认"帧数"，字节换算是本函数内部的事，调用方不再需要 sizeof 运算。
 * i2s_write 可能只写出一部分，所以用循环补齐，单次等待上限 100 ms。
 */
static esp_err_t write_speaker(const pcm16_frame_t *frames, size_t count)
{
    const uint8_t *base = (const uint8_t *)frames;
    const size_t bytes = count * sizeof(pcm16_frame_t);
    size_t total = 0;
    while (total < bytes) {
        size_t written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0, base + total,
                                  bytes - total, &written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) return err;
        if (written == 0) return ESP_ERR_TIMEOUT;
        total += written;
    }
    return ESP_OK;
}

static esp_err_t spk_write_nolock(const int16_t *mono, size_t samples);
static esp_err_t spk_silence_nolock(int blocks);

esp_err_t i2s_spk_write(const int16_t *mono, size_t samples)
{
    spk_lock();
    esp_err_t rc = spk_write_nolock(mono, samples);
    spk_unlock();
    return rc;
}

static esp_err_t spk_write_nolock(const int16_t *mono, size_t samples)
{
    pcm16_frame_t tx[SPK_BLOCK_FRAMES];      /* 512 B 栈缓冲：避免引入跨任务共享的静态 */
    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done;
        if (n > SPK_BLOCK_FRAMES) n = SPK_BLOCK_FRAMES;
        for (size_t i = 0; i < n; ++i) {     /* 音量 + 单转双，与 replay_recording 一致 */
            int16_t out = (int16_t)(mono[done + i] * s_spk_volume);
            tx[i].left = out;
            tx[i].right = out;
        }
        esp_err_t err = write_speaker(tx, n);
        if (err != ESP_OK) return err;       /* 阻塞写 = 免费流控，出错的语义交给上层 */
        done += n;
    }
    return ESP_OK;
}

esp_err_t i2s_spk_write_silence(int blocks)
{
    spk_lock();
    esp_err_t rc = spk_silence_nolock(blocks);
    spk_unlock();
    return rc;
}

static esp_err_t spk_silence_nolock(int blocks)
{
    if (blocks <= 0) blocks = 6;             /* 与 replay_recording 的收尾块数一致 */
    pcm16_frame_t tx[SPK_BLOCK_FRAMES];
    memset(tx, 0, sizeof(tx));
    for (int block = 0; block < blocks; ++block) {
        esp_err_t err = write_speaker(tx, SPK_BLOCK_FRAMES);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/**
 * @brief  回放已录制的单声道 PCM，并保证回放期间麦克风数据不会进入功放
 * @param  saved  已录制的单声道 PCM 采样
 * @param  frames 帧数
 * @param  tx     输出暂存区（双声道，每帧两个相同采样）
 * @param  mode   录音时使用的混音模式（仅用于日志）
 * @return ESP_OK 成功
 *
 * 关键设计：回放数据只来自 saved 缓冲区，绝不读取实时麦克风，
 * 从数据通路上直接断掉"喇叭->麦克风->喇叭"的自激回路（本项目没有 AEC）。
 * 每段数据做 20 ms 淡入淡出（I2S_SAMPLE_RATE/50 帧）避免首尾爆音；
 * 结尾额外写 6 个静音块，把 4 个 TX DMA 缓冲里的残留音推出去再清空，
 * 否则回放尾巴会被截断。
 */
static esp_err_t replay_recording(const int16_t *saved, size_t frames, pcm16_frame_t *tx,
                                   dual_mic_mode_t mode)
{
    ESP_LOGI(TAG, "replay START mode=%s record=0 play=1 duration_ms=%u volume=%.2f",
             mode_names[mode], (unsigned)(frames * 1000 / I2S_SAMPLE_RATE), (double)s_spk_volume);
    for (size_t offset = 0; offset < frames; offset += SPK_BLOCK_FRAMES)
    {
        size_t block_frames = frames - offset;
        if (block_frames > SPK_BLOCK_FRAMES) block_frames = SPK_BLOCK_FRAMES;
        for (size_t i = 0; i < block_frames; ++i)
        {
            int16_t faded = dual_mic_replay_sample(saved[offset + i], (uint32_t)(offset + i),
                                                  (uint32_t)frames, I2S_SAMPLE_RATE / 50);
            int16_t output = (int16_t)(faded * s_spk_volume);
            tx[i].left = output;
            tx[i].right = output;
        }
        esp_err_t err = write_speaker(tx, block_frames);
        if (err != ESP_OK) return err;
    }
    /* 写够静音，把已录声音推出 4 个 TX DMA 缓冲后再清空，否则尾巴会被截断。 */
    memset(tx, 0, sizeof(pcm16_frame_t) * SPK_BLOCK_FRAMES);
    for (int block = 0; block < 6; ++block) {
        esp_err_t err = write_speaker(tx, SPK_BLOCK_FRAMES);
        if (err != ESP_OK) return err;
    }
    ESP_LOGI(TAG, "replay END record=0 play=0; ready for next recording");
    return ESP_OK;
}

/**
 * @brief  双麦录放自测任务
 * @param  arg 未使用
 *
 * 按键语义：
 *   - 短按 BOOT(<400 ms) 松开 -> 循环切换混音模式 LEFT->RIGHT->AVERAGE->DIFFERENCE；
 *   - 按住 BOOT(>=400 ms)    -> 开始录音（最多 3 秒），松开后自动回放。
 *
 * 内部要点：
 *   - RX 按"帧"累积：filled_frames 记已收帧数，凑满 I2S_BLOCK_FRAMES 帧才处理；
 *   - 上电先丢弃 64 个块（约 512 ms）等麦克风稳定；
 *   - 回放结束后丢弃 RX 积压的 8 个块，再等 16 个块（约 128 ms）
 *     让喇叭余振衰减，然后才恢复录音，避免把回放尾巴录进去；
 *   - 实时采集阶段 TX 恒为 0，只有 replay_recording() 里才输出声音。
 *
 * @note saved[] 是 96000 字节静态缓冲（3 秒 x 16 kHz x 2 字节），常驻内部 RAM。
 * @note 本任务与 AFE 音频管线互斥：都从 I2S_NUM_1 读数据，不能并存。
 */
void i2s_loopback_task(void *arg)
{
    (void)arg;
    /* 静态缓冲，避免占用 app_main 栈。I2S DMA 缓冲仍归驱动所有。 */
    static mic_frame_t   rx[MIC_BLOCK_FRAMES];
    static pcm16_frame_t tx[SPK_BLOCK_FRAMES];
    static int16_t       saved[I2S_SAMPLE_RATE * I2S_RECORD_SECONDS];
    dual_mic_filter_t filter = {0};
    mic_stats_t stats;
    reset_stats(&stats);
    dual_mic_mode_t mode = I2S_INITIAL_MODE;
    boot_init();
    bool stable_pressed = false;
    bool candidate_pressed = false;
    bool recording_session = false;
    bool recording_full = false;
    size_t recorded_frames = 0;
    uint32_t boot_edges = 0;
    TickType_t changed_at = xTaskGetTickCount();
    TickType_t pressed_at = changed_at;
    TickType_t last_data_at = changed_at;
    size_t filled_frames = 0;
    unsigned warmup_blocks = 64; /* 16 kHz 下 512 ms，超过麦克风启动时间 */

    ESP_LOGI(TAG, "Short BOOT press: LEFT -> RIGHT -> AVERAGE -> DIFFERENCE");
    ESP_LOGI(TAG, "Hold BOOT >=400 ms: RECORD (max %d s); release: REPLAY. Initial mode=%s",
             I2S_RECORD_SECONDS,
             mode_names[mode]);
    ESP_LOGI(TAG, "Keep the speaker away from microphones during loopback checks");
    ESP_LOGI(TAG, "Record/replay output volume=%.2f; no live microphone loopback", (double)s_spk_volume);
    ESP_LOGI(TAG, "Block = %u frames = %u B (mic) / %u B (spk)",
             (unsigned)MIC_BLOCK_FRAMES, (unsigned)MIC_BLOCK_BYTES, (unsigned)SPK_BLOCK_BYTES);
    ESP_LOGI(TAG, "BOOT GPIO%d: initial level=%d, pressed=LOW, debounce=30 ms, hold=400 ms",
             BTN_BOOT_PIN, gpio_get_level(BTN_BOOT_PIN));
    while (true) {
        size_t received_bytes = 0;
        esp_err_t err = i2s_read(I2S_NUM_1,
                                 (uint8_t *)&rx[filled_frames],
                                 (MIC_BLOCK_FRAMES - filled_frames) * sizeof(mic_frame_t),
                                 &received_bytes, pdMS_TO_TICKS(100));
        TickType_t now = xTaskGetTickCount();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "microphone read failed: %s", esp_err_to_name(err));
            ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (received_bytes % sizeof(mic_frame_t) != 0) {
            /* 理论上不会发生：驱动按帧对齐返回。真发生了就丢掉残块重新攒。 */
            ESP_LOGW(TAG, "RX returned %u bytes, not a whole number of frames; restarting block",
                     (unsigned)received_bytes);
            filled_frames = 0;
            continue;
        }
        if (received_bytes > 0) last_data_at = now;
        filled_frames += received_bytes / sizeof(mic_frame_t);
        if (filled_frames < MIC_BLOCK_FRAMES) {
            if ((now - last_data_at) >= pdMS_TO_TICKS(1000)) {
                ESP_LOGW(TAG, "no microphone data for 1 second; check I2S clocks");
                last_data_at = now;
            }
            ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        filled_frames = 0;
        if (warmup_blocks > 0) {
            --warmup_blocks;
            continue;
        }

        bool pressed = gpio_get_level(BTN_BOOT_PIN) == 0;
        if (pressed != candidate_pressed) {
            ++boot_edges;
            candidate_pressed = pressed;
            changed_at = now;
        }
        if (candidate_pressed != stable_pressed && (now - changed_at) >= pdMS_TO_TICKS(30)) {
            stable_pressed = candidate_pressed;
            if (stable_pressed) {
                pressed_at = now;
                ESP_LOGI(TAG, "BOOT pressed: GPIO%d LOW confirmed", BTN_BOOT_PIN);
            } else {
                ESP_LOGI(TAG, "BOOT released: GPIO%d HIGH confirmed, held_ms=%" PRIu32,
                         BTN_BOOT_PIN, (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS));
                if ((now - pressed_at) < pdMS_TO_TICKS(400)) {
                    mode = (dual_mic_mode_t)((mode + 1) % DUAL_MIC_MODE_COUNT);
                    ESP_LOGI(TAG, "selected mode=%s", mode_names[mode]);
                }
            }
        }
        bool recording_gate = pressed && stable_pressed && (now - pressed_at) >= pdMS_TO_TICKS(400);
        /* 录音状态先吃掉消抖后的按键；回放要等松手才触发。 */
        if (recording_gate && !recording_session) {
            recording_session = true;
            recording_full = false;
            recorded_frames = 0;
            ESP_LOGI(TAG, "record START mode=%s record=1 play=0; speak now, release to replay",
                     mode_names[mode]);
        }
        if (!stable_pressed && recording_session) {
            recording_session = false;
            err = replay_recording(saved, recorded_frames, tx, mode);
            if (err != ESP_OK) ESP_LOGE(TAG, "replay failed: %s", esp_err_to_name(err));
            ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
            recorded_frames = 0;
            filter = (dual_mic_filter_t){0};
            reset_stats(&stats);
            /* 回放期间 RX 仍在积压：先排空 8 个块，再留 128 ms 让喇叭余振衰减。 */
            for (int block = 0; block < 8; ++block) {
                size_t discarded = 0;
                ESP_ERROR_CHECK(i2s_read(I2S_NUM_1, (uint8_t *)rx, sizeof(rx), &discarded, 0));
            }
            warmup_blocks = 16;
            continue;
        }
        for (size_t i = 0; i < MIC_BLOCK_FRAMES; ++i) {
            const mic_frame_t *frame = &rx[i];
            add_stats(&stats, frame);
            int16_t mono = dual_mic_process(&filter, frame->left, frame->right, mode);
            if (recording_session && recorded_frames < I2S_SAMPLE_RATE * I2S_RECORD_SECONDS) {
                saved[recorded_frames++] = mono;
            }
            /* 实时采集阶段恒静音；只有 replay_recording() 里才往 TX 写声音。 */
            tx[i].left = 0;
            tx[i].right = 0;
        }
        if (recording_session && !recording_full && recorded_frames == I2S_SAMPLE_RATE * I2S_RECORD_SECONDS) {
            recording_full = true;
            ESP_LOGI(TAG, "record buffer FULL (%d s), record=0 play=0; release BOOT to replay",
                     I2S_RECORD_SECONDS);
        }
        err = write_speaker(tx, SPK_BLOCK_FRAMES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "speaker write failed: %s", esp_err_to_name(err));
            ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
        }
        if (stats.frames >= I2S_SAMPLE_RATE) {
            uint32_t held_ms = stable_pressed ? (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS) : 0;
            report_stats(&stats, mode, recording_session && !recording_full,
                         pressed ? 0 : 1, stable_pressed, held_ms, boot_edges);
            boot_edges = 0;
            reset_stats(&stats);
        }
    }
}
