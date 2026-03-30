import serial

import struct 

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

def parse_frame_stream_bin(buffer):
    while True:
        sync_idx = buffer.find(b'\xaa\xaa\xaa')

        if sync_idx == -1:
            buffer = buffer[-2:] if len(buffer) >= 2 else buffer
            return buffer

        if sync_idx > 0:
            buffer = buffer[sync_idx:]

        if len(buffer) < HEADER_SIZE:
            return buffer
        
        packet_type = buffer[3]

        if packet_type not in PACKET_DEFS:
            buffer.pop(0)
            continue 

        payload_sz = PACKET_DEFS[packet_type]['size']
        total_packet_sz = HEADER_SIZE + payload_sz + FOOTER_SIZE

        if len(buffer) < total_packet_sz:
            return buffer

        packet_data = buffer[:total_packet_sz]
        calc_crc = calculate_crc16(packet_data[:-2])
        expected_crc = struct.unpack('<H', packet_data[-2:])[0]


        """ # CRC-CALC
        if calc_crc != expected_crc:
            print("CRC Failed")
            buffer.pop(0)
            continue
        """

        timestamp_ms = struct.unpack('<I', packet_data[4:8])[0]
        payload_bytes = packet_data[8:-2]
        payload_tuple = struct.unpack(
            PACKET_DEFS[packet_type]['fmt'], 
            payload_bytes
        )
 
        if packet_type == 0x04: # GPS (ASCII)
            try:
                gps_data = {
                    'status':     payload_tuple[0].decode('ascii').strip('\x00'),
                    'speed':      payload_tuple[1].decode('ascii').strip('\x00'),
                    'course':     payload_tuple[2].decode('ascii').strip('\x00'),
                    'time':       payload_tuple[3].decode('ascii').strip('\x00'),
                    'lat':        payload_tuple[4].decode('ascii').strip('\x00'),
                    'lat_orient': payload_tuple[5].decode('ascii').strip('\x00'),
                    'lon':        payload_tuple[6].decode('ascii').strip('\x00'),
                    'lon_orient': payload_tuple[7].decode('ascii').strip('\x00'),
                    'sat_count':  payload_tuple[8].decode('ascii').strip('\x00')
                }

                print(
                    f"[{timestamp_ms} ms] GPS: "
                    f"{gps_data['time']}, "
                    f"{gps_data['lat']} {gps_data['lat_orient']}, "
                    f"{gps_data['lon']} {gps_data['lon_orient']} | "
                    f"Sats: {gps_data['sat_count']}"
                )

            except UnicodeDecodeError:
                print(f"[{timestamp_ms} ms] GPS string corruption detected. Skipping payload.")

        else:
           print(f"[{timestamp_ms} ms] {PACKET_DEFS[packet_type]['name']} Data: {payload_tuple}")

        buffer = buffer[total_packet_sz:]

def dummy_stream():
    # A perfectly packed IMU frame (Sync, Type, Timestamp, X, Y, Z, CRC)
    yield b'\xaa\xaa\xaa\x01\xe8\x03\x00\x00' + struct.pack('<3f', 1.0, 2.5, 9.8) + b'\xff\xff'

if __name__ == "__main__":

    remainder = b''
    ser = serial.Serial('/dev/rfcomm0', 115200, timeout = 1100)
    #ser = serial.Serial('/dev/ttyUSB0', 115200, timeout = 1100)
    ser.reset_input_buffer()

    while True:
        if ser.in_waiting > 0:
            available = ser.read(ser.in_waiting)
            available = remainder + available
            print(available)
            remainder = parse_frame_stream_bin(available)
                
    #parse_frame_stream(dummy_stream())

