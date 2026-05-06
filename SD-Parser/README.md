# SD-PARSER

## Current support:
|MODULE|status|
|------|------|
|GPS   | ready|
|INA219 (current) | ready |
|MPU6050 | ongoing... |

1. (Ubuntu based) Run from file
``` bash
$ python3 vanilla-websocket.py --type file --source armed_file_gps.bin
```

​		it has an emulator of sampling rate, so the file is not dump entirely on the parser.

2. (Ubuntu based) run from serial

``` bash
$ python3 vanilla-websocket.py --type serial --source /dev/rfcomm0
```

## Observation:

In linux, the bluetooth has to be bound to a "/dev" directory that acts as serial port, run this:

``` bash
$ sudo rfcomm bind 0 <ESP32 MAC ADDRESS>
```
