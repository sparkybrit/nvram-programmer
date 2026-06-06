#include <Arduino.h>

// Pin mapping (Teensy++ 2.0 / AT90USB1286):
//   A[7:0]  -> PORTD
//   A[15:8] -> PORTC
//   A[18:16]-> PORTB[2:0]
//   D[7:0]  -> PORTF (physically bit-reversed wiring)
//   /CE     -> PORTA[0]  (per-byte strobe)
//   /WE     -> PORTA[2]  (held for write burst)
//   /RESET  -> PORTA[4]  (asserted 500 ms after write+verify to restart 68030)
//   /BR     -> PORTE[0]  (output: assert to request 68030 bus)
//   /BGACK  -> PORTE[1]  (output: assert to acknowledge bus grant)
//   /BG     -> PORTE[7]  (input: 68030 asserts to grant bus)
//   /OE     -> tied low on board

#define CE_BIT    (1 << 0)
#define WE_BIT    (1 << 2)
#define RESET_BIT (1 << 4)

#define BR_BIT    (1 << 0)
#define BGACK_BIT (1 << 1)
#define BG_BIT    (1 << 7)

#define WE_NEGATED()  (PORTA |=  WE_BIT)
#define WE_ASSERTED() (PORTA &= ~WE_BIT)
#define CE_NEGATED()  (PORTA |=  CE_BIT)
#define CE_ASSERTED() (PORTA &= ~CE_BIT)

static void set_address(uint32_t addr) {
    PORTD = addr & 0xFF;
    PORTC = (addr >> 8) & 0xFF;
    PORTB = (PORTB & ~0x07) | ((addr >> 16) & 0x07);
}

#define NOP6() __asm__ __volatile__("nop\nnop\nnop\nnop\nnop\nnop")

static void write_byte(uint32_t addr, uint8_t data) {
    PORTF = data;
    set_address(addr);
    CE_ASSERTED();
    NOP6();
    CE_NEGATED();
    NOP6();
}

static uint8_t read_byte(uint32_t addr) {
    set_address(addr);
    CE_ASSERTED();
    NOP6();
    uint8_t data = PINF;
    CE_NEGATED();
    NOP6();
    return data;
}

static bool recv_byte(uint8_t *out) {
    unsigned long t = millis();
    while (!Serial.available()) {
        if (millis() - t > 5000) return false;
    }
    *out = (uint8_t)Serial.read();
    return true;
}

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

// 68030 bus arbitration (acquire):
//   1. Assert /BR  — request the bus
//   2. Wait for /BG low  — CPU grants bus
//   3. Assert /BGACK  — acknowledge grant
//   4. Negate /BR  — release request line
//   5. Drive all bus pins
static bool acquire_bus() {
    PORTE &= ~BR_BIT;                // assert /BR
    unsigned long t = millis();
    while (PINE & BG_BIT) {          // wait for /BG low
        if (millis() - t > 1000) {
            PORTE |= BR_BIT;         // deassert /BR on timeout
            return false;
        }
    }
    PORTE &= ~BGACK_BIT;             // assert /BGACK
    PORTE |=  BR_BIT;                // negate /BR
    PORTA |=  (CE_BIT | WE_BIT);
    DDRA  |=  (CE_BIT | WE_BIT);
    DIDR0  =  0x00;
    PORTF  =  0x00; DDRF  =  0x00;
    PORTD  =  0x00; DDRD  =  0xFF;
    PORTC  =  0x00; DDRC  =  0xFF;
    PORTB &= ~0x07; DDRB |=  0x07;
    return true;
}

// Tri-state all bus pins then negate /BGACK to return bus to 68030.
static void relinquish_bus() {
    WE_NEGATED();
    CE_NEGATED();
    DDRF   =  0x00; PORTF  =  0x00;
    DDRD   =  0x00; PORTD  =  0x00;
    DDRC   =  0x00; PORTC  =  0x00;
    DDRB  &= ~0x07; PORTB &= ~0x07;
    DDRA  &= ~(CE_BIT | WE_BIT);
    PORTA &= ~(CE_BIT | WE_BIT);
    PORTE |=  BGACK_BIT;             // negate /BGACK — bus returned to 68030
}

void setup() {
    Serial.begin(115200);
    // /BR, /BGACK, /RESET are always outputs; /BG is always input.
    // All bus pins (address, data, /CE, /WE) remain tri-state until acquire_bus().
    PORTE |= (BR_BIT | BGACK_BIT);   // /BR=1, /BGACK=1 (both deasserted)
    DDRE  |= (BR_BIT | BGACK_BIT);
    // /BG (E7) stays as input — no DDR change needed
    PORTA |=  RESET_BIT;             // /RESET=1 (deasserted)
    DDRA  |=  RESET_BIT;
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
        if (!acquire_bus()) {
            Serial.println("Error: bus acquire timeout.");
            return;
        }

        WE_ASSERTED();
        DDRF = 0xFF;

        for (uint32_t addr = 0; addr < length; addr++) {
            uint8_t b;
            if (!recv_byte(&b)) {
                WE_NEGATED();
                DDRF = 0x00;
                relinquish_bus();
                Serial.println("Timeout.");
                return;
            }
            write_byte(addr, b);
        }

        WE_NEGATED();
        DDRF = 0x00;

        // Stream read-back to host for verification.
        for (uint32_t addr = 0; addr < length; addr++)
            Serial.write(read_byte(addr));

        relinquish_bus();

        PORTA &= ~RESET_BIT;  // assert /RESET
        delay(500);
        PORTA |=  RESET_BIT;  // deassert /RESET

        Serial.println("Done.");

    } else if (cmd == 'R') {
        if (!acquire_bus()) {
            Serial.println("Error: bus acquire timeout.");
            return;
        }

        PORTF = 0x00; DDRF = 0x00;
        for (uint32_t addr = 0; addr < length; addr++)
            Serial.write(read_byte(addr));

        relinquish_bus();

    } else {
        Serial.println("Error: unknown command.");
    }
}
