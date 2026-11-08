#include "radio_web.h"

#include <cstdlib>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::radio_web {

static const char *const TAG = "radio_web";
static constexpr size_t MAX_BODY = 8192;

void RadioWeb::setup() {
  // FNV-1a over the served page: a firmware with another page has another id.
  this->page_id_ = 2166136261u;
  for (size_t i = 0; i < this->page_size_; i++)
    this->page_id_ = (this->page_id_ ^ this->page_[i]) * 16777619u;
  this->base_->init();
  this->base_->add_handler(this);
}

void RadioWeb::loop() {
  const uint32_t now = millis();
  if (now - this->last_refresh_ >= 1000) {
    this->last_refresh_ = now;
#ifdef USE_RADIO_WEB_COVER
    this->refresh_cover_();
#endif
    std::string stations = this->stations_f_();
    std::string status = this->status_f_();
    // The page reloads the cover when "cover" changes, and itself when "page" differs from its own.
    if (!status.empty() && status.back() == '}')
      status.insert(status.size() - 1, ",\"cover\":" + std::to_string(this->cover_id_) +
                                           ",\"page\":" + std::to_string(this->page_id_));
    std::lock_guard<std::mutex> guard(this->lock_);
    this->stations_ = std::move(stations);
    this->status_ = std::move(status);
  }

  Command command;
  std::string body;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    command = this->command_;
    body = std::move(this->command_body_);
    this->command_ = NONE;
  }
  switch (command) {
    case SAVE:
      ESP_LOGI(TAG, "Saving station list from the web page (%u bytes)", (unsigned) body.size());
      this->save_trigger_.trigger(body);
      break;
    case PLAY:
      this->play_trigger_.trigger(std::atoi(body.c_str()));
      break;
    case VOLUME:
      this->volume_trigger_.trigger(std::atoi(body.c_str()));
      break;
    case STOP:
      this->stop_trigger_.trigger();
      break;
    case PAUSE:
      this->pause_trigger_.trigger();
      break;
    case RESUME:
      this->resume_trigger_.trigger();
      break;
    case SETTING: {
      // One "key=value" per line, so several settings arrive in one request.
      size_t pos = 0;
      while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos)
          end = body.size();
        const std::string line = body.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = line.find('=');
        if (eq != std::string::npos)
          this->setting_trigger_.trigger(line.substr(0, eq), std::strtof(line.c_str() + eq + 1, nullptr));
      }
      break;
    }
    case NONE:
      return;
  }
  this->last_refresh_ = 0;  // the next GET sees the result
}

#ifdef USE_RADIO_WEB_COVER
static uint8_t *put_le(uint8_t *out, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; i++)
    *out++ = (value >> (8 * i)) & 0xFF;
  return out;
}

void RadioWeb::refresh_cover_() {
  image::Image *img = this->cover_f_ ? this->cover_f_() : nullptr;
  const int w = img != nullptr ? img->get_width() : 0, h = img != nullptr ? img->get_height() : 0;
  uint32_t hash = 0;
  if (w > 0 && h > 0) {
    hash = 2166136261u;  // FNV-1a over the pixels; no allocation, so it is cheap to run every second
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const Color c = img->get_pixel(x, y);
        for (const uint8_t b : {c.r, c.g, c.b})
          hash = (hash ^ b) * 16777619u;
      }
    }
    if (hash == 0)
      hash = 1;
  }
  if (hash == this->cover_id_)
    return;

  std::shared_ptr<Buffer> bmp;
  if (hash != 0) {
    // 24-bit BMP, rows top to bottom (negative height), each row padded to 4 bytes.
    const uint32_t row = (w * 3 + 3) & ~3u;
    const uint32_t pixels = row * h;
    const size_t size = 54 + pixels;
    uint8_t *data = RAMAllocator<uint8_t>().allocate(size);
    if (data == nullptr) {
      // cover_id_ stays unchanged, so the next refresh tries again.
      ESP_LOGW(TAG, "No memory for the cover (%u bytes), trying again", (unsigned) size);
      return;
    }
    bmp = std::shared_ptr<Buffer>(new Buffer{data, size});
    uint8_t *out = data;
    *out++ = 'B';
    *out++ = 'M';
    out = put_le(out, size, 4);
    out = put_le(out, 0, 4);
    out = put_le(out, 54, 4);
    out = put_le(out, 40, 4);
    out = put_le(out, w, 4);
    out = put_le(out, static_cast<uint32_t>(-h), 4);
    out = put_le(out, 1, 2);
    out = put_le(out, 24, 2);
    out = put_le(out, 0, 4);
    out = put_le(out, pixels, 4);
    out = put_le(out, 2835, 4);
    out = put_le(out, 2835, 4);
    out = put_le(out, 0, 4);
    out = put_le(out, 0, 4);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const Color c = img->get_pixel(x, y);
        *out++ = c.b;
        *out++ = c.g;
        *out++ = c.r;
      }
      for (uint32_t pad = w * 3; pad < row; pad++)
        *out++ = 0;
    }
  }
  this->cover_id_ = hash;
  std::lock_guard<std::mutex> guard(this->lock_);
  this->cover_ = std::move(bmp);
}
#endif

bool RadioWeb::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(buf);
  if (request->method() == HTTP_GET)
    return url == "/" || url == "/api/stations" || url == "/api/status" || url == "/api/cover";
  if (request->method() == HTTP_POST)
    return url == "/api/stations" || url == "/api/play" || url == "/api/volume" || url == "/api/stop" ||
           url == "/api/pause" || url == "/api/resume" || url == "/api/setting";
  return false;
}

void RadioWeb::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    this->body_.clear();
    this->body_too_big_ = total > MAX_BODY;
  }
  if (!this->body_too_big_)
    this->body_.append(reinterpret_cast<const char *>(data), len);
}

void RadioWeb::handleRequest(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(buf);

  if (request->method() == HTTP_GET) {
    if (url == "/") {
      auto *response = request->beginResponse(200, "text/html", this->page_, this->page_size_);
      response->addHeader("Content-Encoding", "gzip");
      // A new firmware brings a new page; the browser must not keep the old one.
      response->addHeader("Cache-Control", "no-cache");
      request->send(response);
      return;
    }
    if (url == "/api/cover") {
      // A second reference, no copy: the buffer stays valid while it is sent, even if loop() replaces it.
      std::shared_ptr<const Buffer> bmp;
      {
        std::lock_guard<std::mutex> guard(this->lock_);
        bmp = this->cover_;
      }
      if (!bmp) {
        request->send(404, "text/plain", "Kein Cover");
        return;
      }
      auto *response = request->beginResponse(200, "image/bmp", bmp->data, bmp->size);
      // The page asks with ?v=<cover id>, so one URL always means the same picture.
      response->addHeader("Cache-Control", "max-age=86400");
      request->send(response);
      return;
    }
    std::string text;
    {
      std::lock_guard<std::mutex> guard(this->lock_);
      text = url == "/api/status" ? this->status_ : this->stations_;
    }
    auto *response = request->beginResponse(
        200, url == "/api/status" ? "application/json" : "text/plain; charset=utf-8", text);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    return;
  }

  // POST: the body has arrived in body_ via handleBody(), if there was one.
  if (this->body_too_big_) {
    this->body_too_big_ = false;
    request->send(400, "text/plain", "Liste zu groß");
    return;
  }
  Command command = STOP;
  if (url == "/api/stations")
    command = SAVE;
  else if (url == "/api/play")
    command = PLAY;
  else if (url == "/api/volume")
    command = VOLUME;
  else if (url == "/api/setting")
    command = SETTING;
  else if (url == "/api/pause")
    command = PAUSE;
  else if (url == "/api/resume")
    command = RESUME;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    this->command_ = command;
    this->command_body_ = std::move(this->body_);
  }
  this->body_.clear();
  request->send(204);
}

}  // namespace esphome::radio_web
