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
    list(APPEND srcs "gps_sensor.c" "uart.c")
endif()

# Frame logger is always compiled (frame building types needed)
# SD card functions are no-ops when SD is disabled
list(APPEND srcs "frame_logger.c" "health_monitoring.c")

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ".")
