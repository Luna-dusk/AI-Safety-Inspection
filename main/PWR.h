//
// Created by l on 2026/11/12.
//

#ifndef HELLO_WORLD_PWR_H
#define HELLO_WORLD_PWR_H

#include "esp_err.h"

#ifdef __cplusplus
extern  "C" {
#endif

esp_err_t pwr_init(void);

typedef void (*pwr_shutdown_cb_t)(void);
void pwr_set_shutdown_cb(pwr_shutdown_cb_t cb);

bool pwr_is_shutting_down(void);


#ifdef __cplusplus
}
#endif

#endif //HELLO_WORLD_PWR_H
