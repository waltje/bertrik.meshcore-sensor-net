#include <stdint.h>
#include <stdbool.h>
#include <atomic>

#include <Arduino.h>
#include <LittleFS.h>

#include <RadioLib.h>
#include <MiniShell.h>

#define printf Serial.printf

// LoRa settings
#define LORA_CARRIER_FREQ 869.618
#define LORA_BANDWIDTH 62.5
#define LORA_SF 8
#define LORA_CR 8
#define LORA_SYNC_WORD 0x12
#define LORA_POWER 22
#define LORA_PREAMBLE 16
#define LORA_USE_CRC true

static MiniShell shell(&Serial);
static SX1262 radio = new Module(41, 39, 42, 40);
static std::atomic_bool rf_event;
static uint8_t rf_buffer[256];

static void handle_radio_interrupt(void)
{
    rf_event = true;
}

static void printhex(const char *title, const uint8_t *buf, size_t len, int rowsize = 16)
{
    printf("%s", title);
    for (size_t i = 0; i < len; i++) {
        if ((rowsize > 0) && (i % rowsize) == 0) {
            printf("\n%04X:", i);
        }
        printf(" %02X", buf[i]);
    }
    printf("\n");
}

static bool lora_init(void)
{
    int16_t result = radio.begin(LORA_CARRIER_FREQ);
    if (result < 0) {
        return false;
    }
    radio.setSpreadingFactor(LORA_SF);
    radio.setBandwidth(LORA_BANDWIDTH);
    radio.setCodingRate(LORA_CR);
    radio.setSyncWord(LORA_SYNC_WORD);
    radio.setPreambleLength(LORA_PREAMBLE);
    radio.explicitHeader();
    radio.setCRC(1);
    radio.invertIQ(false);

    rf_event = false;
    radio.setDio1Action(handle_radio_interrupt);
    radio.startReceive();
    return true;
}

static int do_reboot(int argc, char *arg[])
{
    ESP.restart();
    return 0;
}

const cmd_t commands[] = {
    { "reboot", do_reboot, "Reboot" },
    { NULL, NULL, NULL }
};

void setup(void)
{
    Serial.begin(115200);
    printf("Hello gateway!\n");

    printf("lora_init()...");
    if (!lora_init()) {
        printf("FAILED!\n");
    } else {
        printf("OK!\n");
    }
}

void loop(void)
{
    shell.process(">", commands);

    // check radio
    if (rf_event.exchange(false)) {
        uint32_t irq_status = radio.getIrqFlags();

        // handle receive
        if (irq_status & RADIOLIB_SX126X_IRQ_RX_DONE) {
            int num_bytes = radio.getPacketLength();
            radio.readData(rf_buffer, num_bytes);
            int rssi = radio.getRSSI();
            int snr = radio.getSNR();
            printf("### Got %d bytes (RSSI: %d, SNR: %d), ", num_bytes, rssi, snr);
            printhex("Raw:", rf_buffer, num_bytes, 0);
        }
        // handle transmit
        if (irq_status & RADIOLIB_SX126X_IRQ_TX_DONE) {
            radio.finishTransmit();
        }
        // clear all interrupts
        radio.clearIrqFlags(irq_status);

        // restart receive
        radio.startReceive();
    }
}
