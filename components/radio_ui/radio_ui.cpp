#include "radio_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>

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

const lv_font_t *fit_lines(const char *text, const lv_font_t *const *fonts, int count, int max_width, int max_lines) {
  for (int i = 0; i < count; i++) {
    // Every word must fit on a line of its own, otherwise LVGL would break it.
    bool words_fit = true;
    const char *p = text;
    while (*p != '\0' && words_fit) {
      while (*p == ' ')
        p++;
      const char *end = p;
      while (*end != '\0' && *end != ' ')
        end++;
      if (end > p) {
        std::string word(p, end - p);
        lv_point_t size;
        lv_text_get_size(&size, word.c_str(), fonts[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        words_fit = size.x <= max_width;
      }
      p = end;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, fonts[i], 0, 0, max_width, LV_TEXT_FLAG_NONE);
    const int lines = (size.y + lv_font_get_line_height(fonts[i]) / 2) / lv_font_get_line_height(fonts[i]);
    if (words_fit && lines <= max_lines)
      return fonts[i];
  }
  return fonts[count - 1];
}

const lv_image_dsc_t *make_backdrop(const lv_image_dsc_t *src) {
  static constexpr int W = 160, H = 128, BLUR = 3;
  static constexpr float DARK = 0.33f;
  static uint16_t *out = nullptr;
  static uint8_t *rgb = nullptr, *tmp = nullptr;
  static lv_image_dsc_t dsc;
  if (src == nullptr || src->data == nullptr || src->header.cf != LV_COLOR_FORMAT_RGB565)
    return nullptr;
  const int sw = src->header.w, sh = src->header.h;
  if (sw <= 0 || sh <= 0)
    return nullptr;
  if (out == nullptr) {
    out = static_cast<uint16_t *>(heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM));
    rgb = static_cast<uint8_t *>(heap_caps_malloc(W * H * 3, MALLOC_CAP_SPIRAM));
    tmp = static_cast<uint8_t *>(heap_caps_malloc(W * H * 3, MALLOC_CAP_SPIRAM));
    if (out == nullptr || rgb == nullptr || tmp == nullptr)
      return nullptr;
  }
  // Scale to fill the screen (the overflowing side is cropped), nearest pixel; the blur
  // smooths the enlarged pixels.
  const int stride = src->header.stride ? src->header.stride : sw * 2;
  const float scale = std::max(W / (float) sw, H / (float) sh);
  const float ox = (sw * scale - W) / 2, oy = (sh * scale - H) / 2;
  for (int y = 0; y < H; y++) {
    const int sy = std::min(sh - 1, (int) ((y + oy) / scale));
    const uint16_t *row = reinterpret_cast<const uint16_t *>(src->data + sy * stride);
    for (int x = 0; x < W; x++) {
      const uint16_t v = row[std::min(sw - 1, (int) ((x + ox) / scale))];
      uint8_t *p = &rgb[(y * W + x) * 3];
      p[0] = ((v >> 11) & 0x1F) * 255 / 31;
      p[1] = ((v >> 5) & 0x3F) * 255 / 63;
      p[2] = (v & 0x1F) * 255 / 31;
    }
  }
  // Box blur: horizontal into tmp, vertical back into rgb; edges repeat the border pixel.
  for (int pass = 0; pass < 2; pass++) {
    const uint8_t *in = pass == 0 ? rgb : tmp;
    uint8_t *dst = pass == 0 ? tmp : rgb;
    const int len = pass == 0 ? W : H, lines = pass == 0 ? H : W;
    for (int l = 0; l < lines; l++) {
      for (int c = 0; c < 3; c++) {
        auto at = [&](int i) {
          i = std::max(0, std::min(len - 1, i));
          return pass == 0 ? in[(l * W + i) * 3 + c] : in[(i * W + l) * 3 + c];
        };
        int sum = 0;
        for (int i = -BLUR; i <= BLUR; i++)
          sum += at(i);
        for (int i = 0; i < len; i++) {
          const uint8_t v = sum / (2 * BLUR + 1);
          if (pass == 0)
            dst[(l * W + i) * 3 + c] = v;
          else
            dst[(i * W + l) * 3 + c] = v;
          sum += at(i + BLUR + 1) - at(i - BLUR);
        }
      }
    }
  }
  for (int i = 0; i < W * H; i++) {
    const uint8_t *p = &rgb[i * 3];
    const uint8_t r = p[0] * DARK, g = p[1] * DARK, b = p[2] * DARK;
    out[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }
  memset(&dsc, 0, sizeof(dsc));
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = W;
  dsc.header.h = H;
  dsc.header.stride = W * 2;
  dsc.data_size = W * H * 2;
  dsc.data = reinterpret_cast<const uint8_t *>(out);
  return &dsc;
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
