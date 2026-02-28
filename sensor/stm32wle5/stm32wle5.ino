#include <stdint.h>
#include <stdbool.h>
#include <atomic>

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
    uint32_t counter;
} nvdata_t;

static MiniShell shell(&Serial);
static STM32WLx radio = new STM32WLx_Module();
static uint8_t rf_buffer[256];
static nvdata_t nvdata;
static uint8_t device_id[4];
static std::atomic_bool rf_event {false};

static void handle_radio_interrupt(void)
{
    rf_event = true;
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
    radio.setDio1Action(handle_radio_interrupt);

    return true;
}

static size_t put_u32(uint8_t *buf, uint32_t value)
{
    uint8_t *ptr = buf;
    *ptr++ = value >> 24;
    *ptr++ = value >> 16;
    *ptr++ = value >> 8;
    *ptr++ = value;
    return ptr - buf;
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

static int do_send(int argc, char *argv[])
{
    BLAKE2s blake;
    uint8_t buf[255];

    if (argc < 2) {
        return -1;
    }
    int len = atoi(argv[1]);
    if (len >= sizeof(buf)) {
        return -2;
    }
    printf("Sending %d bytes...\n", len);

    // build payload
    uint8_t *ptr = buf;
    memcpy(ptr, device_id, 4);
    ptr += 4;
    ptr += put_u32(ptr, nvdata.counter++);
    memset(ptr, len, len);
    ptr += len;
    blake.reset(nvdata.key, 32);
    blake.update(buf, ptr - buf);
    blake.finalize(ptr, 4);
    ptr += 4;
    int buf_len = ptr - buf;
    printhex("Payload", buf, buf_len);

    // build rf buffer
    ptr = rf_buffer;
    *ptr++ = (0x0 << 6) | (0xF << 2) | (0x1 << 0);      // version(0) | rawcustom(0xF) | flood (=1)
    *ptr++ = 0;                 // path
    memcpy(ptr, buf, buf_len);
    ptr += buf_len;

    // transmit
    int rf_len = ptr - rf_buffer;
    printhex("Transmit", rf_buffer, rf_len);
    int16_t result = radio.startTransmit(rf_buffer, rf_len);
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

    // copy to nvdata
    memcpy(nvdata.key, device_key, 32);
    return 0;
}

static int do_reboot(int argc, char *argv[])
{
    NVIC_SystemReset();
    return 0;
}

static int do_save(int argc, char *argv[])
{
    printf("Saving ...");
    // save nvdata to flash
    EEPROM.put(0, nvdata);
    printf("OK\n");
    return 0;
}

const cmd_t commands[] = {
    { "init", do_init, "Initialise the radio" },
    { "receive", do_receive, "Start receiving" },
    { "send", do_send, "<length> Send data payload" },
    { "key", do_key, "<name> <secret> Create key" },
    { "save", do_save, "Save non-volatile data" },
    { "reboot", do_reboot, "Reboot" },
    { NULL, NULL, NULL }
};

void setup(void)
{
    Serial.begin(115200);
    printf("\nSTM32WLE5 started\n");

    // get device id
    uint32_t deviceid = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    put_u32(device_id, deviceid);

    // restore nvdata from flash
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
    if (rf_event.exchange(false)) {
        uint32_t irq_status = radio.getIrqFlags();

        // handle receive
        if (irq_status & RADIOLIB_SX126X_IRQ_RX_DONE) {
            int num_bytes = radio.getPacketLength();
            radio.readData(rf_buffer, num_bytes);
            printf("Got %d bytes, ", num_bytes);
            printhex("data:", rf_buffer, num_bytes);
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
