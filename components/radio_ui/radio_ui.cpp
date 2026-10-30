#include "radio_ui.h"

#include <cmath>
#include <cstdio>

namespace esphome::radio_ui {

static void hide(lv_obj_t *o) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
static void show(lv_obj_t *o) { lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); }
static lv_color_t col(uint32_t hex) { return lv_color_hex(hex); }

// Accent border inside a logo box, drawn after the logo image (border_post). It stays within
// the box, which LVGL redraws as a whole; an outline outside the box was redrawn only in parts.
static void mark_playing(const Slot &s, bool playing) {
  lv_obj_set_style_border_width(s.logo_box, playing ? 2 : 0, 0);
  lv_obj_set_style_border_color(s.logo_box, col(ACCENT), 0);
  lv_obj_set_style_border_opa(s.logo_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_post(s.logo_box, true, 0);
}

void style_gauge(lv_obj_t *arc, int width) {
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, col(TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, col(TEXT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
}

const lv_font_t *fit_font(const char *text, const lv_font_t *const *fonts, int count, int max_width) {
  for (int i = 0; i < count; i++) {
    lv_point_t size;
    lv_text_get_size(&size, text, fonts[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (size.x <= max_width)
      return fonts[i];
  }
  return fonts[count - 1];
}

std::string gain_text(float value) {
  char buf[12];
  if (std::fabs(value) < 0.01f)
    return "0";
  if (std::fabs(value - std::round(value)) < 0.01f)
    snprintf(buf, sizeof(buf), "%+d", (int) std::lround(value));
  else
    snprintf(buf, sizeof(buf), "%+.1f", value);
  return buf;
}

int dim_step(float seconds) {
  int best = 0;
  for (int i = 1; i < DIM_STEP_COUNT; i++) {
    if (std::fabs(DIM_STEPS[i] - seconds) < std::fabs(DIM_STEPS[best] - seconds))
      best = i;
  }
  return best;
}

std::string dim_text(float seconds) {
  const int s = (int) std::lround(seconds);
  if (s <= 0)
    return "Aus";
  char buf[12];
  if (s >= 60 && s % 60 == 0)
    snprintf(buf, sizeof(buf), "%dm", s / 60);
  else
    snprintf(buf, sizeof(buf), "%ds", s);
  return buf;
}

static void render_slot(const Slot &s, const Item *it, bool center, bool editing,
                        station_logos::StationLogos *logos, const std::vector<std::string> &names) {
  if (it == nullptr) {
    hide(s.root);
    return;
  }
  show(s.root);
  hide(s.logo_box);
  hide(s.icon);
  hide(s.arc);
  hide(s.arc_lbl);
  // No whole-object opacity here: it would make LVGL allocate a layer buffer on every
  // redraw. Side logos come pre-dimmed from station_logos, side icons use the dim grey.
  switch (it->kind) {
    case Item::STATION: {
      if (it->station < 0 || it->station >= (int) names.size())
        return;
      show(s.logo_box);
      lv_obj_align(s.logo_lbl, LV_ALIGN_CENTER, 0, 0);
      const lv_image_dsc_t *logo = center ? logos->big(it->station) : logos->small(it->station);
      station_logos::show_logo(s.logo_box, s.logo_img, s.logo_lbl, logo, names[it->station], !center);
      lv_obj_align(s.logo_img, LV_ALIGN_CENTER, 0, 0);
      lv_obj_set_style_image_opa(s.logo_img, LV_OPA_COVER, 0);
      mark_playing(s, it->playing);
      break;
    }
    case Item::BACK:
    case Item::ICON: {
      show(s.icon);
      lv_label_set_text(s.icon, it->icon);
      if (it->playing) {
        // The playing source: the icon sits on a tile background with the accent border.
        show(s.logo_box);
        hide(s.logo_img);
        hide(s.logo_lbl);
        lv_obj_set_style_bg_opa(s.logo_box, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s.logo_box, col(center ? 0x26262E : 0x17171C), 0);
        mark_playing(s, true);
        if (it->letter != nullptr) {
          // The letter sits in the screen of the cast icon, right of the centre.
          show(s.logo_lbl);
          lv_label_set_text(s.logo_lbl, it->letter);
          lv_obj_set_style_text_color(s.logo_lbl, col(center ? TEXT : DIM), 0);
          lv_obj_align(s.logo_lbl, LV_ALIGN_CENTER, center ? 5 : 3, 0);
        }
      }
      uint32_t c = center ? (it->kind == Item::BACK ? SUB : TEXT) : DIM;
      lv_obj_set_style_text_color(s.icon, col(c), 0);
      break;
    }
    case Item::GAUGE: {
      show(s.arc);
      show(s.arc_lbl);
      lv_arc_set_range(s.arc, (int) std::lround(it->lo * 2), (int) std::lround(it->hi * 2));
      lv_arc_set_value(s.arc, (int) std::lround(it->value * 2));
      uint32_t c = editing ? ACCENT : (center ? TEXT : DIM);
      lv_obj_set_style_arc_color(s.arc, col(c), LV_PART_INDICATOR);
      lv_label_set_text(s.arc_lbl, it->value_text.empty() ? gain_text(it->value).c_str() : it->value_text.c_str());
      lv_obj_set_style_text_color(s.arc_lbl, col(c), 0);
      break;
    }
  }
}

void render(const Carousel &c, const char *title, const std::vector<Item> &items, int sel, bool editing,
            station_logos::StationLogos *logos, const std::vector<std::string> &names) {
  const int n = (int) items.size();
  if (n == 0)
    return;
  if (sel < 0)
    sel = 0;
  if (sel >= n)
    sel = n - 1;
  const Item &cur = items[sel];

  lv_label_set_text(c.title, title);
  render_slot(c.center, &cur, true, editing, logos, names);
  render_slot(c.left, (!editing && sel > 0) ? &items[sel - 1] : nullptr, false, false, logos, names);
  render_slot(c.right, (!editing && sel + 1 < n) ? &items[sel + 1] : nullptr, false, false, logos, names);
  if (editing) {
    show(c.chev_left);
    show(c.chev_right);
  } else {
    hide(c.chev_left);
    hide(c.chev_right);
  }

  lv_label_set_text(c.name, cur.label.c_str());
  lv_obj_set_style_text_font(c.name, fit_font(cur.label.c_str(), c.name_fonts, 3, 148), 0);
  if (cur.sub.empty()) {
    hide(c.sub);
  } else {
    show(c.sub);
    lv_label_set_text(c.sub, cur.sub.c_str());
    lv_obj_set_style_text_color(c.sub, col(editing ? ACCENT : SUB), 0);
  }

  // Position: dots for short rows, "k / n" for station rows longer than nine tiles.
  const bool use_dots = n <= 9;
  for (int i = 0; i < 9; i++) {
    if (!use_dots || i >= n) {
      hide(c.dots[i]);
      continue;
    }
    show(c.dots[i]);
    const int x = 80 - (n - 1) * 7 / 2 + i * 7 - 2;
    lv_obj_set_pos(c.dots[i], x, 118);
    lv_obj_set_style_bg_color(c.dots[i], col(i == sel ? TEXT : DIM), 0);
  }
  int stations = 0;
  for (const auto &it : items)
    stations += it.kind == Item::STATION;
  if (!use_dots && cur.kind == Item::STATION) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", cur.station + 1, stations);
    lv_label_set_text(c.count, buf);
    show(c.count);
  } else {
    hide(c.count);
  }
}

}  // namespace esphome::radio_ui
