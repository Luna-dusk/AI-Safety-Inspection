# ESP-IDF 不会自动编译 main/ 下的文件，每个 .c/.cpp 都必须显式列在 SRCS 里。
# 以后新增源文件，把文件名加到这个列表即可。
#
# 改动（相对上一版）：
#   1. 去掉 "Mpu6050_reg.h" / "QMC5883_reg.h" —— 头文件不能列进 SRCS，
#      列进去 CMake 会把它当源文件交给编译器，直接报语法错；
#   2. 补 esp_driver_i2c（新驱动 driver/i2c_master.h）与 esp_timer（实测 dt）。
idf_component_register(SRCS "hello_world_main.c"
                            "Mpu6050.cpp"
                            "QMC5883.cpp"
                    PRIV_REQUIRES spi_flash driver esp_driver_i2c esp_timer
                    INCLUDE_DIRS ".")
