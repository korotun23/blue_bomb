#include "nrf24_scanner.h"

#include <SPI.h>
#include <string.h>

// Pilote minimal "registres" du nRF24L01+ : seul ce qu'il faut pour écouter un canal et lire RPD.
// Pas de bibliothèque externe : on contrôle exactement les temps et les octets échangés.

// ----------------------------------------------------------------- nRF24L01+
#define CMD_W_REGISTER   0x20
#define CMD_FLUSH_RX     0xE2
#define REG_CONFIG       0x00
#define REG_EN_AA        0x01
#define REG_SETUP_RETR   0x04
#define REG_RF_CH        0x05
#define REG_RF_SETUP     0x06
#define REG_STATUS       0x07
#define REG_RPD          0x09

#define CONFIG_PWR_UP    0x02
#define CONFIG_PRIM_RX   0x01        // réception ; EN_CRC reste à 0 (inutile ici)

#if   NRF_DATA_RATE == 0
  #define RF_SETUP_VALUE 0x20        // RF_DR_LOW = 1 : 250 kbit/s
#elif NRF_DATA_RATE == 1
  #define RF_SETUP_VALUE 0x00        // 1 Mbit/s
#else
  #define RF_SETUP_VALUE 0x08        // RF_DR_HIGH = 1 : 2 Mbit/s
#endif

#define FRAME_PERIOD_MS  250
#define POWER_UP_MS      5           // démarrage de l'oscillateur après PWR_UP (4,5 ms au maximum)

struct Module {
    uint8_t csn;
    uint8_t ce;
};

static const Module kCandidates[SPEC_MAX_MODULES] = {
    { NRF_PIN_CSN_A, NRF_PIN_CE_A },
    { NRF_PIN_CSN_B, NRF_PIN_CE_B },
};

static Module            s_active[SPEC_MAX_MODULES];   // modules détectés, sans trou
static uint8_t           s_count   = 0;
static SPIClass         *s_spi     = nullptr;
static QueueHandle_t     s_queue   = nullptr;
static volatile bool     s_run     = false;

static const SPISettings kSpi(8000000, MSBFIRST, SPI_MODE0);   // le nRF24L01+ accepte 10 MHz au plus

// ------------------------------------------------------------ accès aux registres
// À appeler à l'intérieur d'une transaction SPI.

static uint8_t read_reg(const Module &m, uint8_t reg)
{
    digitalWrite(m.csn, LOW);
    s_spi->transfer(reg & 0x1F);              // R_REGISTER = 0x00 | adresse
    uint8_t v = s_spi->transfer(0xFF);
    digitalWrite(m.csn, HIGH);
    return v;
}

static void write_reg(const Module &m, uint8_t reg, uint8_t value)
{
    digitalWrite(m.csn, LOW);
    s_spi->transfer(CMD_W_REGISTER | (reg & 0x1F));
    s_spi->transfer(value);
    digitalWrite(m.csn, HIGH);
}

static void command(const Module &m, uint8_t cmd)
{
    digitalWrite(m.csn, LOW);
    s_spi->transfer(cmd);
    digitalWrite(m.csn, HIGH);
}

// Un module absent lit 0x00 ou 0xFF sur MISO : l'écriture/relecture de deux valeurs différentes échoue.
static bool probe(const Module &m)
{
    write_reg(m, REG_RF_CH, 0x2A);
    uint8_t a = read_reg(m, REG_RF_CH);
    write_reg(m, REG_RF_CH, 0x15);
    uint8_t b = read_reg(m, REG_RF_CH);
    return a == 0x2A && b == 0x15;
}

// ------------------------------------------------------------ alimentation

static void power_up_all()
{
    s_spi->beginTransaction(kSpi);
    for (uint8_t k = 0; k < s_count; k++) {
        const Module &m = s_active[k];
        digitalWrite(m.ce, LOW);
        write_reg(m, REG_CONFIG, CONFIG_PWR_UP | CONFIG_PRIM_RX);
        write_reg(m, REG_EN_AA, 0x00);              // jamais d'ACK : le module ne transmet pas
        write_reg(m, REG_SETUP_RETR, 0x00);
        write_reg(m, REG_RF_SETUP, RF_SETUP_VALUE);
        write_reg(m, REG_STATUS, 0x70);             // efface les drapeaux d'interruption
        command(m, CMD_FLUSH_RX);
    }
    s_spi->endTransaction();
    vTaskDelay(pdMS_TO_TICKS(POWER_UP_MS));
}

static void power_down_all()
{
    s_spi->beginTransaction(kSpi);
    for (uint8_t k = 0; k < s_count; k++) {
        digitalWrite(s_active[k].ce, LOW);
        write_reg(s_active[k], REG_CONFIG, 0x00);   // PWR_UP = 0 : consommation minimale
    }
    s_spi->endTransaction();
}

// ------------------------------------------------------------ balayage

// Un balayage complet : chaque module parcourt sa part des canaux, tous les modules en parallèle.
// Avec 2 modules : le module 0 fait les canaux 0..41, le module 1 les canaux 42..83.
static void sweep_once(uint16_t *hits)
{
    const uint8_t per = SPEC_CHANNELS / s_count;    // 84 ou 42 : divise exactement pour 1 ou 2 modules

    s_spi->beginTransaction(kSpi);
    for (uint8_t step = 0; step < per; step++) {
        for (uint8_t k = 0; k < s_count; k++) {
            write_reg(s_active[k], REG_RF_CH, k * per + step);
        }
        for (uint8_t k = 0; k < s_count; k++) digitalWrite(s_active[k].ce, HIGH);   // écoute
        delayMicroseconds(NRF_SETTLE_US);
        for (uint8_t k = 0; k < s_count; k++) digitalWrite(s_active[k].ce, LOW);    // fin de l'écoute
        // Comme l'exemple "scanner" de RF24 (startListening, attente, stopListening, testRPD) :
        // RPD se lit APRÈS la fin de l'écoute ; il garde le résultat de la période d'écoute écoulée.
        // Lu pendant l'écoute (CE haut), il ne donne rien d'exploitable sur certaines puces.
        for (uint8_t k = 0; k < s_count; k++) {
            if (read_reg(s_active[k], REG_RPD) & 0x01) hits[k * per + step]++;
        }
    }
    // Des paquets parasites ont pu remplir la FIFO de réception : on la vide
    for (uint8_t k = 0; k < s_count; k++) command(s_active[k], CMD_FLUSH_RX);
    s_spi->endTransaction();
}

#if NRF_DEBUG
// Trace série (1 par seconde) : permet de vérifier que les puces sont bien en réception et que RPD réagit
static void debug_print(uint32_t totalHits, uint16_t sweepsPerSec)
{
    Serial.printf("[nrf] %u balayages/s, detections RPD (somme des canaux) = %u\n", (unsigned)sweepsPerSec, (unsigned)totalHits);
    s_spi->beginTransaction(kSpi);
    for (uint8_t k = 0; k < s_count; k++) {
        const Module &m = s_active[k];
        Serial.printf("[nrf]   module %u (CSN %u, CE %u) : CONFIG=0x%02X (attendu 0x03)  RF_SETUP=0x%02X (attendu 0x%02X)  STATUS=0x%02X\n",
                      (unsigned)k, (unsigned)m.csn, (unsigned)m.ce, (unsigned)read_reg(m, REG_CONFIG),
                      (unsigned)read_reg(m, REG_RF_SETUP), (unsigned)RF_SETUP_VALUE, (unsigned)read_reg(m, REG_STATUS));
    }
    s_spi->endTransaction();
}
#endif

static void spectrum_task(void *)
{
    static uint16_t hits[SPEC_CHANNELS];
    uint16_t sweeps      = 0;
    uint32_t windowStart = 0;
    bool     active      = false;

    for (;;) {
        if (!s_run) {
            if (active) { power_down_all(); active = false; }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!active) {
            power_up_all();
            memset(hits, 0, sizeof(hits));
            sweeps      = 0;
            windowStart = millis();
            active      = true;
        }

        sweep_once(hits);
        sweeps++;

        uint32_t elapsed = millis() - windowStart;
        if (elapsed >= FRAME_PERIOD_MS) {
            SpectrumFrame frame;
            for (int i = 0; i < SPEC_CHANNELS; i++) {
                uint32_t pct = (uint32_t)hits[i] * 100 / sweeps;
                frame.level[i] = pct > 100 ? 100 : (uint8_t)pct;
            }
            frame.sweepsPerSec = (uint16_t)((uint32_t)sweeps * 1000 / (elapsed ? elapsed : 1));
            frame.modules      = s_count;
            if (s_queue) xQueueOverwrite(s_queue, &frame);   // la dernière trame écrase l'ancienne
#if NRF_DEBUG
            static uint32_t lastDebug = 0;
            if (millis() - lastDebug >= 1000) {
                uint32_t total = 0;
                for (int i = 0; i < SPEC_CHANNELS; i++) total += hits[i];
                debug_print(total, frame.sweepsPerSec);
                lastDebug = millis();
            }
#endif

            memset(hits, 0, sizeof(hits));
            sweeps      = 0;
            windowStart = millis();
        }

        vTaskDelay(1);   // rend la main (watchdog du cœur 0, tâches Bluetooth)
    }
}

// ------------------------------------------------------------------ API

uint8_t SpectrumScanner::begin(QueueHandle_t frameQueue)
{
    s_queue = frameQueue;

    // Les broches CSN/CE avant toute transaction : CSN au repos à l'état haut, CE à l'état bas
    for (uint8_t k = 0; k < SPEC_MAX_MODULES; k++) {
        pinMode(kCandidates[k].csn, OUTPUT);
        digitalWrite(kCandidates[k].csn, HIGH);
        pinMode(kCandidates[k].ce, OUTPUT);
        digitalWrite(kCandidates[k].ce, LOW);
    }

    // HSPI : bus distinct de celui de l'écran (VSPI), pour ne jamais se disputer le bus avec LVGL
    s_spi = new SPIClass(HSPI);
    s_spi->begin(NRF_PIN_SCK, NRF_PIN_MISO, NRF_PIN_MOSI, -1);

    s_count = 0;
    s_spi->beginTransaction(kSpi);
    for (uint8_t k = 0; k < SPEC_MAX_MODULES; k++) {
        if (probe(kCandidates[k])) s_active[s_count++] = kCandidates[k];
    }
    s_spi->endTransaction();

    if (s_count > 0) {
        xTaskCreatePinnedToCore(spectrum_task, "spectrum", 4096, nullptr, 2, nullptr, 0);
    }
    return s_count;
}

void SpectrumScanner::start() { if (s_count > 0) s_run = true; }
void SpectrumScanner::stop()  { s_run = false; }
bool SpectrumScanner::running() { return s_run; }
uint8_t SpectrumScanner::modules() { return s_count; }
