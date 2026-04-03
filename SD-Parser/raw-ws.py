import socket, hashlib, base64, threading, serial
import time
import signal
import json
import struct

# 1. THE HANDWARE PIPE
# ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.01)


ws_active = True
server_active = True
"""
def signal_handler(sig, frame):
    ws_active = False
"""
FIELDS = ['time',
          'altitude',
          'velocity',
          'horizontalVelocity',
          'acceleration',
          'accelerationX',
          'accelerationY',
          'accelerationZ',
          'temperature',
          'pressure',
          'humidity',
          'gpsLat',
          'gpsLon',
          'gpsLock',
          'pitch',
          'roll',
          'yaw']

def pack_telemetry(data_dict):
    frame_info = 0
    format_str = '<I'
    values = []
    for i, field in enumerate(FIELDS):
        val = data_dict.get(field)
        if val is None: continue

        if field == 'time': format_str += 'd'
        else: format_str += 'f'

        frame_info |= (1 << i) 
        values.append(float(val))
                
    payload = struct.pack(format_str, frame_info, *values)

    payload_len = len(payload)
    if payload_len < 126:
        return bytes([0x82, payload_len]) + payload
    
    elif payload_len < 0xFFFF:
        return bytes([0x82, 0x7E]) + payload_len.to_bytes(2, byteorder='big') + payload
    else:
        print("WARNING: This needs to be implemented.")
        return bytes([0x88, 0x02, 0x03, 0xe8])


def empty_frame():
    return {
        'time': time.time(),
        'altitude': None,
        'velocity': None,
        'horizontalVelocity': None,
        'acceleration': None,
        'accelerationX': None,
        'accelerationY': None,
        'accelerationZ': None,
        'temperature': None,
        'pressure': None,
        'humidity': None,
        'gpsLat': None,
        'gpsLon': None,
        'pitch': None,
        'roll': None,
        'yaw': None
    }
 

def handle_client(conn):
    data = conn.recv(1024).decode()
    # TODO: open port device
    dev_key = data.split("/raw_ws?dev=")[1].split(" HTTP")

    key = data.split('Sec-WebSocket-Key: ')[1].split('\r\n')[0]
    guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    accept = base64.b64encode(hashlib.sha1((key + guid).encode()).digest()).decode()
    
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    conn.send(response.encode())

    while ws_active:
        payload = empty_frame()
        # TODO: fill payload with parsing
        packet = pack_telemetry(payload)
        conn.send(packet)

        """
        line = ser.readline()
        if line:
            # WebSocket Frame: [Final Bit + Text OpCode] [Length] [Payload]
            # For short strings (<126 bytes), first byte is 0x81
            payload = "lol"
            header = bytes([0x01, len(payload)])
            try:
                print(header+payload)
                conn.send(header + payload)
            except: break
            time.sleep(1)
        """


        time.sleep(0.016)

    conn.send(bytes([0x88, 0x02, 0x03, 0xe8]))
    conn.close()
    server_active = False


# 4. THE SERVER
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('0.0.0.0', 5000))
server.listen(1)

print("Shadow Tool Online. Waiting for browser...")
while server_active:
    client_conn, _ = server.accept()
    threading.Thread(target=handle_client, args=(client_conn,)).start()

client_conn.close()
