# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components/Middlewares)
add_compile_options(-fdiagnostics-color=always)

# ── LVGL 内存池改放 PSRAM（必须在 project() 之前，宏要对 lvgl__lvgl 组件生效）──
# 背景：LVGL 默认用 32KB 静态数组当内存池（.bss），等于开机就挖走内部 RAM 32KB。
# 这块板子上屏后内部 RAM 只剩 12KB（WiFi/lwIP/esp-sr/音频队列/TLS 还要抢），
# 而 PSRAM 还有 3.4MB。改成 heap_caps_malloc(MALLOC_CAP_SPIRAM) 后，
# 内部 RAM 多出 32KB，draw buffer/任务栈才放得下。
# 具体分配函数见 components/BSP/LCD/lv_mem_psram.h，配套 sdkconfig 的
# CONFIG_LV_MEM_CUSTOM=y + CONFIG_LV_MEM_CUSTOM_INCLUDE="lv_mem_psram.h"。
include_directories(${CMAKE_CURRENT_LIST_DIR}/components/BSP/LCD)
add_compile_definitions(
    LV_MEM_CUSTOM_ALLOC=lv_mem_psram_alloc
    LV_MEM_CUSTOM_FREE=lv_mem_psram_free
    LV_MEM_CUSTOM_REALLOC=lv_mem_psram_realloc)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(AI-Safety-Inspection)
