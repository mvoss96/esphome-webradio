#include "radio_web.h"

#include <cstdlib>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::radio_web {

static const char *const TAG = "radio_web";
static constexpr size_t MAX_BODY = 8192;

void RadioWeb::setup() {
  this->base_->init();
  this->base_->add_handler(this);
}

void RadioWeb::loop() {
  const uint32_t now = millis();
  if (now - this->last_refresh_ >= 1000) {
    this->last_refresh_ = now;
    std::string stations = this->stations_f_();
    std::string status = this->status_f_();
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
    case NONE:
      return;
  }
  this->last_refresh_ = 0;  // the next GET sees the result
}

bool RadioWeb::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(buf);
  if (request->method() == HTTP_GET)
    return url == "/" || url == "/api/stations" || url == "/api/status";
  if (request->method() == HTTP_POST)
    return url == "/api/stations" || url == "/api/play" || url == "/api/volume" || url == "/api/stop";
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
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    this->command_ = command;
    this->command_body_ = std::move(this->body_);
  }
  this->body_.clear();
  request->send(204);
}

}  // namespace esphome::radio_web
