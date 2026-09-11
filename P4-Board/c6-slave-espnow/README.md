# ESP-NOW bridge for the ESP32-C6 slave

Adds ESP-NOW reception to the **stock esp-hosted slave firmware** without
changing any of it. Wi-Fi, Bluetooth and the whole esp-hosted RPC/data path keep
working exactly as before — the P4 keeps full network capability and *gains*
ESP-NOW.

## Why this works

esp-hosted does not proxy the ESP-NOW API to the host
([issue #19](https://github.com/espressif/esp-hosted-mcu/issues/19), open and
unanswered since Nov 2024). But it does ship a documented, general-purpose
**custom-data channel** — `esp_hosted_send_custom_data()` on the slave,
`esp_hosted_register_rx_callback_custom_data()` on the host, up to 8166 bytes
per message.

So we don't extend the RPC protocol. ESP-NOW runs natively on the C6 (it is an
ordinary ESP32-C6) and its received payloads ride the existing user-data pipe up
to the P4. No protobuf changes, no forked transport, nothing to re-merge when
esp-hosted updates.

ESP-NOW and Wi-Fi station/SoftAP coexist on one radio, so nothing has to be
given up.

## Integration

The slave firmware is generated from Espressif's example, then two files are
added to it.

**1. Generate the slave project**

```bash
idf.py create-project-from-example "espressif/esp_hosted:slave"
cd slave
idf.py set-target esp32c6
```

**2. Copy the bridge in**

```bash
cp <repo>/P4-Board/c6-slave-espnow/espnow_bridge.c main/
cp <repo>/P4-Board/c6-slave-espnow/espnow_bridge.h main/
cp <repo>/P4-Board/common/pilog_link.h             main/
```

`pilog_link.h` is shared verbatim with the rocket firmware and the P4 app. Copy
it rather than editing a second copy — a channel or message-ID mismatch produces
a link that looks healthy from both ends and delivers nothing.

**3. Add it to `main/CMakeLists.txt`**

Append `espnow_bridge.c` to the existing `SRCS` list. Do not replace the list;
the slave needs everything already in it.

**4. Call it from the slave's `app_main`**

At the **end** of `app_main` in `main/esp_hosted_coprocessor.c`:

```c
#include "espnow_bridge.h"
...
    espnow_bridge_init();   /* last line of app_main */
```

Order matters: ESP-NOW requires a started Wi-Fi stack, and the slave brings
Wi-Fi up during its own init. Calling earlier fails cleanly with a log line
rather than misbehaving, but it will not work.

**5. Build and flash through PROG_C6**

```powershell
cd ../c6-flasher
. .\env.ps1
.\verify-link.ps1 -Port COM13
```

then from the slave project:

```bash
idf.py -p COM13 flash monitor
```

Hold the P4 in bootloader mode while flashing (hold BOOT, tap RESET, release)
so it cannot drive shared lines mid-transfer.

## What you should see

On the C6 console, once the rocket is powered:

```
I (3120) espnow_br: ESP-NOW bridge up on channel 6 (expected 6), msg id 0x50494C47
I (9200) espnow_br: rx 200  fwd 200  dropped 0  seq-gaps 0
```

Reading the counters:

| Symptom | Meaning |
|---|---|
| `rx` not increasing | ESP-NOW side. Rocket unpowered, wrong channel, or out of range. |
| `rx` rising, `fwd` not | SDIO side. Host not up, or version mismatch with the P4's component. |
| `dropped` climbing | Host link slower than the incoming rate. Telemetry is best-effort; a few is fine. |
| `seq-gaps` climbing | Packets lost over the air. Expected at range; the decoder resyncs. |

## The channel constraint

ESP-NOW shares the single radio with Wi-Fi, so **both ends must be on the same
channel**.

`espnow_bridge_init()` handles this conservatively: if the slave is *not*
associated with an access point it pins `PILOG_ESPNOW_CHANNEL`. If it *is*
associated, that association owns the channel — forcing ours would break the
host's Wi-Fi — so the bridge leaves it alone and logs a warning telling you
which channel the rocket must use instead.

Practically: while the ground station is standalone (the normal case for a live
flight display) everything sits on channel 6 and this never comes up. It only
matters if you later have the P4 join a network at the same time.

## Restoring stock firmware

Nothing here is destructive to the slave's own function, but to get back to a
completely untouched build, just regenerate the example without steps 2–4 and
reflash. See `../c6-flasher/restore-c6.ps1`.

## Version matching

The slave firmware and the P4's `esp_hosted` component must be compatible
versions. If the P4 logs a version mismatch at `esp_hosted_init()`, align the
component version in `p4-display/main/idf_component.yml` with the slave's, then
rebuild both.
