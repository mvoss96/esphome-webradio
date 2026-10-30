#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "esphome/core/component.h"

namespace esphome::station_logos {

static constexpr int BIG = 56;    // selected tile and playing screen
static constexpr int SMALL = 32;  // neighbour tiles in the carousel
static constexpr int BIG_RADIUS = 12;
static constexpr int SMALL_RADIUS = 7;
static constexpr float SMALL_BRIGHTNESS = 0.6f;  // neighbour tiles are shown dimmed

/// Two letters for the fallback tile ("Radio Bob" -> "RB", "SWR3" -> "SW").
std::string initials(const std::string &name);

/// Fill a logo box (container + image + initials label) for one station.
/// Shows the logo when one is loaded, otherwise a grey tile with initials.
/// dim: neighbour tile, so the fallback tile is drawn darker (the small logo is dimmed already).
void show_logo(lv_obj_t *box, lv_obj_t *img, lv_obj_t *label, const lv_image_dsc_t *logo,
               const std::string &name, bool dim = false);

/// Loads station logos in a background task and keeps them in PSRAM.
///
/// For each station the logo URL is either given (override) or looked up at
/// radio-browser.info by stream URL, then by name. The image is resized by
/// wsrv.nl to 56x56 PNG, decoded with pngle and stored as RGB565 together with
/// a 32x32 copy. LVGL may only be touched from the main loop, so the task only
/// fills buffers and the main loop polls take_changed().
class StationLogos : public Component {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Replace the station list and start loading its logos.
  void load(const std::vector<std::string> &names, const std::vector<std::string> &stream_urls,
            const std::vector<std::string> &logo_urls);

  /// Logo of station i, or nullptr while it is missing.
  const lv_image_dsc_t *big(size_t i);
  const lv_image_dsc_t *small(size_t i);

  /// True once after new logos became available since the last call.
  bool take_changed() { return this->changed_.exchange(false); }

 protected:
  struct Job {
    std::string name;
    std::string stream_url;
    std::string logo_url;
  };
  struct Logo {
    uint16_t *big_px{nullptr};
    uint16_t *small_px{nullptr};
    lv_image_dsc_t big_dsc{};
    lv_image_dsc_t small_dsc{};
    ~Logo();
  };

  static void task_entry_(void *arg);
  void run_();
  std::vector<std::string> candidates_(const Job &job);
  std::unique_ptr<Logo> fetch_(const std::string &logo_url);

  std::mutex mutex_;
  std::vector<Job> jobs_;
  std::vector<std::unique_ptr<Logo>> logos_;
  uint32_t generation_{0};
  TaskHandle_t task_{nullptr};
  std::atomic<bool> changed_{false};
};

}  // namespace esphome::station_logos
