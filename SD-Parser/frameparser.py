import serial
import struct 
from datetime import datetime

# --- 1. PROTOCOL DEFINITION ---
# Header: 3  bytes (Frame Separator) + 1 byte (Mode) + 4 (Timestamp) = 8 bytes
HEADER_SIZE = 8
FOOTER_SIZE = 2 # CRC16

PACKET_DEFS = {
    0x01: {'name': 'MPU6050',   'fmt': '<3f', 'size': 12},
    0x02: {'name': 'BME680',    'fmt': '<3f', 'size': 12},
    0x04: {'name': 'MQ10',      'fmt': '<c10s10s10s11sc12sc4s', 'size': 60}, 
    0x08: {'name': 'INA219',    'fmt': '<3f', 'size': 12}, # TODO: check it on the code
    0x10: {'name': 'SYSSTATE',  'fmt': '<d', 'size': 1}, # TODO: check it on the code
    0x20: {'name': 'RESERVED0', 'fmt': '<3f', 'size': 1}, # Reserved for future sensors
    0x40: {'name': 'RESERVED1', 'fmt': '<3f', 'size': 1}, # Reserved for future sensors
    0x80: {'name': 'RESERVED2', 'fmt': '<3f', 'size': 1}, # Reserved for future sensors
}

def calculate_crc16(data: bytes) -> int:
    return 0xFFFF

def parse_frame_stream(raw_data):
    buffer = bytearray()
    print(buffer)
    for chunk in raw_data:
        buffer.extend(chunk)
        while True:
            sync_idx = buffer.find(b'\xaa\xaa\xaa')

            if sync_idx == -1:
                buffer = buffer[-2] if len(buffer) >= 2 else buffer
                break

            if sync_idx > 0:
                buffer = buffer[sync_idx:]

            if len(buffer) < HEADER_SIZE:
                break;
            
            packet_type = buffer[3]

            if packet_type not in PACKET_DEFS:
                buffer.pop(0)
                continue 

            payload_sz = PACKET_DEFS[packet_type]['size']
            total_packet_sz = HEADER_SIZE + payload_sz + FOOTER_SIZE

            if len(buffer) < total_packet_sz:
                break

            packet_data = buffer[:total_packet_sz]
            calc_crc = calculate_crc16(packet_data[:-2])
            expected_crc = struct.unpack('<H', packet_data[-2:])[0]

            if calc_crc != expected_crc:
                print("CRC Failed")
                buffer.pop(0)
                continue

            timestamp_ms = struct.unpack('<I', packet_data[4:8])[0]
            payload_bytes = packet_data[8:-2]

            if packet_type == 0x02: # GPS (ASCII)
                # Decode the raw bytes directly back into a Python string!
                gps_string = payload_bytes.decode('ascii').strip()
                print(f"[{timestamp_ms}] GPS: {gps_string}")
            else:
                payload_tuple = struct.unpack(
                    PACKET_DEFS[packet_type]['fmt'], 
                    payload_bytes
                )
                print(f"[{timestamp_ms} ms] {PACKET_DEFS[packet_type]['name']} Data: {payload_tuple}")
            buffer = buffer[total_packet_sz:]

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

        nmea_field_str = draw[8].decode('ascii').strip('\x00')
        if len(nmea_field_str) > 0:
            ddict['sat_count']= int(nmea_field_str)

    except UnicodeDecodeError:
        ddict['frame-status'] = '0'
        print(f"[{timestamp_ms} ms] GPS string corruption detected. Skipping payload.")


gps_time_fix = 0.0
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
 
        if packet_type == 0x04: # GPS (ASCII)
            proc_gps(data[-1], payload_tuple)

        else:
           print(f"[{timestamp_ms} ms] {PACKET_DEFS[packet_type]['name']} Data: {payload_tuple}")

        buffer = buffer[total_packet_sz:]

if __name__ == "__main__":

    remainder = b''
    ser = serial.Serial('/dev/rfcomm0', 115200, timeout = 1100)
    #ser = serial.Serial('/dev/ttyUSB0', 115200, timeout = 1100)
    ser.reset_input_buffer()

    while True:
        if ser.in_waiting > 0:
            available = ser.read(ser.in_waiting)
            available = remainder + available
            remainder, _ = parse_frame_stream_bin(available)
