# nvram-programmer

In-circuit programmer for a Dallas DS1250 5V NVSRAM, controlled from a host PC over USB. The programmer hardware is a 5V Teensy++ 2.0 (AT90USB1286).

## Hardware

- **Target chip:** Dallas DS1250 5V NVSRAM (512K × 8-bit, 19-bit address bus)
- **Programmer MCU:** Teensy++ 2.0 (5V, AT90USB1286)
- **Host interface:** USB CDC serial at 115200 baud

![Teensy++ 2.0](images/teensy.png)

### Pin mapping (Teensy++ 2.0)

| Signal  | AVR port/pin  |
|---------|---------------|
| A[7:0]  | PORTD         |
| A[15:8] | PORTC         |
| A16     | PORTB[0]      |
| A17     | PORTB[1]      |
| A18     | PORTB[2]      |
| D[7:0]  | PORTA         |
| /OE     | PORTE[6]      |
| /WE     | PORTE[7]      |
| /CE     | PORTE[0]      |

## Build system

Uses **PlatformIO**. The predecessor Arduino IDE project lives at `../arduino-nvram-programmer/`.

The working `pio` binary is at `~/.platformio/penv/bin/pio` — the system `/usr/bin/pio` is broken on this machine.

Build and upload:
```
~/.platformio/penv/bin/pio run -t upload
```

Open serial monitor:
```
~/.platformio/penv/bin/pio device monitor -b 115200
```

## Serial protocol

The host sends a 4-byte little-endian length prefix followed by the raw binary payload. The Teensy writes each byte sequentially from address 0 and reads it back immediately to verify. A 5-second inter-byte timeout aborts the transfer. Progress is reported as dots (one per 1 KB, newline per 64 KB), followed by a final `PASS` or `FAIL` line.

## Host tool

`nvram_write` is a C program (`nvram_write.c`) that sends a binary file to the Teensy over USB serial.

```
./nvram_write [--port DEV] <binary>
```

Default port is `/dev/ttyACM0`; on this machine the Teensy enumerates as `/dev/ttyACM1`. Build with:
```
gcc -Wall -o nvram_write nvram_write.c
```

## Key source functions

- `write_byte(addr, data)` — deasserts /OE, drives data bus, sets address, pulses /WE low then high to latch
- `read_byte(addr)` — floats data bus, sets address, asserts /OE, reads PINA, deasserts /OE

`/CE` is asserted (PORTE[0] low) once at startup and held for the entire session.

## TODOs in existing code

- `/BUSRQ` assertion/release (bus arbitration with the target CPU) is not yet implemented
- `/RESET` pulse after bus release is not yet implemented
