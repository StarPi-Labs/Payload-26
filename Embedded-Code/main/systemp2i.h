#ifndef _SYSTEM_P2I_H_
#define _SYSTEM_P2I_H_

/* SYSTEM PAYLOAD-PI */
#include "driver/i2c_master.h"
#include "health_monitoring.h"
#include "frame_logger.h"

#define I2C0_HEALTH         15
#define I2C1_HEALTH         14
#define GPS_UART_HEALTH     13
#define LORA_UART_HEALTH    12
#define SD_HEALTH           11

#define MPU6050_HEALTH      7
#define BME680_HEALTH       6
#define INA219_HEALTH       5
#define GPS_HEALTH          4
#define LORA_HEALTH         3
#define FILESYSTEM_HEALTH   2
#define BATTERY_HEALTH      1
#define SENSORS_HEALTH      ((1 << MPU6050_HEALTH) | (1 << BME680_HEALTH) | (1 << INA219_HEALTH) | (1 << GPS_HEALTH))

typedef struct {
    uint32_t lauch_timestamp;
    uint16_t flight_state;
    uint16_t boot_count;
} FlightRecord;

struct Interfaces {
    i2c_master_bus_handle_t mpu6050;
    i2c_master_bus_handle_t bme_ina;

};


struct SysContext {
    volatile uint32_t mode;
    volatile uint32_t events;
    portMUX_TYPE events_guard;   // Protects the events
    SemaphoreHandle_t manager_up;   // Wakes the System Manager
};

struct TaskParams {
    HMBuffer          *hm_buffer;
    LoggerBuffer      *log_buffer;
    struct SysContext *context;
};

typedef struct {
    struct Interfaces port;
    uint16_t health; /* LSB: Devices health
                      * MSB: Peripherals health*/
    struct SysContext context;
    LoggerBuffer *log_buffer;
    HMBuffer     *hm_buffer;
    FlightRecord *record;
} System;

/* System States */
// TODO: Im having seconds thoughts about SENSOR_CHECK, it might be a visual 
// activity rather than a state, i think
enum {
    MODE_INIT, MODE_POST, MODE_SENSOR_CHECK, MODE_ARMED, MODE_BOOST, MODE_COAST
};

/* System Events */
#define SHOCK_3G_DETECTED   (1 << 0)

#endif
