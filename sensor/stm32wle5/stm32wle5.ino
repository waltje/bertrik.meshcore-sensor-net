#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>
#include <RadioLib.h>
#include <MiniShell.h>

#define printf Serial.printf

// LoRa settings
#define LORA_CARRIER_FREQ 869.618
#define LORA_BANDWIDTH    62.5
#define LORA_SF           8
#define LORA_CR           8
#define LORA_SYNC_WORD    0x12
#define LORA_POWER        22
#define LORA_PREAMBLE     16
#define LORA_USE_CRC      true

static MiniShell shell(&Serial);
static STM32WLx radio = new STM32WLx_Module();
static volatile bool rf_received = false;
static uint8_t rf_buffer[256];

static void set_rf_recv_flag(void)
{
    rf_received = true;
}

static bool lora_init(void)
{
    radio.setRfSwitchPins(PA4, PA5);
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
    radio.setPacketReceivedAction(set_rf_recv_flag);

    return true;
}

static int do_init(int argc, char *argv[])
{
    printf("Initialise LoRa\n");
    lora_init();
    return 0;
}

static int do_receive(int argc, char *argv[])
{
    printf("Start receiving...\n");
    int16_t result = radio.startReceive();
    return result;
}

const cmd_t commands[] = {
    { "init", do_init, "Initialise the radio" },
    { "receive", do_receive, "Start receiving" },
    { NULL, NULL, NULL }
};

void setup(void)
{
    Serial.begin(115200);
    if (lora_init()) {
        radio.startReceive();
    } else {
        printf("lora_init failed!\n");
    }
}

void loop(void)
{
    shell.process(">", commands);

    // check radio
    if (rf_received) {
        int num_bytes = radio.getPacketLength();
        int state = radio.readData(rf_buffer, num_bytes);
        printf("Got %d bytes\n", num_bytes);
        rf_received = false;
    }
}
