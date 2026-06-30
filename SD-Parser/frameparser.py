import serial
import struct 
import math
from datetime import datetime
import sys

# --- 1. PROTOCOL DEFINITION ---
# Header: 3  bytes (Frame Separator) + 1 byte (Mode) + 4 (Timestamp) = 8 bytes
HEADER_SIZE = 8
FOOTER_SIZE = 2 # CRC16
gps_time_fix = 0.0  # carries the GPS time: TODO: implement synchronization

# --- 2. INA219 CONFIGURATIONS ---
INA219_MAX_VOLT = 16.0
INA219_ADC_BITS = 2**12
INA219_SHUNT_OMH= 0.1

# PACKET TYPES:
PACKET_TYPE_MPU6050  = 0x01
PACKET_TYPE_BME680   = 0x02
PACKET_TYPE_GPS      = 0x04
PACKET_TYPE_INA219   = 0x08
PACKET_TYPE_SYSSTATE = 0x10
PACKET_TYPE_GAS      = 0x20    # BME680 gas resistance (own frame, 1 float)
PACKET_TYPE_RESERV1  = 0x40
PACKET_TYPE_RESERV2  = 0x80

# Flight modes — must match the firmware enum (systemp2i.h)
MODE_INIT, MODE_POST, MODE_SENSOR_CHECK, MODE_ARMED, MODE_BOOST, MODE_COAST = range(6)
current_mode = MODE_ARMED      # updated by SYSSTATE frames

# MPU accel scale is mode-dependent: +/-16g in BOOST, +/-2g in every other phase
MPU_ACCEL_SENS_2G  = 16384.0   # LSB/g at +/-2g
MPU_ACCEL_SENS_16G = 2048.0    # LSB/g at +/-16g

"""
NOTE:
- fmt:
    f: float (4 bytes)
    c: char (1 byte)
    s: string 
    h: int16 (2 bytes)
    d: int (8 bytes?)

- size: measure in bytes.
""" 
PACKET_DEFS = {
    PACKET_TYPE_MPU6050:  {'name': 'MPU6050',   'fmt': '<7h', 'size': 14},
    PACKET_TYPE_BME680:   {'name': 'BME680',    'fmt': '<3f', 'size': 12},
    PACKET_TYPE_GPS:      {'name': 'MQ10',      'fmt': '<c10s10s10s11sc12sc4s8s', 'size': 68}, 
    PACKET_TYPE_INA219:   {'name': 'INA219',    'fmt': '<2h', 'size': 4},
    PACKET_TYPE_SYSSTATE: {'name': 'SYSSTATE',  'fmt': '<B', 'size': 1},  # flight mode (1 byte)
    PACKET_TYPE_GAS:      {'name': 'GAS',       'fmt': '<f', 'size': 4},  # gas resistance [ohm]
    PACKET_TYPE_RESERV1:  {'name': 'RESERVED1', 'fmt': '<3f', 'size': 1}, # Reserved for future sensors
    PACKET_TYPE_RESERV2:  {'name': 'RESERVED2', 'fmt': '<3f', 'size': 1}, # Reserved for future sensors
}

# NOTE: acceleration sensity changes based on the flying state
mpu6050_accel_sensitivity = 2048.0
mpu6050_gyro_sensitivity = 65.5

def calculate_crc16(data: bytes) -> int:
    crc = 0xFFFF                        # Initialization Vector is the same in the embedded platform
    for d in data:
        crc = crc ^ (d << 8)
        for i in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
        crc &= 0xFFFF
    return crc

def proc_mpu6050(ddict, draw):
    """
    Processes data from MPU6050
    Input: 
    - ddict: dictionary to fill
    - draw: data raw in binary
    """

    accel_sens = MPU_ACCEL_SENS_16G if current_mode == MODE_BOOST else MPU_ACCEL_SENS_2G
    ddict['accelerationX'] = draw[0] / accel_sens
    ddict['accelerationY'] = draw[1] / accel_sens
    ddict['accelerationZ'] = draw[2] / accel_sens
    ddict['acceleration'] = math.sqrt(ddict['accelerationX']**2 + ddict['accelerationY']**2 + ddict['accelerationZ']**2)
    ddict['imu_temp'] = draw[3] / 340 + 36.53
    ddict['roll'] = draw[4] / mpu6050_gyro_sensitivity
    ddict['pitch'] = draw[5] / mpu6050_gyro_sensitivity
    ddict['yaw'] = draw[6] / mpu6050_gyro_sensitivity

def proc_ina219(ddict, draw):

    """
    Processes data from INA219, voltage sensor:
    Input:
    - ddict: dictionary to fill
    - draw: data raw in binary

    ddict:
    - shunt_mVolts: shunt voltage in milivolts
    - bus_volts: Bus voltage in volts
    - current: Bus current in amps
    """
    
    ddict['shunt_mVolts'] = draw[0] / 100.0
    ddict['bus_volts'] = draw[1] * INA219_MAX_VOLT / INA219_ADC_BITS
    ddict['current'] = ddict['shunt_mVolts'] / INA219_SHUNT_OMH
        
def proc_bme680(ddict, draw):
    """BME680 T/P/H: 3 compensated floats -> temperature [C], pressure [kPa], humidity [%RH]."""
    ddict['temperature'] = draw[0]
    ddict['pressure']    = draw[1] / 1000.0   # firmware sends Pa; dashboard wants kPa
    ddict['humidity']    = draw[2]

def proc_gas(ddict, draw):
    """BME680 gas channel: 1 float -> gas resistance [ohm]."""
    ddict['gas_resistance'] = draw[0]

def proc_sysstate(ddict, draw):
    """SYSSTATE: 1 byte = current flight mode. Tracked so the MPU accel scale is right."""
    global current_mode
    current_mode = draw[0]
    ddict['mode'] = draw[0]

def proc_gps(ddict, draw):
    """
    - ddict: dictionary to fill
    - draw: data raw in binary
    """
    try:
        ddict['gpsLock']  = draw[0].decode('ascii').strip('\x00')
        if ddict['gpsLock'] == 'V':
            return

        # Horizontal Velicity from Knots to km/h
        nmea_field_str = draw[1].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['horizontalVelocity'] = float(nmea_field_str)
            ddict['horizontalVelocity'] *= 1.852
        
        # Course of Movement
        nmea_field_str = draw[2].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['course'] = float(nmea_field_str)

        # GPS time "HHMMSS.ss" to Unix Timestamp 
        nmea_field_str = draw[3].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            dt = datetime.strptime(nmea_field_str, "%H%M%S.%f")
            dt_now = datetime.now()
            dt = dt.replace(year=dt_now.year, month=dt_now.month, day=dt_now.day)
            ddict['time'] = dt.timestamp()

        # GPS Latitude "DDMM.mmmmm" to float
        nmea_field_str = draw[4].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['gpsLat'] = float(nmea_field_str[:2]) + float(nmea_field_str[2:]) / 60

        nmea_field_str = draw[5].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['gpsLat_N'] = nmea_field_str

        # GPS Longitud "DDDMM.mmmmm" to float
        nmea_field_str   = draw[6].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['gpsLon'] = float(nmea_field_str[:3]) + float(nmea_field_str[3:]) / 60
    
        nmea_field_str = draw[7].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['gpsLon_W'] = nmea_field_str

        # GPS Satellite Counts
        nmea_field_str = draw[8].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['sat_count']= int(nmea_field_str)

        # GPS Altitude 
        nmea_field_str = draw[9].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['gpsAlt'] = float(nmea_field_str)

    except UnicodeDecodeError:
        ddict['frame-status'] = '0'
        print(f"[{timestamp_ms} ms] GPS string corruption detected. Skipping payload.")


def parse_frame_stream_bin(buffer):
    data = []
    while True:
        sync_idx = buffer.find(b'\xaa\xaa\xaa')

        if sync_idx == -1:
            buffer = buffer[-2:] if len(buffer) >= 2 else buffer
            return buffer, data

        if sync_idx > 0:
            buffer = buffer[sync_idx:]
        
        if len(buffer) < HEADER_SIZE:
            return buffer, data
        
        packet_type = buffer[3]

        if packet_type not in PACKET_DEFS:
            print(f"Packet Type: {packet_type} | buf: {buffer}")
            buffer.pop(0)
            continue 

        payload_sz = PACKET_DEFS[packet_type]['size']
        total_packet_sz = HEADER_SIZE + payload_sz + FOOTER_SIZE

        if len(buffer) < total_packet_sz:
            return buffer, data

        packet_data = buffer[:total_packet_sz]
        calc_crc = calculate_crc16(packet_data[:-2])
        expected_crc = struct.unpack('<H', packet_data[-2:])[0]

        # CRC-CALC
        #if calc_crc != expected_crc:
        #    print(f"[CRC Failed @ Packet Type: {packet_type}:]"
        #          f"\n\tbuffer: {buffer}"
        #          f"\n\tpacket: {packet_data[:-2]}"
        #          f"\n\tcalc CRC: {calc_crc}"
        #          f"\n\texpect CRC: {expected_crc}")
        #    buffer.pop(0)
        #    continue

        data.append({})
        data[-1]['frame-status'] = '1'
        timestamp_ms = struct.unpack('<I', packet_data[4:8])[0] 

        data[-1]['sys-timestamp_ms'] = timestamp_ms

        payload_bytes = packet_data[8:-2]
        payload_tuple = struct.unpack(
            PACKET_DEFS[packet_type]['fmt'], 
            payload_bytes
        )
        
        if packet_type == PACKET_TYPE_GPS: # GPS (ASCII)
            # print(f"[{timestamp_ms} ms] {PACKET_DEFS[packet_type]['name']} Data: {payload_tuple}")
            proc_gps(data[-1], payload_tuple)
            sys.stderr.write("DEPRECATED: consider this is temporal patch because in the embedded system file, one extra byte is transferred.\n")
            total_packet_sz += 1


        elif packet_type == PACKET_TYPE_INA219: # INA216, voltage sensor
            proc_ina219(data[-1], payload_tuple)

        elif packet_type == PACKET_TYPE_MPU6050: # MPU6050, IMU sensor
            # print(f"[{timestamp_ms} ms] {PACKET_DEFS[packet_type]['name']} Data: {payload_tuple}")
            proc_mpu6050(data[-1], payload_tuple)

        elif packet_type == PACKET_TYPE_BME680: # BME680 T/P/H (compensated)
            proc_bme680(data[-1], payload_tuple)

        elif packet_type == PACKET_TYPE_GAS: # BME680 gas resistance
            proc_gas(data[-1], payload_tuple)

        elif packet_type == PACKET_TYPE_SYSSTATE: # flight-mode marker
            proc_sysstate(data[-1], payload_tuple)

        else:
           print(f"[{timestamp_ms} ms] {PACKET_DEFS[packet_type]['name']} Data: {payload_tuple}")

        buffer = buffer[total_packet_sz:]

if __name__ == "__main__":

    remainder = b''
    ser = serial.Serial('/dev/rfcomm0', 
        115200, 
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        xonxoff=False,        # CRITICAL: Disable software flow control
        rtscts=False,         # CRITICAL: Disable hardware flow control
        dsrdtr=False          # CRITICAL: Disable hardware flow control
    )
    #ser = serial.Serial('/dev/ttyUSB0', 115200, timeout = 1100)
    ser.reset_input_buffer()
    
    while True:
        available = ser.read(28)
        available = remainder + available
        remainder, _ = parse_frame_stream_bin(available)
