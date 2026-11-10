#ifndef _APP_STATE_H_
#define _APP_STATE_H_

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif_sntp.h"
#include "wifi.h"
#include <sys/time.h>
#include "stdlib.h"

bool time_is_valid(void);
esp_err_t time_sync(void);

#endif // 