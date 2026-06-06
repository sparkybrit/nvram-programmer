# nvram-programmer

Programmer for a Dallas DS1250 5V NVSRAM, controlled from a host PC over USB. The programmer hardware is a 5V Teensy++ 2.0 (AT90USB1286). The NVRAM is the boot ROM for a Motorola 68030 SBC; the Teensy acts as a DMA master on the 68030 bus.

## Hardware

- **Target chip:** Dallas DS1250Y-70 5V NVSRAM (512K × 8-bit, 19-bit address bus)
- **Programmer MCU:** Teensy++ 2.0 (5V, AT90USB1286)
- **Host interface:** USB CDC serial at 115200 baud
- **Target board CPU:** Motorola 68030 at 16 MHz

![Teensy++ 2.0](images/teensy.png)

### Pin mapping (Teensy++ 2.0 → DS1250 / 68030 bus)

| Signal  | AVR port/pin  | Direction     | Notes                                          |
|---------|---------------|---------------|------------------------------------------------|
| A[7:0]  | PORTD         | output        | tri-state when bus not owned                   |
| A[15:8] | PORTC         | output        | tri-state when bus not owned                   |
| A16     | PORTB[0]      | output        | tri-state when bus not owned                   |
| A17     | PORTB[1]      | output        | tri-state when bus not owned                   |
| A18     | PORTB[2]      | output        | tri-state when bus not owned                   |
| A30     | PORTB[5]      | output        | always 0; tri-state when bus not owned         |
| A31     | PORTB[6]      | output        | always 0; tri-state when bus not owned         |
| D[7:0]  | PORTF         | bidirectional | physical bit order reversed; tri-state at idle |
| /AS     | PORTA[0]      | output        | strobed per byte; tri-state when bus not owned |
| /WE     | PORTA[2]      | output        | held for entire write burst; tri-state at idle |
| FC0     | PORTA[3]      | output        | function code bit 0; driven 0 while bus owned  |
| FC1     | PORTA[4]      | output        | function code bit 1; driven 0 while bus owned  |
| FC2     | PORTA[5]      | output        | function code bit 2; driven 0 while bus owned  |
| /BR     | PORTE[0]      | always output | assert to request 68030 bus                    |
| /BGACK  | PORTE[1]      | always output | assert to acknowledge bus grant                |
| /RESET  | PORTE[6]      | always output | asserted 500 ms after write+verify             |
| /BG     | PORTE[7]      | always input  | 68030 asserts to grant bus                     |
| /OE     | tied low      | —             | outputs always enabled                         |

Note: PORTF D[7:0] wiring is physically reversed (PF0→D7 … PF7→D0), so the
firmware connects to the correct data bits without software bit-reversal.

At startup all bus pins (address, data, /AS, /WE, FC0/FC1/FC2) are tri-state
so the 68030 can boot normally from the NVRAM. /BR, /BGACK, and /RESET are
always driven outputs (deasserted high at startup); /BG is always an input.

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

The working `pio` binary is at `~/.platformio/penv/bin/pio` — the system `/usr/bin/pio` is broken on this machine.

Build and upload:
```
~/.platformio/penv/bin/pio run -t upload
```

## Serial protocol

The host sends a 1-byte command (`W` or `R`), followed by a 4-byte little-endian length, followed by the payload (write only). A 5-second inter-byte timeout aborts a transfer.

**Write (`W`):** Teensy calls `acquire_bus()`, receives and writes length bytes sequentially from address 0 using CE-strobe mode (/WE held low, /AS pulses per byte), then immediately streams length bytes of read-back data to the host, calls `relinquish_bus()`, asserts /RESET for 500 ms, then sends `Done.\n`.

**Read (`R`):** Teensy calls `acquire_bus()`, reads length bytes sequentially from address 0, streams them raw over USB, then calls `relinquish_bus()`.

## Host tools

### nvram_write

Writes a binary file to the NVRAM, receives the read-back, compares, and reports `Verified.` or `FAIL: first mismatch at 0xXXXXX: wrote XX read XX`. The firmware handles the full acquire → write → read-back → relinquish → /RESET cycle in one `W` command.

```
./nvram_write [--port DEV] <binary>
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
