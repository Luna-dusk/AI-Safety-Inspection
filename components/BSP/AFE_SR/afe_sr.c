#include "afe_sr.h"
#include "multinet_cmd.h"
#include "esp_wn_models.h"
#include "esp_wn_iface.h"

static const char *TAG = "AFE_SR";

static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static QueueHandle_t pcm_queue = NULL;
static TaskHandle_t feed_task = NULL;
static TaskHandle_t fetch_task = NULL;

/*
 * esp-sr 库不是多线程安全的：feed()（含 WakeNet/VAD 推理）在 feed 任务（核1）
 * 与 fetch()/multinet 命令识别（TIE DSP，RNNT）在 fetch 任务（核0）并发执行时
 * 会死锁（dl_tie728_memcpy 卡死、中断关闭 -> WDT 硬复位，GDB 已证实）。
 * 所有 esp-sr 调用必须用本锁串行化。
 */
static SemaphoreHandle_t afe_lock = NULL;
/* feed 任务每喂入一帧就 give 一次，fetch 任务取到信号后才 fetch()。
 * 目的：让 fetch 以 feed 的节奏运行，而不是在持锁状态下长时间阻塞等待
 * 输出数据（esp-sr fetch() 默认超时 ~200ms，持锁阻塞会饿死 feed()）。 */
static SemaphoreHandle_t fetch_sem = NULL;

#define AFE_LOCK()   do { if (afe_lock) xSemaphoreTake(afe_lock, portMAX_DELAY); } while (0)
#define AFE_UNLOCK() do { if (afe_lock) xSemaphoreGive(afe_lock); } while (0)

static int16_t tail[AUDIO_FRAME_SAMPLES];
static size_t tail_samples = 0;
/* false=上传/播 TTS 中，忽略 VAD，避免喇叭声被当成用户说话 */
static volatile bool capture_enabled = true;
static volatile int capture_warmup = 0;
static volatile TickType_t session_last_activity = 0;   /* 会话最后活动 tick */

typedef enum {
    SR_IDLE = 0,       /* 休眠：只等唤醒词/按键 */
    SR_LISTENING = 1,  /* 会话：持续多轮 VAD，不需重复唤醒 */
} sr_state_t;

/* fetch 任务当前状态；供 main 同步 LCD（待机/聆听）。只在 fetch 任务里写 */
static volatile sr_state_t afe_cur_state = SR_IDLE;

/**
 * @brief  当前是否处于"会话/聆听"状态
 * @return true=LISTENING，false=待机等唤醒词
 *
 * 供 main 同步 LCD 显示用。读的是 volatile 状态变量，
 * 只在 fetch 任务里写、可在任意任务里读。
 */
bool afe_sr_in_session(void)
{
    return afe_cur_state == SR_LISTENING;
}

/**
 * @brief  打开/关闭音频采集（TTS 播放与上传期间关闭）
 * @param  enabled true=恢复采集，false=暂停采集
 *
 * 关闭时：清空断句残留、冻结双麦自适应，防止把扬声器回灌声当成用户说话。
 * 打开时：复位 AFE 内部缓冲、复位双麦差分与 DC、恢复自适应，
 *         并设置 30 帧（约 1 秒）冷却期，等喇叭余振衰减完再判 VAD。
 *
 * @note 这是本项目对抗"自激"的主要手段——因为没有启用 AEC，
 *       只能靠时间上的软静音来隔离播放与采集。
 * @note 调用方通常是 main：播 TTS 前置 false，播完置 true。
 */
void afe_sr_set_capture_enabled(bool enabled)
{
    bool was_enabled = capture_enabled;
    capture_enabled = enabled;
    if (was_enabled != enabled) {
        ESP_LOGI(TAG, "capture %s", enabled ? "ON" : "OFF");
    }
    if (!enabled) {
        tail_samples = 0;
        /* TTS 播放期间冻结双麦自适应，防止滤波锁到扬声器回灌声 */
        i2s_dual_mic_set_adapt(false);
    } else {
        /* 复位 AFE 内部缓冲，丢弃 TTS 播放期间累积的回声 */
        if (afe_handle && afe_data) {
            AFE_LOCK();
            afe_handle->reset_buffer(afe_data);
            AFE_UNLOCK();
        }
        /* 清双麦差分延迟线/DC，恢复自适应（权重保留，持续收敛） */
        i2s_dual_mic_reset();
        i2s_dual_mic_set_adapt(true);
        capture_warmup = 30;   /* ~1 s 冷却 */
        session_last_activity = xTaskGetTickCount();  /* 刷新会话活动时间 */
    }
}

/**
 * @brief  把一帧 PCM 送入上传队列（带背压保护）
 * @param  frame 待发送的音频帧
 * @return true=入队成功
 *
 * 关键约束：始终给 AUDIO_FRAME_END 预留 1 个槽位。
 * 否则网络慢导致队列满时，END 帧发不出去，状态机会卡在
 * "采集已关闭"里再也回不来（表现为设备"哑住"）。
 */
static bool queue_send_frame(const audio_frame_t *frame)
{
    if (pcm_queue == NULL || frame == NULL) {
        return false;
    }
    /* Keep one queue slot for AUDIO_FRAME_END so a slow network cannot stall
     * the AFE state machine in the capture-disabled state. */
    if (frame->type == AUDIO_FRAME_DATA && uxQueueSpacesAvailable(pcm_queue) <= 1) {
        ESP_LOGW(TAG, "pcm queue reserved END slot, drop data");
        return false;
    }
    if (xQueueSend(pcm_queue, frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "pcm queue full, drop type=%d", (int)frame->type);
        return false;
    }
    return true;
}

/**
 * @brief  发送一个"一句话结束"标记帧
 * @return true=入队成功
 *
 * 上层收到 AUDIO_FRAME_END 后即可把累积的 PCM 送去 ASR 识别。
 */
static bool send_end_frame(void)
{
    audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = AUDIO_FRAME_END;
    frame.bytes = 0;
    if (xQueueSend(pcm_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "send END_FRAME failed");
        return false;
    }
    return true;
}

/**
 * @brief  把任意长度的 PCM 数据切成固定大小的帧推入队列
 * @param  pcm       采样数据
 * @param  pcm_bytes 字节数
 *
 * AFE 每次 fetch 给的数据长度不固定，用 tail 缓冲跨调用累积，
 * 凑满 AUDIO_FRAME_SAMPLES 才组一帧发出，保证下游帧长恒定。
 */
static void push_pcm_bytes(const int16_t *pcm, int pcm_bytes)
{
    if (pcm == NULL || pcm_bytes <= 0) {
        return;
    }

    const int16_t *src = pcm;
    size_t samples_left = (size_t)pcm_bytes / sizeof(int16_t);

    while (samples_left > 0) {
        size_t need = AUDIO_FRAME_SAMPLES - tail_samples;
        size_t take = (samples_left < need) ? samples_left : need;

        memcpy(&tail[tail_samples], src, take * sizeof(int16_t));
        tail_samples += take;
        src += take;
        samples_left -= take;

        if (tail_samples == AUDIO_FRAME_SAMPLES) {
            audio_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            frame.type = AUDIO_FRAME_DATA;
            frame.bytes = AUDIO_FRAME_BYTES;
            memcpy(frame.data, tail, AUDIO_FRAME_BYTES);
            queue_send_frame(&frame);
            tail_samples = 0;
        }
    }
}

/**
 * @brief  丢弃队列中所有未发送的音频帧
 *
 * 用于两种场景：本地命令命中（这句话不需要上传云端）、
 * 以及唤醒/状态切换时清理残留数据。非阻塞调用。
 */
static void drain_pcm_queue(void)
{
    if (pcm_queue == NULL) return;
    audio_frame_t junk;
    while (xQueueReceive(pcm_queue, &junk, 0) == pdTRUE) {
    }
}

/**
 * @brief  进入休眠态：打开 WakeNet，等待唤醒词或按键
 * @param  data         AFE 实例
 * @param  in_speech    断句状态输出（置 false）
 * @param  cur_hangover 静音计数输出（清零）
 *
 * 同时刷新会话活动时间、清空断句残留、把 LCD 切为待机表情。
 *
 * @note 内部调用 afe_handle->enable_wakenet() 并已加锁；
 *       调用方不要在外层重复加锁（互斥量默认非递归）。
 */
static void enter_idle(esp_afe_sr_data_t *data, bool *in_speech, int *cur_hangover)
{
    ESP_LOGW(TAG, "enter_idle() called");
    *in_speech = false;
    *cur_hangover = 0;
    session_last_activity = xTaskGetTickCount();
    tail_samples = 0;
    afe_cur_state = SR_IDLE;
    if (afe_handle && data) {
        AFE_LOCK();
        afe_handle->enable_wakenet(data);
        AFE_UNLOCK();
    }
    ESP_LOGI(TAG, "state -> SLEEP (wait 你好小鑫 or KEY->GND on GPIO%d)",
             (int)KEY_GPIO_PIN);
    lcd_ui_set_status(LCD_STATUS_STANDBY);
    lcd_ui_set_emotion(LCD_EMO_DROOL);   /* 待机：流口水脸 */
}

/**
 * @brief  进入会话态：关闭 WakeNet，开启连续多轮语音交互
 * @param  data         AFE 实例
 * @param  state        状态变量（置 SR_LISTENING）
 * @param  in_speech    断句状态输出
 * @param  cur_hangover 静音计数输出
 * @param  reason       唤醒来源（"WakeNet"/"KEY"/其它），仅用于日志
 *
 * 关闭 WakeNet 后不需要反复说唤醒词，直接把 VAD 检出的语音送去识别；
 * 会话静默 5 分钟后自动回到 IDLE。
 *
 * @note 内部对 disable_wakenet() 加锁，同样不要在外层重复加锁。
 */
static void enter_listening(esp_afe_sr_data_t *data, sr_state_t *state,
                            bool *in_speech, int *cur_hangover,
                            const char *reason)
{
    TickType_t now = xTaskGetTickCount();
    ESP_LOGI(TAG, "wake by %s -> SESSION (tick=%u, timeout=%u)",
             reason ? reason : "?", (unsigned)now,
             (unsigned)pdMS_TO_TICKS(SESSION_IDLE_TIMEOUT_MS));
    if (afe_handle && data) {
        AFE_LOCK();
        afe_handle->disable_wakenet(data);
        AFE_UNLOCK();
    }
    *state = SR_LISTENING;
    afe_cur_state = SR_LISTENING;
    *in_speech = false;
    *cur_hangover = 0;
    session_last_activity = now;
    tail_samples = 0;
    multinet_cmd_reset();
    lcd_ui_set_status(LCD_STATUS_LISTENING);
    lcd_ui_set_emotion(LCD_EMO_NORMAL);   /* 唤醒：流口水脸回正常脸 */
    ESP_LOGI(TAG, "state -> SESSION, capture=%s", capture_enabled ? "ON" : "OFF");
}

/**
 * @brief  AFE 喂数任务（固定在核 1，优先级 21）
 * @param  arg AFE 实例
 *
 * 职责：按 AFE 要求的帧长读取 I2S 数据 -> 喂给 afe_handle->feed() ->
 * 每喂一帧 give 一次 fetch_sem，驱动 fetch 任务。
 *
 * 两条刻意设计：
 *   1) feed 永远不停（包括采集关闭期间），保证 AFE 内部时基连续；
 *   2) 喂完立刻放锁，绝不在持锁状态下等待。
 *
 * @note 缓冲区优先用 MALLOC_CAP_INTERNAL 分配（DMA/内部 RAM 要求），
 *       失败才退化到普通 malloc。
 */
static void afe_feed_task(void *arg)
{
    esp_afe_sr_data_t *data = (esp_afe_sr_data_t *)arg;
    int chunksize = afe_handle->get_feed_chunksize(data);
    int nch = afe_handle->get_feed_channel_num(data);
    size_t bytes = (size_t)chunksize * (size_t)nch * sizeof(int16_t);

    ESP_LOGI(TAG, "feed: chunksize=%d nch=%d bytes=%u", chunksize, nch, (unsigned)bytes);

    int16_t *buf = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (int16_t *)malloc(bytes);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "feed buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t real_bytes = 0;
        if (receive_audio(buf, bytes, &real_bytes, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (real_bytes < bytes) {
            continue;
        }
        AFE_LOCK();
        afe_handle->feed(data, buf);          /* 始终喂，不中断 */
        AFE_UNLOCK();
        if (fetch_sem) {
            xSemaphoreGive(fetch_sem);
        }
    }
}

/**
 * @brief  AFE 取结果任务（固定在核 0，优先级 20）—— 整个语音逻辑的核心
 * @param  arg AFE 实例
 *
 * 每轮：等 fetch_sem -> fetch_with_delay(0) -> 按当前状态分派：
 *
 *   SR_IDLE：
 *     - WakeNet 命中 -> enter_listening()；
 *     - 按键请求     -> enter_listening()；
 *   SR_LISTENING：
 *     - 采集关闭   -> 丢弃 VAD，继续；
 *     - 冷却期内   -> 跳过 VAD；
 *     - 先跑 MultiNet 本地命令：命中即本地执行，并抑制本次上传；
 *     - 否则按 VAD 累积 PCM：语音开始时先推 vad_cache 与实时数据，
 *       连续 12 帧静音(约 380 ms)判为一句结束，推 END 帧并关闭采集等 TTS；
 *     - 长时间无语音 -> 超过 5 分钟回 IDLE。
 *
 * 几个必须保留的细节：
 *   - fetch_sem 保证 fetch 跟着 feed 的节奏走，避免持锁阻塞饿死 feed；
 *   - 每次最多排空 4 个结果，且"上次取空不到 1 秒"时跳过本次 fetch，
 *     是为了绕开 esp-sr 库在空 ringbuffer 上刷警告；
 *   - 即使 fetch_sem 超时也会检查按键，保证界面卡住时还能被按键唤醒。
 *
 * @note 所有 esp-sr 调用（fetch / multinet）都必须在 afe_lock 内，
 *       该库不是线程安全的，并发会死锁（详见文件头注释）。
 */
static void afe_fetch_task(void *arg)
{
    esp_afe_sr_data_t *data = (esp_afe_sr_data_t *)arg;
    sr_state_t state = SR_IDLE;
    bool in_speech = false;
    int cur_hangover = 0;
    const int hangover_frames = CONTINUE_FRAME_TIME;
    const TickType_t idle_timeout_ticks = pdMS_TO_TICKS(SESSION_IDLE_TIMEOUT_MS);
    int local_cmd_cooldown = 0;    /* 本地命令命中后短暂抑制 VAD，避免同一句话被上传 */

    ESP_LOGI(TAG, "fetch task start (session mode, wake=你好小鑫)");
    enter_idle(data, &in_speech, &cur_hangover);

    while (1) {
        /* 等 feed 任务喂入新数据后再 fetch，避免在持锁状态下阻塞等待输出
         * （esp-sr fetch() 空 ringbuffer 时会阻塞并打印 "Ringbuffer of AFE is
         * empty"；持 afe_lock 阻塞会饿死 feed 任务，导致整条链路停摆）。 */
        if (fetch_sem == NULL || xSemaphoreTake(fetch_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
            /* 超时（feed 停摆时）仍检查按键，避免卡死无法唤醒 */
            if (state == SR_IDLE && key_consume_wake_request()) {
                enter_listening(data, &state, &in_speech, &cur_hangover, "KEY");
            }
            continue;
        }

        /* 一次 feed 可能产生多个可 fetch 的结果，全部排空（非阻塞、不持锁久等）。
         * 最多取 4 次，避免排空末尾的空结果触发 esp-sr 库内警告。 */
        static TickType_t last_empty_tick = 0;
        int drain_count = 0;
        for (;;) {
            if (++drain_count > 4) {
                break;
            }
            /* 上一次排空到空不足 1 秒时跳过本次 fetch，避免库内警告刷屏 */
            if (last_empty_tick && (xTaskGetTickCount() - last_empty_tick) < pdMS_TO_TICKS(1000)) {
                break;
            }
            AFE_LOCK();
            afe_fetch_result_t *res = afe_handle->fetch_with_delay(data, 0);
            AFE_UNLOCK();
            if (res == NULL || res->ret_value == ESP_FAIL) {
                last_empty_tick = xTaskGetTickCount();
                /* 当前已无可取数据：检查按键后回到等 feed */
                if (state == SR_IDLE && key_consume_wake_request()) {
                    enter_listening(data, &state, &in_speech, &cur_hangover, "KEY");
                }
                break;
            }

            if (state == SR_IDLE) {
                if (res->wakeup_state == WAKENET_DETECTED ||
                    res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
                    enter_listening(data, &state, &in_speech, &cur_hangover, "WakeNet");
                    continue;
                }
                if (key_consume_wake_request()) {
                    enter_listening(data, &state, &in_speech, &cur_hangover, "KEY");
                }
                continue;
            }

            /* ========== SESSION：多轮对话 + 本地命令 ========== */

            /* 上传/播报中：丢弃 VAD，防止扬声器回灌导致自言自语 */
            if (!capture_enabled) {
                if (in_speech) {
                    in_speech = false;
                    cur_hangover = 0;
                    tail_samples = 0;
                }
                continue;
            }

            /* 刚恢复采集：冷却期跳过 VAD，消除扬声器余振 */
            if (capture_warmup > 0) {
                capture_warmup--;
                continue;
            }

            /* --- 本地命令检测（优先） --- */
            if (res->data != NULL && res->data_size > 0) {
                AFE_LOCK();
                int cmd = multinet_cmd_feed(res->data);
                AFE_UNLOCK();
                if (cmd > 0) {
                    ESP_LOGI(TAG, "local cmd=%d hit, drain queue & cooldown 1.5s", cmd);
                    multinet_cmd_exec(cmd);
                    AFE_LOCK();
                    multinet_cmd_reset();
                    AFE_UNLOCK();
                    drain_pcm_queue();
                    in_speech = false;
                    cur_hangover = 0;
                    tail_samples = 0;
                    local_cmd_cooldown = 50;
                    session_last_activity = xTaskGetTickCount();
                    /* 关闭采集：TTS 回复播放期间避免麦克风回灌触发 VAD 上传 */
                    capture_enabled = false;
                    i2s_dual_mic_set_adapt(false);
                    lcd_ui_set_status(LCD_STATUS_REPLYING);
                    continue;
                } else if (cmd == -1) {
                    AFE_LOCK();
                    multinet_cmd_reset();
                    AFE_UNLOCK();
                }
            }

            /* 本地命令冷却期：继续 MultiNet，抑制 VAD 上传 */
            if (local_cmd_cooldown > 0) {
                local_cmd_cooldown--;
                continue;
            }

            if (res->vad_state == VAD_SPEECH) {
                cur_hangover = 0;
                session_last_activity = xTaskGetTickCount();

                if (!in_speech) {
                    ESP_LOGI(TAG, "VAD speech start");
                    tail_samples = 0;
                    if (res->vad_cache_size > 0 && res->vad_cache != NULL) {
                        push_pcm_bytes(res->vad_cache, res->vad_cache_size);
                    }
                    in_speech = true;
                }

                if (res->data != NULL && res->data_size > 0) {
                    push_pcm_bytes(res->data, res->data_size);
                }
            } else {
                if (in_speech) {
                    cur_hangover++;
                    if (res->data != NULL && res->data_size > 0) {
                        push_pcm_bytes(res->data, res->data_size);
                    }
                    if (cur_hangover >= hangover_frames) {
                        ESP_LOGI(TAG, "utterance end, stay SESSION (capture OFF until TTS done)");
                        tail_samples = 0;
                        bool end_queued = send_end_frame();
                        in_speech = false;
                        cur_hangover = 0;
                        session_last_activity = xTaskGetTickCount();
                        if (!end_queued) {
                            ESP_LOGW(TAG, "utterance aborted: END_FRAME unavailable");
                            capture_enabled = true;
                            i2s_dual_mic_set_adapt(true);
                            lcd_ui_set_status(LCD_STATUS_LISTENING);
                            continue;
                        }
                        /* 立刻静音采集，等 main 播完 TTS 再打开 */
                        capture_enabled = false;
                        i2s_dual_mic_set_adapt(false);
                        lcd_ui_set_status(LCD_STATUS_REPLYING);
                    }
                } else {
                    /* 无语音：检查 5 分钟会话空闲 */
                    TickType_t now = xTaskGetTickCount();
                    TickType_t diff = now - session_last_activity;
                    if (diff >= idle_timeout_ticks) {
                        ESP_LOGW(TAG, "session idle -> SLEEP (now=%u last=%u diff=%u timeout=%u)",
                                 (unsigned)now, (unsigned)session_last_activity,
                                 (unsigned)diff, (unsigned)idle_timeout_ticks);
                        state = SR_IDLE;
                        capture_enabled = true;
                        i2s_dual_mic_set_adapt(true);
                        enter_idle(data, &in_speech, &cur_hangover);
                    }
                }
            }
        }
    }
}

/**
 * @brief  初始化 AFE：加载模型、创建实例，并顺带初始化 LED 与本地命令
 * @return ESP_OK 成功；ESP_FAIL 模型或实例创建失败
 *
 * 关键配置：
 *   - afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF)
 *     其中 "M" 表示单麦输入；
 *   - aec_init = false（不启用回声消除）；
 *   - wakenet_init = true，优先选 nihaoxiaoxin 唤醒词，找不到就取任意一个。
 *
 * @note 硬件是双麦，但这里按单麦配置，第二颗麦的数据实际未被使用。
 *       这也是 README 里"第二麦产噪、VAD 不结束"现象的根源。
 * @note 可重复调用：afe_data 非空时直接返回 ESP_OK，不会重复创建实例。
 */
esp_err_t afe_init(void)
{
    if (afe_data != NULL) {
        return ESP_OK;
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed (check model partition)");
        return ESP_FAIL;
    }

    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    afe_config->aec_init = false;
    afe_config->wakenet_init = true;

    /* 顺便初始化 LED 和本地命令 */
    LED_Init();
    LED(0);
    multinet_cmd_init();

    char *wake_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaoxin");
    if (wake_name == NULL) {
        wake_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    }
    if (wake_name != NULL) {
        afe_config->wakenet_model_name = wake_name;
        ESP_LOGI(TAG, "WakeNet model: %s", wake_name);
    } else {
        ESP_LOGW(TAG, "no WakeNet model in partition, check menuconfig NIHAOXIAOXIN");
    }

    afe_handle = esp_afe_handle_from_config(afe_config);
    if (afe_handle == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }

    afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (afe_data == NULL) {
        ESP_LOGE(TAG, "create_from_config failed");
        afe_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AFE init ok (WakeNet on, session mode)");
    return ESP_OK;
}
/**
 * @brief  启动 AFE 双任务链路
 * @param  queue 上传队列句柄（接收方是网络/ASR 层）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未先调 afe_init；
 *         ESP_ERR_INVALID_ARG 队列为空；ESP_ERR_NO_MEM 资源不足
 *
 * 顺序：初始化按键 -> 保存队列 -> 建 afe_lock 互斥量 -> 建 fetch_sem ->
 *       创建 feed 任务(核1，优先级21) -> 创建 fetch 任务(核0，优先级20)。
 *
 * @note 核 0 跑 WiFi 协议栈、核 1 跑音频，这样分配是为了避免
 *       esp-sr 推理与 WiFi 抖动互相干扰。
 * @note 两个任务创建都带失败回滚：fetch 创建失败会调用 afe_sr_stop()。
 */
esp_err_t afe_sr_start(QueueHandle_t queue)
{
    if (afe_data == NULL || afe_handle == NULL) {
        ESP_LOGE(TAG, "call afe_init first");
        return ESP_ERR_INVALID_STATE;
    }
    if (queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (feed_task != NULL) {
        return ESP_OK;
    }

    key_init();
    pcm_queue = queue;
    tail_samples = 0;

    if (afe_lock == NULL) {
        afe_lock = xSemaphoreCreateMutex();
        if (afe_lock == NULL) {
            ESP_LOGE(TAG, "create afe lock failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (fetch_sem == NULL) {
        fetch_sem = xSemaphoreCreateBinary();
        if (fetch_sem == NULL) {
            ESP_LOGE(TAG, "create fetch sem failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreatePinnedToCore(afe_feed_task, "afe_feed", 8 * 1024,
                                afe_data, 21, &feed_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "create feed task failed");
        feed_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 8 * 1024,
                                afe_data, 20, &fetch_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "create fetch task failed");
        afe_sr_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AFE tasks started");
    return ESP_OK;
}

/**
 * @brief  停止 AFE 链路（当前为空实现）
 *
 * @warning 本函数尚未实现：不会删除 feed/fetch 任务、不释放锁与信号量、
 *          也不销毁 afe_data。由于 afe_sr_start() 在 feed_task 非空时
 *          直接返回 ESP_OK，"停止再启动"目前是不成立的。
 *          若需支持重入或重配，必须在这里补齐任务删除与资源释放。
 */
void afe_sr_stop(void)
{
    
}
