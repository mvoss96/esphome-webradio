#include "radio_ui.h"

#include <algorithm>
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
  char buf[20];
  if (s >= 60 && s % 60 == 0)
    snprintf(buf, sizeof(buf), "%d min", s / 60);
  else
    snprintf(buf, sizeof(buf), "%d s", s);
  return buf;
}

static void render_slot(const Slot &s, int slot, const Item *it, bool center, bool editing,
                        station_logos::StationLogos *logos, const std::vector<std::string> &names) {
  if (it == nullptr) {
    hide(s.root);
    return;
  }
  show(s.root);
  hide(s.logo_box);
  hide(s.icon);
  hide(s.value_lbl);
  // No whole-object opacity here: it would make LVGL allocate a layer buffer on every
  // redraw. Side logos come pre-dimmed from station_logos, side icons use the dim grey.
  switch (it->kind) {
    case Item::STATION: {
      if (it->station < 0 || it->station >= (int) names.size())
        return;
      show(s.logo_box);
      lv_obj_align(s.logo_lbl, LV_ALIGN_CENTER, 0, 0);
      const lv_image_dsc_t *logo = logos->get(slot, it->station, center ? TILE_CENTER : TILE_SIDE, !center);
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
          lv_obj_align(s.logo_lbl, LV_ALIGN_CENTER, center ? 6 : 3, 0);
        }
      }
      uint32_t c = center ? (it->kind == Item::BACK ? SUB : TEXT) : DIM;
      lv_obj_set_style_text_color(s.icon, col(c), 0);
      break;
    }
    case Item::STEPS: {
      // An icon tile like the others; while editing, the value replaces the icon.
      if (center && editing) {
        show(s.value_lbl);
        lv_label_set_text(s.value_lbl, it->value_text.c_str());
        lv_obj_set_style_text_color(s.value_lbl, col(ACCENT), 0);
      } else {
        show(s.icon);
        lv_label_set_text(s.icon, it->icon);
        lv_obj_set_style_text_color(s.icon, col(center ? TEXT : DIM), 0);
      }
      break;
    }
  }
}

void render(const Carousel &c, const std::vector<Item> &items, int sel, bool editing,
            station_logos::StationLogos *logos, const std::vector<std::string> &names) {
  const int n = (int) items.size();
  if (n == 0)
    return;
  if (sel < 0)
    sel = 0;
  if (sel >= n)
    sel = n - 1;
  const Item &cur = items[sel];

  render_slot(c.center, 0, &cur, true, editing, logos, names);
  render_slot(c.left, 1, (!editing && sel > 0) ? &items[sel - 1] : nullptr, false, false, logos, names);
  render_slot(c.right, 2, (!editing && sel + 1 < n) ? &items[sel + 1] : nullptr, false, false, logos, names);
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

  // Step dots below the value while a STEPS item is edited; the current step is larger.
  for (int k = 0; k < DIM_STEP_COUNT; k++) {
    if (!editing || cur.kind != Item::STEPS || k >= cur.step_count) {
      hide(c.steps[k]);
      continue;
    }
    const bool on = k == cur.step;
    const int r = on ? 3 : 2;
    const int x = 30 + 100 * k / std::max(1, cur.step_count - 1);
    show(c.steps[k]);
    lv_obj_set_size(c.steps[k], 2 * r, 2 * r);
    lv_obj_set_pos(c.steps[k], x - r, 72 - r);
    lv_obj_set_style_bg_color(c.steps[k], col(on ? ACCENT : DIM), 0);
  }
}

}  // namespace esphome::radio_ui
