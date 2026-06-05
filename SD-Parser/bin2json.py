import frameparser
import argparse
import json

def hardware_handler(src_stream_port, output_file):
    armed_file = None

    try:
        src_stream = open(src_stream_port, "rb")

    except:
        print(f"Stream type {src_stream_port} not supported.")
        return

    remainder = b''
    timestamp_ms = -1

    while True:
      try:
        available = src_stream.read(28)
        if available is None:
            break

        if armed_file is not None:
            armed_file.write(available)

        available = remainder + available
        remainder, new_data = frameparser.parse_frame_stream_bin(available)
        for packet in new_data:
            # TODO: create json
            if "pitch" in new_data[0]:
                print(new_data[0]['sys-timestamp_ms'], new_data[0]['acceleration'])
            #if "shunt_mVolts" in packet:
            #    print(packet['sys-timestamp_ms'], packet['shunt_mVolts'])
            #if "gpsLock" in new_data[0]:
            #    print(new_data[0]['sys-timestamp_ms'], new_data[0]['gpsAlt'])

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
args.add_argument("--output", default="/tmp/flight.json")

args = args.parse_args()

hardware_handler(args.source, args.output)

