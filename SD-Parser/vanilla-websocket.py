import socket, hashlib, base64, serial
import time
import struct
from concurrent.futures import ThreadPoolExecutor
import frameparser
import threading
import select
import argparse

server_active = True
updated_data = []
data_guard = threading.Lock()

RENDER_FPS = 60 # This can be adjusted based on the screen refresh rate, e.g.:
                # - 15Hz
                # - 30Hz (below 30 for performance)
                # - 60Hz
                # - 120Hz
                # - 240Hz

SCHEMA = {'time' :              'd',
          'sys-timestamp_ms':   'I',
          'frame-status':       'c',
          'altitude':           'f',
          'velocity':           'f',
          'horizontalVelocity': 'f',
          'acceleration':       'f',
          'accelerationX':      'f',
          'accelerationY':      'f',
          'accelerationZ':      'f',
          'imu_temp':           'f',
          'temperature':        'f',
          'pressure':           'f',
          'humidity':           'f',
          'gpsLat':             'f',
          'gpsLat_N':           'c',
          'gpsLon':             'f',
          'gpsLon_W':           'c',
          'gpsLock':            'c',
          'gpsAlt':             'f',
          'pitch':              'f',
          'roll':               'f',
          'yaw':                'f',
          'shunt_mVolts':       'f',
          'bus_volts':          'f',
          'current':            'f'
    }

def empty_frame():
    data_dict = {}
    for field in SCHEMA:
        data_dict[field] = None

    data_dict['time'] = time.time()
    return data_dict

def pack_telemetry(data_dict):
    if len(SCHEMA) > 32:
        print("WARNING: maximum frame fields is 32.")
        return bytes([0x88, 0x02, 0x03, 0xe8])

    frame_info = 0
    format_str = '<I'
    values = []

    for i, field in enumerate(SCHEMA):
        val = data_dict.get(field)
        if val is None: continue

        fmt = SCHEMA[field]
        format_str += fmt
        frame_info |= (1 << i)

        if (fmt == 'c'):    # char need to be casted to binary
            val = val.encode('ascii')
        values.append(val)

    payload = struct.pack(format_str, frame_info, *values)
    payload_len = len(payload)

    if payload_len < 126:
        return bytes([0x82, payload_len]) + payload

    elif payload_len < 0xFFFF:
        return bytes([0x82, 0x7E]) + payload_len.to_bytes(2, byteorder='big') + payload
    else:
        print("WARNING: This needs to be implemented.")
        return bytes([0x88, 0x02, 0x03, 0xe8])


#-- Websocket --#
def nap_for(ts, fps):
    # Computing remaining time for 60 FPS
    ts = time.time() - ts
    ts = 1/fps - ts
    if ts > 0.0:
        time.sleep(ts)

def is_client_dead(ws):
    ready_to_read, _, _ = select.select([ws], [], [], 0)

    if ready_to_read:
        try:
            incoming = ws.recv(1024)
            if not incoming:
                print("Client disconnected (TCP FIN). Closing handler.")
                return True

            if len(incoming) > 0 and incoming[0] == 0x88:
                print("Client sent WebSocket Close frame.")
                return True

        except BlockingIOError:
            return False

        except Exception as e:
            print(f"Connection lost: {e}")
            return True

    return False


def ws_handler(ws, res, header):
    global server_active
    global updated_data

    # TODO: open port device
    dev_key = res.split("?dev=")[1]

    #-- Switching Protocol --#
    key = header.split('Sec-WebSocket-Key: ')[1].split('\r\n')[0]
    guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    accept = base64.b64encode(hashlib.sha1((key + guid).encode()).digest()).decode()

    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )

    ws.send(response.encode())

    #-- Sending Packages --#
    ws.setblocking(False)
    while server_active:
        ts = time.time()

        # Check Clients status 
        if is_client_dead(ws):
            break

        # Send data
        with data_guard:
            data = updated_data
            updated_data = None

        if data is None:
            nap_for(ts, RENDER_FPS)
            continue
        #else:

        for d in data:
            packet = pack_telemetry(d)
            try: ws.send(packet)
            except: break

        nap_for(ts, RENDER_FPS)

    try:
        ws.send(bytes([0x88, 0x02, 0x03, 0xe8]))
    except:
        print("Websocket closed")

    ws.close()

#-- server functions --#

def static_resource(conn, filename, mime=None):

    content_type = 'Content-Type: '
    if filename.endswith(".html"): content_type += "text/html; charset=UTF-8\r\n"
    elif filename.endswith(".js"): content_type += "text/javascript\r\n"
    elif filename.endswith(".css"): content_type += "text/css\r\n"
    else:
        if mime is None:
            mime = "text; charset=UTF-8"
            print(f"[Warning:] Content-Type undefined, trying as '{mime}'.")
        content_type += f"{mime}\r\n"

    response = (
        "HTTP/1.1 200 OK\r\n"
        f"{content_type}"
        "Content-Lenght: {}\r\n"
        "\r\n"
        "{}"
    )

    with open(filename) as f:
        content = f.read()
        response = response.format(len(content), content)
        conn.send(response.encode())


def return_404(conn):

    content = "Wrong place, wrong time"
    response = (
        "HTTP/1.1 200 OK\r\n"
        f"Content-Lenght: {len(content)}\r\n"
        "\r\n"
        f"{content}"
    )

    conn.send(response.encode())


def client_handler(conn):
    data = conn.recv(1024).decode()
    resource = data.split(" ")[1].split(" HTTP")[0]

    if resource.startswith('/raw_ws'):
        ws_handler(conn, resource, data)

    elif resource.startswith('/index') or resource == '/':
        static_resource(conn, "index.html")

    elif resource == '/telemetry.js':
        static_resource(conn, "ws-frameparser.js")

    elif resource == '/telemetry-use.js':
        static_resource(conn, "vanilla-telemetry-example.js")

    else:
        print("resource requested", resource)
        return_404(conn)

    conn.close()

def emulate_data_sampling_rate(data_dict, onboard_time_ms, pc_time):
    new_timestamp_ms = data_dict.get('sys-timestamp_ms')
    if new_timestamp_ms is None:
        return onboard_time_ms, False

    new_timestamp_ms = new_timestamp_ms / 1000
    if onboard_time_ms < 0:
        return new_timestamp_ms, True

    ts = (new_timestamp_ms - onboard_time_ms) - (time.time() - pc_time)
    if ts > 0.0:
        time.sleep(ts)
    return new_timestamp_ms, False

def hardware_handler():
    global server_active
    global updated_data
    global src_stream_port
    global stream_type

    armed_file = None
    print("Stream Port", src_stream_port)
    print("stream_type", stream_type)
    if stream_type == 'serial':
        src_stream = serial.Serial(src_stream_port, 115200, timeout=None)
        src_stream.reset_input_buffer()
        armed_file = open("armed_file.bin","wb")

    elif stream_type == 'file':
        src_stream = open(src_stream_port, "rb")

    else:
        print(f"Stream type {src_stream_port} not supported.")
        return

    remainder = b''
    timestamp_ms = -1

    while server_active:
      try:
        ts = time.time()
        if stream_type == 'serial':
            available = src_stream.read(28)
        else:
            # NOTE: This needs to load larger numbers of data, the size of a page 32KB
            available = src_stream.read(28)
            if available is None:
                print("We are done")
                break

        if armed_file is not None:
            armed_file.write(available)

        available = remainder + available
        remainder, new_data = frameparser.parse_frame_stream_bin(available)

        if len(new_data) == 0:
            continue

        if stream_type == 'file': # Emulate data timing
            timestamp_ms, keep_going = emulate_data_sampling_rate(new_data[0], timestamp_ms, ts)
            if keep_going: continue

        with data_guard:
            updated_data = new_data

      except Exception as e:
        print(f"\n[!] Frame parser choked! Error: {e}")
        print(f"[!] Bad data chunk: {available}")
        remainder = b''
        continue

    print("Closing files...")
    src_stream.close()
    if armed_file is not None:
        armed_file.close()

#-- User Input --#
args = argparse.ArgumentParser()
args.add_argument("--source", default="/dev/rfcomm0")
args.add_argument("--type", default="serial")

args = args.parse_args()
src_stream_port = args.source
stream_type = args.type
if stream_type != 'serial' and stream_type != 'file':
    print(f"Stream source {stream_type} not supported. 'serial' or 'file'")
    exit(1)

#-- Mini SERVER --#
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('0.0.0.0', 5000))
server.listen(1)

MAX_ALLOCATED_THREADS = 10

print("Shadow Tool Online. Waiting for browser...")

with ThreadPoolExecutor(max_workers=MAX_ALLOCATED_THREADS) as pool:
    try:
        # Serial starts
        pool.submit(hardware_handler)

        # Server starts
        while server_active:
            client_conn, addr = server.accept()
            pool.submit(client_handler, client_conn)

    except KeyboardInterrupt:
        print("\nShutting down server...")
        server_active = False

    except Exception as e:
        print(f"\nError occurred due to {e}")

server.close()
