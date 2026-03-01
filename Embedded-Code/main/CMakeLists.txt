set(srcs "main.c")

if(CONFIG_ENABLE_I2C_BUS)
    list(APPEND srcs "i2c_bus.c")
endif()

if(CONFIG_ENABLE_MPU6050)
    list(APPEND srcs "mpu6050.c")
endif()

if(CONFIG_ENABLE_BME280)
    list(APPEND srcs "bme280.c")
endif()

if(CONFIG_ENABLE_INA219)
    list(APPEND srcs "ina219.c")
endif()

if(CONFIG_ENABLE_GPS)
    list(APPEND srcs "gps_sensor.c")
endif()

if(CONFIG_ENABLE_SD_CARD)
    list(APPEND srcs "sd_logger.c")
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ".")