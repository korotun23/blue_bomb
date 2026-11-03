#include "lv_conf.h"
#include <lvgl.h>

#include "ui.h"

LV_FONT_DECLARE(ui_font_icons_20);
LV_FONT_DECLARE(ui_font_icons_32);
 
#define UI_ICON_BLUETOOTH        "\xEE\x80\x80"   /* Bluetooth */
#define UI_ICON_BLUETOOTH_AUDIO  "\xEE\x80\x81"   /* Bluetooth audio */

// Only keep widgets which will be modified after creation
static lv_obj_t *s_main_container = nullptr;
static lv_obj_t *s_button_scanner = nullptr;
static lv_obj_t *s_button_label = nullptr;
static lv_obj_t *s_detail_container = nullptr;
static lv_obj_t *s_button_back = nullptr;

static ui_device_handler_t s_device_handler = nullptr;
static bool s_detail_open = false;

#define DETAIL_NAME_WIDTH   70
#define DETAIL_VALUE_WIDTH  150

static uint16_t s_device_count = 0;
static ui_scan_handler_t s_scan_handler = nullptr;

// --------------------------------------------------------
//  EVENTS
// --------------------------------------------------------

static void button_scan_event(lv_event_t *e)
{
    if(lv_event_get_code(e)==LV_EVENT_CLICKED && s_scan_handler){
        s_scan_handler();
    };
}

static void row_event(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && s_device_handler){
        lv_obj_t *row = (lv_obj_t *)lv_event_get_target(e);
        // Child n° 0 is the header, rows start at child n° 1 and are never reordered
        int32_t index = lv_obj_get_index(row) - 1;
        if(index >= 0) s_device_handler((uint16_t)index);
    }
}

// Bascule entre la liste (contrôle : bouton de scan) et le détail (contrôle : bouton Back)
static void set_detail_view(bool detail)
{
    s_detail_open = detail;
    if(detail){
        lv_obj_add_flag(s_main_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_detail_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_button_scanner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_button_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_detail_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_main_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_button_back, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_button_scanner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void button_back_event(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) set_detail_view(false);
}

// ---------------------------------------------------------
//  API
// ---------------------------------------------------------

void ui_set_scan_handler(ui_scan_handler_t cb){
    s_scan_handler = cb;
}

void ui_set_device_handler(ui_device_handler_t cb){
    s_device_handler = cb;
}

void ui_clear_devices()

{
    // Header is kept >> Child n° 0
    while(lv_obj_get_child_count(s_main_container) > 1){
        lv_obj_delete(lv_obj_get_child(s_main_container, 1));
    }
    s_device_count = 0;
}

// Texte d'une ligne : numéro, libellé, RSSI ("--" si inconnu)
static void set_row_text(lv_obj_t *row, uint16_t number, const char *label, int rssi)
{
    if(rssi == 0){
        lv_label_set_text_fmt(row, "%u  %s  --", (unsigned)number, label);
    } else {
        lv_label_set_text_fmt(row, "%u  %s  %d dBm", (unsigned)number, label, rssi);
    }
}

bool ui_add_device(const char *label, int rssi)
{
    if(s_device_count >= UI_MAX_DEVICES) return false;
    s_device_count ++;

    lv_obj_t *row = lv_label_create(s_main_container);
    lv_obj_set_width(row, lv_pct(100));
    lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_left(row, 5, 0);
    lv_obj_set_style_text_color(row, lv_color_hex(0xFFFFFF), 0);
    // Ligne cliquable : marges verticales pour faciliter l'appui, fond coloré quand elle est pressée
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_top(row, 3, 0);
    lv_obj_set_style_pad_bottom(row, 3, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x00C217), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, row_event, LV_EVENT_CLICKED, nullptr);
    set_row_text(row, s_device_count, label, rssi);
    return true;
}

void ui_update_device(uint16_t index, const char *label, int rssi)
{
    // Child n° 0 is the header, rows start at child n° 1
    lv_obj_t *row = lv_obj_get_child(s_main_container, index + 1);
    if(row == nullptr) return;
    set_row_text(row, index + 1, label, rssi);
}

void ui_set_scan_state(UiScanState state, uint16_t cycle)
{
    switch(state){
    case UI_SCAN_RUNNING:
        lv_label_set_text_fmt(s_button_label, "Stop scan (%u)", (unsigned)cycle);
        lv_obj_remove_state(s_button_scanner, LV_STATE_DISABLED);
        break;
    case UI_SCAN_STOPPING:
        lv_label_set_text(s_button_label, "Stopping...");
        lv_obj_add_state(s_button_scanner, LV_STATE_DISABLED);
        break;
    default:
        lv_label_set_text(s_button_label, "Start scan");
        lv_obj_remove_state(s_button_scanner, LV_STATE_DISABLED);
        break;
    }
}

void ui_set_device_stale(uint16_t index, bool stale)
{
    lv_obj_t *row = lv_obj_get_child(s_main_container, index + 1);   // child n° 0 is the header
    if(row == nullptr) return;
    lv_obj_set_style_text_color(row, lv_color_hex(stale ? 0x6E6E6E : 0xFFFFFF), 0);
}

void ui_remove_device(uint16_t index)
{
    lv_obj_t *row = lv_obj_get_child(s_main_container, index + 1);
    if(row == nullptr) return;
    lv_obj_delete(row);
    if(s_device_count > 0) s_device_count --;
}

static void create_detail_row(const UiDetailField &f)
{
    lv_obj_t *row = lv_obj_create(s_detail_container);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 5, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_width(name, DETAIL_NAME_WIDTH);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(name, lv_color_hex(0x8A8A8A), 0);
    lv_label_set_text(name, f.name);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_width(value, DETAIL_VALUE_WIDTH);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(value, f.value);
}

void ui_detail_show(const UiDetailField *fields, uint8_t count)
{
    if(lv_obj_get_child_count(s_detail_container) != count){
        lv_obj_clean(s_detail_container);
        for(uint8_t i = 0; i < count; i++) create_detail_row(fields[i]);
    } else {
        for(uint8_t i = 0; i < count; i++){
            lv_obj_t *row = lv_obj_get_child(s_detail_container, i);
            lv_label_set_text(lv_obj_get_child(row, 0), fields[i].name);
            lv_label_set_text(lv_obj_get_child(row, 1), fields[i].value);
        }
    }
    if(!s_detail_open){
        lv_obj_scroll_to_y(s_detail_container, 0, LV_ANIM_OFF);
        set_detail_view(true);
    }
}

bool ui_detail_visible()
{
    return s_detail_open;
}

// ---------------------------------------------------------
//  UI BUILDING
// ---------------------------------------------------------

void ui_init()
{
    lv_display_t *dispp = lv_display_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_color_hex(0x20F32E), lv_color_hex(0xEDEDED),
                                              true, LV_FONT_DEFAULT_MONTSERRAT_14);
    lv_display_set_theme(dispp, theme);
 
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
 
    // ---- Barre de titre
    lv_obj_t *title_container = lv_obj_create(screen);
    lv_obj_set_size(title_container, 240, 32);
    lv_obj_remove_flag(title_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(title_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(title_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(title_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
 
    lv_obj_t *title = lv_label_create(title_container);
    lv_obj_set_size(title, 200, 24);
    lv_obj_set_pos(title, 40, 6);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title, "Bluetooth scanner");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00C217), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT_MONTSERRAT_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
 
    lv_obj_t *icon = lv_label_create(title_container);
    lv_obj_set_size(icon, 32, 32);
    lv_label_set_text(icon, UI_ICON_BLUETOOTH);
    lv_obj_set_style_text_font(icon, &ui_font_icons_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x1C8BBA), LV_PART_MAIN | LV_STATE_DEFAULT);
 
    // ---- Conteneur principal (liste des périphériques)
    s_main_container = lv_obj_create(screen);
    lv_obj_set_size(s_main_container, 240, 240);
    lv_obj_set_y(s_main_container, 40);
    lv_obj_add_flag(s_main_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_main_container, LV_DIR_VER);
    lv_obj_set_flex_flow(s_main_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_main_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(s_main_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_main_container, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(s_main_container, LV_BORDER_SIDE_FULL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(s_main_container, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(s_main_container, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(s_main_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(s_main_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
 
    lv_obj_t *header = lv_label_create(s_main_container);   // enfant n°0, jamais supprimé
    lv_obj_set_size(header, 240, 24);
    lv_obj_set_pos(header, -32, 40);
    lv_label_set_long_mode(header, LV_LABEL_LONG_WRAP);
    lv_label_set_text(header, "N°  Name       UUID      RSSI");
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(header, LV_FONT_DEFAULT_MONTSERRAT_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(header, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(header, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
 
    // ---- Conteneur de détail (même emplacement et même style que la liste, masqué au départ)
    s_detail_container = lv_obj_create(screen);
    lv_obj_set_size(s_detail_container, 240, 240);
    lv_obj_set_y(s_detail_container, 40);
    lv_obj_add_flag(s_detail_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_detail_container, LV_DIR_VER);
    lv_obj_set_flex_flow(s_detail_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_detail_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(s_detail_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_detail_container, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(s_detail_container, LV_BORDER_SIDE_FULL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(s_detail_container, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(s_detail_container, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(s_detail_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(s_detail_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_detail_container, LV_OBJ_FLAG_HIDDEN);

    // ---- Barre de contrôle (fond du bouton)

    lv_obj_t *control_container = lv_obj_create(screen);
    lv_obj_set_size(control_container, 240, 32);
    lv_obj_set_y(control_container, 288);
    lv_obj_remove_flag(control_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(control_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(control_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(control_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
 
    // ---- Bouton de scan
    s_button_scanner = lv_button_create(screen);
    lv_obj_set_size(s_button_scanner, 240, 32);
    lv_obj_set_y(s_button_scanner, 288);
 
    lv_obj_t *content = lv_obj_create(s_button_scanner);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(content, 6, 0);
 
    lv_obj_t *button_icon = lv_label_create(content);
    lv_label_set_text(button_icon, UI_ICON_BLUETOOTH_AUDIO);
    lv_obj_set_style_text_font(button_icon, &ui_font_icons_20, 0);
 
    s_button_label = lv_label_create(content);
    lv_label_set_text(s_button_label, "Start scan");
    lv_obj_center(content);
 
    lv_obj_set_style_bg_color(s_button_scanner, lv_color_hex(0x00C217), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(s_button_scanner, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_button_scanner, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_button_scanner, LV_FONT_DEFAULT_MONTSERRAT_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_button_scanner, button_scan_event, LV_EVENT_CLICKED, nullptr);

    // ---- Bouton Back (même emplacement que le bouton de scan, masqué au départ)
    s_button_back = lv_button_create(screen);
    lv_obj_set_size(s_button_back, 240, 32);
    lv_obj_set_y(s_button_back, 288);
    lv_obj_t *back_label = lv_label_create(s_button_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT "  Back");
    lv_obj_center(back_label);
    lv_obj_set_style_bg_color(s_button_back, lv_color_hex(0x00C217), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(s_button_back, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_button_back, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_button_back, LV_FONT_DEFAULT_MONTSERRAT_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_button_back, button_back_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_button_back, LV_OBJ_FLAG_HIDDEN);
}