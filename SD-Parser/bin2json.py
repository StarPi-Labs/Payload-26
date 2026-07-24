import frameparser
import argparse
import json
import sys

MAX_READ_SZ = 160

def hardware_handler(src_stream_port, output_file, sample_sz):

    try:
        src = open(src_stream_port, "rb")

    except:
        print(f"Stream type {src_stream_port} not supported.")
        return

    remainder = b''
    packets = []
    counter = 0

    while True:
      try:
        available = src.read(MAX_READ_SZ)
        if len(available) == 0:
            break

        available = remainder + available
        remainder, new_data = frameparser.parse_frame_stream_bin(available)
        packets.extend(new_data)

        if sample_sz > 0:
            counter += 1
            if counter == sample_sz:
                break

        # DEBUGGING: Allows us to keep track of the timing of the sensors
        #for packet in new_data:
        #    #if "pitch" in packet:
        #    #    print(packet['sys-timestamp_ms'], packet['acceleration'])
        #    #if "shunt_mVolts" in packet:
        #    #    print(packet['sys-timestamp_ms'], packet['shunt_mVolts'])
        #    if "gpsLock" in packet:
        #        print(packet['sys-timestamp_ms'], packet['gpsAlt'])
        # END Debugging

      except Exception as e:
        print(f"\n[!] Frame parser choked! Error: {e}")
        print(f"[!] Bad data chunk: {available}")
        remainder = b''
        continue

    # Pretty-printed (one field per line), not a single giant minified line.
    with open(output_file, "w") as dst:
        json.dump(packets, dst, indent=2)

    print(f"Closing files... ({len(packets)} packets written)")
    src.close()

#-- User Input --#
args = argparse.ArgumentParser()

args.add_argument("--source", 
                  default="/dev/rfcomm0")

args.add_argument("--output", 
                  default="/tmp/flight.json")

args.add_argument("--packet-length", 
                  default=-1, 
                  type=int, 
                  help="Number of bytes to read: "
                       "READ_SZ = MAX_READ_SZ * packet_length, "
                       f"MAX_READ_SZ = {MAX_READ_SZ}, "
                       "default value is -1 and it parses the entire file.")

args = args.parse_args()

sys.stderr.write(f"outfile: {args.output}\n")

hardware_handler(args.source, args.output, args.packet_length)

