#include <Arduino.h>

// Pin mapping (Teensy++ 2.0 / AT90USB1286):
//   A[7:0]  -> PORTD
//   A[15:8] -> PORTC
//   A[18:16]-> PORTB[2:0]
//   D[7:0]  -> PORTF (physically bit-reversed wiring)
//   /CE     -> PORTA[0]  (per-byte strobe)
//   /WE     -> PORTA[2]  (held for write burst)
//   /RESET  -> PORTA[4]  (asserted 500 ms after write+verify to restart 68030)
//   /BUSRQ  -> PORTA[5]  (assert to request 68030 bus)
//   /BUSACK -> PORTA[6]  (input: low when 68030 has granted bus)
//   /OE     -> tied low on board

#define CE_BIT     (1 << 0)
#define WE_BIT     (1 << 2)
#define RESET_BIT  (1 << 4)
#define BUSRQ_BIT  (1 << 5)
#define BUSACK_BIT (1 << 6)

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

// Assert /BUSRQ and wait up to 1 s for /BUSACK low, then drive all bus pins.
static bool acquire_bus() {
    PORTA &= ~BUSRQ_BIT;
    unsigned long t = millis();
    while (PINA & BUSACK_BIT) {
        if (millis() - t > 1000) {
            PORTA |= BUSRQ_BIT;
            return false;
        }
    }
    PORTA |=  (CE_BIT | WE_BIT);
    DDRA  |=  (CE_BIT | WE_BIT);
    DIDR0  =  0x00;
    PORTF  =  0x00; DDRF  =  0x00;
    PORTD  =  0x00; DDRD  =  0xFF;
    PORTC  =  0x00; DDRC  =  0xFF;
    PORTB &= ~0x07; DDRB |=  0x07;
    return true;
}

// Tri-state all bus pins and deassert /BUSRQ.
static void relinquish_bus() {
    WE_NEGATED();
    CE_NEGATED();
    DDRF   =  0x00; PORTF  =  0x00;
    DDRD   =  0x00; PORTD  =  0x00;
    DDRC   =  0x00; PORTC  =  0x00;
    DDRB  &= ~0x07; PORTB &= ~0x07;
    DDRA  &= ~(CE_BIT | WE_BIT);
    PORTA &= ~(CE_BIT | WE_BIT);
    PORTA |=  BUSRQ_BIT;
}

void setup() {
    Serial.begin(115200);
    // /RESET and /BUSRQ are the only outputs at startup; all bus pins tri-state.
    PORTA |= (RESET_BIT | BUSRQ_BIT);
    DDRA  |= (RESET_BIT | BUSRQ_BIT);
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
