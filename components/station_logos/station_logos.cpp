#include "station_logos.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <pngle.h>

#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::station_logos {

static const char *const TAG = "station_logos";
// Plain HTTP: public data, and no TLS handshake (heavy ECC maths) per request.
static const char *const API = "http://de1.api.radio-browser.info/json/stations/";
static constexpr size_t MAX_JSON = 48 * 1024;
static constexpr size_t MAX_PNG = 32 * 1024;
static constexpr size_t MAX_CANDIDATES = 3;

// ---------------- helpers ----------------

/// Append the UTF-8 character starting at s[pos] to out, upper-cased if ASCII.
static size_t append_char(const std::string &s, size_t pos, std::string &out) {
  size_t len = 1;
  unsigned char c = s[pos];
  if (c >= 0xF0)
    len = 4;
  else if (c >= 0xE0)
    len = 3;
  else if (c >= 0xC0)
    len = 2;
  if (len == 1) {
    out += (char) toupper(c);
  } else {
    out += s.substr(pos, len);
  }
  return len;
}

std::string initials(const std::string &name) {
  std::vector<size_t> starts;
  bool in_word = false;
  for (size_t i = 0; i < name.size(); i++) {
    bool sep = name[i] == ' ' || name[i] == '-';
    if (!sep && !in_word)
      starts.push_back(i);
    in_word = !sep;
  }
  std::string out;
  if (starts.size() >= 2) {
    append_char(name, starts[0], out);
    append_char(name, starts[1], out);
  } else if (!starts.empty()) {
    size_t n = append_char(name, starts[0], out);
    if (starts[0] + n < name.size())
      append_char(name, starts[0] + n, out);
  }
  return out;
}

void show_logo(lv_obj_t *box, lv_obj_t *img, lv_obj_t *label, const lv_image_dsc_t *logo,
               const std::string &name, bool dim) {
  if (logo != nullptr) {
    // Corners and dimming are already in the pixels; the box only sits behind them.
    lv_image_set_src(img, logo);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  } else {
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(label, initials(name).c_str());
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(dim ? 0x17171C : 0x26262E), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(dim ? 0x787880 : 0xC4C4CC), 0);
  }
}

static std::string url_encode(const std::string &s) {
  static const char *const HEX = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char) c;
    } else {
      out += '%';
      out += HEX[c >> 4];
      out += HEX[c & 0x0F];
    }
  }
  return out;
}

/// Lower-case ASCII letters and digits only, for comparing station names.
static std::string normalize(const std::string &s) {
  std::string out;
  for (unsigned char c : s)
    if (isalnum(c))
      out += (char) tolower(c);
  return out;
}

// One receive buffer in PSRAM for all downloads. The task handles one request at a
// time, and a fixed buffer avoids growing heap blocks in the small internal RAM:
// a failed allocation there aborts the firmware, because exceptions are disabled.
static constexpr size_t BUF_SIZE = 48 * 1024;
static char *buf = nullptr;

struct Body {
  size_t len;
  size_t max;
  bool overflow;
};

static esp_err_t http_event(esp_http_client_event_t *evt) {
  auto *body = static_cast<Body *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
    body->len = 0;
    body->overflow = false;
  } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (body->len + evt->data_len > body->max) {
      body->overflow = true;
    } else {
      memcpy(buf + body->len, evt->data, evt->data_len);
      body->len += evt->data_len;
    }
  }
  return ESP_OK;
}

/// GET url into buf. Returns the body length, or 0 on network error, non-200 or a body
/// larger than max bytes.
static size_t http_get(const std::string &url, size_t max) {
  if (buf == nullptr)
    return 0;
  Body body{0, std::min(max, BUF_SIZE), false};
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 10000;
  cfg.user_agent = "esphome-web-radio/1.0";
  cfg.event_handler = http_event;
  cfg.user_data = &body;
  cfg.buffer_size = 2048;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr)
    return 0;
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK || status != 200 || body.overflow) {
    ESP_LOGD(TAG, "GET failed (%s, HTTP %d%s): %s", esp_err_to_name(err), status,
             body.overflow ? ", too large" : "", url.c_str());
    return 0;
  }
  return body.len;
}

/// ArduinoJson allocator that keeps parsed documents in PSRAM.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *ptr) override { heap_caps_free(ptr); }
  void *reallocate(void *ptr, size_t size) override {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};
static PsramAllocator psram_allocator;

struct Candidate {
  std::string url;
  int votes;
  bool name_match;
};

/// Collect favicon URLs from a radio-browser.info JSON array in buf.
static void parse_candidates(size_t len, const std::string &wanted, std::vector<Candidate> &out) {
  JsonDocument filter(&psram_allocator);
  filter[0]["name"] = true;
  filter[0]["favicon"] = true;
  filter[0]["votes"] = true;
  JsonDocument doc(&psram_allocator);
  if (deserializeJson(doc, buf, len, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
    return;
  for (JsonObject st : doc.as<JsonArray>()) {
    const char *fav = st["favicon"] | "";
    if (fav[0] == '\0' || out.size() >= 20)
      continue;
    std::string name = normalize(st["name"] | "");
    bool match = !wanted.empty() && name.find(wanted) != std::string::npos;
    out.push_back({fav, st["votes"] | 0, match});
  }
}


// ---------------- decoding ----------------

struct Decode {
  uint8_t rgb[STORE * STORE * 3];
  uint32_t w{0}, h{0};
};

static void png_init(pngle_t *pngle, uint32_t w, uint32_t h) {
  auto *d = static_cast<Decode *>(pngle_get_user_data(pngle));
  d->w = w;
  d->h = h;
}

static void png_draw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  auto *d = static_cast<Decode *>(pngle_get_user_data(pngle));
  // Centre images smaller than STORE; blend transparency over white.
  const uint32_t ox = d->w < STORE ? (STORE - d->w) / 2 : 0;
  const uint32_t oy = d->h < STORE ? (STORE - d->h) / 2 : 0;
  const uint32_t a = rgba[3];
  uint8_t px[3];
  for (int c = 0; c < 3; c++)
    px[c] = (rgba[c] * a + 255 * (255 - a)) / 255;
  for (uint32_t yy = y; yy < y + h; yy++) {
    for (uint32_t xx = x; xx < x + w; xx++) {
      uint32_t tx = xx + ox, ty = yy + oy;
      if (tx >= (uint32_t) STORE || ty >= (uint32_t) STORE)
        continue;
      memcpy(&d->rgb[(ty * STORE + tx) * 3], px, 3);
    }
  }
}

static inline uint16_t rgb565(const uint8_t *p) {
  return ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
}

static inline void rgb888(uint16_t v, uint8_t *p) {
  p[0] = ((v >> 11) & 0x1F) * 255 / 31;
  p[1] = ((v >> 5) & 0x3F) * 255 / 63;
  p[2] = (v & 0x1F) * 255 / 31;
}

/// Scale a square RGB888 image from ssize to dsize by averaging the source pixels whose
/// centres fall into each target pixel. dsize must not be larger than ssize.
static void scale(const uint8_t *src, int ssize, uint8_t *dst, int dsize) {
  for (int dy = 0; dy < dsize; dy++) {
    for (int dx = 0; dx < dsize; dx++) {
      uint32_t sum[3] = {0, 0, 0}, n = 0;
      for (int y = dy * ssize / dsize; y <= (dy + 1) * ssize / dsize && y < ssize; y++) {
        if ((2 * y + 1) * dsize < 2 * dy * ssize || (2 * y + 1) * dsize >= 2 * (dy + 1) * ssize)
          continue;
        for (int x = dx * ssize / dsize; x <= (dx + 1) * ssize / dsize && x < ssize; x++) {
          if ((2 * x + 1) * dsize < 2 * dx * ssize || (2 * x + 1) * dsize >= 2 * (dx + 1) * ssize)
            continue;
          const uint8_t *p = &src[(y * ssize + x) * 3];
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          n++;
        }
      }
      uint8_t *d = &dst[(dy * dsize + dx) * 3];
      for (int c = 0; c < 3; c++)
        d[c] = n ? sum[c] / n : 0xFF;
    }
  }
}

/// Convert size x size RGB888 to RGB565 with rounded corners and an optional dimming,
/// both blended towards the page background. LVGL would need a temporary layer per
/// frame for clip_corner or a whole-object opacity; baking it in avoids those buffers.
static void bake(const uint8_t *rgb, int size, int radius, float brightness, uint16_t *out) {
  static const uint8_t BG[3] = {0x06, 0x06, 0x08};
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      // Distance from the nearest corner circle centre, only inside the corner squares.
      float coverage = 1.0f;
      const int cx = x < radius ? radius : (x >= size - radius ? size - radius : -1);
      const int cy = y < radius ? radius : (y >= size - radius ? size - radius : -1);
      if (cx >= 0 && cy >= 0) {
        const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        const float dist = std::sqrt(dx * dx + dy * dy);
        coverage = std::max(0.0f, std::min(1.0f, radius - dist + 0.5f));
      }
      const float k = coverage * brightness;
      const uint8_t *p = &rgb[(y * size + x) * 3];
      uint8_t px[3];
      for (int c = 0; c < 3; c++)
        px[c] = (uint8_t) (BG[c] + (p[c] - BG[c]) * k);
      out[y * size + x] = rgb565(px);
    }
  }
}

static void init_dsc(lv_image_dsc_t &dsc, uint16_t *px, int size) {
  memset(&dsc, 0, sizeof(dsc));
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = size;
  dsc.header.h = size;
  dsc.header.stride = size * 2;
  dsc.data_size = size * size * 2;
  dsc.data = reinterpret_cast<const uint8_t *>(px);
}

/// FNV-1a hash of the parts that decide which logo a station gets; never 0.
static uint32_t job_key(const std::string &name, const std::string &stream_url, const std::string &logo_url) {
  uint32_t h = 2166136261u;
  for (const std::string *s : {&name, &stream_url, &logo_url}) {
    for (unsigned char c : *s)
      h = (h ^ c) * 16777619u;
    h = (h ^ '\t') * 16777619u;
  }
  return h ? h : 1;
}

// ---------------- storage ----------------

// One flash record per logo: header, then STORE x STORE RGB565 pixels. Three 4 KB
// sectors, so a record can be erased on its own.
static constexpr uint32_t RECORD_SIZE = 0x3000;
static constexpr uint32_t RECORD_MAGIC = 0x4F474F4C;  // "LOGO"
static constexpr uint32_t PIXEL_OFFSET = 16;
static constexpr size_t PIXEL_BYTES = STORE * STORE * 2;
static constexpr int MEM_RECORDS = 48;  // PSRAM fallback without the partition

struct RecordHeader {
  uint32_t magic;
  uint32_t key;
  uint32_t size;
  uint32_t reserved;
};

bool StationLogos::read_record_(int r, uint16_t *out) {
  if (this->partition_ == nullptr) {
    if (this->mem_records_[r] == nullptr)
      return false;
    memcpy(out, this->mem_records_[r], PIXEL_BYTES);
    return true;
  }
  return esp_partition_read(this->partition_, r * RECORD_SIZE + PIXEL_OFFSET, out, PIXEL_BYTES) == ESP_OK;
}

bool StationLogos::write_record_(int r, uint32_t key, const uint16_t *px) {
  if (this->partition_ == nullptr) {
    if (this->mem_records_[r] == nullptr)
      this->mem_records_[r] = static_cast<uint16_t *>(heap_caps_malloc(PIXEL_BYTES, MALLOC_CAP_SPIRAM));
    if (this->mem_records_[r] == nullptr)
      return false;
    memcpy(this->mem_records_[r], px, PIXEL_BYTES);
    return true;
  }
  const uint32_t off = r * RECORD_SIZE;
  // Pixels first, header last: a record cut short by a reset has no valid header.
  RecordHeader hdr{RECORD_MAGIC, key, STORE, 0};
  return esp_partition_erase_range(this->partition_, off, RECORD_SIZE) == ESP_OK &&
         esp_partition_write(this->partition_, off + PIXEL_OFFSET, px, PIXEL_BYTES) == ESP_OK &&
         esp_partition_write(this->partition_, off, &hdr, sizeof(hdr)) == ESP_OK;
}

// ---------------- component ----------------

void StationLogos::setup() {
  buf = static_cast<char *>(heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  for (auto &s : this->slots_)
    s.px = static_cast<uint16_t *>(heap_caps_malloc(PIXEL_BYTES, MALLOC_CAP_SPIRAM));
  this->read_px_ = static_cast<uint16_t *>(heap_caps_malloc(PIXEL_BYTES, MALLOC_CAP_SPIRAM));
  this->rgb_ = static_cast<uint8_t *>(heap_caps_malloc(STORE * STORE * 3, MALLOC_CAP_SPIRAM));
  this->scaled_ = static_cast<uint8_t *>(heap_caps_malloc(STORE * STORE * 3, MALLOC_CAP_SPIRAM));
  bool ok = buf != nullptr && this->read_px_ != nullptr && this->rgb_ != nullptr && this->scaled_ != nullptr;
  for (auto &s : this->slots_)
    ok &= s.px != nullptr;
  if (!ok) {
    ESP_LOGE(TAG, "No PSRAM for the logo buffers; logos disabled");
    return;
  }

  this->partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "logos");
  if (this->partition_ != nullptr) {
    this->record_count_ = this->partition_->size / RECORD_SIZE;
    this->record_keys_.assign(this->record_count_, 0);
    for (int r = 0; r < this->record_count_; r++) {
      RecordHeader hdr;
      if (esp_partition_read(this->partition_, r * RECORD_SIZE, &hdr, sizeof(hdr)) == ESP_OK &&
          hdr.magic == RECORD_MAGIC && hdr.size == STORE)
        this->record_keys_[r] = hdr.key;
    }
    ESP_LOGI(TAG, "Logo partition: %d records", this->record_count_);
  } else {
    ESP_LOGW(TAG, "No \"logos\" partition; logos are kept in PSRAM and loaded after every restart");
    this->record_count_ = MEM_RECORDS;
    this->record_keys_.assign(MEM_RECORDS, 0);
    this->mem_records_.assign(MEM_RECORDS, nullptr);
  }

  // Idle priority on core 0: the task only gets spare CPU time, so neither the main
  // loop on core 1 nor the idle tasks (task watchdog) are starved.
  xTaskCreatePinnedToCore(task_entry_, "logos", 12288, this, tskIDLE_PRIORITY, &this->task_, 0);
}

void StationLogos::load(const std::vector<std::string> &names, const std::vector<std::string> &stream_urls,
                        const std::vector<std::string> &logo_urls) {
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->jobs_.clear();
    for (size_t i = 0; i < names.size(); i++) {
      const std::string stream = i < stream_urls.size() ? stream_urls[i] : "";
      const std::string logo = i < logo_urls.size() ? logo_urls[i] : "";
      this->jobs_.push_back({names[i], stream, logo, job_key(names[i], stream, logo)});
    }
    this->record_of_.assign(names.size(), -1);
    this->generation_++;
  }
  if (this->task_ != nullptr)
    xTaskNotifyGive(this->task_);
}

const lv_image_dsc_t *StationLogos::get(int slot, size_t i, int size, bool dim) {
  if (slot < 0 || slot >= SLOT_COUNT || size < 8 || size > STORE || this->slots_[slot].px == nullptr)
    return nullptr;
  int r;
  uint32_t key;
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (i >= this->record_of_.size() || this->record_of_[i] < 0)
      return nullptr;
    r = this->record_of_[i];
    key = this->jobs_[i].key;
  }
  Slot &s = this->slots_[slot];
  if (s.key == key && s.size == size && s.dim == dim)
    return &s.dsc;
  if (!this->read_record_(r, this->read_px_))
    return nullptr;
  for (int k = 0; k < STORE * STORE; k++)
    rgb888(this->read_px_[k], &this->rgb_[k * 3]);
  const uint8_t *src = this->rgb_;
  if (size != STORE) {
    scale(this->rgb_, STORE, this->scaled_, size);
    src = this->scaled_;
  }
  bake(src, size, std::max(7, size * 12 / 56), dim ? DIM_BRIGHTNESS : 1.0f, s.px);
  // Same buffer as before: lv_image_set_src() still redraws the widget.
  init_dsc(s.dsc, s.px, size);
  s.key = key;
  s.size = size;
  s.dim = dim;
  return &s.dsc;
}

void StationLogos::task_entry_(void *arg) { static_cast<StationLogos *>(arg)->run_(); }

void StationLogos::run_() {
  RAMAllocator<uint16_t> px_alloc;
  uint16_t *px = px_alloc.allocate(STORE * STORE);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool restart = true;
    while (restart && px != nullptr) {
      restart = false;
      std::vector<Job> jobs;
      uint32_t gen;
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        jobs = this->jobs_;
        gen = this->generation_;
      }

      // Stored logos first; they need no network.
      std::vector<bool> missing(jobs.size(), true);
      for (size_t i = 0; i < jobs.size(); i++) {
        for (int r = 0; r < this->record_count_; r++) {
          if (this->record_keys_[r] != jobs[i].key)
            continue;
          std::lock_guard<std::mutex> lock(this->mutex_);
          if (gen == this->generation_)
            this->record_of_[i] = r;
          missing[i] = false;
          break;
        }
      }
      this->changed_ = true;

      size_t found = 0, stored = jobs.size() - std::count(missing.begin(), missing.end(), true);
      for (size_t i = 0; i < jobs.size() && !restart; i++) {
        if (!missing[i])
          continue;
        while (!network::is_connected())
          vTaskDelay(pdMS_TO_TICKS(1000));
        bool ok = false;
        for (const auto &url : this->candidates_(jobs[i])) {
          ok = this->fetch_(url, px);
          if (ok)
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!ok) {
          ESP_LOGD(TAG, "No logo for '%s'", jobs[i].name.c_str());
          continue;
        }
        // A free record is one whose logo no station of this list uses.
        int free_r = -1;
        for (int r = 0; r < this->record_count_ && free_r < 0; r++) {
          bool used = false;
          for (const auto &j : jobs)
            used |= j.key == this->record_keys_[r];
          if (!used)
            free_r = r;
        }
        if (free_r < 0) {
          ESP_LOGW(TAG, "Logo storage full; no logo for '%s'", jobs[i].name.c_str());
          continue;
        }
        this->record_keys_[free_r] = 0;
        if (!this->write_record_(free_r, jobs[i].key, px)) {
          ESP_LOGW(TAG, "Writing the logo for '%s' failed", jobs[i].name.c_str());
          continue;
        }
        this->record_keys_[free_r] = jobs[i].key;
        std::lock_guard<std::mutex> lock(this->mutex_);
        if (gen != this->generation_) {
          restart = true;  // list replaced meanwhile; start over with the new one
        } else {
          this->record_of_[i] = free_r;
          this->changed_ = true;
          found++;
        }
      }
      if (!restart)
        ESP_LOGI(TAG, "Logos: %u stored, %u downloaded, %u stations", (unsigned) stored, (unsigned) found,
                 (unsigned) jobs.size());
    }
  }
}

std::vector<std::string> StationLogos::candidates_(const Job &job) {
  if (!job.logo_url.empty())
    return {job.logo_url};

  const std::string wanted = normalize(job.name);
  std::vector<Candidate> found;
  size_t len = 0;
  if (!job.stream_url.empty() &&
      (len = http_get(std::string(API) + "byurl?url=" + url_encode(job.stream_url), MAX_JSON)) > 0)
    parse_candidates(len, wanted, found);
  bool any_match = false;
  for (const auto &c : found)
    any_match |= c.name_match;
  if (!any_match &&
      (len = http_get(std::string(API) + "search?limit=10&order=votes&reverse=true&hidebroken=true&name=" +
                          url_encode(job.name),
                      MAX_JSON)) > 0)
    parse_candidates(len, wanted, found);

  // Name matches first, then by votes.
  std::stable_sort(found.begin(), found.end(), [](const Candidate &a, const Candidate &b) {
    if (a.name_match != b.name_match)
      return a.name_match;
    return a.votes > b.votes;
  });
  std::vector<std::string> urls;
  for (const auto &c : found) {
    if (std::find(urls.begin(), urls.end(), c.url) == urls.end())
      urls.push_back(c.url);
    if (urls.size() >= MAX_CANDIDATES)
      break;
  }
  return urls;
}

/// Download logo_url through wsrv.nl as a 68x68 PNG and decode it to RGB565 into out.
bool StationLogos::fetch_(const std::string &logo_url, uint16_t *out) {
  const std::string url = "http://wsrv.nl/?url=" + url_encode(logo_url) +
                          "&w=68&h=68&fit=contain&cbg=white&bg=white&output=png";
  const size_t png_len = http_get(url, MAX_PNG);
  if (png_len == 0)
    return false;

  RAMAllocator<Decode> dec_alloc;
  Decode *dec = dec_alloc.allocate(1);
  if (dec == nullptr)
    return false;
  memset(dec->rgb, 0xFF, sizeof(dec->rgb));
  dec->w = dec->h = 0;

  RAMAllocator<pngle_t> png_alloc;
  pngle_t *pngle = png_alloc.allocate(1, PNGLE_T_SIZE);
  if (pngle == nullptr) {
    dec_alloc.deallocate(dec, 1);
    return false;
  }
  memset(pngle, 0, PNGLE_T_SIZE);
  pngle_reset(pngle);
  pngle_set_user_data(pngle, dec);
  pngle_set_init_callback(pngle, png_init);
  pngle_set_draw_callback(pngle, png_draw);
  int fed = pngle_feed(pngle, buf, png_len);
  bool ok = fed >= 0 && dec->w > 0;
  if (!ok)
    ESP_LOGD(TAG, "PNG decode failed: %s", pngle_error(pngle));
  pngle_reset(pngle);
  png_alloc.deallocate(pngle, PNGLE_T_SIZE);

  if (ok) {
    for (int k = 0; k < STORE * STORE; k++)
      out[k] = rgb565(&dec->rgb[k * 3]);
  }
  dec_alloc.deallocate(dec, 1);
  return ok;
}

}  // namespace esphome::station_logos
