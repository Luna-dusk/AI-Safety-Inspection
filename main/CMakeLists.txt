# ESP-IDF 不会自动编译 main/ 下的文件，每个 .c/.cpp 都必须显式列在 SRCS 里。
# 以后新增源文件，把文件名加到这个列表即可。
idf_component_register(SRCS "hello_world_main.c"
                            "Mpu6050.cpp"
                            "Mpu6050_reg.h"
                            "QMC5883.cpp"
                            "QMC5883_reg.h"
                    PRIV_REQUIRES spi_flash driver
                    INCLUDE_DIRS ".")
