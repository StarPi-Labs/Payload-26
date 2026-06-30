# SD-PARSER

## Current support:
|MODULE|status|
|------|------|
|GPS   | ready|
|INA219 | ready |
|MPU6050 | ready |
|BME680 | ongoing... |

## Formating from binary to json

NOTE: The json file is over 10x larger than the binary file, for example: a 52M file can easily translate translate into more than 600MB.

```
python3 bin2json.py --source <path/to/bin/file> --output <path/to/json/file>
```

The json file is just the direct translation of the python dictionary in `vanilla-websocket.py`, variable `SCHEMA`:

``` json
[
  {
    "frame-status": "1", 
     "sys-timestamp-ms": 23378, 
     <sensor 0 information: ....>"
  }, 
  {
    "frame-status": "1",
    "sys-timestamp-ms": 24378,
    <sensor 1 information: ...>"
  },
  {
    ...
  },
  {}
]
```




## Running for health monitoring

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
