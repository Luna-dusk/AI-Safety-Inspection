#include "ai_frames.h"

#include "esp_log.h"

static const char *TAG = "AI_FRAMES";
void ai_frames_init(ai_frames_t *frames, ai_frames_cb_t cb, void *ctx)
{
    frames->cb = cb;
    frames->ctx = ctx;
    frames->hdr_got = 0;
    frames->want = 0;
    frames->got = 0;
    frames->err = false;
}
int ai_frames_feed(ai_frames_t *frames,const uint8_t *data,size_t len)
{
    if(frames->err)
    {
        ESP_LOGE(TAG,"stream corrupted, feed ignored");
        return -1;
    }
    for(size_t i=0;i<len;i++)
    {
        if (frames->hdr_got < 3) {
            frames->hdr[frames->hdr_got++] = data[i];
            if (frames->hdr_got < 3) continue;
            frames->want = ((uint16_t)frames->hdr[1] << 8) | frames->hdr[2];   /* u16 BE */
            frames->got  = 0;
            if (frames->want == 0) {                       /* 0 长帧：服务器 bug，跳过 */
                frames->cb((ai_payload_type_t)frames->hdr[0], NULL, 0, frames->ctx);
                frames->hdr_got = 0;
                continue;
            }
            if (frames->want > AI_FRAME_MAX_PAYLOAD) {     /* 流错位/被污染，不可恢复 */
                frames->err = true;
                return -1;
            }
        } else {
            frames->buf[frames->got++] = data[i];               /* 逐字节略慢但最简单； */
        } 
        if (frames->hdr_got == 3 && frames->got >= frames->want) {
            frames->cb((ai_payload_type_t)frames->hdr[0], frames->buf, frames->want, frames->ctx);
            frames->hdr_got = 0;
        }
    }
    return 0;
}