set(src_dirs
                "LED_Driver"
                "I2S"
                "WIFI"
                "AFE_SR"
                "KEY"
                "MultiNet"
                "LCD"
                "ESP_NOW"
                "IMU"
                "FALL_DET"
                "AI_CHAT/AI_FRAMES"
                "AI_CHAT/Transport"
                "AI_CHAT/CHAT")


set(include_dirs
                "LED_Driver"
                "I2S"
                "WIFI"
                "AFE_SR"
                "KEY"
                "MultiNet"
                "LCD"
                "ESP_NOW"
                "IMU"
                "FALL_DET"
                "AI_CHAT/AI_FRAMES"
                "AI_CHAT/Transport"
                "AI_CHAT/CHAT"
                "AI_CHAT" )

set(requires
                nvs_flash
                esp_netif
                esp_wifi
                esp_event
                esp_http_client
                esp_http_server
                lwip
                driver
                esp_timer
                freertos
                log
                json
                spiffs               # 兜底话术预录 PCM 存 storage 分区（A.8）
                mbedtls
                esp-sr
                esp_websocket_client  # v2 长连接传输（附录 B，AI_CHAT/Transport）
                # TODO(LCD): 以下两项等 LCD/ 目录拷过来、并在 main/idf_component.yml
                # 加一行 `lvgl/lvgl: ~8.3.0`（与源工程 ai_client 一致）之后再解开。
                # 现在 BSP 里既没有 LCD 源码也没有 lvgl 依赖，留着会直接让 CMake 解析失败。
                # esp_lcd
                # lvgl__lvgl
                )

# 配网页 portal.html 内嵌进固件（wifi_provision.c 引用 _binary_portal_html_*）
idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires}
                       EMBED_TXTFILES "WIFI/portal.html")
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
