#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/web_server_base/web_server_base.h"
#ifdef USE_RADIO_WEB_COVER
#include "esphome/components/image/image.h"
#endif

namespace esphome::radio_web {

/// Serves the radio's web page and a small API on the built-in web server.
///
///   GET  /              the page (index.html, gzip)
///   GET  /api/stations  station list, one "name<TAB>url<TAB>logo" line per station
///   GET  /api/status    JSON built by the status lambda, plus "cover": id of the cover (0 for none)
///                       and "page": id of the served page
///   GET  /api/cover     the image of the cover lambda as BMP (with the cover option)
///   POST /api/stations  replace the list (same text format)  -> on_save
///   POST /api/play      body: station index                   -> on_play
///   POST /api/volume    body: volume 0..100                   -> on_volume
///   POST /api/stop                                            -> on_stop
///   POST /api/pause                                           -> on_pause
///   POST /api/resume                                          -> on_resume
///   POST /api/setting   body: "key=value" lines               -> on_setting per line
///
/// Requests arrive in the web server task. The component only copies data there;
/// lambdas and triggers run in loop(), on the main task.
class RadioWeb : public Component, public AsyncWebHandler {
 public:
  /// Byte buffer from RAMAllocator (PSRAM preferred), freed with the last reference.
  struct Buffer {
    uint8_t *data;
    size_t size;
    ~Buffer() { RAMAllocator<uint8_t>().deallocate(this->data, this->size); }
  };

  explicit RadioWeb(web_server_base::WebServerBase *base) : base_(base) {}

  void set_page(const uint8_t *data, size_t size) {
    this->page_ = data;
    this->page_size_ = size;
  }
  void set_stations_lambda(std::function<std::string()> f) { this->stations_f_ = std::move(f); }
  void set_status_lambda(std::function<std::string()> f) { this->status_f_ = std::move(f); }
#ifdef USE_RADIO_WEB_COVER
  void set_cover_lambda(std::function<image::Image *()> f) { this->cover_f_ = std::move(f); }
#endif
  Trigger<std::string> *get_save_trigger() { return &this->save_trigger_; }
  Trigger<int> *get_play_trigger() { return &this->play_trigger_; }
  Trigger<int> *get_volume_trigger() { return &this->volume_trigger_; }
  Trigger<> *get_stop_trigger() { return &this->stop_trigger_; }
  Trigger<> *get_pause_trigger() { return &this->pause_trigger_; }
  Trigger<> *get_resume_trigger() { return &this->resume_trigger_; }
  Trigger<std::string, float> *get_setting_trigger() { return &this->setting_trigger_; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;

 protected:
  /// One request from the page, carried from the web server task to loop().
  enum Command { NONE, SAVE, PLAY, VOLUME, STOP, PAUSE, RESUME, SETTING };

  web_server_base::WebServerBase *base_;
  const uint8_t *page_{nullptr};
  size_t page_size_{0};
  uint32_t page_id_{0};      // hash of the page, sent in the status
  std::function<std::string()> stations_f_;
  std::function<std::string()> status_f_;
#ifdef USE_RADIO_WEB_COVER
  /// Encode the image of the cover lambda as BMP into cover_ when it changed.
  void refresh_cover_();
  std::function<image::Image *()> cover_f_;
#endif
  Trigger<std::string> save_trigger_;
  Trigger<int> play_trigger_;
  Trigger<int> volume_trigger_;
  Trigger<> stop_trigger_;
  Trigger<> pause_trigger_;
  Trigger<> resume_trigger_;
  Trigger<std::string, float> setting_trigger_;

  std::mutex lock_;          // guards the fields below
  std::string stations_;     // copies for GET requests, refreshed in loop()
  std::string status_;
  // BMP of the cover in PSRAM, null if there is none. Shared so that a request sends it without a copy
  // while loop() can already put the next one in place.
  std::shared_ptr<const Buffer> cover_;
  uint32_t cover_id_{0};     // hash of the cover's pixels, 0 if there is none; loop() only
  Command command_{NONE};    // newest command wins
  std::string command_body_;

  std::string body_;         // request body, web server task only
  bool body_too_big_{false};
  uint32_t last_refresh_{0};
};

}  // namespace esphome::radio_web
