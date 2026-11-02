#ifndef BSP_I2S_H
#define BSP_I2S_H

#include "dual_mic_pcm.h"

// MAX98357: I2S0 TX. Both output slots carry the same mono signal.
#define I2S_SPK_BCK_PIN      15
#define I2S_SPK_WS_PIN       16
#define I2S_SPK_DATAOUT_PIN  7

// Two INMP441 share BCLK/WS/SD. Left L/R -> GND; right L/R -> 3.3V.
#define I2S_MIC_BCK_PIN      5
#define I2S_MIC_WS_PIN       4
#define I2S_MIC_DATAIN_PIN   6
#define BTN_BOOT_PIN        0

#define I2S_SAMPLE_RATE     16000
#define I2S_BLOCK_FRAMES    128
#define SPK_VOLUME         0.25f
#define I2S_RECORD_SECONDS 3
#define I2S_INITIAL_MODE    DUAL_MIC_LEFT

void init_i2s(void);
void boot_init(void);
void i2s_loopback_task(void *arg);

#endif
