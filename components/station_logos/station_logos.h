#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "esphome/core/component.h"

namespace esphome::station_logos {

static constexpr int STORE = 74;                // stored logo size, the playing screen
static constexpr int SLOT_COUNT = 4;            // logos on screen at once
static constexpr float DIM_BRIGHTNESS = 0.6f;   // neighbour tiles are shown dimmed

/// Two letters for the fallback tile ("Radio Bob" -> "RB", "SWR3" -> "SW").
std::string initials(const std::string &name);

/// Fill a logo box (container + image + initials label) for one station.
/// Shows the logo when one is loaded, otherwise a grey tile with initials.
/// dim: neighbour tile, so the fallback tile is drawn darker (the logo itself is dimmed already).
void show_logo(lv_obj_t *box, lv_obj_t *img, lv_obj_t *label, const lv_image_dsc_t *logo,
               const std::string &name, bool dim = false);

/// Loads station logos in a background task and stores them in the flash partition "logos".
///
/// For each station the logo URL is either given (override) or looked up at
/// radio-browser.info by stream URL, then by name. wsrv.nl resizes the image to a
/// 74x74 PNG; it is decoded once and stored as RGB565 in a fixed-size flash record,
/// so it survives restarts. Only the logos on screen are kept in RAM: get() reads a
/// record and scales it into one of SLOT_COUNT display buffers. Without the partition
/// the records are kept in PSRAM instead.
class StationLogos : public Component {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Replace the station list and start loading its logos.
  void load(const std::vector<std::string> &names, const std::vector<std::string> &stream_urls,
            const std::vector<std::string> &logo_urls);

  /// Logo of station i at size x size in display buffer slot, or nullptr while it is missing.
  /// Each slot belongs to one image widget; the buffer is reused for the next call on that slot.
  /// Main loop only.
  const lv_image_dsc_t *get(int slot, size_t i, int size, bool dim);

  /// True once after new logos became available since the last call.
  bool take_changed() { return this->changed_.exchange(false); }

 protected:
  struct Job {
    std::string name;
    std::string stream_url;
    std::string logo_url;
    uint32_t key;
  };
  struct Slot {
    uint16_t *px{nullptr};
    lv_image_dsc_t dsc{};
    uint32_t key{0};
    int size{0};
    bool dim{false};
  };

  static void task_entry_(void *arg);
  void run_();
  std::vector<std::string> candidates_(const Job &job);
  bool fetch_(const std::string &logo_url, uint16_t *out);
  bool read_record_(int r, uint16_t *out);
  bool write_record_(int r, uint32_t key, const uint16_t *px);

  const esp_partition_t *partition_{nullptr};
  int record_count_{0};
  std::vector<uint32_t> record_keys_;  // key per record, 0 = empty; background task only
  std::vector<uint16_t *> mem_records_;  // PSRAM records when the partition is missing

  std::mutex mutex_;
  std::vector<Job> jobs_;
  std::vector<int> record_of_;  // record per station, -1 while missing
  uint32_t generation_{0};
  TaskHandle_t task_{nullptr};
  std::atomic<bool> changed_{false};

  // Main loop buffers for get().
  Slot slots_[SLOT_COUNT];
  uint16_t *read_px_{nullptr};
  uint8_t *rgb_{nullptr};
  uint8_t *scaled_{nullptr};
};

}  // namespace esphome::station_logos
