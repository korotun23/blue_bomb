#pragma once
#include <stdint.h>

// UI Layer 

#define UI_MAX_DEVICES 100

typedef void (*ui_scan_handler_t)(void);   // appui sur le bouton Start / Stop

enum UiScanState {
    UI_SCAN_IDLE,       // bouton "Start scan"
    UI_SCAN_RUNNING,    // bouton "Stop scan (n)", n = numéro de cycle
    UI_SCAN_STOPPING    // bouton "Stopping..." désactivé
};
typedef void (*ui_device_handler_t)(uint16_t index);   // appelée au clic sur une ligne (index 0 = première)

// Une ligne de la page de détail : "Address" / "aa:bb:cc:dd:ee:ff"
struct UiDetailField {
    const char *name;
    const char *value;
};

void ui_init();
void ui_set_scan_handler(ui_scan_handler_t cb);
void ui_set_device_handler(ui_device_handler_t cb);

void ui_clear_devices();
bool ui_add_device(const char *label, int rssi);   // rssi == 0 : inconnu, affiche "--"
void ui_update_device(uint16_t index, const char *label, int rssi);   // index 0 = première ligne
void ui_set_device_stale(uint16_t index, bool stale);                 // ligne grisée : appareil non vu récemment
void ui_remove_device(uint16_t index);                                // supprime la ligne, les suivantes remontent
                                                                      // (à l'appelant de renuméroter avec ui_update_device)
void ui_set_scan_state(UiScanState state, uint16_t cycle);            // texte et état du bouton

// Page de détail. Le retour à la liste est géré par l'UI (bouton Back).
// Si la page est déjà ouverte et que le nombre de champs est identique, les textes
// sont mis à jour sur place (pas de reconstruction, le défilement est conservé).
void ui_detail_show(const UiDetailField *fields, uint8_t count);
bool ui_detail_visible();
