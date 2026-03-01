/**
 * @file acc.c
 * @brief DEPRECATED — MPU6050 driver has moved to mpu6050.c / mpu6050.h
 *
 * This file is kept for reference only.  The modular driver in mpu6050.c
 * implements proper ESP-IDF I2C master calls, WHO_AM_I verification,
 * and exposes a sensor_driver_t that main.c registers automatically
 * when CONFIG_ENABLE_MPU6050 is set in menuconfig.
 *
 * Original raw-data snippet preserved below for reference:
 */

#if 0  /* ── reference only, do not compile ── */

#define MPU6050_ADDR 0x68
#define PWR_MGMT_1   0x6B
#define ACCEL_XOUT_H 0x3B

// Wake up MPU6050
i2c_write_byte(MPU6050_ADDR, PWR_MGMT_1, 0x00);

// Read 14 bytes: accel (6), temp (2), gyro (6)
uint8_t data[14];
i2c_read_bytes(MPU6050_ADDR, ACCEL_XOUT_H, data, 14);

// Parse data
int16_t accel_x = (data[0] << 8) | data[1];
int16_t accel_y = (data[2] << 8) | data[3];
int16_t accel_z = (data[4] << 8) | data[5];
int16_t temp    = (data[6] << 8) | data[7];
int16_t gyro_x  = (data[8] << 8) | data[9];
int16_t gyro_y  = (data[10] << 8) | data[11];
int16_t gyro_z  = (data[12] << 8) | data[13];

#endif