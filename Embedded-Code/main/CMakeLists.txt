set(srcs "main.c")

if(CONFIG_ENABLE_I2C_BUS)
    list(APPEND srcs "i2c_bus.c")
endif()

if(CONFIG_ENABLE_MPU6050)
    list(APPEND srcs "mpu6050.c")
endif()

if(CONFIG_ENABLE_BME680)
    list(APPEND srcs "bme680.c")
endif()

if(CONFIG_ENABLE_INA219)
    list(APPEND srcs "ina219.c")
endif()

if(CONFIG_ENABLE_GPS)
    list(APPEND srcs "gps_sensor.c" "uart.c" "nmea.c")
endif()

# Frame logger is always compiled (frame building types needed)
# SD card functions are no-ops when SD is disabled
# telemetry_net.c self-stubs when CONFIG_ENABLE_TELEMETRY_NET is off, so callers
# never need an #ifdef
list(APPEND srcs "frame_logger.c" "health_monitoring.c" "bt_serial_bridge.c" "sdif.c" "flight_stats.c" "flight_state.c" "status_led.c" "telemetry_net.c" "telemetry_espnow.c")

idf_component_register(
    SRCS ${srcs}
    # ../../P4-Board/common holds pilog_link.h, shared verbatim with the ground
    # station firmwares so the channel and message IDs cannot drift apart.
    INCLUDE_DIRS "." "../../P4-Board/common"
    REQUIRES bt nvs_flash esp_driver_uart esp_driver_i2c esp_driver_gpio esp_driver_rmt fatfs
             esp_wifi esp_netif esp_event esp_timer lwip
)
