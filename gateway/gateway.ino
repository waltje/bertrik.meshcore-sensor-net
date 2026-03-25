#include <stdint.h>
#include <stdbool.h>
#include <atomic>

#include <Arduino.h>
#include <LittleFS.h>

#include <ESPAsyncWiFiManager.h>
#include <ESPmDNS.h>

#include <RadioLib.h>
#include <MiniShell.h>

#include "config.h"

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

static DNSServer dns;
static AsyncWebServer server(80);
static AsyncWiFiManager wifiManager(&server, &dns);

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

static int do_datetime(int argc, char *argv[])
{
    time_t now = time(NULL);
    struct tm *info = localtime(&now);

    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", info);
    printf("Date/time is now %s\n", buf);

    return 0;
}

const cmd_t commands[] = {
    { "reboot", do_reboot, "Reboot" },
    { "datetime", do_datetime, "Show current date/time" },
    { NULL, NULL, NULL }
};

void setup(void)
{
    Serial.begin(115200);
    printf("Hello gateway!\n");

    wifiManager.autoConnect("meshcore-gw");

    // NTP
    configTzTime("CET-1CEST,M3.5.0/02,M10.5.0/03", "nl.pool.ntp.org");

    printf("lora_init()...");
    if (!lora_init()) {
        printf("FAILED!\n");
    } else {
        printf("OK!\n");
    }

    LittleFS.begin(true);

    // load settings, save defaults if necessary
    config_begin(LittleFS, "/config.json");
    if (!config_load()) {
        config_set_value("mqtt_broker_host", "stofradar.nl");
        config_set_value("mqtt_broker_port", "1883");
        config_set_value("mqtt_user", "");
        config_set_value("mqtt_pass", "");
        config_set_value("mqtt_topic", "");
        config_set_value("lora_freq", "869.618");
        config_set_value("lora_bw", "62.5");
        config_set_value("lora_sf", "8");
        config_set_value("lora_cr", "8");
        config_set_value("lora_sync", "12");
        config_save();
    }
    config_serve(server, "/config", "/config.html");
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.begin();

    MDNS.begin("meshcore-gw");
    MDNS.addService("_http", "_tcp", 80);
}

void loop(void)
{
    // handle shell
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
