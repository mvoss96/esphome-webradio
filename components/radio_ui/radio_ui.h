#pragma once

#include <string>
#include <vector>

#include <lvgl.h>

#include "esphome/components/station_logos/station_logos.h"

namespace esphome::radio_ui {

// Colours. Three brightness levels only, because the TN panel loses dark greys at an angle.
static constexpr uint32_t TEXT = 0xFFFFFF;
static constexpr uint32_t SUB = 0xC4C4CC;
static constexpr uint32_t DIM = 0x787880;
static constexpr uint32_t TRACK = 0x46464E;
static constexpr uint32_t ACCENT = 0xFFB020;

// Tile sizes in the carousel; the boxes in the YAML match them.
static constexpr int TILE_CENTER = 68;
static constexpr int TILE_SIDE = 40;

// Tabler icon codepoints, UTF-8 encoded.
static const char *const ICON_BACK = "";
static const char *const ICON_TUNE = "";
static const char *const ICON_INFO = "";
static const char *const ICON_RADIO = "";
static const char *const ICON_SETTINGS = "";
static const char *const ICON_CAST = "";
static const char *const ICON_WIFI = "";
static const char *const ICON_SIGNAL = "";
static const char *const ICON_NETWORK = "";
static const char *const ICON_CPU = "";
static const char *const ICON_VOLUME = "";
static const char *const ICON_MUTE = "";

/// One tile of a carousel.
struct Item {
  enum Kind { BACK, ICON, STATION, GAUGE };
  Kind kind{ICON};
  const char *icon{""};
  std::string label;
  std::string sub;
  int station{-1};  // index into the station list for STATION
  float value{0}, lo{0}, hi{1};  // GAUGE range in dB
  std::string value_text;        // GAUGE: shown instead of the dB value when set
  bool playing{false};           // STATION / ICON: tile background with an accent border
  const char *letter{nullptr};   // ICON while playing: letter inside the screen of the cast icon
};

/// Steps of the "dim after" setting in seconds; 0 = never.
static constexpr int DIM_STEPS[] = {0, 10, 15, 30, 60, 120, 300};
static constexpr int DIM_STEP_COUNT = sizeof(DIM_STEPS) / sizeof(DIM_STEPS[0]);

/// Index of the step closest to seconds.
int dim_step(float seconds);

/// "Aus", "15s" or "2m" for a dim time in seconds.
std::string dim_text(float seconds);

/// Widgets of one tile position. Only the parts matching the item kind are shown.
struct Slot {
  lv_obj_t *root;
  lv_obj_t *logo_box;
  lv_obj_t *logo_img;
  lv_obj_t *logo_lbl;
  lv_obj_t *icon;
  lv_obj_t *arc;
  lv_obj_t *arc_lbl;
};

/// All widgets of the carousel page.
struct Carousel {
  lv_obj_t *name;
  lv_obj_t *sub;
  lv_obj_t *chev_left;
  lv_obj_t *chev_right;
  Slot center, left, right;
  const lv_font_t *name_fonts[3];  // largest first
};

/// Style an lv_arc as a 270 degree gauge without knob.
void style_gauge(lv_obj_t *arc, int width);

/// Largest font of the list in which text fits into max_width pixels.
const lv_font_t *fit_font(const char *text, const lv_font_t *const *fonts, int count, int max_width);

/// "0", "+6" or "-1.5" for a gain in dB.
std::string gain_text(float value);

/// Show items[sel] in the centre with its neighbours, name and sub text.
/// While editing, the neighbours are hidden and arrows appear left and right.
/// Station logos use the display buffer slots 0 (centre), 1 (left) and 2 (right).
void render(const Carousel &c, const std::vector<Item> &items, int sel, bool editing,
            station_logos::StationLogos *logos, const std::vector<std::string> &names);

}  // namespace esphome::radio_ui
