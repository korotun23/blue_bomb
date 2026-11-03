/* LCD Wizard icons at 20 px: bluetooth-audio.
 * From LCD Wizard, released under the Unlicense - free to use, no attribution required.
 * Show one with lv_label_set_text(label, UI_ICON_...) on a label using this font. */
/*******************************************************************************
 * Size: 20 px
 * Bpp: 4
 * Opts: --bpp 4 --size 20 --no-compress, 1 icon (LCD Wizard icon library)
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef UI_FONT_ICONS_20
#define UI_FONT_ICONS_20 1
#endif

#if UI_FONT_ICONS_20

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+E001 bluetooth-audio */
    0x0, 0x0, 0xa2, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0xdc, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0xd4, 0xb0, 0x0, 0x0, 0x0, 0x0,
    0x11, 0x0, 0xd2, 0x5a, 0x10, 0x0, 0x44, 0x0,
    0x2e, 0x0, 0xd2, 0x9, 0x80, 0x0, 0x4f, 0x0,
    0x2, 0xd0, 0xd2, 0x88, 0x0, 0x65, 0x7, 0x80,
    0x0, 0x3c, 0xd6, 0x90, 0x0, 0x5b, 0x2, 0xe0,
    0x0, 0x4, 0xda, 0x0, 0x0, 0xf, 0x10, 0xf0,
    0x0, 0x4, 0xda, 0x0, 0x0, 0xf, 0x10, 0xf0,
    0x0, 0x3c, 0xd6, 0x90, 0x0, 0x5b, 0x2, 0xe0,
    0x2, 0xd0, 0xd2, 0x88, 0x0, 0x65, 0x7, 0x80,
    0x2e, 0x0, 0xd2, 0x9, 0x80, 0x0, 0x4f, 0x0,
    0x11, 0x0, 0xd2, 0x5a, 0x10, 0x0, 0x44, 0x0,
    0x0, 0x0, 0xd4, 0xb0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0xdc, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0xa2, 0x0, 0x0, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 320, .box_w = 16, .box_h = 16, .ofs_x = 2, .ofs_y = 2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 57345, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t ui_font_icons_20 = {
#else
lv_font_t ui_font_icons_20 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 20,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if UI_FONT_ICONS_20*/

