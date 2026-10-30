#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome::radio_web {

/// Serves the radio's web page and a small API on the built-in web server.
///
///   GET  /              the page (index.html, gzip)
///   GET  /api/stations  station list, one "name<TAB>url<TAB>logo" line per station
///   GET  /api/status    JSON built by the status lambda
///   POST /api/stations  replace the list (same text format)  -> on_save
///   POST /api/play      body: station index                   -> on_play
///   POST /api/volume    body: volume 0..100                   -> on_volume
///   POST /api/stop                                            -> on_stop
///
/// Requests arrive in the web server task. The component only copies data there;
/// lambdas and triggers run in loop(), on the main task.
class RadioWeb : public Component, public AsyncWebHandler {
 public:
  explicit RadioWeb(web_server_base::WebServerBase *base) : base_(base) {}

  void set_page(const uint8_t *data, size_t size) {
    this->page_ = data;
    this->page_size_ = size;
  }
  void set_stations_lambda(std::function<std::string()> f) { this->stations_f_ = std::move(f); }
  void set_status_lambda(std::function<std::string()> f) { this->status_f_ = std::move(f); }
  Trigger<std::string> *get_save_trigger() { return &this->save_trigger_; }
  Trigger<int> *get_play_trigger() { return &this->play_trigger_; }
  Trigger<int> *get_volume_trigger() { return &this->volume_trigger_; }
  Trigger<> *get_stop_trigger() { return &this->stop_trigger_; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;

 protected:
  /// One request from the page, carried from the web server task to loop().
  enum Command { NONE, SAVE, PLAY, VOLUME, STOP };

  web_server_base::WebServerBase *base_;
  const uint8_t *page_{nullptr};
  size_t page_size_{0};
  std::function<std::string()> stations_f_;
  std::function<std::string()> status_f_;
  Trigger<std::string> save_trigger_;
  Trigger<int> play_trigger_;
  Trigger<int> volume_trigger_;
  Trigger<> stop_trigger_;

  std::mutex lock_;          // guards the fields below
  std::string stations_;     // copies for GET requests, refreshed in loop()
  std::string status_;
  Command command_{NONE};    // newest command wins
  std::string command_body_;

  std::string body_;         // request body, web server task only
  bool body_too_big_{false};
  uint32_t last_refresh_{0};
};

}  // namespace esphome::radio_web
