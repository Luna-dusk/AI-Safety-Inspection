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
                "PWR"
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
                "PWR"
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
                esp_lcd               # ST7789 SPI 面板驱动（LCD/lcd_ui.c）
                lvgl__lvgl            # LVGL 8.3（main/idf_component.yml 里 ~8.3.0）
                )

# 配网页 portal.html 内嵌进固件（wifi_provision.c 引用 _binary_portal_html_*）
idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires}
                       EMBED_TXTFILES "WIFI/portal.html"
                       EMBED_FILES "I2S/spk_test.pcm")
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
