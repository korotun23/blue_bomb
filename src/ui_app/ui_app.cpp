// Pont entre l'interface générée (src/ui/, réécrite à chaque export de LCD Wizard) et l'application.
// Voir ui_app.h. Rien ici ne modifie les fichiers générés : tout se fait à l'exécution,
// sur les widgets exportés (ui_screen_*, ui_button_*, ui_container_*...).

#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <ui/ui.h>
#include "ui_app.h"

#define ANIM_MS 300                 // transition entre écrans (le designer utilise 500 pour le menu)

// Graphique du spectre : coordonnées dans l'objet "plot" (240 px de large, contenu de ui_container_chart)
#define SPEC_PLOT_X   3
#define SPEC_PLOT_W   232           // du canal 0 au canal 83
#define SPEC_PLOT_Y   4
#define SPEC_PLOT_H   130
#define SPEC_BAR_W    2
#define SPEC_COLOR_BLE   0xB000B0   // repères des canaux d'annonce BLE
#define SPEC_COLOR_WIFI  0x1C5FA0   // repères du centre des canaux Wi-Fi 1, 6 et 11
#define STALE_COLOR   0x6E6E6E

static lv_obj_t *s_current = nullptr;           // écran affiché (suivi par les événements de chargement)

static ui_scan_handler_t     s_scan_handler   = nullptr;
static ui_device_handler_t   s_device_handler = nullptr;
static ui_spectrum_handler_t s_spec_handler   = nullptr;

// ---------------------------------------------------------------------------
//  Navigation
// ---------------------------------------------------------------------------

static void go_forward(lv_obj_t *screen)
{
    if(lv_screen_active() != screen) lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, ANIM_MS, 0, false);
}

static void go_back(lv_obj_t *screen)
{
    if(lv_screen_active() != screen) lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, ANIM_MS, 0, false);
}

// Suit l'écran affiché, et prévient l'application à l'entrée / la sortie de l'analyseur
static void screen_event(lv_event_t *e)
{
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_target(e);
    switch(lv_event_get_code(e)){
    case LV_EVENT_SCREEN_LOAD_START:
        s_current = scr;
        if(scr == ui_screen_analyzer && s_spec_handler) s_spec_handler(UI_SPEC_OPENED);
        break;
    case LV_EVENT_SCREEN_UNLOAD_START:
        if(scr == ui_screen_analyzer && s_spec_handler) s_spec_handler(UI_SPEC_CLOSED);
        break;
    default: break;
    }
}

static void menu_analyzer_event(lv_event_t *e)   { if(lv_event_get_code(e) == LV_EVENT_CLICKED) go_forward(ui_screen_analyzer); }
static void analyzer_back_event(lv_event_t *e)   { if(lv_event_get_code(e) == LV_EVENT_CLICKED) go_back(ui_screen_main); }
static void details_back_event(lv_event_t *e)    { if(lv_event_get_code(e) == LV_EVENT_CLICKED) go_back(ui_screen_bt_scanner); }

// L'écran "Bluetooth scanner" n'a pas de bouton Back dans le designer : on revient au menu
// en touchant le titre, ou par un balayage vers la droite.
static void bt_leave_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_CLICKED){
        go_back(ui_screen_main);
    } else if(c == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT){
        lv_indev_wait_release(lv_indev_active());
        go_back(ui_screen_main);
    }
}

static void scan_button_event(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && s_scan_handler) s_scan_handler();
}

static void spec_toggle_event(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && s_spec_handler) s_spec_handler(UI_SPEC_TOGGLE);
}

// ---------------------------------------------------------------------------
//  Liste des résultats (écran Bluetooth scanner)
//  Le widget "ui_Panel_result" dessiné dans le designer sert de MODÈLE : il est masqué, et chaque
//  ligne en reprend la taille, les couleurs, les polices et les positions (lues à l'exécution).
//  Il reste l'enfant n° 0 du conteneur : les lignes commencent à l'enfant n° 1.
// ---------------------------------------------------------------------------

struct RowLabelStyle {
    int32_t x, y, w, h;
    lv_color_t color;
    const lv_font_t *font;
    char prefix[16];                    // "Name : ", "UUID: ", "RSSI: " lus dans le modèle
};

struct RowStyle {
    int32_t w, h, radius, border_w, pad_l, pad_r, pad_t, pad_b;
    lv_color_t bg, border;
    lv_opa_t bg_opa;
    lv_border_side_t border_side;
    RowLabelStyle lab[3];
};

static RowStyle s_row;
static uint16_t s_device_count = 0;

static void capture_row_style()
{
    lv_obj_t *p = ui_Panel_result;
    s_row.w           = lv_obj_get_style_width(p, LV_PART_MAIN);
    s_row.h           = lv_obj_get_style_height(p, LV_PART_MAIN);
    s_row.radius      = lv_obj_get_style_radius(p, LV_PART_MAIN);
    s_row.bg          = lv_obj_get_style_bg_color(p, LV_PART_MAIN);
    s_row.bg_opa      = lv_obj_get_style_bg_opa(p, LV_PART_MAIN);
    s_row.border      = lv_obj_get_style_border_color(p, LV_PART_MAIN);
    s_row.border_w    = lv_obj_get_style_border_width(p, LV_PART_MAIN);
    s_row.border_side = lv_obj_get_style_border_side(p, LV_PART_MAIN);
    s_row.pad_l       = lv_obj_get_style_pad_left(p, LV_PART_MAIN);
    s_row.pad_r       = lv_obj_get_style_pad_right(p, LV_PART_MAIN);
    s_row.pad_t       = lv_obj_get_style_pad_top(p, LV_PART_MAIN);
    s_row.pad_b       = lv_obj_get_style_pad_bottom(p, LV_PART_MAIN);

    lv_obj_t *src[3] = { ui_label_name, ui_label_uuid, ui_label_rssi };
    for(int i = 0; i < 3; i++){
        RowLabelStyle &l = s_row.lab[i];
        l.x     = lv_obj_get_style_x(src[i], LV_PART_MAIN);
        l.y     = lv_obj_get_style_y(src[i], LV_PART_MAIN);
        l.w     = lv_obj_get_style_width(src[i], LV_PART_MAIN);
        l.h     = lv_obj_get_style_height(src[i], LV_PART_MAIN);
        l.color = lv_obj_get_style_text_color(src[i], LV_PART_MAIN);
        l.font  = lv_obj_get_style_text_font(src[i], LV_PART_MAIN);
        snprintf(l.prefix, sizeof(l.prefix), "%s", lv_label_get_text(src[i]));
    }
}

static void row_event(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || !s_device_handler) return;
    int32_t index = lv_obj_get_index((lv_obj_t *)lv_event_get_target(e)) - 1;   // enfant 0 = modèle masqué
    if(index >= 0) s_device_handler((uint16_t)index);
}

static void set_row_texts(lv_obj_t *row, const char *name, const char *uuid, int rssi)
{
    lv_label_set_text_fmt(lv_obj_get_child(row, 0), "%s%s", s_row.lab[0].prefix, name);
    lv_label_set_text_fmt(lv_obj_get_child(row, 1), "%s%s", s_row.lab[1].prefix, uuid);
    if(rssi == 0) lv_label_set_text_fmt(lv_obj_get_child(row, 2), "%s--", s_row.lab[2].prefix);
    else          lv_label_set_text_fmt(lv_obj_get_child(row, 2), "%s%d dBm", s_row.lab[2].prefix, rssi);
}

static lv_obj_t *row_at(uint16_t index)
{
    return lv_obj_get_child(ui_container_result, (int32_t)index + 1);
}

void ui_clear_devices()
{
    while(lv_obj_get_child_count(ui_container_result) > 1) lv_obj_delete(lv_obj_get_child(ui_container_result, -1));
    s_device_count = 0;
    lv_obj_scroll_to_y(ui_container_result, 0, LV_ANIM_OFF);
}

bool ui_add_device(const char *name, const char *uuid, int rssi)
{
    if(s_device_count >= UI_MAX_DEVICES) return false;

    lv_obj_t *row = lv_obj_create(ui_container_result);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, s_row.w, s_row.h);
    lv_obj_set_style_bg_color(row, s_row.bg, 0);
    lv_obj_set_style_bg_opa(row, s_row.bg_opa, 0);
    lv_obj_set_style_bg_color(row, lv_color_lighten(s_row.bg, 40), LV_STATE_PRESSED);   // retour visuel à l'appui
    lv_obj_set_style_border_color(row, s_row.border, 0);
    lv_obj_set_style_border_width(row, s_row.border_w, 0);
    lv_obj_set_style_border_side(row, s_row.border_side, 0);
    lv_obj_set_style_radius(row, s_row.radius, 0);
    lv_obj_set_style_pad_left(row, s_row.pad_l, 0);
    lv_obj_set_style_pad_right(row, s_row.pad_r, 0);
    lv_obj_set_style_pad_top(row, s_row.pad_t, 0);
    lv_obj_set_style_pad_bottom(row, s_row.pad_b, 0);

    for(int i = 0; i < 3; i++){
        const RowLabelStyle &l = s_row.lab[i];
        lv_obj_t *lab = lv_label_create(row);
        lv_obj_set_pos(lab, l.x, l.y);
        lv_obj_set_size(lab, l.w, l.h);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_DOT);      // une ligne, "..." si le texte est trop long
        lv_obj_set_style_text_color(lab, l.color, 0);
        lv_obj_set_style_text_font(lab, l.font, 0);
    }
    lv_obj_add_event_cb(row, row_event, LV_EVENT_CLICKED, nullptr);
    set_row_texts(row, name, uuid, rssi);
    s_device_count++;
    return true;
}

void ui_update_device(uint16_t index, const char *name, const char *uuid, int rssi)
{
    lv_obj_t *row = row_at(index);
    if(row) set_row_texts(row, name, uuid, rssi);
}

void ui_set_device_stale(uint16_t index, bool stale)
{
    lv_obj_t *row = row_at(index);
    if(!row) return;
    for(int i = 0; i < 3; i++){
        lv_obj_set_style_text_color(lv_obj_get_child(row, i), stale ? lv_color_hex(STALE_COLOR) : s_row.lab[i].color, 0);
    }
}

void ui_remove_device(uint16_t index)
{
    lv_obj_t *row = row_at(index);
    if(!row) return;
    lv_obj_delete(row);
    if(s_device_count > 0) s_device_count--;
}

void ui_set_scan_state(UiScanState state, uint16_t cycle)
{
    switch(state){
    case UI_SCAN_RUNNING:
        lv_label_set_text_fmt(ui_button_start_bt_scanner_label, "Stop scan (%u)", (unsigned)cycle);
        lv_obj_remove_state(ui_button_start_bt_scanner, LV_STATE_DISABLED);
        break;
    case UI_SCAN_STOPPING:
        lv_label_set_text(ui_button_start_bt_scanner_label, "Stopping...");
        lv_obj_add_state(ui_button_start_bt_scanner, LV_STATE_DISABLED);
        break;
    case UI_SCAN_ERROR:
        lv_label_set_text(ui_button_start_bt_scanner_label, "BT error");
        lv_obj_remove_state(ui_button_start_bt_scanner, LV_STATE_DISABLED);
        break;
    default:
        lv_label_set_text(ui_button_start_bt_scanner_label, "Start scan");
        lv_obj_remove_state(ui_button_start_bt_scanner, LV_STATE_DISABLED);
        break;
    }
}

// ---------------------------------------------------------------------------
//  Page de détail
//  ui_label_details (le modèle du designer) est masqué ; chaque champ devient une ligne
//  "nom / valeur" qui en reprend la couleur et la bordure. Enfant n° 0 = le modèle masqué.
// ---------------------------------------------------------------------------

#define DETAIL_NAME_W   84
#define DETAIL_VALUE_W  140     // 240 - 2 x 5 (marges) - 84 - 6 (espace)

static lv_color_t s_detail_name_color;
static lv_color_t s_detail_border_color;
static int32_t    s_detail_border_w;
static lv_border_side_t s_detail_border_side;

static void create_detail_row(const UiDetailField &f)
{
    lv_obj_t *row = lv_obj_create(ui_container_details);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 5, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_border_width(row, s_detail_border_w, 0);
    lv_obj_set_style_border_side(row, s_detail_border_side, 0);
    lv_obj_set_style_border_color(row, s_detail_border_color, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_width(name, DETAIL_NAME_W);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(name, &ui_font_roboto_mono_12, 0);
    lv_obj_set_style_text_color(name, s_detail_name_color, 0);
    lv_label_set_text(name, f.name);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_width(value, DETAIL_VALUE_W);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(value, &ui_font_roboto_mono_12, 0);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_label_set_text(value, f.value);
}

void ui_detail_show(const UiDetailField *fields, uint8_t count)
{
    if((int)lv_obj_get_child_count(ui_container_details) - 1 != count){
        while(lv_obj_get_child_count(ui_container_details) > 1) lv_obj_delete(lv_obj_get_child(ui_container_details, -1));
        for(uint8_t i = 0; i < count; i++) create_detail_row(fields[i]);
    } else {
        for(uint8_t i = 0; i < count; i++){
            lv_obj_t *row = lv_obj_get_child(ui_container_details, i + 1);
            lv_label_set_text(lv_obj_get_child(row, 0), fields[i].name);
            lv_label_set_text(lv_obj_get_child(row, 1), fields[i].value);
        }
    }
    if(s_current != ui_screen_device_details){
        lv_obj_scroll_to_y(ui_container_details, 0, LV_ANIM_OFF);
        go_forward(ui_screen_device_details);
    }
}

bool ui_detail_visible()
{
    return s_current == ui_screen_device_details;
}

// ---------------------------------------------------------------------------
//  Analyseur 2,4 GHz
//  Le graphique est construit à l'intérieur de ui_container_chart (240 x 228 utiles) :
//  un cadre, 84 barres de 2 px (une par canal de 1 MHz), des repères, l'axe et deux lignes de texte.
// ---------------------------------------------------------------------------

static lv_obj_t *s_plot = nullptr;
static lv_obj_t *s_spec_bars[UI_SPEC_CHANNELS];
static uint8_t   s_spec_shown[UI_SPEC_CHANNELS];     // dernière hauteur dessinée (en %)
static lv_obj_t *s_spec_line1 = nullptr;
static lv_obj_t *s_spec_line2 = nullptr;

static int spec_x(int i)
{
    return SPEC_PLOT_X + (i * SPEC_PLOT_W) / (UI_SPEC_CHANNELS - 1);
}

// Vert jusqu'à 39 %, orange jusqu'à 74 %, rouge au-delà
static lv_color_t spec_color(uint8_t level)
{
    if(level == 0)  return lv_color_hex(0x2A2A2A);
    if(level >= 75) return lv_color_hex(0xFF3B30);
    if(level >= 40) return lv_color_hex(0xFFB000);
    return lv_color_hex(0x00C217);
}

static lv_obj_t *plain_rect(int x, int y, int w, int h, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(s_plot);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    return o;
}

static lv_obj_t *plot_label(int x, int y, int w, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(s_plot);
    lv_obj_set_pos(l, x, y);
    if(w > 0){
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void create_spectrum_plot()
{
    s_plot = lv_obj_create(ui_container_chart);
    lv_obj_remove_style_all(s_plot);
    lv_obj_remove_flag(s_plot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_plot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_plot, 240, 228);

    // Cadre du graphique (même gris que les lignes de la liste)
    lv_obj_t *frame = plain_rect(0, 0, 240, SPEC_PLOT_H + 2 * SPEC_PLOT_Y, lv_color_black());
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(0x3C4049), 0);
    lv_obj_set_style_radius(frame, 2, 0);

    // Repères : canaux d'annonce BLE (2402, 2426, 2480 MHz) et centres Wi-Fi 1, 6, 11 (2412, 2437, 2462 MHz)
    const int ble[3]  = { 2, 26, 80 };
    const int wifi[3] = { 12, 37, 62 };
    for(int k = 0; k < 3; k++){
        plain_rect(spec_x(ble[k]),  SPEC_PLOT_Y, 1, SPEC_PLOT_H, lv_color_hex(SPEC_COLOR_BLE));
        plain_rect(spec_x(wifi[k]), SPEC_PLOT_Y, 1, SPEC_PLOT_H, lv_color_hex(SPEC_COLOR_WIFI));
    }

    // Une barre par canal, créées après les repères pour être dessinées par-dessus
    for(int i = 0; i < UI_SPEC_CHANNELS; i++){
        s_spec_bars[i]  = plain_rect(spec_x(i), SPEC_PLOT_Y + SPEC_PLOT_H - 1, SPEC_BAR_W, 1, spec_color(0));
        s_spec_shown[i] = 0;
    }

    // Axe des fréquences
    const int axis_y = 2 * SPEC_PLOT_Y + SPEC_PLOT_H + 3;
    plot_label(spec_x(0),        axis_y, 0, &ui_font_roboto_mono_12, 0x8A8A8A, "2400");
    plot_label(spec_x(40) - 14,  axis_y, 0, &ui_font_roboto_mono_12, 0x8A8A8A, "2440");
    plot_label(238 - 29,         axis_y, 0, &ui_font_roboto_mono_12, 0x8A8A8A, "2480");

    // Légende des repères
    const int legend_y = axis_y + 18;
    plain_rect(3,  legend_y + 3, 8, 8, lv_color_hex(SPEC_COLOR_BLE));
    plot_label(15, legend_y, 0, &ui_font_roboto_mono_12, 0xBBBBBB, "BLE adv");
    plain_rect(80, legend_y + 3, 8, 8, lv_color_hex(SPEC_COLOR_WIFI));
    plot_label(92, legend_y, 0, &ui_font_roboto_mono_12, 0xBBBBBB, "Wi-Fi 1/6/11");

    // Lignes d'information (composées par l'application)
    s_spec_line1 = plot_label(3, legend_y + 20, 234, &ui_font_roboto_mono_14, 0xFFFFFF, "");
    s_spec_line2 = plot_label(3, legend_y + 40, 234, &ui_font_roboto_mono_14, 0x8A8A8A, "");
}

void ui_spectrum_set_running(bool running)
{
    // Le bouton du designer a une icône "play" : visible quand le balayage est à l'arrêt seulement
    lv_obj_t *content = lv_obj_get_parent(ui_button_start_label);
    lv_obj_t *icon = lv_obj_get_child_count(content) >= 2 ? lv_obj_get_child(content, 0) : nullptr;
    lv_label_set_text(ui_button_start_label, running ? "Pause" : "Start");
    if(icon){
        if(running) lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }
}

bool ui_spectrum_visible()
{
    return s_current == ui_screen_analyzer;
}

void ui_spectrum_update(const uint8_t *level, const char *line1, const char *line2)
{
    for(int i = 0; i < UI_SPEC_CHANNELS; i++){
        uint8_t v = level[i] > 100 ? 100 : level[i];
        if(v == s_spec_shown[i]) continue;              // rien à redessiner
        s_spec_shown[i] = v;
        int32_t h = (v * SPEC_PLOT_H) / 100;
        if(h < 1) h = 1;                                 // trait de base visible même à 0
        lv_obj_set_size(s_spec_bars[i], SPEC_BAR_W, h);
        lv_obj_set_y(s_spec_bars[i], SPEC_PLOT_Y + SPEC_PLOT_H - h);
        lv_obj_set_style_bg_color(s_spec_bars[i], spec_color(v), 0);
    }
    lv_label_set_text(s_spec_line1, line1);
    lv_label_set_text(s_spec_line2, line2);
}

// ---------------------------------------------------------------------------
//  Initialisation
// ---------------------------------------------------------------------------

void ui_set_scan_handler(ui_scan_handler_t cb)         { s_scan_handler = cb; }
void ui_set_device_handler(ui_device_handler_t cb)     { s_device_handler = cb; }
void ui_set_spectrum_handler(ui_spectrum_handler_t cb) { s_spec_handler = cb; }

void ui_app_init()
{
    ui_init();                                           // interface générée : crée les 4 écrans

    // Corrections de mise en page de l'export (elles sont refaites à chaque démarrage, donc survivent
    // à un nouvel export) :
    //  - les écrans sont des conteneurs flex en colonne : le thème LVGL y ajoute 8 px entre les enfants
    //    (entête, zone centrale, barre de boutons), ce qui fait déborder la barre de boutons de 16 px
    //    hors de l'écran de 320 px ;
    //  - "Device details" et "2.4Ghz Analyzer" ont 10 px de marge à gauche et à droite alors que leurs
    //    widgets font déjà 240 px : tout est décalé et coupé à droite. "Menu" et "Bluetooth scanner" n'en ont pas.
    lv_obj_t *screens[4] = { ui_screen_main, ui_screen_bt_scanner, ui_screen_device_details, ui_screen_analyzer };
    for(int i = 0; i < 4; i++){
        lv_obj_set_style_pad_row(screens[i], 0, 0);
        lv_obj_set_style_pad_left(screens[i], 0, 0);
        lv_obj_set_style_pad_right(screens[i], 0, 0);
        lv_obj_add_event_cb(screens[i], screen_event, LV_EVENT_SCREEN_LOAD_START, nullptr);
        lv_obj_add_event_cb(screens[i], screen_event, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    }
    s_current = lv_screen_active();

    // Menu : l'entrée "Bluetooth scanner" est déjà câblée par le designer ; "WiFi scanner" et "Tools"
    // n'ont pas d'écran dans cette version.
    lv_obj_add_event_cb(ui_button_analyzer, menu_analyzer_event, LV_EVENT_CLICKED, nullptr);

    // Bluetooth scanner : liste défilante, Start / Stop, retour au menu (titre ou balayage)
    capture_row_style();
    lv_obj_add_flag(ui_Panel_result, LV_OBJ_FLAG_HIDDEN);                       // le modèle ne s'affiche pas
    lv_obj_add_flag(ui_container_result, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui_container_result, LV_DIR_VER);
    lv_obj_set_flex_align(ui_container_result, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui_container_result, 4, 0);
    lv_obj_add_event_cb(ui_button_start_bt_scanner, scan_button_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(ui_container_header_2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_container_header_2, bt_leave_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_screen_bt_scanner, bt_leave_event, LV_EVENT_GESTURE, nullptr);

    // Détail : champs défilants, bouton Back
    s_detail_name_color   = lv_obj_get_style_text_color(ui_label_details, LV_PART_MAIN);
    s_detail_border_color = lv_obj_get_style_border_color(ui_label_details, LV_PART_MAIN);
    s_detail_border_w     = lv_obj_get_style_border_width(ui_label_details, LV_PART_MAIN);
    s_detail_border_side  = lv_obj_get_style_border_side(ui_label_details, LV_PART_MAIN);
    lv_obj_add_flag(ui_label_details, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_container_details, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui_container_details, LV_DIR_VER);
    lv_obj_set_flex_align(ui_container_details, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui_container_details, 0, 0);
    lv_obj_add_event_cb(ui_button_back, details_back_event, LV_EVENT_CLICKED, nullptr);

    // Analyseur : graphique, Back, Start / Pause
    create_spectrum_plot();
    lv_obj_add_event_cb(ui_button_back_2, analyzer_back_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_button_start, spec_toggle_event, LV_EVENT_CLICKED, nullptr);
}
