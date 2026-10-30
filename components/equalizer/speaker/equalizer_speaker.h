#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

#include <cstdint>
#include <vector>

namespace esphome {
namespace equalizer_speaker {

enum BiquadType : uint8_t {
  BIQUAD_LOW_SHELF = 0,
  BIQUAD_PEAKING = 1,
  BIQUAD_HIGH_SHELF = 2,
  BIQUAD_LOW_PASS = 3,
  BIQUAD_HIGH_PASS = 4,
};

struct Band {
  BiquadType type{BIQUAD_PEAKING};
  float frequency_hz{1000.0f};
  float gain_db{0.0f};
  float q{0.707f};

  // esp-dsp layout: [b0, b1, b2, a1, a2], a0-normalised.
  // Filled by dsps_biquad_gen_*_f32(); consumed by dsps_biquad_f32().
  float coeffs[5]{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  // Per-channel delay line. dsps_biquad_f32 expects a contiguous float[2]
  // (w[0]=z1, w[1]=z2). Stereo => independent state per channel.
  float state[2][2]{{0.0f, 0.0f}, {0.0f, 0.0f}};
};

class EqualizerSpeaker : public Component, public speaker::Speaker {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  // speaker::Speaker
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  void finish() override;

  bool has_buffered_data() const override;

  void set_mute_state(bool mute_state) override;
  bool get_mute_state() override { return this->output_speaker_->get_mute_state(); }

  void set_volume(float volume) override;
  float get_volume() override { return this->output_speaker_->get_volume(); }

  void set_pause_state(bool pause_state) override { this->output_speaker_->set_pause_state(pause_state); }
  bool get_pause_state() const override { return this->output_speaker_->get_pause_state(); }

  // Configuration
  void set_output_speaker(speaker::Speaker *output_speaker) { this->output_speaker_ = output_speaker; }
  void init_bands(size_t count) { this->bands_.reserve(count); }
  void add_band(BiquadType type, float frequency_hz, float gain_db, float q);

  // Runtime tuning (for HA `number` entities). Coefficients are recomputed
  // immediately; the next play() call picks them up.
  void set_band_gain(size_t index, float gain_db);
  void set_band_frequency(size_t index, float frequency_hz);
  void set_band_q(size_t index, float q);

  // Read-back accessors used by EqualizerNumber to publish current state on
  // boot and after parameter changes.
  size_t band_count() const { return this->bands_.size(); }
  float get_band_gain(size_t index) const {
    return index < this->bands_.size() ? this->bands_[index].gain_db : 0.0f;
  }
  float get_band_frequency(size_t index) const {
    return index < this->bands_.size() ? this->bands_[index].frequency_hz : 0.0f;
  }
  float get_band_q(size_t index) const {
    return index < this->bands_.size() ? this->bands_[index].q : 0.0f;
  }

  // Global bypass. When disabled, play() forwards bytes verbatim to the
  // output speaker, exactly like the bands_.empty() fast path. The transition
  // off→on resets all band delay lines to avoid resuming with stale history.
  void set_enabled(bool enabled);
  bool get_enabled() const { return this->enabled_; }

 protected:
  // Recompute the RBJ cookbook coefficients for a band given its current
  // frequency / gain / q and the current sample rate.
  void recompute_coefficients_(Band &band) const;

  // Run the biquad cascade on `frame_count` interleaved int16 frames at
  // `input`, writing the result to `output`. Buffers must not alias. Uses
  // `float_scratch_` internally for the int16↔float conversion and the
  // esp-dsp cascade.
  void process_frames_(const int16_t *input, int16_t *output, size_t frame_count, uint8_t channels);

  speaker::Speaker *output_speaker_{nullptr};
  std::vector<Band> bands_;

  // Cached from set_audio_stream_info() at start() time.
  uint32_t sample_rate_{48000};
  uint8_t channels_{2};

  bool enabled_{true};

  // Most frames one play() call processes. play() reports fewer bytes when a caller
  // hands over more, and the caller sends the rest with the next call.
  static constexpr size_t MAX_FRAMES = 1024;

  // Scratch buffers, allocated once in setup() for MAX_FRAMES stereo frames.
  // play() runs in the audio tasks; allocating there failed for large chunks
  // (Sendspin FLAC) and aborted the firmware. Float buffer holds the samples
  // for the esp-dsp cascade; int16 buffer holds the output for the downstream speaker.
  std::vector<float> float_scratch_;
  std::vector<int16_t> int16_scratch_;

  uint32_t state_start_ms_{0};
};

}  // namespace equalizer_speaker
}  // namespace esphome

#endif  // USE_ESP32
