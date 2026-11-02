set(src_dirs
                "LED_Driver"
                "I2S"
                "WIFI")


set(include_dirs
                "LED_Driver"
                "I2S"
                "WIFI")

set(requires
                driver
                esp_wifi
                esp_event
                nvs_flash
                esp_netif
                esp_http_server)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
