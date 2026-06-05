# nvram-programmer

Programmer for a Dallas DS1250 5V NVSRAM, controlled from a host PC over USB. The programmer hardware is a 5V Teensy++ 2.0 (AT90USB1286).

## Hardware

- **Target chip:** Dallas DS1250Y-70 5V NVSRAM (512K × 8-bit, 19-bit address bus)
- **Programmer MCU:** Teensy++ 2.0 (5V, AT90USB1286)
- **Host interface:** USB CDC serial at 115200 baud
- **Target board CPU:** Motorola 68030 at 16 MHz

![Teensy++ 2.0](images/teensy.png)

### Pin mapping (Teensy++ 2.0 → DS1250)

| Signal  | AVR port/pin  | Notes                        |
|---------|---------------|------------------------------|
| A[7:0]  | PORTD         |                              |
| A[15:8] | PORTC         |                              |
| A16     | PORTB[0]      |                              |
| A17     | PORTB[1]      |                              |
| A18     | PORTB[2]      |                              |
| D[7:0]  | PORTF         | physical bit order reversed  |
| /CE     | PORTA[0]      | strobed per byte             |
| /OE     | tied low      | outputs always enabled       |
| /WE     | PORTA[2]      | held for entire write burst  |

Note: PORTF D[7:0] wiring is physically reversed (PF0→D7 … PF7→D0), so the
firmware connects to the correct data bits without software bit-reversal.

## Build system

Uses **PlatformIO**. The predecessor Arduino IDE project lives at `../arduino-nvram-programmer/`.

The working `pio` binary is at `~/.platformio/penv/bin/pio` — the system `/usr/bin/pio` is broken on this machine.

Build and upload:
```
~/.platformio/penv/bin/pio run -t upload
```

## Serial protocol

The host sends a 1-byte command (`W` or `R`), followed by a 4-byte little-endian length, followed by the payload (write only). A 5-second inter-byte timeout aborts a transfer.

**Write (`W`):** Teensy receives length bytes, writes them sequentially from address 0 using CE-strobe mode (/WE held low, /CE pulses per byte), then replies `Written.\n`.

**Read (`R`):** Teensy reads length bytes sequentially from address 0 and streams them raw over USB with no framing.

## Host tools

### nvram_write

Writes a binary file to the NVRAM, reads it back in full, compares, and reports `Verified.` or `FAIL: first mismatch at 0xXXXXX: wrote XX read XX`.

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

- `write_byte(addr, data)` — sets PORTF=data, sets address on PORTD/PORTC/PORTB, strobes /CE low then high (with /WE held asserted) to latch
- `read_byte(addr)` — sets address, strobes /CE low, reads PINF, deasserts /CE

`/WE` is held asserted for an entire write burst; `/OE` is tied low permanently. `/CE` toggles per byte. Each strobe is 6 NOPs (375 ns) wide at 16 MHz — well within the DS1250-70's 70 ns spec even with breadboard wiring capacitance.

## TODOs in existing code

- `/BUSRQ` assertion/release (bus arbitration with the target CPU) is not yet implemented
- `/RESET` pulse after bus release is not yet implemented
