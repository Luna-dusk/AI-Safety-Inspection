#ifndef MYESP_NOW_H
#define MYESP_NOW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"       /* MACSTR / MAC2STR */
#include "esp_wifi.h"
#include "esp_now.h"
#include "wifi.h"          /* wifi_get_event_group() / WIFI_CONNECTED_BIT */

/* 回调投递事件的等待上限（tick）。
 * 注意：回调运行在 WiFi 任务上下文，正式收发音频/高频消息时必须改成 0
 * （队列满立刻丢弃），否则会拖垮 WiFi 协议栈。 */
#define ESPNOW_MAXDELAY 512

/* ESPNOW 事件类型枚举 */
typedef enum {
    ESPNOW_SEND_CB,   // 发送回调事件
    ESPNOW_RECV_CB,   // 接收回调事件
} espnow_event_id_t;

/* 发送回调事件的数据 */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];   // 目标设备的 MAC 地址
    esp_now_send_status_t status;         // 发送状态（成功/失败）
} espnow_event_send_cb_t;

/* 接收回调事件的数据 */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];   // 发送方设备的 MAC 地址
    uint8_t *data;                        // 接收到的数据缓冲区指针（消费方负责 free）
    int data_len;                         // 接收到的数据长度（字节）
} espnow_event_recv_cb_t;

/* 事件信息联合体：同一时刻只可能是发送或接收事件 */
typedef union {
    espnow_event_send_cb_t send_cb;       // 发送回调信息
    espnow_event_recv_cb_t recv_cb;       // 接收回调信息
} espnow_event_info_t;

/* ESPNOW 事件结构体
 * 当 ESPNOW 的发送/接收回调函数被触发时，
 * 会把该事件投递到 ESPNOW 任务队列中统一处理
 */
typedef struct {
    espnow_event_id_t id;                 // 事件类型（发送/接收）
    espnow_event_info_t info;             // 事件具体信息
} espnow_event_t;

/* ESPNOW 数据帧类型枚举 */
enum {
    EXAMPLE_ESPNOW_DATA_BROADCAST,        // 广播数据
    EXAMPLE_ESPNOW_DATA_UNICAST,          // 单播数据
    EXAMPLE_ESPNOW_DATA_MAX,              // 类型数量上限（用于合法性检查）
};

/* 用户自定义的 ESPNOW 数据帧格式（后续协议帧骨架，当前版本暂未使用） */
typedef struct {
    uint8_t type;                         // 数据类型：广播或单播
    uint8_t state;                        // 状态：是否已收到过广播数据
    uint16_t seq_num;                     // 数据序列号（用于检测丢包）
    uint16_t crc;                         // 数据 CRC16 校验值
    uint32_t magic;                       // 魔术字，用于判断哪个设备应发送单播数据
    uint8_t payload[0];                   // 实际负载（柔性数组，长度可变）
} espnow_data_t;

/* ESPNOW 发送参数配置结构体（example 骨架，当前版本暂未使用） */
typedef struct {
    bool unicast;                         // 是否发送单播数据
    bool broadcast;                       // 是否发送广播数据
    uint8_t state;                        // 状态：是否已收到过广播数据
    uint32_t magic;                       // 魔术字，用于匹配设备
    uint16_t count;                       // 要发送的单播数据总数量
    uint16_t delay;                       // 两次发送之间的延时，单位：毫秒
    int len;                              // 要发送的数据长度，单位：字节
    uint8_t *buffer;                      // 指向待发送数据的缓冲区
    uint8_t dest_mac[ESP_NOW_ETH_ALEN];   // 目标设备的 MAC 地址
} espnow_send_param_t;

/* 对端（peer）设备信息，定义在 myesp_now.c。
 * 头文件里只能 extern 声明——带初始化的定义放在头文件里，
 * 两个 .c 一旦同时包含本头文件就会 multiple definition 链接错误。 */
extern esp_now_peer_info_t peer;

/**
 * @brief  初始化 ESP-NOW（非阻塞，快速返回）
 *
 * 内部建事件队列并启动 espnow 任务：任务里先等 WiFi 拿到 IP（两边连同一
 * AP 后信道才一致），再 esp_now_init + 注册回调 + 加 peer，然后循环消费
 * 收发事件。WiFi 连不上时最多等 30s 后放弃，不拖累其它功能。
 *
 * 调用前提：esp_create_wifi_task() 已先被调用。
 */
esp_err_t my_esp_now_init(void);

#endif /* MYESP_NOW_H */
