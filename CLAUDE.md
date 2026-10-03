# nvram-programmer

Programmer for a Dallas DS1250 5V NVSRAM, controlled from a host PC over USB. The programmer hardware is a 5V Teensy++ 2.0 (AT90USB1286). The NVRAM is the boot ROM for a Motorola 68030 SBC; the Teensy acts as a DMA master on the 68030 bus.

## Hardware

- **Target chip:** Dallas DS1250Y-70 5V NVSRAM (512K × 8-bit, 19-bit address bus).
  Datasheet (Markdown): `../datasheets/Dallas DS121250AB/Dallas DS121250AB.md` in the private
  [datasheets](https://github.com/sparkybrit/datasheets) repo, cloned at `~/Projects/datasheets`.
  The datasheet is not kept in this repo.
- **Programmer MCU:** Teensy++ 2.0 (5V, AT90USB1286)
- **Host interface:** USB CDC serial at 115200 baud
- **Power:** from the SBC's 5 V rail, **not USB** — the Teensy's USB power trace is cut, so the cable
  carries data only. The programmer is powered, and enumerates as `/dev/ttyACM0`, only while the SBC is on.
  It also means the Teensy is always powered alongside the SBC, so its tri-stated bus pins are held
  high-impedance by running firmware rather than floating on an unpowered chip.
- **Target board CPU:** Motorola 68030 at 16 MHz

![Teensy++ 2.0](images/teensy.png)

### Pin mapping (Teensy++ 2.0 → DS1250 / 68030 bus)

| Signal  | AVR port/pin  | Direction     | Notes                                          |
|---------|---------------|---------------|------------------------------------------------|
| A[7:0]  | PORTD         | output        | tri-state when bus not owned; **A6 = PD6 carries the on-board LED**, see below |
| A[15:8] | PORTC         | output        | tri-state when bus not owned                   |
| A16     | PORTB[0]      | output        | tri-state when bus not owned                   |
| A17     | PORTB[1]      | output        | tri-state when bus not owned                   |
| A18     | PORTB[2]      | output        | tri-state when bus not owned                   |
| A30     | PORTB[5]      | output        | always 0; tri-state when bus not owned         |
| A31     | PORTB[6]      | output        | always 0; tri-state when bus not owned         |
| D[7:0]  | PORTF         | bidirectional | tri-state at idle                              |
| /AS     | PORTA[0]      | output        | strobed per byte; tri-state when bus not owned |
| /WE     | PORTA[2]      | output        | held for entire write burst; tri-state at idle |
| FC0     | PORTA[3]      | output        | function code bit 0; driven 0 while bus owned  |
| FC1     | PORTA[4]      | output        | function code bit 1; driven 0 while bus owned  |
| FC2     | PORTA[5]      | output        | function code bit 2; driven 0 while bus owned  |
| /BR     | PORTE[0]      | always output | assert to request 68030 bus                    |
| /BGACK  | PORTE[1]      | always output | assert to acknowledge bus grant                |
| /RESET  | PORTE[6]      | wired-OR      | pin held 0; DDR toggled to assert (output low 500 ms) then release (input/high-Z) |
| /BG     | PORTE[7]      | always input  | 68030 asserts to grant bus                     |
| /OE     | tied low      | —             | outputs always enabled                         |

At startup all bus pins (address, data, /AS, /WE, FC0/FC1/FC2) are tri-state
so the 68030 can boot normally from the NVRAM. /BR, /BGACK, and /RESET are
always driven outputs (deasserted high at startup); /BG is always an input.

**Exception: A6 carries the Teensy's on-board LED.** The Teensy++ 2.0 has an LED,
with its series resistor, from PD6 to GND — and PD6 is A6 here. Tri-stating does
not remove that load: the LED is on the Teensy's own board, outside the AVR, so it
sits on the 68030's A6 line permanently and conducts a few mA whenever A6 is high.
It slows A6's rising edge on a line that addresses both the SBC's SRAM and this
NVRAM, and the LED visibly flickers with SBC address activity. The firmware never
uses the LED.

Fix: remove the LED's series resistor (or the LED). Do **not** move A6 to another
pin instead — keeping all of `A[7:0]` on PORTD is what lets `write_byte` set the low
address byte in a single port write. Tracked in `../sparky1/CLAUDE.md` (To Do),
which also covers buffering the programmer off the bus.

### 68030 bus arbitration protocol

`acquire_bus()` sequence:
1. Assert /BR — Teensy requests the bus
2. Wait for /BG low — 68030 grants the bus (timeout 1 s)
3. Assert /BGACK — Teensy acknowledges it has taken the bus
4. Negate /BR — release the request line
5. Drive address, data, /AS, /WE, and FC0/FC1/FC2 pins as outputs (FC driven 0)

`relinquish_bus()` sequence:
1. Deassert /AS and /WE
2. Tri-state address, data, /AS, /WE, and FC0/FC1/FC2 pins
3. Negate /BGACK — signals to 68030 that bus is free

## Build system

Uses **PlatformIO**. The predecessor Arduino IDE project lives at `../arduino-nvram-programmer/`.

Firmware repository — the code programmed into the Teensy is this project: <https://github.com/sparkybrit/nvram-programmer>

The working `pio` binary is at `~/.platformio/penv/bin/pio` — the system `/usr/bin/pio` is broken on this machine.

Build and upload:
```
~/.platformio/penv/bin/pio run -t upload
```

## Serial protocol

The host sends a 1-byte command (`W`, `R` or `X`). `W` and `R` are followed by a 4-byte little-endian length, then the payload (write only). A 5-second inter-byte timeout aborts a transfer.

**Write (`W`):** Teensy calls `acquire_bus()`, receives and writes length bytes sequentially from address 0 using CE-strobe mode (/WE held low, /AS pulses per byte), then immediately streams length bytes of read-back data to the host, calls `relinquish_bus()`, asserts /RESET for 500 ms, then sends `Done.\n`.

**Read (`R`):** Teensy calls `acquire_bus()`, reads length bytes sequentially from address 0, streams them raw over USB, then calls `relinquish_bus()`.

**Reset (`X`):** no length or payload. Teensy asserts /RESET for 500 ms, then sends `Done.\n`. The bus is not touched, so the 68030 reboots from the NVRAM's current contents.

## Host tools

### nvram_write

Writes a binary file to the NVRAM, receives the read-back, compares, and reports `Verified.` or `FAIL: first mismatch at 0xXXXXX: wrote XX read XX`. The firmware handles the full acquire → write → read-back → relinquish → /RESET cycle in one `W` command.

```
./nvram_write [--port DEV] <binary>
```

Default port: `/dev/ttyACM0`. Build with `make`.

### nvram_reset

Resets the SBC without reprogramming: sends `X` and waits for `Done.`, then prints `Reset.`

```
./nvram_reset [--port DEV]
```

Default port: `/dev/ttyACM0`. Build with `make`.

### nvram_read

Reads the NVRAM contents to a file.

```
./nvram_read [--port DEV] [--length N] <outfile>
```

Default port: `/dev/ttyACM0`, default length: 512 × 1024. Build with `make`.

## Key source functions

- `acquire_bus()` — asserts /BR, waits for /BG low, asserts /BGACK, negates /BR, then drives all bus pins as outputs
- `relinquish_bus()` — tri-states all bus pins, negates /BGACK to return bus to 68030
- `write_byte(addr, data)` — sets PORTF=data, sets address on PORTD/PORTC/PORTB, strobes /AS low then high (with /WE held asserted) to latch
- `read_byte(addr)` — sets address, strobes /AS low, reads PINF, deasserts /AS

`/WE` is held asserted for an entire write burst; `/OE` is tied low permanently. `/AS` toggles per byte. Each strobe is 6 NOPs (375 ns) wide at 16 MHz — well within the DS1250-70's 70 ns spec even with breadboard wiring capacitance.
