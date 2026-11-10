#include "multinet_cmd.h"

#include "esp_log.h"
#include "esp_mn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "LED.h"
#include "i2s.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "MNET";

static esp_mn_iface_t *mn_handle = NULL;
static model_iface_data_t *mn_data = NULL;
static bool mn_inited = false;
static multinet_reply_cb_t reply_cb = NULL;

/**
 * @brief  注册"命令命中后"的语音回复回调
 * @param  cb 回调函数指针；传 NULL 表示取消
 *
 * 本模块只负责识别与本地执行，语音回复（例如从服务器拉 TTS）交给上层，
 * 用回调解耦，避免 BSP 直接依赖网络层。
 */
void multinet_cmd_set_reply_cb(multinet_reply_cb_t cb)
{
    reply_cb = cb;
}

/**
 * @brief  注册中文命令词表并提交给 MultiNet 引擎
 *
 * 用拼音串注册；同义说法通过多次 esp_mn_commands_add 映射到同一个 command_id，
 * 例如 "da kai deng" 与 "kai deng" 都映射到 MNET_CMD_LIGHT_ON。
 * 提交后若有非法短语，逐条打印出错的 command_id。
 *
 * @note 日志打印的短语数是 MNET_CMD_MAX-1（=3），但实际 add 了 9 条
 *       （三个命令各带若干同义说法），该数字仅作提示，与实际条数不一致。
 */
static void register_commands(void)
{
    esp_mn_commands_alloc(mn_handle, mn_data);

    /* 同义可多条映射到同一 command_id */
    esp_mn_commands_add(MNET_CMD_HELP,  "jiu ming");
    esp_mn_commands_add(MNET_CMD_HELP,  "jiu jiu wo");
    esp_mn_commands_add(MNET_CMD_HELP,  "wo shuai dao le");
    esp_mn_commands_add(MNET_CMD_HELP,  "wo bu shu fu");
    esp_mn_commands_add(MNET_CMD_COME_HERE,  "bang wo jiao ren");
    esp_mn_commands_add(MNET_CMD_COME_HERE,  "kuai lai ren");
    esp_mn_commands_add(MNET_CMD_NO_PROBLEM, "wo mei shi");
    esp_mn_commands_add(MNET_CMD_NO_PROBLEM, "mei shi");
    esp_mn_commands_add(MNET_CMD_NO_PROBLEM, "mei wen ti");

    esp_mn_error_t *err = esp_mn_commands_update();
    if (err) {
        for (int i = 0; i < err->num; i++) {
            ESP_LOGW(TAG, "bad phrase id=%d", err->phrases[i]->command_id);
        }
    } else {
        ESP_LOGI(TAG, "commands registered: %d phrases",
                 MNET_CMD_MAX - 1);
    }
    esp_mn_active_commands_print();
}

/**
 * @brief  初始化 MultiNet 模型并注册命令表
 * @return ESP_OK 成功；ESP_FAIL 模型缺失或实例创建失败
 *
 * 流程：从 model 分区加载模型列表 -> 筛选中文 MultiNet 模型 ->
 *       取句柄 -> 创建实例（6000 为内部缓冲尺寸）-> 注册命令 ->
 *       把检测阈值调到 0.3（越小越灵敏）。
 *
 * @note 依赖 menuconfig 选中 mn6_cn 且 model 分区已烧录模型，
 *       否则 esp_srmodel_init 或模型筛选会直接失败。
 * @note 本函数在 afe_init() 中被顺带调用，与 AFE 共用同一份模型分区。
 */
esp_err_t multinet_cmd_init(void)
{
    if (mn_inited) return ESP_OK;

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "esp_srmodel_init failed");
        return ESP_FAIL;
    }

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "no CN multinet model (check menuconfig mn6_cn)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MultiNet model: %s", mn_name);

    mn_handle = esp_mn_handle_from_name(mn_name);
    if (!mn_handle) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name failed");
        return ESP_FAIL;
    }

    mn_data = mn_handle->create(mn_name, 6000);
    if (!mn_data) {
        ESP_LOGE(TAG, "create failed");
        return ESP_FAIL;
    }

    register_commands();

    /* 降低检测阈值，提高识别灵敏度（0=最高灵敏度，1=最严格） */
    mn_handle->set_det_threshold(mn_data, 0.3f);

    mn_inited = true;
    ESP_LOGI(TAG, "MultiNet ready, chunk=%d", mn_handle->get_samp_chunksize(mn_data));
    return ESP_OK;
}

/**
 * @brief  销毁 MultiNet 实例
 *
 * 只销毁实例；模型列表本身由 esp-sr 库自行管理。
 *
 * @note 未把 mn_handle 置空，重复 init 时会重新赋值，不影响功能。
 */
void multinet_cmd_deinit(void)
{
    if (mn_data) {
        mn_handle->destroy(mn_data);
        mn_data = NULL;
    }
    mn_inited = false;
}

/**
 * @brief  喂入一帧音频做本地命令识别
 * @param  pcm 单帧 PCM（长度应为 multinet_cmd_chunksize() 的返回值）
 * @return >0 命中的 command_id；0 仍在检测中；-1 检测超时
 *
 * 由 afe_sr.c 的 fetch 任务在 SESSION 状态下每帧调用一次；
 * 命中时取 results[0]（概率最高的一条）作为命令。
 *
 * @note 必须在持有 afe_lock 的前提下调用——esp-sr 不是线程安全的。
 * @note 调用方需在返回 -1 时执行 multinet_cmd_reset() 复位检测状态。
 */
int multinet_cmd_feed(const int16_t *pcm)
{
    if (!mn_inited || !mn_handle || !mn_data || !pcm) return 0;

    esp_mn_state_t st = mn_handle->detect(mn_data, (int16_t *)pcm);

    if (st == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *r = mn_handle->get_results(mn_data);
        if (r && r->num > 0) {
            ESP_LOGI(TAG, "DETECTED cmd_id=%d prob=%.2f phrase=%d",
                     r->command_id[0], r->prob[0], r->phrase_id[0]);
            return r->command_id[0];
        }
    }
    if (st == ESP_MN_STATE_TIMEOUT) {
        ESP_LOGI(TAG, "TIMEOUT");
        return -1;
    }
    return 0; /* DETECTING */
}

/**
 * @brief  清空 MultiNet 内部检测状态
 *
 * 进入 LISTENING、命令命中后、检测超时后都需要调用，
 * 否则上一次的累积状态会污染下一次识别。
 *
 * @note 同样需要在 afe_lock 保护下调用。
 */
void multinet_cmd_reset(void)
{
    if (mn_data && mn_handle) {
        mn_handle->clean(mn_data);
    }
}

/**
 * @brief  执行一条已命中的本地命令
 * @param  cmd_id MNET_CMD_* 命令编号
 *
 * 目前三个命令都只打日志（具体行为待接入）；
 * 执行完统一触发 reply_cb，由上层决定是否播放语音回复。
 */
void multinet_cmd_exec(int cmd_id)
{
    switch (cmd_id) {
    case MNET_CMD_HELP:
        ESP_LOGI(TAG, "EXEC: HELP");
        //行为
        break;
    case MNET_CMD_COME_HERE:
        ESP_LOGI(TAG, "EXEC: COME HERE");
        //行为
        break;
    case MNET_CMD_NO_PROBLEM: {
        //行为
        ESP_LOGI(TAG, "EXEC: NO PROBLEM");
        break;
    }
    default:
        ESP_LOGW(TAG, "unknown cmd %d", cmd_id);
        break;
    }

    /* 语音回复：由 main 注册的回调从服务器拉取对应 TTS 并播放 */
    if (reply_cb) {
        reply_cb(cmd_id);
    }
}

/**
 * @brief  返回 MultiNet 每帧所需的采样点数
 * @return 采样点数；模型未初始化时返回 0
 *
 * 应与 AFE fetch 出来的 data_size/2 保持一致，否则喂进去的长度不对，
 * 识别结果会不稳定或完全不触发。
 */
int multinet_cmd_chunksize(void)
{
    if (mn_data && mn_handle) {
        return mn_handle->get_samp_chunksize(mn_data);
    }
    return 0;
}
