#include <Arduino.h>

// Pin mapping (Teensy++ 2.0 / AT90USB1286):
//   A[7:0]  -> PORTD
//   A[15:8] -> PORTC
//   A[18:16]-> PORTB[2:0]
//   D[7:0]  -> PORTF (physically bit-reversed wiring)
//   /AS     -> PORTA[0]  (per-byte strobe)
//   /WE     -> PORTA[2]  (held for write burst)
//   FC0     -> PORTA[3]  (function code, driven 0 while bus owned)
//   FC1     -> PORTA[4]  (function code, driven 0 while bus owned)
//   FC2     -> PORTA[5]  (function code, driven 0 while bus owned)
//   /BR     -> PORTE[0]  (output: assert to request 68030 bus)
//   /BGACK  -> PORTE[1]  (output: assert to acknowledge bus grant)
//   /RESET  -> PORTE[6]  (asserted 500 ms after write+verify to restart 68030)
//   /BG     -> PORTE[7]  (input: 68030 asserts to grant bus)
//   /OE     -> tied low on board

// PORTA bits
#define AS_BIT    (1 << 0)
#define WE_BIT    (1 << 2)
#define FC0_BIT   (1 << 3)
#define FC1_BIT   (1 << 4)
#define FC2_BIT   (1 << 5)
#define FC_BITS   (FC0_BIT | FC1_BIT | FC2_BIT)

// PORTE bits
#define BR_BIT    (1 << 0)
#define BGACK_BIT (1 << 1)
#define RESET_BIT (1 << 6)
#define BG_BIT    (1 << 7)

#define WE_NEGATED()  (PORTA |=  WE_BIT)
#define WE_ASSERTED() (PORTA &= ~WE_BIT)
#define AS_NEGATED()  (PORTA |=  AS_BIT)
#define AS_ASSERTED() (PORTA &= ~AS_BIT)

static void set_address(uint32_t addr) {
    PORTD = addr & 0xFF;
    PORTC = (addr >> 8) & 0xFF;
    PORTB = (PORTB & ~0x07) | ((addr >> 16) & 0x07);
}

#define NOP6() __asm__ __volatile__("nop\nnop\nnop\nnop\nnop\nnop")

static void write_byte(uint32_t addr, uint8_t data) {
    PORTF = data;
    set_address(addr);
    AS_ASSERTED();
    NOP6();
    AS_NEGATED();
    NOP6();
}

static uint8_t read_byte(uint32_t addr) {
    set_address(addr);
    AS_ASSERTED();
    NOP6();
    uint8_t data = PINF;
    AS_NEGATED();
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
//   5. Drive all bus pins; FC0/FC1/FC2 driven to 0
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
    PORTA  = (PORTA & ~FC_BITS) | AS_BIT | WE_BIT;  // FC=0, /AS=1, /WE=1
    DDRA  |= (AS_BIT | WE_BIT | FC_BITS);
    DIDR0  =  0x00;
    PORTF  =  0x00; DDRF  =  0x00;
    PORTD  =  0x00; DDRD  =  0xFF;
    PORTC  =  0x00; DDRC  =  0xFF;
    PORTB &= ~0x07; DDRB |=  0x07;
    return true;
}

// Tri-state all bus pins (including FC0/FC1/FC2) then negate /BGACK.
static void relinquish_bus() {
    WE_NEGATED();
    AS_NEGATED();
    DDRF   =  0x00; PORTF  =  0x00;
    DDRD   =  0x00; PORTD  =  0x00;
    DDRC   =  0x00; PORTC  =  0x00;
    DDRB  &= ~0x07; PORTB &= ~0x07;
    DDRA  &= ~(AS_BIT | WE_BIT | FC_BITS);
    PORTA &= ~(AS_BIT | WE_BIT | FC_BITS);
    PORTE |=  BGACK_BIT;             // negate /BGACK — bus returned to 68030
}

void setup() {
    Serial.begin(115200);
    // /BR, /BGACK, /RESET are always outputs; /BG is always input.
    // FC0/FC1/FC2 and all bus pins remain tri-state until acquire_bus().
    PORTE |= (BR_BIT | BGACK_BIT | RESET_BIT);
    DDRE  |= (BR_BIT | BGACK_BIT | RESET_BIT);
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

        PORTE &= ~RESET_BIT;  // assert /RESET
        delay(500);
        PORTE |=  RESET_BIT;  // deassert /RESET

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
