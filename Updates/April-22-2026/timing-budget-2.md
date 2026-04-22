# Timing Budget V2: Adjusted to implementation

## NEW Frame (10 bytes)

``` txt
+--------------------------------------------------------+---------+------+
| 0xAA | 0xAA | 0xAA | 0xXX  | 0xXX | 0xXX | 0xXX | 0xXX | (n)0xXX | 0xXX |
+--------------------+-------+---------------------------+---------+------+
| FRAME SEPARATOR    | FInfo | TIMESTAM in miliseconds   | PAYLOAD | CRC  |
+--------------------+-------+---------------------------+---------+------+
```

Minimum Frame size (payload = 0 bytes): 10 bytes

## Payloads

| SENSOR | Payload size | Frame Size | Frequency |
|-|-|-|-|
| GPS            | 60 | 70 | 1        |
| INA219         | 4  | 14 | 10       |
| MPU6050 + Temp | 14 | 24 | min: 100 |
| BME680         | 16(hum)+24(temp)+24(press) | 74 | min:25,max:50|

Production Data Rate = 70(1) + 14(10) + 24(100)+74(50) = 6310 bytes / second

Writing Data Rate = 45000 bytes/second

Giving this i can even rise the MPU6050 sampling rate, which is the ultimate goal:

Production data rate = 45000 bytes/second = 70(1) + 14(10) + 24(MPU6050_SAMPLING_RATE)+74(50)

MPU6050_SAMPLING_RATE = (45000  - 70 - 140  - 74(50)) / 24 = 1712 Hz

However, 1.7kHz as sampling rate is too extreme and it would be operating in the edge. Considering the mPU6050 has internal an internal low pass filter of 260 and 256 Hz for the accelerometer and the gyroscope, respectively. A recommended sampling rate starts at 520Hz up to around 1000 Hz. The higher the better. This will avoid aliasing.
