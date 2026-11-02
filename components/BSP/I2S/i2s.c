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
#include "freertos/task.h"

static const char *TAG = "DUAL_MIC";
static const char *const mode_names[] = {"LEFT", "RIGHT", "AVERAGE", "DIFFERENCE"};

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

static void reset_stats(mic_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    for (int ch = 0; ch < 2; ++ch) {
        stats->minimum[ch] = INT16_MAX;
        stats->maximum[ch] = INT16_MIN;
    }
}

static void add_stats(mic_stats_t *stats, const int32_t slots[2])
{
    int32_t pcm[2] = {dual_mic_pcm16(slots[0]), dual_mic_pcm16(slots[1])};
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

void init_i2s(void)
{
    const i2s_config_t speaker = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 4,
        .dma_buf_len = I2S_BLOCK_FRAMES,
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

    const i2s_config_t microphone = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .bits_per_chan = I2S_BITS_PER_CHAN_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = I2S_BLOCK_FRAMES,
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
    ESP_LOGI(TAG, "RX: 16000 Hz, stereo 32-bit slots, BCLK=1024000 Hz; BCLK=%d WS=%d SD=%d",
             I2S_MIC_BCK_PIN, I2S_MIC_WS_PIN, I2S_MIC_DATAIN_PIN);
}

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

static esp_err_t write_speaker(const int16_t *samples, size_t bytes)
{
    size_t total = 0;
    while (total < bytes) {
        size_t written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0, (const uint8_t *)samples + total,
                                  bytes - total, &written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) return err;
        if (written == 0) return ESP_ERR_TIMEOUT;
        total += written;
    }
    return ESP_OK;
}

/* This function reads only saved PCM. Live microphone data cannot reach TX
 * during replay, breaking the speaker -> microphone -> speaker feedback path. */
static esp_err_t replay_recording(const int16_t *saved, size_t frames, int16_t *tx,
                                   dual_mic_mode_t mode)
{
    ESP_LOGI(TAG, "replay START mode=%s record=0 play=1 duration_ms=%u volume=%.2f",
             mode_names[mode], (unsigned)(frames * 1000 / I2S_SAMPLE_RATE), (double)SPK_VOLUME);
    for (size_t offset = 0; offset < frames; offset += I2S_BLOCK_FRAMES) {
        size_t block_frames = frames - offset;
        if (block_frames > I2S_BLOCK_FRAMES) block_frames = I2S_BLOCK_FRAMES;
        for (size_t i = 0; i < block_frames; ++i) {
            int16_t faded = dual_mic_replay_sample(saved[offset + i], (uint32_t)(offset + i),
                                                  (uint32_t)frames, I2S_SAMPLE_RATE / 50);
            int16_t output = (int16_t)(faded * SPK_VOLUME);
            tx[i * 2] = output;
            tx[i * 2 + 1] = output;
        }
        esp_err_t err = write_speaker(tx, block_frames * 2 * sizeof(int16_t));
        if (err != ESP_OK) return err;
    }
    // Queue enough silence for the saved audio to leave the four TX DMA buffers
    // before clearing them; otherwise the tail could be truncated.
    memset(tx, 0, I2S_BLOCK_FRAMES * 2 * sizeof(int16_t));
    for (int block = 0; block < 6; ++block) {
        esp_err_t err = write_speaker(tx, I2S_BLOCK_FRAMES * 2 * sizeof(int16_t));
        if (err != ESP_OK) return err;
    }
    ESP_LOGI(TAG, "replay END record=0 play=0; ready for next recording");
    return ESP_OK;
}

void i2s_loopback_task(void *arg)
{
    (void)arg;
    // Static buffers keep the app_main stack small. I2S DMA remains driver-owned.
    static int32_t rx[I2S_BLOCK_FRAMES * 2];
    static int16_t tx[I2S_BLOCK_FRAMES * 2];
    static int16_t saved[I2S_SAMPLE_RATE * I2S_RECORD_SECONDS];
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
    size_t filled = 0;
    unsigned warmup_blocks = 64; // 512 ms at 16 kHz, exceeding microphone startup time.

    ESP_LOGI(TAG, "Short BOOT press: LEFT -> RIGHT -> AVERAGE -> DIFFERENCE");
    ESP_LOGI(TAG, "Hold BOOT >=400 ms: RECORD (max %d s); release: REPLAY. Initial mode=%s",
             I2S_RECORD_SECONDS,
             mode_names[mode]);
    ESP_LOGI(TAG, "Keep the speaker away from microphones during loopback checks");
    ESP_LOGI(TAG, "Record/replay output volume=%.2f; no live microphone loopback", (double)SPK_VOLUME);
    ESP_LOGI(TAG, "BOOT GPIO%d: initial level=%d, pressed=LOW, debounce=30 ms, hold=400 ms",
             BTN_BOOT_PIN, gpio_get_level(BTN_BOOT_PIN));
    while (true) {
        size_t received = 0;
        esp_err_t err = i2s_read(I2S_NUM_1, (uint8_t *)rx + filled, sizeof(rx) - filled,
                                 &received, pdMS_TO_TICKS(100));
        TickType_t now = xTaskGetTickCount();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "microphone read failed: %s", esp_err_to_name(err));
            ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        filled += received;
        if (received > 0) last_data_at = now;
        if (filled < sizeof(rx)) {
            if ((now - last_data_at) >= pdMS_TO_TICKS(1000)) {
                ESP_LOGW(TAG, "no microphone data for 1 second; check I2S clocks");
                last_data_at = now;
            }
            ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        filled = 0;
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
        // Recording state consumes the debounced button, replay waits for release.
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
            // RX continued while saved audio played. Drain its eight queued
            // blocks, then leave another 128 ms for speaker decay before capture.
            for (int block = 0; block < 8; ++block) {
                size_t discarded = 0;
                ESP_ERROR_CHECK(i2s_read(I2S_NUM_1, rx, sizeof(rx), &discarded, 0));
            }
            warmup_blocks = 16;
            continue;
        }
        for (size_t frame = 0; frame < I2S_BLOCK_FRAMES; ++frame) {
            const int32_t *pair = &rx[frame * 2];
            add_stats(&stats, pair);
            int16_t mono = dual_mic_process(&filter, pair, mode);
            if (recording_session && recorded_frames < I2S_SAMPLE_RATE * I2S_RECORD_SECONDS) {
                saved[recorded_frames++] = mono;
            }
            // Always silent during live capture. TX receives saved audio only
            // inside replay_recording(), after the recording has stopped.
            tx[frame * 2] = 0;
            tx[frame * 2 + 1] = 0;
        }
        if (recording_session && !recording_full && recorded_frames == I2S_SAMPLE_RATE * I2S_RECORD_SECONDS) {
            recording_full = true;
            ESP_LOGI(TAG, "record buffer FULL (%d s), record=0 play=0; release BOOT to replay",
                     I2S_RECORD_SECONDS);
        }
        err = write_speaker(tx, sizeof(tx));
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
