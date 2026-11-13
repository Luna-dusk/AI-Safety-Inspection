#ifndef BSP_I2S_H
#define BSP_I2S_H

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "dual_mic_pcm.h"

/* MAX98357: I2S0 TX。左右时隙填同一个单声道采样。 */
#define I2S_SPK_BCK_PIN      15
#define I2S_SPK_WS_PIN       16
#define I2S_SPK_DATAOUT_PIN  7

/* 两颗 INMP441 共用 BCLK/WS/SD：左麦 L/R -> GND，右麦 L/R -> 3.3V。 */
#define I2S_MIC_BCK_PIN      5
#define I2S_MIC_WS_PIN       4
#define I2S_MIC_DATAIN_PIN   6
#define BTN_BOOT_PIN         0

#define I2S_SAMPLE_RATE      16000
#define I2S_BLOCK_FRAMES     128
#define SPK_VOLUME           0.95f
#define I2S_RECORD_SECONDS   3
/* 仅对 receive_audio()（单声道混音路径）与录放自测生效。
 * 语音主链路走 receive_audio_stereo()，不受这个宏影响——AFE 要的是两路原始信号。 */
#define I2S_INITIAL_MODE     DUAL_MIC_LEFT

/**
 * @brief I2S 麦克风的一帧：左右两个声道，各占一个 32 位时隙
 *
 * INMP441 输出 24 位有符号数据，左对齐放在 32 位时隙的高位，
 * 低 16 位是无效填充（见 dual_mic_pcm16()）。
 * 内存布局与 I2S1 的 DMA 数据流严格一一对应，可以直接整块交给 i2s_read()。
 */
typedef struct {
    int32_t left;
    int32_t right;
} mic_frame_t;

/**
 * @brief 双声道 PCM16 的一帧：用于 I2S0 扬声器输出
 *
 * MAX98357 是单声道功放，因此写入时左右两个字段填同一个值。
 * 内存布局与 I2S0 的 16bit 立体声时隙一一对应。
 */
typedef struct {
    int16_t left;
    int16_t right;
} pcm16_frame_t;

/* 每块的帧数（收发两侧统一按"帧"计量，不再出现字节数常量） */
#define MIC_BLOCK_FRAMES     I2S_BLOCK_FRAMES
#define SPK_BLOCK_FRAMES     I2S_BLOCK_FRAMES
#define MIC_BLOCK_BYTES      (MIC_BLOCK_FRAMES * sizeof(mic_frame_t))
#define SPK_BLOCK_BYTES      (SPK_BLOCK_FRAMES * sizeof(pcm16_frame_t))

/**
 * @brief  把一帧 32 位麦克风数据降采样成 PCM16 双声道帧
 * @param  frame 原始 32 位时隙帧
 * @return 对应的 PCM16 帧（每声道取高 16 位）
 */
static inline pcm16_frame_t mic_frame_to_pcm16(mic_frame_t frame)
{
    pcm16_frame_t out;
    out.left  = dual_mic_pcm16(frame.left);
    out.right = dual_mic_pcm16(frame.right);
    return out;
}

/**
 * @brief  初始化 I2S0(扬声器 TX) 与 I2S1(双麦 RX)
 * @note   只装驱动不读数据；与 i2s_loopback_task 共用 I2S1，不可并存。
 */
void init_i2s(void);

/**
 * @brief  把 BOOT 键(GPIO0)配置为上拉输入，仅自测 demo 使用
 * @note   与 key.h 的 BOOT_GPIO_PIN 是同一个引脚，改接线需同步改两处。
 */
void boot_init(void);

/**
 * @brief  双麦录放自测任务：短按切混音模式，长按录音、松手回放
 * @param  arg 未使用
 * @note   常驻约 96 KB 静态缓冲；与 AFE 音频管线互斥。
 */
void i2s_loopback_task(void *arg);

/**
 * @brief  扬声器响度自测任务（开机播一段语音，验证 5V 供电响度/喇叭余量）
 *
 * 编译期开关在 spk_selftest.c 的 SPK_SELFTEST_ON_BOOT；关掉后本函数为空，
 * 可以安全地留在 main 里。播放走 i2s_spk_write()，与业务同一条通路。
 */
void spk_selftest_start(void);

/* ══════════ 音频帧管线（AFE / MultiNet / 上传链路共用）══════════ */

/** 每帧采样点数；16 bit 单声道 → 每帧字节数 = AUDIO_FRAME_SAMPLES * 2 */
#define AUDIO_FRAME_SAMPLES  128
#define AUDIO_FRAME_BYTES    (AUDIO_FRAME_SAMPLES * sizeof(int16_t))

/** PCM 队列深度：128 帧缓存（约 1 秒 @16 kHz） */
#define PCM_QUEUE_LEN        128

/**
 * @brief 音频帧类型
 * @note  DATA = 0 表示有效数据（与"0 为成功"的习惯一致）；
 *        END  = 1 表示一句话结束，下游可把累积的 PCM 送去识别。
 */
typedef enum {
    AUDIO_FRAME_DATA = 0,
    AUDIO_FRAME_END  = 1,
} audio_frame_type_t;

/**
 * @brief 一帧单声道 PCM16 数据，用于 AFE → 上传队列
 *
 * data 是单声道采样数组（不是字节流），下游按 int16_t 直接取用。
 * DATA 帧的 bytes 恒为 AUDIO_FRAME_BYTES，END 帧为 0。
 */
typedef struct {
    audio_frame_type_t type;
    size_t             bytes;
    int16_t            data[AUDIO_FRAME_SAMPLES];
} audio_frame_t;

/**
 * @brief  设置扬声器音量
 * @param  vol 0.0 ~ 1.0，越界自动夹到边界
 */
void  i2s_spk_set_volume(float vol);

/** @brief 读取当前扬声器音量（0.0 ~ 1.0） */
float i2s_spk_get_volume(void);

/**
 * @brief  向扬声器写入单声道 PCM16（外部播放唯一入口，附录 A.9 #1）
 * @param  mono    单声道 16 kHz 采样数组
 * @param  samples 采样数（不是字节数），任意长度，内部按 128 帧分块
 * @return ESP_OK 全部写出；其它为驱动错误，调用方据此决定是否中断本轮播放
 *
 * 阻塞写即流控（附录 A.9 硬约束 3）：喇叭消费不动就卡在这里，TCP 窗口自动反压。
 * 线程约束：同一时刻只允许一个任务调用（当前 = ai_chat 任务）。
 */
esp_err_t i2s_spk_write(const int16_t *mono, size_t samples);

/**
 * @brief  写静音块，把 TX DMA 缓冲里的残留音频推出去（附录 A.9 #7）
 * @param  blocks 静音块数（每块 128 帧）；<=0 取默认 6
 * @return ESP_OK 成功
 * @note   不写这一步，播放尾巴约 32 ms 会被 4 个 TX DMA 缓冲吞掉（与 replay 收尾同理）。
 */
esp_err_t i2s_spk_write_silence(int blocks);

/**
 * @brief  从 I2S1 读取双麦数据，合成单声道 PCM16
 * @param  buf           输出缓冲区（int16_t 采样）
 * @param  len           buf 的字节长度，必须是 sizeof(int16_t) 的整数倍
 * @param  bytes_read    实际写入的字节数（传 NULL 表示不关心）
 * @param  ticks_to_wait 每块读取的等待上限
 * @return ESP_OK 成功（可能是部分读取）；ESP_ERR_INVALID_ARG 参数非法；其它为驱动错误
 *
 * 内部按 mic_frame_t 整帧读取，逐帧经 dual_mic_process() 混成单声道，
 * 并对每声道做 DC 跟踪。混音模式取 I2S_INITIAL_MODE（当前=只取左麦）。
 *
 * @note 与 i2s_loopback_task 争用 I2S1，两者不能同时运行。
 * @note 语音主链路已改用 receive_audio_stereo()；本接口保留给单麦/自测场景。
 */
esp_err_t receive_audio(int16_t *buf, size_t len, size_t *bytes_read, TickType_t ticks_to_wait);

/**
 * @brief  从 I2S1 读取双麦数据，输出**双通道交错**的 PCM16（esp-sr 双麦 AFE 专用）
 * @param  buf           输出缓冲区，容量必须 >= frames * 2 个 int16_t
 * @param  frames        需要的帧数（一帧 = 左+右各一个采样）
 * @param  frames_read   实际写出的帧数（可为 NULL）
 * @param  ticks_to_wait 每块读取的等待上限
 * @return ESP_OK 成功（可能是部分读取）；ESP_ERR_INVALID_ARG 参数非法；其它为驱动错误
 *
 * 输出布局是 [L0,R0, L1,R1, ...]，与 esp-sr 文档要求的"通道交错"一致：
 * feed_buff 大小 = feed_chunksize * feed_nch * sizeof(int16_t)，
 * feed_nch = afe_handle->get_feed_channel_num()。
 *
 * 两路各自只做去直流，不做任何混音/差分——空间分离交给 AFE 的 SE(BSS)。
 * （官方 S3 双麦流水线 AEC -> SE(BSS) -> VAD -> WakeNet 里没有 NS：双通道时
 *   BSS 顶掉 NS，所以压稳态噪声要靠另外挂 nsnet2，本工程尚未启用。）
 * 两颗 INMP441 共用 BCLK/WS/SD（靠 L/R 脚分到左右时隙），因此两路是严格同步
 * 采样的，这正是 BSS 这类空间算法需要的前提。
 *
 * @note 与 receive_audio() 共用同一块整帧暂存区，二者不能并发调用
 *       （当前只有 afe_feed 一个任务会调）。
 * @note 与 i2s_loopback_task 争用 I2S1，两者不能同时运行。
 */
esp_err_t receive_audio_stereo(int16_t *buf, size_t frames, size_t *frames_read,
                               TickType_t ticks_to_wait);

/**
 * @brief  清空双麦 DC 估计（不重置混音模式）
 * @note   语音链路开始/结束时调用，避免上一段的 DC 残留拖尾。
 */
void i2s_dual_mic_reset(void);

/**
 * @brief  允许/禁止双麦自适应降噪
 * @param  on true = 允许，false = 冻结
 * @note   当前实现没有 LMS 自适应核，本函数只记录状态并打日志。
 *         保留接口是为了与源工程 ai_client 的调用点保持一致，方便以后补算法。
 */
void i2s_dual_mic_set_adapt(bool on);

#endif /* BSP_I2S_H */
