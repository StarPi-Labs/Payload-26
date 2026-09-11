# Flashing the ESP32-C6 on the ESP32-P4-Function-EV-Board

Everything needed to put custom firmware on the board's Wi-Fi co-processor
through the **PROG_C6** header — and to put the factory firmware back.

---

## Contents

1. [Why you need this](#1-why-you-need-this)
2. [Read before you flash](#2-read-before-you-flash)
3. [The PROG_C6 header](#3-the-prog_c6-header)
4. [Choosing an adapter](#4-choosing-an-adapter)
5. [Wiring](#5-wiring)
6. [The procedure](#6-the-procedure)
7. [Script reference](#7-script-reference)
8. [Troubleshooting](#8-troubleshooting)
9. [Restoring the factory firmware](#9-restoring-the-factory-firmware)

---

## 1. Why you need this

The ESP32-P4 has **no radio**. It borrows the on-board ESP32-C6 over SDIO using
*esp-hosted*, and esp-hosted does not expose the ESP-NOW API to the host — see
[issue #19](https://github.com/espressif/esp-hosted-mcu/issues/19), open and
unanswered since November 2024, and the
[supported-features list](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/features.md),
which covers station / SoftAP / SoftAP+station only.

The C6 itself is an ordinary ESP32-C6 where ESP-NOW works perfectly. So instead
of patching esp-hosted's RPC layer, we run ESP-NOW **directly on the C6** — which
means replacing the firmware on it, which means being able to flash it.

There is no USB connector wired to the C6. The only route in is the PROG_C6
header.

---

## 2. Read before you flash

> **Flashing custom firmware to the C6 removes the P4's Wi-Fi and Bluetooth.**
>
> The P4 talks to the C6 using the esp-hosted protocol. Overwrite the slave
> firmware and that conversation stops. The P4 keeps working, keeps its display
> and touch — it simply has no radio until you restore it.

This is **fully reversible**. [Section 9](#9-restoring-the-factory-firmware)
rebuilds Espressif's own slave firmware from source, so you do **not** need a
backup of the shipped image. Read that section before you start, not after.

Two more precautions:

- **Do not connect the adapter's VCC/3V3.** Power the P4 board from its own USB.
  Feeding 3.3 V in from an adapter while the board is also USB-powered risks
  back-feeding the regulator. Share **GND only**.
- **Hold the P4 in bootloader mode while flashing the C6.** Espressif's own
  esp-hosted documentation calls for this so the P4 does not drive shared lines
  mid-transfer. Hold the P4's **BOOT**, tap **RESET**, release both.

---

## 3. The PROG_C6 header

A 3×2 header, labelled J2 on the schematic. Physical layout:

```
        ┌─────────────┐
        │  EN  │  NC  │
        ├──────┼──────┤
        │ TX0  │ GND  │
        ├──────┼──────┤
        │ RX0  │ Boot │
        └─────────────┘
```

| Silkscreen | Schematic net | What it is | Why it matters |
|---|---|---|---|
| `EN`   | `C6_CHIP_PU` | Chip enable / reset | Pulsed low to reset the C6 |
| `NC`   | —            | Not connected | Leave alone |
| `TX0`  | `C6_U0TXD`   | C6 **transmits** | Goes to your adapter's **RX** |
| `GND`  | `GND`        | Ground | Required signal reference |
| `RX0`  | `C6_U0RXD`   | C6 **receives** | Comes from your adapter's **TX** |
| `Boot` | `C6_IO9`     | Strapping pin | Held low at reset ⇒ download mode |

**`Boot` being broken out is the important part.** With both `EN` and `Boot`
available, an adapter with DTR/RTS can enter download mode entirely
automatically — no button presses, no jumper wires, standard `idf.py flash`.

---

## 4. Choosing an adapter

Work down this list and stop at the first one you can satisfy.

| | Option | Needs | Verdict |
|---|---|---|---|
| **A** | USB-UART adapter you already own | DTR **and** RTS broken out | Try first — likely zero cost |
| **B** | Spare ESP32-S3/S2 running `esp-usb-bridge` | A spare S2/S3 | Recommended fallback |
| **C** | ESP-Prog | Buying one (~€12) | Purpose-built, always works |

### Option A — the adapter you already have

You use a USB-TTL adapter for payload telemetry. **Look at its pin labels.** If
you see `DTR` and `RTS` alongside `TX`/`RX`/`GND`, you are done — wire it up per
[section 5](#5-wiring) and skip to the procedure.

Many cheap CP2102/CH340 boards expose only `TX`/`RX`/`VCC`/`GND`/`3V3`. Those
**cannot** trigger download mode by themselves. You can still flash manually
(see [8.3](#83-adapter-has-no-dtrrts)), but it is fiddly; prefer option B.

### Option B — a spare ESP32-S3 as a software ESP-Prog

Espressif maintains [**esp-usb-bridge**](https://github.com/espressif/esp-usb-bridge),
which turns an ESP32-**S2** or **S3** into a USB↔UART bridge with JTAG and
proper auto-reset. It is ESP-Prog implemented in firmware.

```bash
git clone --recursive https://github.com/espressif/esp-usb-bridge.git
cd esp-usb-bridge
idf.py set-target esp32s3
idf.py menuconfig
idf.py -p <SPARE_S3_PORT> flash
```

In `menuconfig`, the pins live under **Serial Handler Configuration**. The
defaults are `BOOT=4`, `RESET=7`, `RxD=6`, `TxD=5` — see
[section 5.1](#51-using-esp-usb-bridge-option-b--concrete-pins) for the wiring
table. Confirm them in your own checkout; they are project settings, not fixed
hardware.

Note there is no pin called "EN" in that menu — **`RESET` is the EN pin.**

Once flashed, the bridge enumerates as a new COM port and behaves like any other
serial adapter: `idf.py -p <BRIDGE_PORT> flash` just works.

> **Use a *spare* S3 if you have one.** Flashing the bridge onto your rocket's S3
> overwrites the payload firmware, and you would have to reflash it afterwards —
> workable, just tedious, and easy to forget before a test flight.

### Option C — ESP-Prog

The route Espressif documents. Its `PROG` header maps straight onto PROG_C6 and
its auto-reset circuit is designed for exactly this.

---

## 5. Wiring

**The crossover is the single most common mistake.** TX goes to RX.

| PROG_C6 | → | Adapter / bridge | Purpose |
|---|---|---|---|
| `TX0`  | → | `RX`  | C6 output → adapter input |
| `RX0`  | ← | `TX`  | adapter output → C6 input |
| `GND`  | ↔ | `GND` | shared reference — **required** |
| `EN`   | ← | `RTS` | reset control |
| `Boot` | ← | `DTR` | strapping control |
| `NC`   |   | —     | leave unconnected |
| —      |   | `VCC` | **leave unconnected** (see [section 2](#2-read-before-you-flash)) |

`RTS→EN` and `DTR→Boot` is esptool's standard convention, not an arbitrary
choice — esptool drives exactly these two lines to sequence the chip into
download mode. Swapping them means download mode never triggers.

Direct connection without the two-transistor circuit found on dev boards is
normally fine for occasional flashing.

### 5.1 Using esp-usb-bridge (option B) — concrete pins

A bridge has no DTR/RTS lines; it drives dedicated GPIOs instead. The defaults
in *Serial Handler Configuration*:

| PROG_C6 | Signal | Bridge setting | Bridge GPIO |
|---|---|---|---|
| `EN`   | `C6_CHIP_PU` | **RESET** | `7` |
| `Boot` | `C6_IO9`     | **BOOT**  | `4` |
| `RX0`  | `C6_U0RXD`   | **TxD**   | `5` |
| `TX0`  | `C6_U0TXD`   | **RxD**   | `6` |
| `GND`  | `GND`        | GND       | GND |
| `NC`   | —            | —         | leave unconnected |

**`RESET` is `EN`.** esp-usb-bridge names the pin for what it does; Espressif's
silkscreen names it `EN` and the schematic calls it `C6_CHIP_PU`. One signal,
three names — there is no separate "EN" setting to hunt for.

**Ignore the Debug Probe Configuration** (TDI/TDO/TCK/TMS, defaults 3/9/10/8).
That is JTAG debugging and plays no part in flashing. Five wires, not nine.

`TxD`/`RxD` are named from the **bridge's** point of view, which is why bridge
`TxD`→C6 `RX0` and bridge `RxD`→C6 `TX0`. If a handshake fails with everything
else correct, swapping just these two is the first thing to try —
`verify-link.ps1` says so too.

> Confirm these numbers in your own `menuconfig` before wiring. They are
> project settings, not fixed hardware, and upstream defaults can change.

---

## 6. The procedure

### Step 1 — Power the P4 board

Its own USB cable. The adapter supplies signals only, never power.

### Step 2 — Wire PROG_C6

Per [section 5](#5-wiring). Double-check the crossover before powering on.

### Step 3 — Hold the P4 in bootloader mode

Hold **BOOT**, tap **RESET**, release both. This parks the P4 so it cannot
interfere with the C6 mid-flash.

### Step 4 — Verify the link *before* flashing anything

```powershell
.\verify-link.ps1
```

Run with no arguments first and it lists your serial ports. Then:

```powershell
.\verify-link.ps1 -Port COM7
```

Success looks like:

```
  [OK]   C6 responded. Wiring is correct and download mode works.
         MAC: 40:4c:ca:xx:xx:xx
```

**Do not skip this.** A failed handshake costs five seconds. A failed flash can
leave the C6 half-written with no radio until you recover it.

### Step 5 — Flash the ESP-NOW receiver

```powershell
.\flash-c6.ps1 -Port COM7
```

Confirms the warning, then runs `set-target esp32c6`, `build`, `flash` on
[`../espnow-link-test/c6-receiver`](../espnow-link-test/README.md).

### Step 6 — Watch it work

```powershell
idf.py -p COM7 monitor
```

Then power up the S3 transmitter. You should see:

```
I (2312) espnow_rx: LINK UP — first packet from 30:ED:A0:12:34:56
I (4312) espnow_rx: rx 8 (+4)  lost 0 (0.0%)  rssi -41 dBm
```

Walk away from the transmitter and watch RSSI fall and loss climb. **That curve
is your real usable range** — much more useful than a yes/no.

---

## 7. Script reference

| Script | Does |
|---|---|
| `env.ps1` | Activates ESP-IDF reliably. **Dot-source this first.** |
| `verify-link.ps1` | esptool handshake + wiring diagnostics. Run before flashing, always. |
| `flash-c6.ps1` | Builds and flashes the ESP-NOW receiver, with a confirmation. |
| `restore-c6.ps1` | Rebuilds and flashes Espressif's esp-hosted slave firmware. |

Typical session:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\env.ps1
.\verify-link.ps1 -Port COM13
.\flash-c6.ps1    -Port COM13
```

All are Windows PowerShell 5.1 compatible and deliberately **ASCII-only** —
PowerShell 5.1 reads `.ps1` files in the system ANSI codepage unless they carry
a UTF-8 BOM, so a stray em-dash or accented character becomes mojibake and
produces baffling parse errors. Keep it that way when editing.

### Why `env.ps1` instead of ESP-IDF's `export.ps1`

`export.ps1` picks whichever `python` is first on PATH, then looks for a
virtualenv named after *that* python's version. If they disagree it fails with:

```
ERROR: ESP-IDF Python virtual environment
"...\python_env\idf5.5_py3.12_env\Scripts\python.exe" not found.
```

On this machine `python` resolves to **Inkscape's** bundled Python 3.12, while
the ESP-IDF venv was built with ESP-IDF's own managed Python 3.11 — so
`export.ps1` hunts for a `py3.12` venv that never existed. The VS Code ESP-IDF
extension avoids this by using the managed interpreter directly, which is why
builds work there but not in a plain terminal.

`env.ps1` reads which venvs actually exist, finds the managed python whose
version matches, puts it first on PATH, and only then dot-sources `export.ps1`.

It defaults to the **v5.5.1** tree, whose virtualenv is intact. The v6.0.1
install at `C:\esp\v6.0.1\esp-idf` has **no virtualenv at all** and will not
activate until you run its `install.ps1`. v5.5 is ≥ 5.4, so it builds both
esp32c6 and esp32s3 targets fine — there is no need to fix v6.0.1 for this work.

Point it elsewhere with `. .\env.ps1 -IdfPath C:\esp\v6.0.1\esp-idf` once that
install is repaired.

### "running scripts is disabled on this system"

Windows blocks unsigned local scripts by default. Nothing is wrong with the
files. Enable them for the current session only — no admin rights, nothing
persisted:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

Or bypass a single run without changing any policy (the ESP-IDF environment is
inherited by the child process, so `esptool` still resolves):

```powershell
powershell -ExecutionPolicy Bypass -File .\verify-link.ps1 -Port COM13
```

To make it permanent for your user — the usual developer setting, which still
blocks unsigned scripts downloaded from the internet:

```powershell
Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned
```

---

## 8. Troubleshooting

### 8.1 "Failed to connect" / "No serial data received"

In order of likelihood:

1. **TX/RX not crossed.** `TX0→RX`, `RX0→TX`. Swap the two wires and retry —
   this is the answer most of the time.
2. **GND not connected.**
3. **DTR/RTS swapped.** `RTS→EN`, `DTR→Boot`.
4. **P4 not in bootloader mode** (step 3).
5. **Board not powered**, or powered only from the adapter.

### 8.2 Something answers, but it is not a C6

You are talking to the bridge/adapter board itself, or to the P4. Your `TX0`/`RX0`
wires are not landing on PROG_C6.

### 8.3 Adapter has no DTR/RTS

Manual download-mode entry:

1. Connect `Boot` to `GND` (jumper wire).
2. Briefly connect `EN` to `GND`, then release it.
3. Remove the `Boot`–`GND` jumper.
4. Immediately run `verify-link.ps1` or your flash command.

The C6 stays in download mode until reset, so there is no rush after step 3 — but
this must be repeated before *every* flash, which is why option B is nicer.

### 8.4 "could not open port" / access denied

A serial monitor still has it open. Close it.

### 8.5 Flashed fine, but the console is blank

The C6's console must be on **UART0**, not USB-Serial-JTAG — the C6 has no USB
here, so USB-JTAG output goes nowhere and looks identical to "received nothing".
`CONFIG_ESP_CONSOLE_UART_DEFAULT=y` in the receiver's `sdkconfig.defaults`
handles this; confirm it survived `set-target`, which regenerates `sdkconfig`.

### 8.6 Both boards brown out under load

Wi-Fi TX peaks draw hard. Power the two boards from separate supplies rather
than one hub.

---

## 9. Restoring the factory firmware

Gives the P4 back its Wi-Fi and Bluetooth.

```powershell
.\restore-c6.ps1 -Port COM7
```

It fetches Espressif's `esp_hosted:slave` example, sets the target to esp32c6,
opens `menuconfig`, builds and flashes.

> **In menuconfig, set the transport to SDIO.** That is how the C6 is wired to
> the P4 on this board. A different transport builds cleanly and then silently
> never talks to the P4 — a confusing failure worth avoiding.

Manual equivalent:

```bash
idf.py create-project-from-example "espressif/esp_hosted:slave"
cd slave
idf.py set-target esp32c6
idf.py menuconfig          # transport = SDIO
idf.py -p COM7 flash monitor
```

Power-cycle the whole board afterwards.

**On versions:** the board ships with slave firmware v0.0.6; this builds whatever
your component manager resolves, normally newer. Host and slave should be
compatible versions — if the P4 later reports a version mismatch, update the
`esp_hosted` component on the P4 side to match.
