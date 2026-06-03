#include <Arduino.h>

// Pin mapping (Teensy++ 2.0 / AT90USB1286):
//   A[7:0]  -> PORTD
//   A[15:8] -> PORTC
//   A18:16  -> PORTB[2:0]
//   D[7:0]  -> PORTA
//   /OE     -> PORTE[6]
//   /WE     -> PORTE[7]

#define OE_NEGATED()  (PORTE |=  (1 << 6))
#define OE_ASSERTED() (PORTE &= ~(1 << 6))
#define WE_NEGATED()  (PORTE |=  (1 << 7))
#define WE_ASSERTED() (PORTE &= ~(1 << 7))
#define CE_NEGATED()  (PORTE |=  (1 << 0))
#define CE_ASSERTED() (PORTE &= ~(1 << 0))

static void set_address(uint32_t addr) {
    PORTD = addr & 0xFF;
    PORTC = (addr >> 8) & 0xFF;
    PORTB = (PORTB & ~0x07) | ((addr >> 16) & 0x07);
}

static void write_byte(uint32_t addr, uint8_t data) {
    OE_NEGATED();                // /OE deasserted before driving data bus
    PORTA = data;
    DDRA  = 0xFF;           // data bus -> output
    set_address(addr);
    WE_ASSERTED();
    __asm__ __volatile__("nop\nnop\nnop\nnop"); // >=70 ns tWP
    WE_NEGATED();                // data latched on rising /WE
}

static uint8_t read_byte(uint32_t addr) {
    DDRA  = 0x00;           // data bus -> input BEFORE asserting /OE
    PORTA = 0x00;           // no pull-ups
    WE_NEGATED();
    set_address(addr);
    OE_ASSERTED();
    __asm__ __volatile__("nop\nnop\nnop\nnop\nnop\nnop"); // ~375 ns tACC
    uint8_t data = PINA;
    OE_NEGATED();
    return data;
}

// Waits up to 5 s for the next byte; returns false on timeout.
static bool recv_byte(uint8_t *out) {
    unsigned long t = millis();
    while (!Serial.available()) {
        if (millis() - t > 5000) return false;
    }
    *out = (uint8_t)Serial.read();
    return true;
}

// Reads a 4-byte little-endian length prefix.  Blocks indefinitely on the
// first byte; uses the 5 s timeout for the remaining three.
static bool recv_length(uint32_t *out) {
    while (!Serial.available());
    uint8_t b[4];
    b[0] = (uint8_t)Serial.read();
    for (uint8_t i = 1; i < 4; i++) {
        if (!recv_byte(&b[i])) return false;
    }
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return true;
}

void setup() {
    Serial.begin(115200);

    // Set control line states BEFORE enabling outputs to avoid spurious pulses
    // (port regs reset to 0x00, which would glitch /WE and /OE low).
    PORTE |= (1 << 6) | (1 << 7); // /OE=1, /WE=1 (deasserted)
    PORTE &= ~(1 << 0);            // /CE=0 (asserted — held low for entire session)
    DDRE  |= (1 << 0) | (1 << 6) | (1 << 7); // PE0, PE6, PE7 -> outputs

    PORTA = 0x00; DDRA = 0x00;    // data bus: input, no pull-ups
    PORTD = 0x00; DDRD = 0xFF;    // A[7:0] -> output
    PORTC = 0x00; DDRC = 0xFF;    // A[15:8] -> output
    PORTB &= ~0x07; DDRB |= 0x07; // A[18:16] -> output
}

void loop() {
    const uint32_t MAX_SIZE = 512UL * 1024UL;

    uint32_t length;
    if (!recv_length(&length) || length == 0 || length > MAX_SIZE) {
        Serial.println("Error: bad length.");
        return;
    }

    Serial.print("Writing "); Serial.print(length); Serial.println(" bytes...");

    uint32_t errors = 0;
    for (uint32_t addr = 0; addr < length; addr++) {
        uint8_t b;
        if (!recv_byte(&b)) {
            Serial.println("\nTimeout.");
            return;
        }

        write_byte(addr, b);
        uint8_t rb = read_byte(addr);

        if (rb != b) {
            if (errors < 10) {
                Serial.println();
                Serial.print("MISMATCH @0x"); Serial.print(addr, HEX);
                Serial.print(": wrote 0x"); Serial.print(b, HEX);
                Serial.print(" read 0x"); Serial.println(rb, HEX);
            }
            errors++;
        }

        if (addr % 1024 == 0) {
            Serial.print('.');
            if (addr % (64UL * 1024) == 0 && addr > 0) Serial.println();
        }
    }

    Serial.println();
    if (errors > 0) {
        Serial.print(errors); Serial.println(" errors — FAIL");
    } else {
        Serial.print("Wrote "); Serial.print(length); Serial.println(" bytes — PASS");
    }
}
