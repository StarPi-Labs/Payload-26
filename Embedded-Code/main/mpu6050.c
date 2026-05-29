/**
 * @file mpu6050.c
 * @brief MPU-6050 accelerometer + gyroscope driver.
 *
 * Reads 14 bytes per sample:
 *   accel_x(2) accel_y(2) accel_z(2)  temp(2)  gyro_x(2) gyro_y(2) gyro_z(2)
 *
 */

#include "mpu6050.h"
#include "i2c_bus.h"
#include "esp_log.h"
#include "systemp2i.h"
#include "driver/gpio.h"

static const char *TAG = "MPU6050";

/* ── Register map ─────────────────────────────────────────── */
#define MPU6050_ADDR            0x68

#define REG_SMPLRT_DIV          0x19
#define REG_CONFIG              0x1A
#define REG_GYRO_CONFIG         0x1B
#define REG_ACCEL_CONFIG        0x1C
#define REG_INT_PIN_CFG         0x37
#define REG_ACCEL_XOUT_H        0x3B   /* Start of 14-byte burst read */
#define REG_PWR_MGMT_1          0x6B
#define REG_WHO_AM_I            0x75
#define VAL_WHO_AM_I            0x68

/* Configuration Values */
#define MPU6050_SAMPLERATE_1Khz 0x07    // - No recommended for FAT32 loggers
#define MPU6050_SAMPLERATE_80hz 0x63    // - I think on FAT32 logging, this can 
                                        //   go up to 100hz
#define MPU6050_LPF_260Hz       0x00    // - Recommended for high sampling rate (> 500hz)
#define MPU6050_LPF_21Hz        0x04    // - Recommended for 80Hz sampling rate 
#define MPU6050_GYRO_500deg     0x08    // - Ask Flight dynamics if they can tell the maximum rotation
#define MPU6050_ACCEL_16g       0x18    // - Wide dynamic range for boosting
#define MPU6050_ACCEL_2g        0x00    // - Narrow dynamic range for coasting (or ballistic)
#define MPU6050_INT_UP_50us     0x00    // - Interruption pin, active high, pull-up, pulse active for 50us.
#define MPU6050_INT_BIT_STATUS  0x01    // - Enable bit status

#define MPU6050_DATA_LEN        14     /* accel(6) + temp(2) + gyro(6) */

#define MPU6050_MAX_FAILED_ATTEMPTS 60

static i2c_master_dev_handle_t s_dev = NULL;

#define MPU6050_TIMEOUT_1000ms 1000

void _enable_interruption_locally(void) {
    if (ESP_OK != gpio_set_direction(MPU6050_INT_PIN, GPIO_MODE_INPUT)) {
        ESP_LOGE(TAG, "Couldn't set INT Pin as inpu");
        return;
    }
    if (ESP_OK != gpio_set_pull_mode(MPU6050_INT_PIN, GPIO_PULLUP_ONLY)) {
        ESP_LOGE(TAG, "Couldn't set with pullups.");
        return;
    }
    if (ESP_OK != gpio_set_intr_type(MPU6050_INT_PIN, GPIO_INTR_POSEDGE)) {
        ESP_LOGE(TAG, "Couldn't set interrup for positive edge.");
        return;
    }
    esp_err_t ret = gpio_install_isr_service(0);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "Couldn't install ISR: %s", esp_err_to_name(ret));
        return;
    }
}

esp_err_t mpu6050_config(void) {
    uint8_t cfg_payload[5];

    /* Wake up (clear SLEEP bit) and use internal 8 MHz oscillator */
    esp_err_t ret = i2c_bus_write_byte(s_dev, REG_PWR_MGMT_1, 0x01);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(100));

    // Configuration Registers
    // - REG25 (sampling rate): 0x07 (1Khz Sampling rate)
    // - REG26 (Low pass Filter): 0x00 (Acc filter 260Hz, Gyro filter 256Hz)
    // - REG27 (GYRO_CONFIG): 0x08 (500deg/s) do we need self test?
    // - REG28 (ACCEL_CONFIG): 0b00011000, do we need self test?
    // ...
    // - REG55 (INT_PIN_CFG):
    // - REG56 (INT_ENABLE):
    
    /* IMU Configuration */
    cfg_payload[0] = REG_SMPLRT_DIV;    // Starting register
    cfg_payload[1] = MPU6050_SAMPLERATE_80hz;
    cfg_payload[2] = MPU6050_LPF_21Hz;
    cfg_payload[3] = MPU6050_GYRO_500deg;
    cfg_payload[4] = MPU6050_ACCEL_2g;
                                    
    ret = i2c_master_transmit(s_dev, cfg_payload, sizeof(cfg_payload), MPU6050_TIMEOUT_1000ms);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "Something went wrong in stream configugation: %s.", esp_err_to_name(ret));
    }
    
    /* Interruption PIN configuration */
    cfg_payload[0] = REG_INT_PIN_CFG;
    cfg_payload[1] = MPU6050_INT_UP_50us;
    cfg_payload[2] = MPU6050_INT_BIT_STATUS;// Debug only, because we will 
                                            // never read this while fully operational
    ret = i2c_master_transmit(s_dev, cfg_payload, 3, MPU6050_TIMEOUT_1000ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Interrupt configuration went wrong %s.", esp_err_to_name(ret));
    }

    /* Start ESP32 interruption */
    _enable_interruption_locally();
    return ESP_OK;
}


/* ── Public API ───────────────────────────────────────────── */

esp_err_t mpu6050_init(i2c_master_bus_handle_t bus) {
    /* Bind Device to the i2c bus */
    esp_err_t ret = i2c_bind(bus, &s_dev, MPU6050_ADDR);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to bind MPU6050 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Ping device */
    uint8_t sensor_id = 0;
    ret = i2c_bus_read_bytes(s_dev, REG_WHO_AM_I, &sensor_id, 1);

    /* Was connection possible? */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect with sensor: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Is it the right sensor? */
    if (sensor_id != VAL_WHO_AM_I) {
        ESP_LOGE(TAG, "Wrong Sensor: Expected 0x%02X, got 0x%02X", VAL_WHO_AM_I, sensor_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    mpu6050_config();
    
    return ret;
}

union MPU6050Info {
    uint8_t raw[MPU6050_DATA_LEN];
    int16_t info[MPU6050_DATA_LEN / 2];
};

// TODO: we probably dont need this.
#include "esp_timer.h"

void mpu6050_raw2int(int16_t *dest, uint8_t *src) {
    dest[0] = (int16_t) ((src[0] << 8)  | src[1]);
    dest[1] = (int16_t) ((src[2] << 8)  | src[3]);
    dest[2] = (int16_t) ((src[4] << 8)  | src[5]);
    dest[3] = (int16_t) ((src[6] << 8)  | src[7]);
    dest[4] = (int16_t) ((src[8] << 8)  | src[9]);
    dest[5] = (int16_t) ((src[10] << 8) | src[11]);
    dest[6] = (int16_t) ((src[12] << 8) | src[13]);
}

esp_err_t i2c_safe_read(i2c_master_bus_handle_t *bus, uint8_t *dest, size_t sz) {
    esp_err_t ret;
    ret = i2c_bus_read_bytes(s_dev, REG_ACCEL_XOUT_H, dest, sz);

    if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_TIMEOUT) {
        // unbind devices from bus
        i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(*bus);

        gpio_set_direction(MPU6050_SCL, GPIO_MODE_OUTPUT_OD);
        for (int i = 0; i < 9; i++) {
            gpio_set_level(MPU6050_SCL, 1); esp_rom_delay_us(5);
            gpio_set_level(MPU6050_SDA, 0); esp_rom_delay_us(5);
        }

        // Restart i2c
        sys_i2c_init(MPU6050_I2C, MPU6050_SDA, MPU6050_SCL);
        mpu6050_init(*bus);

        return ESP_FAIL;
    }
    return ret;
}

void mpu6050_task(void *arg) {

    uint8_t mpu_raw[MPU6050_DATA_LEN];
    int16_t mpu_info[MPU6050_DATA_LEN / 2];
    struct TaskParams *tparams = (struct TaskParams *) arg;
    i2c_master_bus_handle_t *bus = (i2c_master_bus_handle_t *) tparams->args;
    esp_err_t ret;
    uint8_t attempts = 0;
    uint16_t telemetry_counter = 0;

    while(1) {
        xTaskNotifyStateClear(NULL);
        ulTaskNotifyTake(0, portMAX_DELAY);

        // ret = i2c_bus_read_bytes(s_dev, REG_ACCEL_XOUT_H, mpu_raw, MPU6050_DATA_LEN);
        ret = i2c_safe_read(bus, mpu_raw, MPU6050_DATA_LEN);

        if (ret != ESP_OK) {
            attempts++;
            vTaskDelay(pdMS_TO_TICKS(10)); // 10 ms breath
            if (MPU6050_MAX_FAILED_ATTEMPTS == attempts){
                // TODO:should I report this event? MPU6050 down?
                // TODO: yes, we should.
                break;
            }
            continue;
        } else {
            attempts = 0;
        }

        mpu6050_raw2int(mpu_info, mpu_raw);

        switch(tparams->context->mode) {
        case MODE_POST:
        case MODE_ARMED:
            telemetry_counter++;
            if (telemetry_counter >= MPU6050_HM_SKIP_SAMPLES) {
                telemetry_counter = 0;
                // DEBUGGING ONLY {
                // ESP_LOGI(TAG, "%d %d %d %d %d %d %d\n", mpu_info[0], mpu_info[1], mpu_info[2], mpu_info[3], mpu_info[4], mpu_info[5], mpu_info[6]);
                // }
 
                hm_send(
                    tparams->hm_buffer,
                    SBIT_MPU6050,
                    (uint8_t*) mpu_info,
                    MPU6050_DATA_LEN);
            }
            // TODO:  check acceleration larger than 3g to change state
            //
            break;
        case MODE_BOOST:
        case MODE_COAST:
            write_to_ring_buffer(
                tparams->log_buffer,
                SBIT_MPU6050,
                (uint8_t *)mpu_info,
                MPU6050_DATA_LEN);
            // TODO: Check when acceleration is below 2g to change mpu dynamic range
            // if (longitudinal_ACCEL < 1.5g) {
            // sp_err_t ret = i2c_bus_write_byte(s_dev, REG_ACCEL_CONFIG, MPU6050_ACCEL_2g);
            // }
            break;
        }
    }

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

}

static void IRAM_ATTR mpu6050_data_ready_int(void *arg) {
    TaskHandle_t *task_handle = (TaskHandle_t *) arg;
    vTaskNotifyGiveFromISR(*task_handle, 0);
}



void mpu6050_start_isr(TaskHandle_t *task_handle) {
    gpio_isr_handler_add(MPU6050_INT_PIN, mpu6050_data_ready_int, (void *)task_handle);
}



/* ── Driver descriptor ────────────────────────────────────── */

/*
const sensor_driver_t mpu6050_driver = {
    .name     = "MPU6050",
    .data_len = MPU6050_DATA_LEN,
    .init     = mpu6050_init,
    .read     = mpu6050_read,
};
*/
