#include <Arduino.h>

// Pin mapping (Teensy++ 2.0 / AT90USB1286):
//   A[7:0]  -> PORTD
//   A[15:8] -> PORTC
//   A18:16  -> PORTB[2:0]
//   D[7:0]  -> PORTF
//   /CE     -> PORTA[0]  (per-byte strobe)
//   /WE     -> PORTA[2]  (held for entire write burst)
//   /OE     -> tied low on board

#define WE_NEGATED()  (PORTA |=  (1 << 2))
#define WE_ASSERTED() (PORTA &= ~(1 << 2))
#define CE_NEGATED()  (PORTA |=  (1 << 0))
#define CE_ASSERTED() (PORTA &= ~(1 << 0))

static void set_address(uint32_t addr) {
    PORTD = addr & 0xFF;
    PORTC = (addr >> 8) & 0xFF;
    PORTB = (PORTB & ~0x07) | ((addr >> 16) & 0x07);
}

#define NOP6() __asm__ __volatile__("nop\nnop\nnop\nnop\nnop\nnop")

// Caller must: assert /WE, set DDRF=0xFF before the write burst.
static void write_byte(uint32_t addr, uint8_t data) {
    PORTF = data;
    set_address(addr);
    CE_ASSERTED();
    NOP6();
    CE_NEGATED();
    NOP6();
}

// Caller must: set DDRF=0x00 before the read burst.
static uint8_t read_byte(uint32_t addr) {
    set_address(addr);
    CE_ASSERTED();
    NOP6();
    uint8_t data = PINF;
    CE_NEGATED();
    NOP6();
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

    // Deassert /CE and /WE BEFORE enabling outputs to avoid spurious pulses.
    PORTA |= (1 << 0) | (1 << 2); // /CE=1, /WE=1
    DDRA  |= (1 << 0) | (1 << 2); // PA0, PA2 -> outputs

    DIDR0 = 0x00;                  // ensure digital input buffers enabled on PORTF (ADC pins)
    PORTF = 0x00; DDRF = 0x00;    // data bus: input, no pull-ups
    PORTD = 0x00; DDRD = 0xFF;    // A[7:0] -> output
    PORTC = 0x00; DDRC = 0xFF;    // A[15:8] -> output
    PORTB &= ~0x07; DDRB |= 0x07; // A[18:16] -> output
}

void loop() {
    const uint32_t MAX_SIZE = 512UL * 1024UL;

    while (!Serial.available());
    uint8_t cmd = (uint8_t)Serial.read();

    uint32_t length;
    if (!recv_length(&length) || length == 0 || length > MAX_SIZE) {
        Serial.println("Error: bad length.");
        return;
    }

    if (cmd == 'W') {
        WE_ASSERTED();
        DDRF = 0xFF;

        for (uint32_t addr = 0; addr < length; addr++) {
            uint8_t b;
            if (!recv_byte(&b)) {
                WE_NEGATED();
                DDRF = 0x00;
                Serial.println("Timeout.");
                return;
            }
            write_byte(addr, b);
        }

        WE_NEGATED();
        DDRF = 0x00;
        Serial.println("Written.");

    } else if (cmd == 'R') {
        PORTF = 0x00; DDRF = 0x00;

        for (uint32_t addr = 0; addr < length; addr++)
            Serial.write(read_byte(addr));

    } else {
        Serial.println("Error: unknown command.");
    }
}
