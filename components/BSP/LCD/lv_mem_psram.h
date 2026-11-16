#ifndef __LV_MEM_PSRAM_H__
#define __LV_MEM_PSRAM_H__

/**
 * @file  lv_mem_psram.h
 * @brief LVGL 内存池后端：优先 PSRAM，失败退回内部 RAM
 *
 * LVGL 默认把 `LV_MEM_SIZE` 的**静态数组**放在 `.bss`（`lv_mem.c: work_mem_int[]`），
 * 等于开机就从内部 RAM 里挖走 32KB。这块板子内部 RAM 是稀缺资源（见 HANDOFF 坑 31），
 * 所以开 `CONFIG_LV_MEM_CUSTOM` 指到这里。LVGL 的对象/样式/文本缓冲不参与 DMA，
 * 放 PSRAM 合法。
 *
 * 注意：**draw buffer 不能走这里**。它要 SPI DMA 直接读，必须留在内部 RAM，
 *   见 lcd_ui.c 的 `heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`。
 *
 * 这三个函数通过根 CMakeLists.txt 的 `add_compile_definitions()` 注入成
 * `LV_MEM_CUSTOM_ALLOC / FREE / REALLOC`（要对 lvgl__lvgl 组件也生效，
 * 只能在 project() 之前做全局定义）。
 */

#include <stddef.h>
#include "esp_heap_caps.h"

#define LV_MEM_PSRAM_CAPS   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

/** 优先 PSRAM；PSRAM 不可用（未焊/初始化失败/占满）时退回内部 RAM */
static inline void *lv_mem_psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, LV_MEM_PSRAM_CAPS);
    if (p == NULL) {
        p = heap_caps_malloc(size, LV_MEM_INTERNAL_CAPS);
    }
    return p;
}

static inline void lv_mem_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}

/** realloc 失败时原指针仍然有效，所以可以安全地再试一次内部 RAM */
static inline void *lv_mem_psram_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, LV_MEM_PSRAM_CAPS);
    if (p == NULL) {
        p = heap_caps_realloc(ptr, size, LV_MEM_INTERNAL_CAPS);
    }
    return p;
}

#endif /* __LV_MEM_PSRAM_H__ */