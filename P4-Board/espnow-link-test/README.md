# ESP-NOW link test — ESP32-S3 → ESP32-C6

Minimal pair of firmwares that answer one question: **does ESP-NOW actually
carry packets from the rocket's S3 to the P4 board, and over what range?**

No sensors, no framing, no display. Just a sequenced broadcast and a receiver
that reports loss and RSSI.

## Read this first: the code goes on the S3 and the **C6**, not the P4

The ESP32-P4 has **no radio of its own**. It borrows the on-board ESP32-C6 over
SDIO through *esp-hosted*, and esp-hosted does **not** expose the ESP-NOW API to
the host — that is
[issue #19](https://github.com/espressif/esp-hosted-mcu/issues/19), open and
unanswered by Espressif since November 2024. The
[supported-features list](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/features.md)
covers station, SoftAP and SoftAP+station only.

The C6 itself, however, is a perfectly ordinary ESP32-C6 where ESP-NOW works
natively. So rather than fighting the RPC layer, this test puts ESP-NOW
**directly on the C6**. The P4 is not involved at all in proving the link works.

## ⚠ This replaces the C6's factory firmware

Flashing `c6-receiver` overwrites the esp-hosted slave firmware (the board ships
with v0.0.6). **While it is loaded, the P4 has no Wi-Fi and no Bluetooth**,
because the co-processor no longer speaks the esp-hosted protocol.

This is reversible — see [Restoring the factory firmware](#restoring-the-factory-firmware)
at the bottom. Read that section *before* you flash, not after.

## What you need

- The **PROG_C6** header on the ESP32-P4-Function-EV-Board (Espressif calls it
  the "ESP32-C6 Module Programming Connector"). Their user guide says it works
  "with ESP-Prog **or other UART tools**", so the USB-UART adapter you already
  use for payload telemetry may be enough.
- **Check the header's pinout on the silkscreen before wiring.** ESP-Prog drives
  both `EN` and the boot-strap pin to enter download mode automatically. If the
  header only breaks out `EN`, `TXD`, `RXD` and `GND`, a plain UART adapter
  cannot trigger download mode on its own and you will need ESP-Prog (or to
  strap the C6's `IO9` low manually while toggling `EN`).

Wiring, per Espressif's esp-hosted docs:

| ESP-Prog / adapter | PROG_C6 |
|---|---|
| `ESP_EN`  | `EN`  |
| `ESP_TXD` | `TXD` |
| `ESP_RXD` | `RXD` |
| `GND`     | `GND` |

## Flashing

**Hold the P4 in bootloader mode while you flash the C6**, so it does not drive
the shared SDIO lines and corrupt the transfer. (Hold BOOT, tap RESET, release.)

### C6 receiver

```bash
cd P4-Board/espnow-link-test/c6-receiver
idf.py set-target esp32c6
idf.py -p <PROG_C6 port> flash monitor
```

### S3 transmitter

```bash
cd P4-Board/espnow-link-test/s3-sender
idf.py set-target esp32s3
idf.py -p <S3 port> flash monitor
```

Requires ESP-IDF v5.4 or newer for the C6 target.

## What you should see

On the S3:

```
I (312) espnow_tx: ESP-NOW transmitter up
I (312) espnow_tx:   my MAC   : 30:ED:A0:12:34:56
I (312) espnow_tx:   channel  : 6
I (812) espnow_tx: sent seq=0
```

On the C6, once packets arrive:

```
I (2312) espnow_rx: LINK UP — first packet from 30:ED:A0:12:34:56
I (4312) espnow_rx: rx 8 (+4)  lost 0 (0.0%)  rssi -41 dBm
```

**Walk away from the transmitter and watch RSSI fall and loss climb.** That
curve is your real usable range — far more useful than a yes/no answer. Below
roughly −85 dBm expect loss to rise sharply.

### If nothing arrives

1. **Channel mismatch** — by far the most common cause. `ESPNOW_CHANNEL` is
   defined in *both* `main.c` files and they must agree.
2. **Blank C6 console** — if you see no output at all, not even the boot banner,
   the console is probably going to USB-Serial-JTAG instead of UART0. That is
   what `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` in `sdkconfig.defaults` prevents;
   confirm it survived `set-target`, which regenerates `sdkconfig`.
3. **Both boards powered from the same USB hub** can brown out under Wi-Fi TX
   peaks. Try separate supplies.

## Restoring the factory firmware

To give the P4 its Wi-Fi and Bluetooth back, flash the official esp-hosted slave
firmware onto the C6 through the same PROG_C6 header:

```bash
idf.py create-project-from-example "espressif/esp_hosted:slave"
cd slave
idf.py set-target esp32c6
idf.py menuconfig      # select SDIO transport
idf.py -p <PROG_C6 port> flash monitor
```

Because it is rebuilt from Espressif's own example, this does not depend on
having kept a backup of the shipped image.

## If the link works — what comes next

The C6 keeps ESP-NOW and becomes a dedicated **ESP-NOW → UART bridge**: it
receives the rocket's telemetry frames and forwards the identical bytes to a P4
GPIO over a two-wire jumper (TX + GND). The P4 then just reads a UART and
decodes the same frame format everything else in this repo already speaks — no
wireless stack on the P4 at all, and no dependency on esp-hosted ever gaining
ESP-NOW support.

The trade-off to decide then: the P4 keeps no Wi-Fi in that arrangement, which
is fine for the all-local live flight display (Mode 1) but rules out the P4
reaching the internet.
