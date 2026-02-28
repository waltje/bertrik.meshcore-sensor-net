#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>
#include <EEPROM.h>

#include <RadioLib.h>
#include <MiniShell.h>
#include <Crypto.h>
#include <BLAKE2s.h>

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

// structure for non-volatile data, restored on bootup
typedef struct {
    uint8_t key[32];
} nvdata_t;

static MiniShell shell(&Serial);
static STM32WLx radio = new STM32WLx_Module();
static volatile bool rf_received = false;
static uint8_t rf_buffer[256];
static nvdata_t nvdata;
static uint8_t device_id[4];

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

static void printhex(const char *title, const uint8_t *buf, size_t len)
{
    printf("%s", title);
    for (size_t i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            printf("\n%04X:", i);
        }
        printf(" %02X", buf[i]);
    }
    printf("\n");
}

static int do_key(int argc, char *argv[])
{
    if (argc < 3) {
        printhex("Device id:", device_id, sizeof(device_id));
        printhex("Hash key:", nvdata.key, sizeof(nvdata.key));
        return 0;
    }

    char *name = argv[1];
    char *secret = argv[2];
    printf("name=%s, secret=%s\n", name, secret);

    BLAKE2s blake;
    uint8_t device_key[32];
    blake.reset(secret, strlen(secret));
    blake.update(name, strlen(name));
    blake.update(device_id, sizeof(device_id));
    blake.finalize(device_key, 32);
    printhex("Key", device_key, 32);

    // save nvdata to flash
    memcpy(nvdata.key, device_key, 32);
    EEPROM.put(0, nvdata);

    return 0;
}

static int do_reboot(int argc, char *argv[])
{
    NVIC_SystemReset();
    return 0;
}

const cmd_t commands[] = {
    { "init", do_init, "Initialise the radio" },
    { "receive", do_receive, "Start receiving" },
    { "key", do_key, "<name> <secret> Create key" },
    { "reboot", do_reboot, "Reboot" },
    { NULL, NULL, NULL }
};

void setup(void)
{
    Serial.begin(115200);
    printf("\nSTM32WLE5 started\n");

    // get device id
    uint32_t deviceid = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    device_id[0] = deviceid >> 24;
    device_id[1] = deviceid >> 16;
    device_id[2] = deviceid >> 8;
    device_id[3] = deviceid >> 0;

    // restore nvdata from flash
    EEPROM.begin();
    EEPROM.get(0, nvdata);

    // init radio
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
        printf("Got %3d bytes, ", num_bytes);
        printhex("data:", rf_buffer, num_bytes);
        rf_received = false;
    }
}
