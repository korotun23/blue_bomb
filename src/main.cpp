// App layer
// Link between Bluetooth (BLE + Classic) and the interface
//
// Mode inventaire : les cycles BLE puis classique s'enchaînent jusqu'à l'appui sur Stop.
// La liste est cumulative ; un appareil non vu depuis STALE_MS est grisé, et c'est le
// premier à être remplacé si la liste est pleine.

#include <Arduino.h>

#include <drivers/display_driver.h>
#include <ui/ui.h>
#include <scanners/bluetooth/bt_scanner.h>
#include <utilities/company_ids.h>

#define BT_QUEUE_LENGTH        64
#define BLE_SCAN_MS            10000
#define CLASSIC_INQUIRY_UNITS  8       // 8 x 1,28 s = 10,24 s (durée recommandée par la spécification)
#define QUEUE_BATCH            16      // messages traités par passage de loop()
#define RSSI_UI_STEP           5       // dB : en dessous, on ne redessine pas la ligne de la liste
#define STALE_MS               60000   // sans signal depuis ce délai : ligne grisée (un cycle dure ~20 s)
#define TICK_MS                1000    // vieillissement des lignes et rafraîchissement du détail
#define DETAIL_MAX_FIELDS      14
#define DETAIL_VALUE_SIZE      96

static QueueHandle_t btQueue = nullptr;
static BtScanner     btScanner;

// ---------------------------------------------------------------------------
//  Table des périphériques
//  Un même périphérique envoie plusieurs messages (annonce puis scan response,
//  résultat d'inquiry puis nom...) : on les fusionne ici, par adresse.
//  La ligne n de la table correspond à la ligne n de la liste affichée.
// ---------------------------------------------------------------------------

struct DeviceRow {
    BtDeviceInfo info;       // données fusionnées de tous les messages reçus
    char     key[20];        // type + adresse
    char     label[40];      // texte affiché dans la liste
    uint8_t  quality;        // qualité du libellé : on ne le remplace que par mieux
    int8_t   shownRssi;      // RSSI actuellement affiché dans la liste
    bool     stale;          // ligne actuellement grisée
    uint16_t packets;        // nombre de messages reçus pour ce périphérique
    uint32_t firstSeen;      // millis() au premier message
    uint32_t lastSeen;       // millis() au dernier message
};

static DeviceRow rows[UI_MAX_DEVICES];
static uint16_t  rowCount = 0;

static int      detailIndex    = -1;    // ligne affichée dans la page de détail
static uint32_t overflow       = 0;     // appareils ignorés faute de place
static uint32_t overflowLogged = 0;

// Référence de temps pour les "âges" : l'heure actuelle pendant un scan, l'heure de l'arrêt
// ensuite (une fois le scan arrêté, plus rien n'est "perdu" : on fige l'affichage).
static bool     scanActive = false;
static uint32_t stoppedAt  = 0;

static uint32_t ref_time() { return scanActive ? millis() : stoppedAt; }

static uint32_t age_ms(uint32_t since)
{
    int32_t a = (int32_t)(ref_time() - since);
    return a < 0 ? 0 : (uint32_t)a;
}

static void format_age(uint32_t ms, char *out, size_t size)
{
    if (ms < 2000)         snprintf(out, size, "now");
    else if (ms < 60000)   snprintf(out, size, "%u s ago", (unsigned)(ms / 1000));
    else if (ms < 3600000) snprintf(out, size, "%u min ago", (unsigned)(ms / 60000));
    else                   snprintf(out, size, "%u h ago", (unsigned)(ms / 3600000));
}

// ------------------------------------------------------------------ textes

static const char *cod_major_name(uint32_t cod)
{
    switch ((cod >> 8) & 0x1F) {          // bits 8 à 12 : classe majeure
        case 1:  return "Computer";
        case 2:  return "Phone";
        case 3:  return "Network";
        case 4:  return "Audio/Video";
        case 5:  return "Peripheral";
        case 6:  return "Imaging";
        case 7:  return "Wearable";
        case 8:  return "Toy";
        case 9:  return "Health";
        default: return nullptr;
    }
}

// Classes de service du Class of Device (bits 13 à 23)
static void cod_services_text(uint32_t cod, char *out, size_t size)
{
    static const struct { uint8_t bit; const char *name; } services[] = {
        {13, "Limited disc."}, {16, "Positioning"}, {17, "Networking"}, {18, "Rendering"},
        {19, "Capturing"}, {20, "Object transfer"}, {21, "Audio"}, {22, "Telephony"}, {23, "Information"}
    };
    size_t o = 0;
    out[0] = '\0';
    for (const auto &s : services) {
        if (!(cod & (1UL << s.bit))) continue;
        o += snprintf(out + o, size - o, "%s%s", o ? ", " : "", s.name);
        if (o >= size) { o = size - 1; break; }
    }
    if (o == 0) snprintf(out, size, "-");
}

static const char *ble_service_name(uint16_t uuid)
{
    switch (uuid) {
        case 0x1800: return "Generic Access";
        case 0x1801: return "Generic Attribute";
        case 0x180A: return "Device Info";
        case 0x180D: return "Heart Rate";
        case 0x180F: return "Battery";
        case 0x1812: return "HID";
        case 0x181A: return "Env. Sensing";
        case 0xFD6F: return "Exposure Notif.";
        default:     return nullptr;
    }
}

static void ble_services_text(const BtDeviceInfo &d, char *out, size_t size)
{
    size_t o = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < d.svcCount; i++) {
        char one[24];
        const char *name = ble_service_name(d.svc[i]);
        if (name) snprintf(one, sizeof(one), "%s", name);
        else      snprintf(one, sizeof(one), "0x%04X", d.svc[i]);
        if (o + strlen(one) + 6 >= size) break;
        o += snprintf(out + o, size - o, "%s%s", o ? ", " : "", one);
    }
    if (d.svcMore && o + 5 < size) snprintf(out + o, size - o, "%s...", o ? ", " : "");
    if (out[0] == '\0') snprintf(out, size, "-");
}

static const char *ble_addr_type_name(uint8_t t)
{
    switch (t) {
        case 0:  return "Public";
        case 1:  return "Random";
        case 2:  return "Private (public)";
        case 3:  return "Private (random)";
        default: return "-";
    }
}

// Choisit le texte de la liste et retourne sa qualité (plus c'est haut, mieux c'est) :
//   5 nom, 4 constructeur connu, 3 classe d'appareil (classique), 2 identifiant brut, 1 adresse
// Le texte est tronqué à 15 caractères pour que le RSSI reste visible sur la ligne.
static uint8_t make_label(const BtDeviceInfo &d, char *out, size_t size)
{
    const char *tag = (d.type == BT_TYPE_CLASSIC) ? "BT" : "LE";

    if (d.name[0]) {
        snprintf(out, size, "%s %.15s", tag, d.name);
        return 5;
    }
    const char *vendor = d.hasCompany ? companyName(d.companyId) : nullptr;
    if (vendor) {
        snprintf(out, size, "%s %.15s", tag, vendor);
        return 4;
    }
    const char *cls = d.cod ? cod_major_name(d.cod) : nullptr;
    if (cls) {
        snprintf(out, size, "%s %.15s", tag, cls);
        return 3;
    }
    if (d.hasCompany) {
        snprintf(out, size, "%s ID 0x%04X", tag, d.companyId);
        return 2;
    }
    snprintf(out, size, "%s %s", tag, d.address);
    return 1;
}

// ------------------------------------------------------------ page de détail

struct DetailBuilder {
    UiDetailField fields[DETAIL_MAX_FIELDS];
    char          values[DETAIL_MAX_FIELDS][DETAIL_VALUE_SIZE];
    uint8_t       count;

    // Réserve une ligne et retourne son buffer de texte
    char *add(const char *name) {
        fields[count].name  = name;
        fields[count].value = values[count];
        values[count][0] = '\0';
        return values[count++];
    }
};

static DetailBuilder detail;     // statique : évite de charger la pile (on est dans un callback LVGL)

static void show_detail(int idx)
{
    const DeviceRow &r = rows[idx];
    const BtDeviceInfo &d = r.info;
    const size_t N = DETAIL_VALUE_SIZE;
    char age[24];
    detail.count = 0;

    snprintf(detail.add("Name"), N, "%s", d.name[0] ? d.name : "-");
    snprintf(detail.add("Type"), N, "%s", d.type == BT_TYPE_CLASSIC ? "Bluetooth Classic" : "Bluetooth LE");
    snprintf(detail.add("Address"), N, "%s", d.address);
    if (d.type == BT_TYPE_BLE) {
        snprintf(detail.add("Addr type"), N, "%s", ble_addr_type_name(d.addrType));
    }
    if (d.hasRssi) snprintf(detail.add("RSSI"), N, "%d dBm", d.rssi);
    else           snprintf(detail.add("RSSI"), N, "-");

    if (d.hasCompany) {
        const char *vendor = companyName(d.companyId);
        if (vendor) snprintf(detail.add("Company"), N, "%s (0x%04X)", vendor, d.companyId);
        else        snprintf(detail.add("Company"), N, "ID 0x%04X", d.companyId);
    } else {
        snprintf(detail.add("Company"), N, "-");
    }

    if (d.type == BT_TYPE_CLASSIC) {
        const char *cls = d.cod ? cod_major_name(d.cod) : nullptr;
        if (d.cod) snprintf(detail.add("Class"), N, "%s (0x%06lX)", cls ? cls : "Other", (unsigned long)d.cod);
        else       snprintf(detail.add("Class"), N, "-");
        cod_services_text(d.cod, detail.add("Services"), N);
    } else {
        ble_services_text(d, detail.add("Services"), N);
        if (d.hasTxPower) snprintf(detail.add("TX power"), N, "%+d dBm", d.txPower);
        else              snprintf(detail.add("TX power"), N, "-");
        snprintf(detail.add("Connectable"), N, "%s", d.connectable == 1 ? "Yes" : d.connectable == 2 ? "No" : "-");
    }

    format_age(age_ms(r.firstSeen), age, sizeof(age));
    snprintf(detail.add("First seen"), N, "%s", age);
    format_age(age_ms(r.lastSeen), age, sizeof(age));
    snprintf(detail.add("Last seen"), N, "%s", age);
    snprintf(detail.add("Packets"), N, "%u", (unsigned)r.packets);

    ui_detail_show(detail.fields, detail.count);
    detailIndex = idx;
}

// Appelée par l'UI au clic sur une ligne
static void on_device_selected(uint16_t index)
{
    if (index < rowCount) show_detail(index);
}

// ------------------------------------------------------------ fusion des messages

static void merge_info(BtDeviceInfo &dst, const BtDeviceInfo &src)
{
    // Un nom plus long est plus complet (un nom abrégé est le début du nom complet)
    if (strlen(src.name) > strlen(dst.name)) memcpy(dst.name, src.name, sizeof(dst.name));

    if (src.hasRssi) { dst.hasRssi = true; dst.rssi = src.rssi; }
    if (src.hasCompany && !dst.hasCompany) { dst.hasCompany = true; dst.companyId = src.companyId; }
    if (src.cod) dst.cod = src.cod;

    if (src.type == BT_TYPE_BLE) {
        dst.addrType = src.addrType;
        if (src.hasTxPower) { dst.hasTxPower = true; dst.txPower = src.txPower; }
        if (src.connectable) dst.connectable = src.connectable;
        for (uint8_t i = 0; i < src.svcCount; i++) btAddService(dst, src.svc[i]);
        if (src.svcMore) dst.svcMore = true;
    }
}

static int find_row(const char *key)
{
    for (uint16_t i = 0; i < rowCount; i++) {
        if (strcmp(rows[i].key, key) == 0) return i;
    }
    return -1;
}

// Ligne la plus ancienne parmi celles qui sont grisées ; -1 s'il n'y en a pas
static int find_evictable()
{
    int best = -1;
    uint32_t bestAge = 0;
    for (uint16_t i = 0; i < rowCount; i++) {
        uint32_t a = age_ms(rows[i].lastSeen);
        if (a >= STALE_MS && a >= bestAge) { best = i; bestAge = a; }
    }
    return best;
}

// Supprime une ligne : les suivantes remontent, il faut donc les renuméroter
static void remove_row(int idx)
{
    ui_remove_device(idx);
    memmove(&rows[idx], &rows[idx + 1], (rowCount - idx - 1) * sizeof(DeviceRow));
    rowCount--;
    for (int j = idx; j < rowCount; j++) ui_update_device(j, rows[j].label, rows[j].shownRssi);
}

static void handle_device(const BtDeviceInfo &d)
{
    char key[20];
    snprintf(key, sizeof(key), "%c%s", d.type == BT_TYPE_CLASSIC ? 'C' : 'L', d.address);
    uint32_t now = millis();

    int idx = find_row(key);

    if (idx < 0) {                                   // nouveau périphérique
        if (rowCount >= UI_MAX_DEVICES) {
            // Liste pleine : on remplace le plus ancien appareil grisé. Pas pendant que la page
            // de détail est ouverte (les indices bougeraient sous ses pieds).
            int victim = ui_detail_visible() ? -1 : find_evictable();
            if (victim < 0) { overflow++; return; }
            remove_row(victim);
        }

        DeviceRow &r = rows[rowCount];
        r.info      = d;
        r.packets   = 1;
        r.stale     = false;
        r.firstSeen = now;
        r.lastSeen  = now;
        r.shownRssi = d.hasRssi ? d.rssi : 0;        // 0 = inconnu, la ligne affichera "--"
        r.quality   = make_label(r.info, r.label, sizeof(r.label));
        snprintf(r.key, sizeof(r.key), "%s", key);

        if (!ui_add_device(r.label, r.shownRssi)) return;
        rowCount++;
        return;
    }

    DeviceRow &r = rows[idx];                        // périphérique déjà connu : fusion
    merge_info(r.info, d);
    r.lastSeen = now;
    if (r.packets < 0xFFFF) r.packets++;

    if (r.stale) {                                   // il est revenu
        r.stale = false;
        ui_set_device_stale(idx, false);
    }

    // Liste : on ne redessine la ligne que si le libellé ou le RSSI a vraiment changé
    char label[40];
    uint8_t quality = make_label(r.info, label, sizeof(label));
    bool changed = false;

    if (quality >= r.quality && strcmp(label, r.label) != 0) {
        snprintf(r.label, sizeof(r.label), "%s", label);
        r.quality = quality;
        changed = true;
    }
    if (r.info.hasRssi && (r.shownRssi == 0 || abs((int)r.info.rssi - (int)r.shownRssi) >= RSSI_UI_STEP)) {
        r.shownRssi = r.info.rssi;
        changed = true;
    }
    if (changed) ui_update_device(idx, r.label, r.shownRssi);
}

// Vide la queue : QUEUE_BATCH messages maximum par passage pour ne pas bloquer LVGL
static void process_bt_queue()
{
    BtDeviceInfo dev;
    uint8_t processed = 0;
    while (processed < QUEUE_BATCH && xQueueReceive(btQueue, &dev, 0) == pdTRUE) {
        handle_device(dev);
        processed++;
    }
}

// Une fois par seconde : grise les appareils absents, rafraîchit la page de détail ouverte
static void service_tick()
{
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < TICK_MS) return;
    last = now;

    for (uint16_t i = 0; i < rowCount; i++) {
        bool stale = age_ms(rows[i].lastSeen) >= STALE_MS;
        if (stale != rows[i].stale) {
            rows[i].stale = stale;
            ui_set_device_stale(i, stale);
        }
    }

    if (ui_detail_visible() && detailIndex >= 0 && detailIndex < rowCount) show_detail(detailIndex);

    if (overflow != overflowLogged) {
        overflowLogged = overflow;
        Serial.printf("Liste pleine (%d) : %lu apparition(s) ignoree(s)\n", UI_MAX_DEVICES, (unsigned long)overflow);
    }
}

// ------------------------------------------------------------ scan

// Bouton Start / Stop
static void on_scan_button()
{
    if (btScanner.isStopping()) return;

    if (btScanner.isScanning()) {                    // Stop
        btScanner.stopScan();
        return;
    }

    // Start : nouvelle session, on repart d'une liste vide
    xQueueReset(btQueue);
    rowCount       = 0;
    detailIndex    = -1;
    overflow       = 0;
    overflowLogged = 0;
    ui_clear_devices();
    scanActive = true;
    btScanner.startScan(BLE_SCAN_MS, CLASSIC_INQUIRY_UNITS, true);
}

// Met à jour le bouton quand l'état du scan ou le numéro de cycle change
static void update_scan_state()
{
    static UiScanState lastState = UI_SCAN_IDLE;
    static uint16_t    lastCycle = 0;

    bool scanning = btScanner.isScanning();
    UiScanState state = !scanning ? UI_SCAN_IDLE
                      : btScanner.isStopping() ? UI_SCAN_STOPPING : UI_SCAN_RUNNING;
    uint16_t cycle = btScanner.cycle();

    if (state == lastState && !(state == UI_SCAN_RUNNING && cycle != lastCycle)) return;

    if (state == UI_SCAN_IDLE) {                     // fin de session : le temps se fige
        stoppedAt  = millis();
        scanActive = false;
    }
    if (lastCycle > 0 && (cycle != lastCycle || state == UI_SCAN_IDLE)) {   // bilan du cycle terminé
        Serial.printf("Cycle %u : %u appareils, %lu message(s) perdu(s)\n",
                      (unsigned)lastCycle, (unsigned)rowCount, (unsigned long)btScanner.droppedCount());
    }
    ui_set_scan_state(state, cycle);
    lastState = state;
    lastCycle = cycle;
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Starting...");

    btQueue = xQueueCreate(BT_QUEUE_LENGTH, sizeof(BtDeviceInfo));
    if (btQueue == nullptr) {
        Serial.println("ERREUR : creation de la queue impossible");
        while (true) delay(1000);
    }

    display_init();
    ui_init();
    ui_set_scan_handler(on_scan_button);
    ui_set_device_handler(on_device_selected);

    Serial.printf("Heap avant Bluetooth : %u octets\n", (unsigned)ESP.getFreeHeap());
    if (!btScanner.begin(btQueue)) {
        Serial.println("ERREUR : initialisation Bluetooth impossible");
    }
    Serial.printf("Heap apres Bluetooth : %u octets\n", (unsigned)ESP.getFreeHeap());

    Serial.println("Setup complete.");
}

void loop()
{
    process_bt_queue();
    service_tick();
    update_scan_state();
    display_update();
    delay(5);
}
