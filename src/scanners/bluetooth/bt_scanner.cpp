#include "bt_scanner.h"

#include <string.h>
#include <stdio.h>

#include <esp32-hal-bt.h>   // btStart()

// Le cœur Arduino libère au démarrage la mémoire du contrôleur Bluetooth (BLE et/ou classique) quand
// rien ne déclare s'en servir ; esp_bt_controller_init() échoue ensuite avec ESP_ERR_INVALID_STATE.
//  - cœur Arduino-ESP32 3.3 et plus récents : ces deux en-têtes déclarent l'usage du BLE et du classique ;
//  - cœurs plus anciens : l'appel à btStart() ci-dessous suffit (ils n'ont pas ces en-têtes).
#if __has_include(<esp32-hal-alloc-ble-mem.h>)
#include <esp32-hal-alloc-ble-mem.h>
#endif
#if __has_include(<esp32-hal-alloc-bt-classic-mem.h>)
#include <esp32-hal-alloc-bt-classic-mem.h>
#endif
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gap_bt_api.h>

// Le BLE et le classique partagent la radio : on les enchaîne au lieu de les
// lancer en même temps (plus fiable, et la liste se remplit sans conflit).
//
//   IDLE -> BLE_SETUP -> BLE_SCAN -> CLASSIC -> IDLE
//
// Les callbacks s'exécutent dans la tâche Bluedroid, jamais dans loop() :
// ils ne font que remplir la queue (timeout 0, on ne bloque jamais la pile BT).

enum Phase : uint8_t {
    PHASE_IDLE,
    PHASE_BLE_SETUP,    // paramètres envoyés, on attend la confirmation
    PHASE_BLE_SCAN,
    PHASE_CLASSIC
};

// Après stopScan(), délai maximal laissé à la pile pour confirmer l'arrêt
#define STOP_GRACE_MS 4000UL

// Marge ajoutée à la durée prévue : la découverte des noms classiques, après
// l'inquiry proprement dite, peut prendre plusieurs secondes.
#define SCAN_GUARD_MS 20000UL

static QueueHandle_t  s_queue         = nullptr;
static volatile Phase s_phase         = PHASE_IDLE;
static uint32_t       s_bleSeconds    = 0;
static uint8_t        s_classicUnits  = 0;
static uint32_t       s_deadline      = 0;     // fin maximale du cycle en cours (garde-fou)
static volatile bool  s_repeat        = false; // enchaîner les cycles
static volatile bool  s_stop          = false; // arrêt demandé
static volatile uint16_t s_cycle      = 0;
static volatile uint32_t s_dropped    = 0;

// ------------------------------------------------------------------ utilitaires

static void addr_to_str(const uint8_t *a, char *out)   // out : 18 octets
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}

// Copie un nom reçu par radio vers un buffer sûr pour LVGL : ASCII imprimable
// uniquement (la police par défaut n'a pas les accents), tronqué, terminé par \0.
// Une suite d'octets non ASCII (un caractère UTF-8) devient un seul '?'.
static void copy_name(char *dst, size_t dstSize, const uint8_t *src, size_t len)
{
    size_t o = 0;
    bool lastWasReplaced = false;
    for (size_t i = 0; i < len && o + 1 < dstSize; i++) {
        uint8_t c = src[i];
        if (c == 0) break;
        if (c >= 0x20 && c < 0x7F) {
            dst[o++] = (char)c;
            lastWasReplaced = false;
        } else if (c >= 0x80) {
            if (!lastWasReplaced) { dst[o++] = '?'; lastWasReplaced = true; }
        }
        // les caractères de contrôle sont ignorés
    }
    dst[o] = '\0';
}

// Lit une suite de structures AD [longueur][type][données...] (annonces BLE
// et EIR du classique ont le même format). Remplit le nom et l'identifiant constructeur.
static void parse_ad(const uint8_t *data, size_t len, BtDeviceInfo &info)
{
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t l = data[i];
        if (l == 0) break;                    // fin des données (ou remplissage)
        if (i + 1 + l > len) break;           // structure tronquée : on s'arrête
        uint8_t type = data[i + 1];
        const uint8_t *p = data + i + 2;
        size_t pl = l - 1;

        switch (type) {
            case 0x09:                         // nom complet : prioritaire
                copy_name(info.name, sizeof(info.name), p, pl);
                break;
            case 0x08:                         // nom abrégé : seulement s'il n'y en a pas
                if (!info.name[0]) copy_name(info.name, sizeof(info.name), p, pl);
                break;
            case 0x02:                         // UUID de services 16 bits (liste incomplète)
            case 0x03:                         // idem (liste complète)
                for (size_t k = 0; k + 1 < pl; k += 2) btAddService(info, (uint16_t)(p[k] | (p[k + 1] << 8)));
                break;
            case 0x0A:                         // puissance d'émission annoncée
                if (pl >= 1) { info.txPower = (int8_t)p[0]; info.hasTxPower = true; }
                break;
            case 0xFF:                         // données constructeur : 2 premiers octets = identifiant (little-endian)
                if (pl >= 2 && !info.hasCompany) {
                    info.companyId  = (uint16_t)(p[0] | (p[1] << 8));
                    info.hasCompany = true;
                }
                break;
        }
        i += 1 + l;
    }
}

static void post(const BtDeviceInfo &info)
{
    if (s_queue && xQueueSend(s_queue, &info, 0) != pdTRUE) s_dropped++;   // file pleine : on ignore
}

// ------------------------------------------------------------ enchaînement

static void start_ble();
static void start_classic();

// Fin d'un cycle complet : on repart pour un autre ou on s'arrête
static void begin_cycle();
static void finish_cycle()
{
    if (s_repeat && !s_stop) begin_cycle();
    else                     s_phase = PHASE_IDLE;
}

// Fin de la phase BLE : on enchaîne avec le classique, sauf arrêt demandé
static void on_ble_finished()
{
    if (s_classicUnits && !s_stop) start_classic();
    else                           finish_cycle();
}

static void begin_cycle()
{
    s_cycle++;
    s_deadline = millis() + s_bleSeconds * 1000UL + s_classicUnits * 1280UL + SCAN_GUARD_MS;
    if (s_bleSeconds)        start_ble();
    else if (s_classicUnits) start_classic();
    else                     s_phase = PHASE_IDLE;
}

static void start_classic()
{
    s_phase = PHASE_CLASSIC;
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, s_classicUnits, 0) != ESP_OK) {
        s_stop = true;          // échec : on abandonne la session (évite une boucle d'erreurs)
        finish_cycle();
    }
}

static void start_ble()
{
    static esp_ble_scan_params_t params = {};
    params.scan_type          = BLE_SCAN_TYPE_ACTIVE;       // demande les scan responses (noms)
    params.own_addr_type      = BLE_ADDR_TYPE_PUBLIC;
    params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    params.scan_interval      = 0xA0;                       // 100 ms (unité 0,625 ms)
    params.scan_window        = 0xA0;                       // fenêtre = intervalle : écoute continue
    params.scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE; // tout remonter : l'application fusionne

    s_phase = PHASE_BLE_SETUP;
    if (esp_ble_gap_set_scan_params(&params) != ESP_OK) on_ble_finished();
}

// --------------------------------------------------------------- callbacks BLE

static void gap_ble_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (s_phase != PHASE_BLE_SETUP) break;
        if (s_stop) { on_ble_finished(); break; }         // arrêt demandé pendant la configuration
        s_phase = PHASE_BLE_SCAN;
        if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS ||
            esp_ble_gap_start_scanning(s_bleSeconds) != ESP_OK) {
            on_ble_finished();
        }
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (s_phase == PHASE_BLE_SCAN && param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            on_ble_finished();
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:               // arrêt manuel (stopScan)
        if (s_phase == PHASE_BLE_SCAN) on_ble_finished();
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        const auto &r = param->scan_rst;

        if (r.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            BtDeviceInfo info = {};
            info.type    = BT_TYPE_BLE;
            info.hasRssi = true;
            info.rssi    = (int8_t)r.rssi;
            addr_to_str(r.bda, info.address);
            info.addrType = (uint8_t)r.ble_addr_type;
            switch ((int)r.ble_evt_type) {      // 0 et 1 : connectable, 2 et 3 : non connectable, 4 : scan response
                case 0: case 1: info.connectable = 1; break;
                case 2: case 3: info.connectable = 2; break;
                default: break;
            }
            // Les données d'annonce sont suivies de la scan response dans le même tableau
            parse_ad(r.ble_adv,                    r.adv_data_len, info);
            parse_ad(r.ble_adv + r.adv_data_len,   r.scan_rsp_len, info);
            post(info);
        }
        else if (r.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            if (s_phase == PHASE_BLE_SCAN) on_ble_finished();
        }
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------- callbacks classique

static void gap_bt_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_BT_GAP_DISC_RES_EVT: {
        // Appelé une première fois à la découverte, puis de nouveau quand le nom
        // a été récupéré : les propriétés présentes varient d'un appel à l'autre.
        BtDeviceInfo info = {};
        info.type = BT_TYPE_CLASSIC;
        addr_to_str(param->disc_res.bda, info.address);

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            const esp_bt_gap_dev_prop_t *p = param->disc_res.prop + i;
            switch (p->type) {
                case ESP_BT_GAP_DEV_PROP_COD:
                    if (p->len == (int)sizeof(uint32_t)) memcpy(&info.cod, p->val, sizeof(uint32_t));
                    break;
                case ESP_BT_GAP_DEV_PROP_RSSI:
                    if (p->len == 1) { info.rssi = *(const int8_t *)p->val; info.hasRssi = true; }
                    break;
                case ESP_BT_GAP_DEV_PROP_BDNAME:
                    copy_name(info.name, sizeof(info.name), (const uint8_t *)p->val, p->len);
                    break;
                case ESP_BT_GAP_DEV_PROP_EIR:
                    parse_ad((const uint8_t *)p->val, p->len, info);
                    break;
                default:
                    break;
            }
        }
        post(info);
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && s_phase == PHASE_CLASSIC) {
            finish_cycle();
        }
        break;

    default:
        break;
    }
}

// --------------------------------------------------------------------- API

// Exécute un appel ESP-IDF ; en cas d'erreur, affiche laquelle et abandonne begin()
#define BT_STEP(call)                                                                     \
    do {                                                                                  \
        esp_err_t _e = (call);                                                            \
        if (_e != ESP_OK) {                                                               \
            Serial.printf("BT: %s -> %s (0x%x)\n", #call, esp_err_to_name(_e), (unsigned)_e); \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

bool BtScanner::begin(QueueHandle_t queue)
{
    s_queue = queue;

    // btStart() initialise ET active le contrôleur. On ne l'appelle que si le contrôleur est au repos :
    // après un échec partiel (contrôleur initialisé mais pas activé), le rappeler renvoie
    // ESP_ERR_INVALID_STATE et masque la vraie cause ; les étapes ci-dessous reprennent là où on s'est arrêté.
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        if (!btStart()) {
            Serial.printf("BT: btStart() a echoue (etat controleur %d, heap libre %u, plus grand bloc %u)\n",
                          (int)esp_bt_controller_get_status(), (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            return false;
        }
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        BT_STEP(esp_bt_controller_init(&cfg));
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        BT_STEP(esp_bt_controller_enable(ESP_BT_MODE_BTDM));   // BLE + classique
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        BT_STEP(esp_bluedroid_init());
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        BT_STEP(esp_bluedroid_enable());
    }

    BT_STEP(esp_ble_gap_register_callback(gap_ble_cb));
    BT_STEP(esp_bt_gap_register_callback(gap_bt_cb));

    // On ne veut pas que l'ESP32 apparaisse lui-même dans les scans des autres
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    return true;
}

void BtScanner::startScan(uint32_t bleMs, uint8_t classicUnits, bool repeat)
{
    if (s_phase != PHASE_IDLE) return;
    if (classicUnits > 48) classicUnits = 48;      // limite de l'API : 0x30 x 1,28 s

    s_bleSeconds   = (bleMs + 999) / 1000;
    s_classicUnits = classicUnits;
    s_repeat       = repeat;
    s_stop         = false;
    s_cycle        = 0;
    begin_cycle();
}

void BtScanner::stopScan()
{
    if (s_phase == PHASE_IDLE) return;
    s_stop     = true;
    s_deadline = millis() + STOP_GRACE_MS;         // si la confirmation de la pile se perd
    if (s_phase == PHASE_BLE_SCAN)     esp_ble_gap_stop_scanning();
    else if (s_phase == PHASE_CLASSIC) esp_bt_gap_cancel_discovery();
    // PHASE_BLE_SETUP : l'arrêt est pris en compte à la confirmation des paramètres
}

bool BtScanner::isScanning()
{
    // Garde-fou : si un événement de fin se perdait, on ne reste pas bloqué
    if (s_phase != PHASE_IDLE && (int32_t)(millis() - s_deadline) > 0) {
        esp_ble_gap_stop_scanning();
        esp_bt_gap_cancel_discovery();
        s_stop  = true;
        s_phase = PHASE_IDLE;
    }
    return s_phase != PHASE_IDLE;
}

bool BtScanner::isStopping()     { return s_stop && s_phase != PHASE_IDLE; }
uint16_t BtScanner::cycle()      { return s_cycle; }
uint32_t BtScanner::droppedCount() { return s_dropped; }
