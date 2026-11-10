#pragma once

#include <string>
#include <vector>

#include <lvgl.h>

#include "esphome/core/defines.h"
#include "esphome/components/station_logos/station_logos.h"

namespace esphome::radio_ui {

// Colours. Three brightness levels only, because the TN panel loses dark greys at an angle.
static constexpr uint32_t BG = 0x060608;
static constexpr uint32_t TEXT = 0xFFFFFF;
static constexpr uint32_t SUB = 0xC4C4CC;
static constexpr uint32_t DIM = 0x787880;
static constexpr uint32_t TRACK = 0x46464E;
static constexpr uint32_t ACCENT = 0xFFB020;

// Screen size as LVGL sees it and the tile sizes in the carousel, from the radio_ui options.
// The layout is drawn for 160x128; the boxes in the YAML use the same tile sizes.
static constexpr int SCREEN_W = RADIO_UI_SCREEN_WIDTH;
static constexpr int SCREEN_H = RADIO_UI_SCREEN_HEIGHT;
static constexpr int TILE_CENTER = RADIO_UI_TILE_CENTER;
static constexpr int TILE_SIDE = RADIO_UI_TILE_SIDE;

/// A length of the 160x128 layout on this screen, rounded: horizontal, vertical, and by the
/// smaller of the two factors (for sizes that stay square).
constexpr int scale_x(int v) { return (2 * v * SCREEN_W + 160) / 320; }
constexpr int scale_y(int v) { return (2 * v * SCREEN_H + 128) / 256; }
constexpr int scale_f(int v) { return SCREEN_W * 128 <= SCREEN_H * 160 ? scale_x(v) : scale_y(v); }

// Tabler icon codepoints, UTF-8 encoded.
static const char *const ICON_BACK = "";
static const char *const ICON_TUNE = "";
static const char *const ICON_INFO = "";
static const char *const ICON_RADIO = "";
static const char *const ICON_SETTINGS = "";
static const char *const ICON_CAST = "";
static const char *const ICON_DIM = "";
static const char *const ICON_WIFI = "";
static const char *const ICON_SIGNAL = "";
static const char *const ICON_NETWORK = "";
static const char *const ICON_CPU = "";
static const char *const ICON_VOLUME = "";
static const char *const ICON_MUTE = "";

/// One tile of the main menu.
struct Item {
  enum Kind { ICON, STATION };
  Kind kind{ICON};
  const char *icon{""};
  std::string label;
  std::string sub;
  int station{-1};  // index into the station list for STATION
  bool playing{false};           // STATION / ICON: tile background with an accent border
  const char *letter{nullptr};   // ICON while playing: letter inside the screen of the cast icon
  const lv_image_dsc_t *cover{nullptr};  // ICON while playing: cover shown instead of the icon
};

/// Steps of the "dim after" setting in seconds; 0 = never.
static constexpr int DIM_STEPS[] = {0, 10, 15, 30, 60, 120, 300};
static constexpr int DIM_STEP_COUNT = sizeof(DIM_STEPS) / sizeof(DIM_STEPS[0]);

/// Index of the step closest to seconds.
int dim_step(float seconds);

/// "Aus", "15 s" or "2 min" for a dim time in seconds.
std::string dim_text(float seconds);

/// Widgets of one tile position. Only the parts matching the item kind are shown.
struct Slot {
  lv_obj_t *root;
  lv_obj_t *logo_box;
  lv_obj_t *logo_img;
  lv_obj_t *logo_lbl;
  lv_obj_t *icon;
};

/// All widgets of the carousel page.
struct Carousel {
  lv_obj_t *name;
  lv_obj_t *sub;
  Slot center, left, right;
  const lv_font_t *name_fonts[3];  // largest first
};

/// Largest font of the list in which text fits into max_width pixels.
const lv_font_t *fit_font(const char *text, const lv_font_t *const *fonts, int count, int max_width);

/// Largest font of the list in which text wraps into at most max_lines lines of max_width
/// pixels without breaking a word; the last font when none fits.
const lv_font_t *fit_lines(const char *text, const lv_font_t *const *fonts, int count, int max_width, int max_lines);

/// Background for the playing screen: src scaled to fill the screen, blurred and darkened.
/// nullptr when src is missing or not RGB565. Main loop only; every call reuses one buffer.
const lv_image_dsc_t *make_backdrop(const lv_image_dsc_t *src);

/// "0", "+6" or "-1.5" for a gain in dB.
std::string gain_text(float value);

/// Show items[sel] in the centre with its neighbours, name and sub text.
/// Station logos use the display buffer slots 0 (centre), 1 (left) and 2 (right).
void render(const Carousel &c, const std::vector<Item> &items, int sel,
            station_logos::StationLogos *logos, const std::vector<std::string> &names);

/// Page transition: body starts 10 px (scaled) lower and slides up in 220 ms (ease out).
void settle_in(lv_obj_t *body);

}  // namespace esphome::radio_ui
