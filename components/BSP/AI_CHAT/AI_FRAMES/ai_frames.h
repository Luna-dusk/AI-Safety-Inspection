#ifndef AI_FRAMES_H
#define AI_FRAMES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    AI_PT_AUDIO = 0x01,
    AI_PT_JSON  = 0x02,
} ai_payload_type_t;

/* 每凑齐一个完整帧回调一次。payload 只在回调期间有效。 */
typedef void (*ai_frames_cb_t)(ai_payload_type_t type,
                               const uint8_t *payload, uint16_t len, void *ctx);

#define AI_FRAME_MAX_PAYLOAD  12288          /* > 10240（320ms 音频块）+ 余量 */

/* 解析器实例。零 IDF 依赖 → 可以在 PC 上单独编译自测。 */
typedef struct {
    ai_frames_cb_t cb;
    void          *ctx;
    uint8_t        hdr[3];                   
    uint8_t        hdr_got;                 
    uint8_t        buf[AI_FRAME_MAX_PAYLOAD];
    uint16_t       want;                     /* 当前帧期望长度 */
    uint16_t       got;                      /* 已收 */
    bool           err;                      /* 流已损坏，喂了也没用 */
} ai_frames_t;

void ai_frames_init(ai_frames_t *f, ai_frames_cb_t cb, void *ctx);
/* 返回：0=正常消化；-1=流损坏（长度超限），调用方应断开连接 */
int ai_frames_feed(ai_frames_t *f, const uint8_t *data, size_t len);

#endif