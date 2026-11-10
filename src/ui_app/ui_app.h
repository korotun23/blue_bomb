#pragma once
#include <stdint.h>

// Couche "pont" entre l'interface générée par LCD Wizard (src/ui/*.c) et l'application.
//
// Les fichiers de src/ui/ sont réécrits à chaque export du designer : on n'y touche jamais.
// Ce module (hors de src/ui/) s'accroche aux widgets générés au démarrage, après ui_init() :
//   - il branche les boutons (navigation, Start/Stop, Back...) ;
//   - il remplit la liste des résultats, la page de détail et le graphique du spectre.
// L'application (main.cpp) ne manipule que cette API : aucun lv_obj_t n'en sort.
//
// Appeler ces fonctions UNIQUEMENT depuis loop() (LVGL n'est pas thread-safe).

#define UI_MAX_DEVICES 40

// ---------------------------------------------------------------- événements
typedef void (*ui_scan_handler_t)(void);               // appui sur Start / Stop scan
typedef void (*ui_device_handler_t)(uint16_t index);   // clic sur une ligne (0 = première)

enum UiSpectrumEvent {
    UI_SPEC_OPENED,     // l'écran analyseur vient d'être ouvert
    UI_SPEC_CLOSED,     // il vient d'être quitté
    UI_SPEC_TOGGLE      // appui sur Start / Pause
};
typedef void (*ui_spectrum_handler_t)(UiSpectrumEvent ev);

enum UiScanState {
    UI_SCAN_IDLE,       // bouton "Start scan"
    UI_SCAN_RUNNING,    // bouton "Stop scan (n)", n = numéro de cycle
    UI_SCAN_STOPPING,   // bouton "Stopping..." désactivé
    UI_SCAN_ERROR       // bouton "BT error" : le Bluetooth n'a pas pu démarrer (un nouvel appui réessaie)
};

// Une ligne de la page de détail : "Address" / "aa:bb:cc:dd:ee:ff"
struct UiDetailField {
    const char *name;
    const char *value;
};

#define UI_SPEC_CHANNELS 84     // canal i = (2400 + i) MHz

// ---------------------------------------------------------------- initialisation
// Crée l'interface (ui_init() du designer) puis la câble. À appeler une fois, après display_init().
void ui_app_init();

void ui_set_scan_handler(ui_scan_handler_t cb);
void ui_set_device_handler(ui_device_handler_t cb);
void ui_set_spectrum_handler(ui_spectrum_handler_t cb);

// ---------------------------------------------------------------- écran "Bluetooth scanner"
// Une ligne = trois textes, dans l'ordre du modèle dessiné (Name / UUID / RSSI). Les préfixes
// ("Name : ", "UUID: ", "RSSI: ") sont lus dans le modèle du designer : on ne fournit que la valeur.
// rssi == 0 : inconnu, affiche "--".
void ui_clear_devices();
bool ui_add_device(const char *name, const char *uuid, int rssi);
void ui_update_device(uint16_t index, const char *name, const char *uuid, int rssi);
void ui_set_device_stale(uint16_t index, bool stale);   // ligne grisée : appareil non vu récemment
void ui_remove_device(uint16_t index);                  // les lignes suivantes remontent
void ui_set_scan_state(UiScanState state, uint16_t cycle);

// ---------------------------------------------------------------- écran "Device details"
// Ouvre l'écran s'il ne l'est pas. S'il est déjà ouvert avec le même nombre de champs, les textes
// sont mis à jour sur place (le défilement est conservé). Le bouton Back est géré par l'UI.
void ui_detail_show(const UiDetailField *fields, uint8_t count);
bool ui_detail_visible();

// ---------------------------------------------------------------- écran "2.4Ghz Analyzer"
// level : UI_SPEC_CHANNELS valeurs de 0 à 100 (hauteur des barres, en %).
// line1 / line2 : deux lignes de texte sous le graphique, composées par l'application.
void ui_spectrum_set_running(bool running);     // texte du bouton : "Pause" / "Start"
void ui_spectrum_update(const uint8_t *level, const char *line1, const char *line2);
bool ui_spectrum_visible();
