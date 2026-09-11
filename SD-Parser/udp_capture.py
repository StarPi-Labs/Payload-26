#!/usr/bin/env python3
"""
udp_capture.py -- receive the Pi-LOG live telemetry broadcast over Wi-Fi and
write it to a .bin identical in format to an SD card capture.

The payload (CONFIG_ENABLE_TELEMETRY_NET) runs its own access point and
broadcasts the exact byte stream the SD logger writes, as UDP datagrams. This
script joins nothing itself -- connect your PC to the payload's Wi-Fi network
first (default SSID "PiLOG-Telemetry"), then run this.

The output file is byte-for-byte the same shape as an SD capture, so the rest
of the ground toolchain works on it unchanged:

    python udp_capture.py --output flight.bin
    python bin2json.py    --source flight.bin --output flight.json
    python json2telemetry.py --source flight.json

Live stats are printed as frames arrive, so you can confirm the link is healthy
before committing to a flight. Because the transport drops whole chunks when
saturated rather than stalling the payload, a few CRC failures under load are
expected and harmless -- the parser resyncs on the next 0xAA 0xAA 0xAA sync
word. A CRC failure *rate* that climbs with range is the number to watch.

Usage:
    python udp_capture.py
    python udp_capture.py --output bench.bin --port 5005
    python udp_capture.py --duration 60
"""
import argparse
import socket
import sys
import time
from datetime import datetime

import frameparser

DEFAULT_PORT = 5005


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output", default=None,
                    help="raw capture file (default: udp_capture_<timestamp>.bin)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"UDP broadcast port (default {DEFAULT_PORT}, must match "
                         "CONFIG_TELEMETRY_NET_PORT)")
    ap.add_argument("--duration", type=float, default=None,
                    help="stop after this many seconds (default: run until Ctrl-C)")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress the periodic stats line")
    args = ap.parse_args()

    out_path = args.output or f"udp_capture_{datetime.now():%Y%m%d_%H%M%S}.bin"

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("", args.port))
    except OSError as e:
        print(f"[!] cannot bind UDP :{args.port} -- {e}", file=sys.stderr)
        print("    Is another capture already running?", file=sys.stderr)
        sys.exit(1)
    sock.settimeout(1.0)

    print(f"[+] listening on UDP :{args.port} -> {out_path}")
    print("    (connect this PC to the payload's Wi-Fi network first)")
    print("    Ctrl-C to stop\n")

    buffer = b""
    total_bytes = 0
    total_frames = 0
    datagrams = 0
    started = None
    last_rx = None
    last_report = time.monotonic()

    try:
        with open(out_path, "wb") as f:
            while True:
                if args.duration and started and (time.monotonic() - started) >= args.duration:
                    break
                try:
                    data, _addr = sock.recvfrom(2048)
                except socket.timeout:
                    if last_rx and (time.monotonic() - last_rx) > 3.0 and not args.quiet:
                        print(f"[!] no data for {time.monotonic() - last_rx:.0f}s "
                              "-- payload off, out of range, or wrong network?")
                        last_rx = time.monotonic()
                    continue

                now = time.monotonic()
                if started is None:
                    started = now
                    print("[+] first datagram received -- link is up")
                last_rx = now

                f.write(data)
                f.flush()
                total_bytes += len(data)
                datagrams += 1

                # Decode incrementally purely for the live stats; the file on
                # disk is the untouched raw stream.
                buffer += data
                buffer, frames = frameparser.parse_frame_stream_bin(buffer)
                total_frames += len(frames)

                if not args.quiet and (now - last_report) >= 1.0:
                    elapsed = now - started
                    crc_fails = getattr(frameparser, "crc_fail_count", 0)
                    print(f"\r  {elapsed:6.1f}s  {datagrams:6d} dgram  "
                          f"{total_bytes/1024:8.1f} KiB  {total_frames:7d} frames  "
                          f"{total_bytes/elapsed/1024:6.1f} KiB/s  "
                          f"crc-fail {crc_fails}", end="", flush=True)
                    last_report = now
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    elapsed = (time.monotonic() - started) if started else 0.0
    crc_fails = getattr(frameparser, "crc_fail_count", 0)
    print("\n")
    if not started:
        print("[!] no data received at all.")
        print("    Check: payload built with CONFIG_ENABLE_TELEMETRY_NET, PC joined")
        print(f"    the payload's AP, and the port matches ({args.port}).")
        sys.exit(1)

    print(f"[+] {total_bytes} bytes / {total_frames} frames in {elapsed:.1f}s "
          f"({total_bytes/elapsed/1024:.1f} KiB/s)")
    if crc_fails:
        pct = 100.0 * crc_fails / max(1, total_frames + crc_fails)
        print(f"[+] {crc_fails} CRC failures ({pct:.1f}%) -- expected under load; "
              "watch whether this grows with distance")
    print(f"[+] written to {out_path}")
    print(f"[+] next: python bin2json.py --source {out_path} --output flight.json")


if __name__ == "__main__":
    main()
