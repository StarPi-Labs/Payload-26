import serial
import struct 
from datetime import datetime

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
PACKET_TYPE_MQ10     = 0x04
PACKET_TYPE_INA219   = 0x08
PACKET_TYPE_SYSSTATE = 0x10
PACKET_TYPE_RESERV0  = 0x20
PACKET_TYPE_RESERV1  = 0x40
PACKET_TYPE_RESERV2  = 0x80

PACKET_DEFS = {
    PACKET_TYPE_MPU6050:  {'name': 'MPU6050',   'fmt': '<3f', 'size': 12},
    PACKET_TYPE_BME680:   {'name': 'BME680',    'fmt': '<3f', 'size': 12},
    PACKET_TYPE_MQ10:     {'name': 'MQ10',      'fmt': '<c10s10s10s11sc12sc4s8s', 'size': 68}, 
    PACKET_TYPE_INA219:   {'name': 'INA219',    'fmt': '<2h', 'size': 4},
    PACKET_TYPE_SYSSTATE: {'name': 'SYSSTATE',  'fmt': '<d', 'size': 1}, # TODO: size? check it on the code
    PACKET_TYPE_RESERV0:  {'name': 'RESERVED0', 'fmt': '<3f', 'size': 1}, # Reserved for future sensors
    PACKET_TYPE_RESERV1:  {'name': 'RESERVED1', 'fmt': '<3f', 'size': 1}, # Reserved for future sensors
    PACKET_TYPE_RESERV2:  {'name': 'RESERVED2', 'fmt': '<3f', 'size': 1}, # Reserved for future sensors
}

def calculate_crc16(data: bytes) -> int:
    # TODO: implement this heheh
    return 0xFFFF

def proc_ina219(ddict, draw):

    """
    Processes data from the INA219 voltage and powere sensors:
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
            buffer.pop(0)
            continue 

        payload_sz = PACKET_DEFS[packet_type]['size']
        total_packet_sz = HEADER_SIZE + payload_sz + FOOTER_SIZE

        if len(buffer) < total_packet_sz:
            return buffer, data

        packet_data = buffer[:total_packet_sz]
        calc_crc = calculate_crc16(packet_data[:-2])
        expected_crc = struct.unpack('<H', packet_data[-2:])[0]

        """ # TODO: CRC-CALC
        if calc_crc != expected_crc:
            print("CRC Failed")
            buffer.pop(0)
            continue
        """

        data.append({})
        data[-1]['frame-status'] = '1'
        timestamp_ms = struct.unpack('<I', packet_data[4:8])[0] 

        data[-1]['sys-timestamp_ms'] = timestamp_ms

        payload_bytes = packet_data[8:-2]
        payload_tuple = struct.unpack(
            PACKET_DEFS[packet_type]['fmt'], 
            payload_bytes
        )
        
        if packet_type == PACKET_TYPE_MQ10: # GPS (ASCII)
            print(f"[{timestamp_ms} ms] {PACKET_DEFS[packet_type]['name']} Data: {payload_tuple}")
            proc_gps(data[-1], payload_tuple)

        elif packet_type == PACKET_TYPE_INA219: # INA216, voltage sensor
            proc_ina219(data[-1], payload_tuple)

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
