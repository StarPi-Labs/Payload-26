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
INA219_SHUNT_OMH= 0.1          # default; overridden by the calibration frame

# PACKET TYPES:
PACKET_TYPE_MPU6050  = 0x01
PACKET_TYPE_BME680   = 0x02
PACKET_TYPE_GPS      = 0x04
PACKET_TYPE_INA219   = 0x08
PACKET_TYPE_SYSSTATE = 0x10
PACKET_TYPE_GAS      = 0x20    # BME680 raw gas registers (2 bytes)
PACKET_TYPE_CALIB    = 0x40    # calibration frame: raw constants for ground-side conversion
PACKET_TYPE_RESERV2  = 0x80

# Flight modes — must match the firmware enum (systemp2i.h)
MODE_INIT, MODE_POST, MODE_SENSOR_CHECK, MODE_ARMED, MODE_BOOST, MODE_COAST = range(6)
current_mode = MODE_ARMED      # updated by SYSSTATE frames
crc_fail_count = 0             # frames dropped by the CRC check

# MPU accel scale is mode-dependent: wide range in BOOST, fine range elsewhere.
# Defaults match the firmware (+/-16g boost, +/-2g else); the calibration frame overrides.
mpu_accel_sens_low  = 16384.0  # LSB/g outside BOOST (+/-2g)
mpu_accel_sens_high = 2048.0   # LSB/g in BOOST (+/-16g)
mpu6050_gyro_sensitivity = 65.5  # LSB/(deg/s) at +/-500 dps

"""
NOTE:
- fmt:
    f: float (4 bytes)
    c: char (1 byte)
    s: string
    h: int16 (2 bytes)
    B: uint8 (1 byte)
    x: pad byte (skipped)

- size: measure in bytes.
- ALL sensor payloads are RAW register values; the conversion to physical
  units happens here, using the constants from the calibration frame (0x40)
  that the firmware writes at the head of every log session / telemetry boot.
"""
PACKET_DEFS = {
    PACKET_TYPE_MPU6050:  {'name': 'MPU6050',   'fmt': '<7h', 'size': 14},
    PACKET_TYPE_BME680:   {'name': 'BME680',    'fmt': '<8B', 'size': 8},   # RAW press(3) temp(3) hum(2)
    # Firmware sends sizeof(struct GPSInfo) - 1 = 68 bytes (the struct's last
    # field, 'available', is bookkeeping-only and deliberately excluded from
    # the wire). No trailing pad byte -- there is nothing extra to consume.
    PACKET_TYPE_GPS:      {'name': 'MQ10',      'fmt': '<c10s10s10s11sc12sc4s8s', 'size': 68},
    PACKET_TYPE_INA219:   {'name': 'INA219',    'fmt': '<2h', 'size': 4},
    PACKET_TYPE_SYSSTATE: {'name': 'SYSSTATE',  'fmt': '<B', 'size': 1},    # flight mode (1 byte)
    PACKET_TYPE_GAS:      {'name': 'GAS',       'fmt': '<2B', 'size': 2},   # RAW gas_r_msb, gas_r_lsb
    PACKET_TYPE_CALIB:    {'name': 'CALIB',     'fmt': '<49s', 'size': 49}, # calib payload v1
    PACKET_TYPE_RESERV2:  {'name': 'RESERVED2', 'fmt': '<3f', 'size': 1},   # Reserved for future sensors
}

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

# --- 3. BME680 GROUND-SIDE COMPENSATION (Bosch float algorithms) ------------
# The firmware logs RAW registers; the calibration frame (0x40) carries the raw
# calib blob read from the sensor at power-up. Blob layout (40 B):
#   [0:23]  coeff array 1, registers 0x8A..0xA0
#   [23:37] coeff array 2, registers 0xE1..0xEE
#   [37:40] extra { res_heat_val(0x00), res_heat_range(0x02), range_sw_err(0x04) }
bme_cal = None        # dict, set once a calibration frame is seen
_bme_t_fine = 0.0     # carried from temperature comp into pressure comp
_K1_RANGE = [0,0,0,0,0,-1,0,-0.8,0,0,-0.2,-0.5,0,-1,0,0]
_K2_RANGE = [0,0,0,0,0.1,0.7,0,-0.8,-0.1,0,0,0,0,0,0,0]

def _s8(v):  return v - 256 if v > 127 else v
def _u16(msb, lsb): return (msb << 8) | lsb
def _s16(msb, lsb):
    v = (msb << 8) | lsb
    return v - 65536 if v > 32767 else v

def parse_bme680_calib(blob):
    cb1 = lambda reg: blob[reg - 0x8A]        # coeff array 1
    cb2 = lambda reg: blob[23 + (reg - 0xE1)] # coeff array 2
    return {
        't1': _u16(cb2(0xEA), cb2(0xE9)), 't2': _s16(cb1(0x8B), cb1(0x8A)), 't3': _s8(cb1(0x8C)),
        'p1': _u16(cb1(0x8F), cb1(0x8E)), 'p2': _s16(cb1(0x91), cb1(0x90)), 'p3': _s8(cb1(0x92)),
        'p4': _s16(cb1(0x95), cb1(0x94)), 'p5': _s16(cb1(0x97), cb1(0x96)),
        'p6': _s8(cb1(0x99)),  'p7': _s8(cb1(0x98)),
        'p8': _s16(cb1(0x9D), cb1(0x9C)), 'p9': _s16(cb1(0x9F), cb1(0x9E)), 'p10': cb1(0xA0),
        'h1': (cb2(0xE3) << 4) | (cb2(0xE2) & 0x0F),
        'h2': (cb2(0xE1) << 4) | (cb2(0xE2) >> 4),
        'h3': _s8(cb2(0xE4)), 'h4': _s8(cb2(0xE5)), 'h5': _s8(cb2(0xE6)),
        'h6': cb2(0xE7),      'h7': _s8(cb2(0xE8)),
        'range_sw_err': int(_s8(blob[39] & 0xF0) / 16),
    }

def _bme_comp_temp(adc_t):
    global _bme_t_fine
    var1 = ((adc_t / 16384.0) - (bme_cal['t1'] / 1024.0)) * bme_cal['t2']
    var2 = (((adc_t / 131072.0) - (bme_cal['t1'] / 8192.0)) ** 2) * bme_cal['t3'] * 16.0
    _bme_t_fine = var1 + var2
    return _bme_t_fine / 5120.0                       # degC

def _bme_comp_press(adc_p):
    var1 = (_bme_t_fine / 2.0) - 64000.0
    var2 = var1 * var1 * (bme_cal['p6'] / 131072.0)
    var2 = var2 + (var1 * bme_cal['p5'] * 2.0)
    var2 = (var2 / 4.0) + (bme_cal['p4'] * 65536.0)
    var1 = (((bme_cal['p3'] * var1 * var1) / 16384.0) + (bme_cal['p2'] * var1)) / 524288.0
    var1 = (1.0 + (var1 / 32768.0)) * bme_cal['p1']
    if var1 == 0.0:
        return 0.0
    p = 1048576.0 - adc_p
    p = ((p - (var2 / 4096.0)) * 6250.0) / var1
    var1 = (bme_cal['p9'] * p * p) / 2147483648.0
    var2 = p * (bme_cal['p8'] / 32768.0)
    var3 = ((p / 256.0) ** 3) * (bme_cal['p10'] / 131072.0)
    return p + (var1 + var2 + var3 + (bme_cal['p7'] * 128.0)) / 16.0   # Pa

def _bme_comp_hum(adc_h, temp_c):
    var1 = adc_h - ((bme_cal['h1'] * 16.0) + ((bme_cal['h3'] / 2.0) * temp_c))
    var2 = var1 * ((bme_cal['h2'] / 262144.0) *
                   (1.0 + ((bme_cal['h4'] / 16384.0) * temp_c) +
                          ((bme_cal['h5'] / 1048576.0) * temp_c * temp_c)))
    var3 = bme_cal['h6'] / 16384.0
    var4 = bme_cal['h7'] / 2097152.0
    h = var2 + ((var3 + (var4 * temp_c)) * var2 * var2)
    return max(0.0, min(100.0, h))                    # %RH

def _bme_comp_gas(gas_adc, gas_range):
    var1 = 1340.0 + (5.0 * bme_cal['range_sw_err'])
    var2 = var1 * (1.0 + _K1_RANGE[gas_range] / 100.0)
    var3 = 1.0 + (_K2_RANGE[gas_range] / 100.0)
    return 1.0 / (var3 * 0.000000125 * (1 << gas_range) *
                  (((gas_adc - 512.0) / var2) + 1.0))  # ohm

def proc_calib(ddict, blob):
    """CALIB v1 (49 B): { version u8, bme680_blob[40], accel_fs_low u16,
    accel_fs_high u16, gyro_fs u16, ina_shunt_mohm u16 } — updates all the
    conversion constants used by the other proc_* functions."""
    global bme_cal, mpu_accel_sens_low, mpu_accel_sens_high
    global mpu6050_gyro_sensitivity, INA219_SHUNT_OMH

    ddict['calib_version'] = blob[0]
    if blob[0] != 1:
        print(f"[!] Unknown calibration frame version {blob[0]} — ignored.")
        return

    bme_blob = blob[1:41]
    fs_low, fs_high, gyro_fs, shunt_mohm = struct.unpack('<4H', blob[41:49])

    if any(bme_blob):
        bme_cal = parse_bme680_calib(bme_blob)
    if fs_low:
        mpu_accel_sens_low = 32768.0 / fs_low
    if fs_high:
        mpu_accel_sens_high = 32768.0 / fs_high
    if gyro_fs:   # datasheet sensitivities; fall back to the exact ratio
        mpu6050_gyro_sensitivity = {250: 131.0, 500: 65.5,
                                    1000: 32.8, 2000: 16.4}.get(gyro_fs, 32768.0 / gyro_fs)
    if shunt_mohm:
        INA219_SHUNT_OMH = shunt_mohm / 1000.0

    ddict['mpu_accel_fs_g'] = [fs_low, fs_high]
    ddict['gyro_fs_dps']    = gyro_fs
    ddict['ina_shunt_mohm'] = shunt_mohm
    print(f"[CALIB] v1: BME680 cal {'OK' if bme_cal else 'MISSING'}, "
          f"accel fs {fs_low}/{fs_high} g, gyro {gyro_fs} dps, shunt {shunt_mohm} mohm")

def proc_mpu6050(ddict, draw):
    """
    Processes data from MPU6050
    Input:
    - ddict: dictionary to fill
    - draw: data raw in binary
    """

    accel_sens = mpu_accel_sens_high if current_mode == MODE_BOOST else mpu_accel_sens_low
    ddict['accelerationX'] = draw[0] / accel_sens
    ddict['accelerationY'] = draw[1] / accel_sens
    ddict['accelerationZ'] = draw[2] / accel_sens
    ddict['acceleration'] = math.sqrt(ddict['accelerationX']**2 + ddict['accelerationY']**2 + ddict['accelerationZ']**2)
    ddict['imu_temp'] = draw[3] / 333.87 + 21.0   # MPU6500 die (WHO_AM_I 0x70)
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
    """BME680 T/P/H: 8 RAW bytes (press[3] temp[3] hum[2], register order) ->
    temperature [C], pressure [kPa], humidity [%RH] via the calibration frame."""
    adc_p = (draw[0] << 12) | (draw[1] << 4) | (draw[2] >> 4)
    adc_t = (draw[3] << 12) | (draw[4] << 4) | (draw[5] >> 4)
    adc_h = (draw[6] << 8)  |  draw[7]
    if bme_cal is None:                       # no calibration frame seen (yet)
        ddict['bme_adc'] = [adc_t, adc_p, adc_h]
        return
    ddict['temperature'] = _bme_comp_temp(adc_t)              # also updates t_fine
    ddict['pressure']    = _bme_comp_press(adc_p) / 1000.0    # Pa -> kPa for the dashboard
    ddict['humidity']    = _bme_comp_hum(adc_h, ddict['temperature'])

def proc_gas(ddict, draw):
    """BME680 gas: 2 RAW bytes (gas_r_msb, gas_r_lsb) -> gas resistance [ohm]."""
    gas_adc   = (draw[0] << 2) | (draw[1] >> 6)
    gas_range =  draw[1] & 0x0F
    if bme_cal is None:
        ddict['gas_adc'] = [gas_adc, gas_range]
        return
    ddict['gas_resistance'] = _bme_comp_gas(gas_adc, gas_range)

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

    except (UnicodeDecodeError, ValueError):
        ddict['frame-status'] = '0'
        print("[!] GPS string corruption detected. Skipping payload.")


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
            buffer = buffer[1:]    # not a real frame (sync bytes inside data): resync
            continue

        payload_sz = PACKET_DEFS[packet_type]['size']
        total_packet_sz = HEADER_SIZE + payload_sz + FOOTER_SIZE

        if len(buffer) < total_packet_sz:
            return buffer, data

        packet_data = buffer[:total_packet_sz]
        calc_crc = calculate_crc16(packet_data[:-2])
        expected_crc = struct.unpack('<H', packet_data[-2:])[0]

        if calc_crc != expected_crc:
            # Corrupted frame (e.g. console text spliced into the stream, or a
            # capture cut mid-frame): drop one byte and hunt the next sync.
            global crc_fail_count
            crc_fail_count += 1
            if crc_fail_count == 1 or crc_fail_count % 50 == 0:
                print(f"[!] CRC fail #{crc_fail_count} "
                      f"(type 0x{packet_type:02X}) — frame dropped")
            buffer = buffer[1:]
            continue

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
            proc_gps(data[-1], payload_tuple)

        elif packet_type == PACKET_TYPE_CALIB: # calibration constants ('<49s' -> one bytes blob)
            proc_calib(data[-1], payload_tuple[0])

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
    import argparse
    import os

    ap = argparse.ArgumentParser(
        description="Parse Pi-LOG binary frames live from a serial port, or from "
                    "a recorded .bin file. Optionally save the raw byte stream "
                    "(the SD-card-equivalent capture) to a file with --dump.")
    ap.add_argument("--source", default="COM7",
                    help="serial port (Windows: COM7 / Linux: /dev/ttyUSB0) "
                         "or the path of a recorded .bin file")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--dump", default=None, metavar="FILE",
                    help="also write the raw bytes to FILE (SD-equivalent capture)")
    ap.add_argument("--quiet", action="store_true",
                    help="don't print each parsed packet (capture only)")
    args = ap.parse_args()

    from_file = os.path.isfile(args.source)
    if from_file:
        src = open(args.source, "rb")
    else:
        src = serial.Serial(None, args.baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1,          # short timeout -> responsive live printing
            xonxoff=False,        # CRITICAL: Disable software flow control
            rtscts=False,         # CRITICAL: Disable hardware flow control
            dsrdtr=False          # CRITICAL: Disable hardware flow control
        )
        src.port = args.source
        src.dtr = False   # keep DTR/RTS deasserted: on a DevKitC "UART" port
        src.rts = False   # they drive the EN/IO0 auto-reset circuit — asserting
                          # them while attaching could RESET the payload.
        src.open()
        src.reset_input_buffer()

    dump = open(args.dump, "wb") if args.dump else None
    remainder = b''
    n_packets = 0
    try:
        while True:
            available = src.read(4096)
            if not available:
                if from_file:
                    break                 # end of recorded file
                continue                  # serial: keep waiting
            if dump:
                dump.write(available)
                dump.flush()
            remainder, packets = parse_frame_stream_bin(remainder + available)
            n_packets += len(packets)
            if not args.quiet:
                for p in packets:
                    print(p)
    except KeyboardInterrupt:
        pass
    finally:
        src.close()
        if dump:
            dump.close()
            print(f"\n[+] raw stream saved to {args.dump}")
        print(f"[+] {n_packets} packets parsed")
