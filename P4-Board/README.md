# Pi-LOG ground station (ESP32-P4-Function-EV-Board)

Live flight display for the Star-PI payload. Telemetry travels:

```
  rocket ESP32-S3            ground ESP32-C6              ground ESP32-P4
 ┌────────────────┐         ┌─────────────────┐         ┌─────────────────┐
 │ sensors        │         │ esp-hosted      │  SDIO   │ esp-hosted host │
 │   ↓ log_sink() │ ESP-NOW │ slave (stock)   │ ──────► │   ↓ custom data │
 │ SD card  ◄─────┤ ──────► │   + espnow      │         │ telemetry_codec │
 │ telemetry_espnow│        │     bridge      │         │   ↓             │
 └────────────────┘         └─────────────────┘         │ LVGL live UI    │
                                                        └─────────────────┘
```

The bytes on the air are **identical** to what the SD card records, so the same
frame format is decoded by `SD-Parser/frameparser.py` on the ground and by
`telemetry_codec` on the P4. One protocol, one source of truth.

## Layout

| Path | What |
|---|---|
| `common/pilog_link.h` | Channel, message ID, chunk header. Shared **verbatim** by all three firmwares. |
| `components/telemetry_codec/` | C port of `frameparser.py`. Decodes the stream into a forward-filled snapshot. |
| `c6-slave-espnow/` | Add-on for the stock esp-hosted slave: ESP-NOW → host. Keeps Wi-Fi/BT. |
| `p4-display/` | The LVGL live display app. |
| `c6-flasher/` | Tooling to flash the C6 through the PROG_C6 header, and restore it. |
| `espnow-link-test/` | Minimal S3↔C6 link test. Proves the radio path before integrating. |

## Bring-up order

Each step is independently verifiable, so a failure localises immediately
instead of leaving you guessing which of three firmwares is at fault.

**1. Prove ESP-NOW works at all** — [`espnow-link-test/`](espnow-link-test/README.md)

Two throwaway firmwares, no esp-hosted involved. Confirms the radio path and
gives you an RSSI/packet-loss curve for range.

**2. Get the flashing rig working** — [`c6-flasher/`](c6-flasher/README.md)

```powershell
. .\env.ps1
.\verify-link.ps1 -Port COM13
```

Confirms the C6 answers over PROG_C6 before anything is written to it.

**3. Build the C6 bridge** — [`c6-slave-espnow/`](c6-slave-espnow/README.md)

Generate the stock slave example, drop in two files, add one call. Flash it.
Watch the counters on its console.

**4. Build the P4 display** — `p4-display/`

```powershell
. ..\c6-flasher\env.ps1 -IdfPath C:\esp\v6.0.1\esp-idf
cd p4-display
idf.py set-target esp32p4
idf.py menuconfig     # pick the LCD panel your kit shipped with
idf.py -p <P4 port> flash monitor
```

**5. Enable the rocket transmitter**

In `Embedded-Code/`, `idf.py menuconfig` → Star-PI Payload Configuration →
**Broadcast live telemetry over ESP-NOW**. Build and flash the S3.

## The one gotcha worth knowing in advance

**Pick the right LCD panel in menuconfig.** The board ships with different
panels, and the wrong choice builds perfectly and then displays nothing — which
looks exactly like a dead telemetry link. It is under the BSP's menu.

Second: **ESP-NOW shares the radio with Wi-Fi**, so if the C6 ever associates
with an access point, the AP's channel wins and the rocket must follow it. While
the ground station is standalone this never arises. The bridge detects the case
and logs which channel to use.

## Status

Written but **not compiled or hardware-tested** — no P4 board was in front of
the machine that produced this, and the frame decoder could not be unit-tested
locally (no host C compiler available). Treat the first build as a real
bring-up, not a formality. In particular the decoder is the piece most worth
sanity-checking against a known capture: feed it a `.bin` from `SD-Parser/` and
compare against `bin2json.py` output.
