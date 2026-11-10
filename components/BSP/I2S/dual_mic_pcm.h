#ifndef DUAL_MIC_PCM_H
#define DUAL_MIC_PCM_H

#include <stdint.h>

typedef enum {
    DUAL_MIC_LEFT,
    DUAL_MIC_RIGHT,
    DUAL_MIC_AVERAGE,
    DUAL_MIC_DIFFERENCE,
    DUAL_MIC_MODE_COUNT
} dual_mic_mode_t;

typedef struct {
    int64_t dc_q16[2];
} dual_mic_filter_t;

/* 20 ms fades are applied to the stored recording, not to live microphone
 * samples. Q15 scaling stays within PCM16 range, including INT16_MIN. */
static inline int16_t dual_mic_replay_sample(int16_t sample, uint32_t frame,
                                            uint32_t frames, uint32_t fade_frames)
{
    if (frames == 0 || frame >= frames) return 0;
    if (fade_frames == 0) return sample;
    uint32_t distance = frame;
    uint32_t to_end = frames - 1 - frame;
    if (to_end < distance) distance = to_end;
    if (distance >= fade_frames) return sample;
    int32_t envelope = (int32_t)(((uint64_t)distance * 32768) / fade_frames);
    return (int16_t)(((int32_t)sample * envelope) / 32768);
}

static inline int16_t dual_mic_saturate(int32_t value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

/* INMP441: signed 24-bit sample in the upper bits of a 32-bit I2S slot.
 * Discard the lower 16 bits for PCM16. ESP32-S3/GCC uses arithmetic >>. */
static inline int16_t dual_mic_pcm16(int32_t slot)
{
    return (int16_t)(slot >> 16);
}

static inline int16_t dual_mic_mix(int32_t left, int32_t right, dual_mic_mode_t mode)
{
    switch (mode) {
    case DUAL_MIC_LEFT: return dual_mic_saturate(left);
    case DUAL_MIC_RIGHT: return dual_mic_saturate(right);
    case DUAL_MIC_AVERAGE: return dual_mic_saturate((left + right) / 2);
    case DUAL_MIC_DIFFERENCE: return dual_mic_saturate((left - right) / 2);
    default: return 0;
    }
}

/* Q16 DC tracking avoids integer-rounding dead zones. No adaptive noise
 * cancellation: preserve both microphones for independent hardware checks.
 * left/right are the two 32-bit I2S slots of one frame, passed by value. */
static inline int16_t dual_mic_process(dual_mic_filter_t *filter,
                                     int32_t left, int32_t right, dual_mic_mode_t mode)
{
    int32_t pcm[2] = {dual_mic_pcm16(left), dual_mic_pcm16(right)};
    for (int ch = 0; ch < 2; ++ch) {
        filter->dc_q16[ch] += ((int64_t)pcm[ch] * 65536 - filter->dc_q16[ch]) / 256;
        pcm[ch] -= (int32_t)(filter->dc_q16[ch] / 65536);
    }
    return dual_mic_mix(pcm[0], pcm[1], mode);
}

#endif
