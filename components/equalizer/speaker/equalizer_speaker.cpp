#include "equalizer_speaker.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "esp_dsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome {
namespace equalizer_speaker {

static const char *const TAG = "equalizer_speaker";
static const uint32_t START_TIMEOUT_MS = 5000;

static const char *biquad_type_to_str(BiquadType t) {
  switch (t) {
    case BIQUAD_LOW_SHELF:
      return "low_shelf";
    case BIQUAD_PEAKING:
      return "peaking";
    case BIQUAD_HIGH_SHELF:
      return "high_shelf";
    case BIQUAD_LOW_PASS:
      return "low_pass";
    case BIQUAD_HIGH_PASS:
      return "high_pass";
  }
  return "?";
}

void EqualizerSpeaker::setup() {
  // Forward latency callbacks transparently — the EQ adds zero frames of delay.
  this->output_speaker_->add_audio_output_callback(
      [this](uint32_t new_frames, int64_t write_timestamp) { this->audio_output_callback_(new_frames, write_timestamp); });

  // Nothing else to do until something calls start().
  this->disable_loop();
}

void EqualizerSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG, "Equalizer Speaker:");
  ESP_LOGCONFIG(TAG, "  Enabled: %s", YESNO(this->enabled_));
  ESP_LOGCONFIG(TAG, "  Bands: %u", static_cast<unsigned>(this->bands_.size()));
  for (size_t i = 0; i < this->bands_.size(); i++) {
    const Band &b = this->bands_[i];
    ESP_LOGCONFIG(TAG, "    %u: %s @ %.1f Hz, gain %.2f dB, Q %.3f", static_cast<unsigned>(i),
                  biquad_type_to_str(b.type), b.frequency_hz, b.gain_db, b.q);
  }
}

void EqualizerSpeaker::add_band(BiquadType type, float frequency_hz, float gain_db, float q) {
  Band band;
  band.type = type;
  band.frequency_hz = frequency_hz;
  band.gain_db = gain_db;
  band.q = q;
  this->recompute_coefficients_(band);
  this->bands_.push_back(band);
}

void EqualizerSpeaker::set_band_gain(size_t index, float gain_db) {
  if (index >= this->bands_.size())
    return;
  this->bands_[index].gain_db = gain_db;
  this->recompute_coefficients_(this->bands_[index]);
}

void EqualizerSpeaker::set_band_frequency(size_t index, float frequency_hz) {
  if (index >= this->bands_.size())
    return;
  this->bands_[index].frequency_hz = frequency_hz;
  this->recompute_coefficients_(this->bands_[index]);
}

void EqualizerSpeaker::set_band_q(size_t index, float q) {
  if (index >= this->bands_.size())
    return;
  this->bands_[index].q = q;
  this->recompute_coefficients_(this->bands_[index]);
}

void EqualizerSpeaker::set_enabled(bool enabled) {
  if (enabled == this->enabled_) {
    return;
  }
  // On disabled→enabled, the band delay lines still hold history from the
  // last active session — picking up with stale state can produce a brief
  // transient. Reset them so the cascade restarts clean.
  if (enabled) {
    for (auto &band : this->bands_) {
      std::memset(band.state, 0, sizeof(band.state));
    }
  }
  this->enabled_ = enabled;
}

// Mostly delegates to esp-dsp's biquad coefficient generators
// (dsps_biquad_gen_*_f32). Output layout: band.coeffs = [b0, b1, b2, a1, a2],
// a0-normalised — same convention esp-dsp uses internally.
//
// Exception: esp-dsp 1.7.1's `peakingEQ_f32` has no gain parameter (it
// produces a unity-gain peak), which is useless for a parametric EQ band.
// For BIQUAD_PEAKING we therefore compute the standard RBJ cookbook
// coefficients ourselves.
void EqualizerSpeaker::recompute_coefficients_(Band &band) const {
  const float fs = static_cast<float>(this->sample_rate_);
  // esp-dsp expects normalised frequency in (0, 0.5).
  const float f = std::max(10.0f, std::min(band.frequency_hz, 0.49f * fs)) / fs;
  const float q = band.q;
  const float gain = band.gain_db;

  esp_err_t err = ESP_OK;
  switch (band.type) {
    case BIQUAD_LOW_SHELF:
      err = dsps_biquad_gen_lowShelf_f32(band.coeffs, f, gain, q);
      break;
    case BIQUAD_HIGH_SHELF:
      err = dsps_biquad_gen_highShelf_f32(band.coeffs, f, gain, q);
      break;
    case BIQUAD_LOW_PASS:
      err = dsps_biquad_gen_lpf_f32(band.coeffs, f, q);
      break;
    case BIQUAD_HIGH_PASS:
      err = dsps_biquad_gen_hpf_f32(band.coeffs, f, q);
      break;
    case BIQUAD_PEAKING: {
      // RBJ Audio EQ Cookbook, peaking EQ section. esp-dsp's peakingEQ does
      // not accept a gain parameter, so we compute it manually.
      const float w0 = 2.0f * static_cast<float>(M_PI) * f;
      const float cos_w0 = std::cos(w0);
      const float sin_w0 = std::sin(w0);
      const float A = std::pow(10.0f, gain / 40.0f);
      const float alpha = sin_w0 / (2.0f * q);
      const float a0 = 1.0f + alpha / A;
      band.coeffs[0] = (1.0f + alpha * A) / a0;          // b0
      band.coeffs[1] = (-2.0f * cos_w0) / a0;            // b1
      band.coeffs[2] = (1.0f - alpha * A) / a0;          // b2
      band.coeffs[3] = (-2.0f * cos_w0) / a0;            // a1
      band.coeffs[4] = (1.0f - alpha / A) / a0;          // a2
      break;
    }
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "biquad coefficient generation failed (type=%u, err=%d)",
             static_cast<unsigned>(band.type), static_cast<int>(err));
  }
}

// Run the biquad cascade. Layout:
//   - Float scratch holds samples interleaved (L/R/L/R for stereo, L/L/L for mono).
//   - For stereo we use dsps_biquad_sf32, which expects exactly that layout and
//     uses a 4-float delay line per band (z1_ch0, z2_ch0, z1_ch1, z2_ch1) — that
//     matches our `band.state[2][2]` memory layout 1:1.
//   - For mono we use dsps_biquad_f32 with the first 2 floats of band.state.
// On ESP32-S3 the linker remaps dsps_biquad_f32/_sf32 to the SIMD variants via
// the macros in esp-dsp's dsps_biquad.h.
void EqualizerSpeaker::process_frames_(const int16_t *input, int16_t *output, size_t frame_count,
                                       uint8_t channels) {
  const size_t total_samples = frame_count * channels;
  if (this->float_scratch_.size() < total_samples) {
    this->float_scratch_.resize(total_samples);
  }
  float *buf = this->float_scratch_.data();

  // int16 → float, normalised to ±1.0.
  static constexpr float kInt16ToFloat = 1.0f / 32768.0f;
  for (size_t i = 0; i < total_samples; i++) {
    buf[i] = static_cast<float>(input[i]) * kInt16ToFloat;
  }

  for (auto &band : this->bands_) {
    if (channels == 2) {
      dsps_biquad_sf32(buf, buf, frame_count, band.coeffs, &band.state[0][0]);
    } else {
      dsps_biquad_f32(buf, buf, frame_count, band.coeffs, band.state[0]);
    }
  }

  // float → int16 with rounding and saturation.
  for (size_t i = 0; i < total_samples; i++) {
    const float v = buf[i] * 32768.0f;
    int32_t s = static_cast<int32_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
    if (s > INT16_MAX) {
      s = INT16_MAX;
    } else if (s < INT16_MIN) {
      s = INT16_MIN;
    }
    output[i] = static_cast<int16_t>(s);
  }
}

size_t EqualizerSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_stopped()) {
    this->start();
  }
  if (this->output_speaker_ == nullptr || !this->output_speaker_->is_running()) {
    // The output speaker starts in the main loop. Wait here, otherwise a caller that retries in a
    // loop (the audio decoder task, priority 2) spins, starves the main loop and the output never
    // starts; the task watchdog then resets the device.
    vTaskDelay(ticks_to_wait > 0 ? ticks_to_wait : 1);
    return 0;
  }

  // Fast path: bypass switch off OR no bands configured → no DSP work.
  if (!this->enabled_ || this->bands_.empty()) {
    return this->output_speaker_->play(data, length, ticks_to_wait);
  }

  const size_t bytes_per_frame = static_cast<size_t>(this->channels_) * sizeof(int16_t);
  if (bytes_per_frame == 0) {
    return this->output_speaker_->play(data, length, ticks_to_wait);
  }

  const size_t frame_count = length / bytes_per_frame;
  if (frame_count == 0) {
    // Trailing partial frame — forward as-is, downstream knows what to do.
    return this->output_speaker_->play(data, length, ticks_to_wait);
  }

  const size_t total_samples = frame_count * this->channels_;
  if (this->int16_scratch_.size() < total_samples) {
    this->int16_scratch_.resize(total_samples);
  }

  this->process_frames_(reinterpret_cast<const int16_t *>(data), this->int16_scratch_.data(), frame_count,
                        this->channels_);

  return this->output_speaker_->play(reinterpret_cast<const uint8_t *>(this->int16_scratch_.data()),
                                     total_samples * sizeof(int16_t), ticks_to_wait);
}

void EqualizerSpeaker::start() {
  if (this->state_ == speaker::STATE_STARTING || this->state_ == speaker::STATE_RUNNING)
    return;

  // Pick up the stream info that the upstream caller configured on us.
  this->sample_rate_ = this->audio_stream_info_.get_sample_rate();
  this->channels_ = this->audio_stream_info_.get_channels();
  if (this->channels_ == 0)
    this->channels_ = 1;
  if (this->channels_ > 2)
    this->channels_ = 2;

  // Sample rate may have changed since add_band() was called → refresh.
  for (auto &band : this->bands_) {
    std::memset(band.state, 0, sizeof(band.state));
    this->recompute_coefficients_(band);
  }

  this->output_speaker_->set_audio_stream_info(this->audio_stream_info_);
  this->output_speaker_->start();

  this->state_ = speaker::STATE_STARTING;
  this->state_start_ms_ = App.get_loop_component_start_time();
  this->enable_loop();
}

void EqualizerSpeaker::stop() {
  if (this->state_ == speaker::STATE_STOPPED || this->state_ == speaker::STATE_STOPPING)
    return;
  this->state_ = speaker::STATE_STOPPING;
  this->state_start_ms_ = App.get_loop_component_start_time();
  if (this->output_speaker_ != nullptr) {
    this->output_speaker_->stop();
  }
  this->enable_loop();
}

void EqualizerSpeaker::finish() {
  if (this->output_speaker_ != nullptr) {
    this->output_speaker_->finish();
  }
}

bool EqualizerSpeaker::has_buffered_data() const {
  return (this->output_speaker_ != nullptr) && this->output_speaker_->has_buffered_data();
}

void EqualizerSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->output_speaker_->set_mute_state(mute_state);
}

void EqualizerSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  this->output_speaker_->set_volume(volume);
}

void EqualizerSpeaker::loop() {
  switch (this->state_) {
    case speaker::STATE_STARTING:
      if (this->output_speaker_->is_running()) {
        this->state_ = speaker::STATE_RUNNING;
      } else if (App.get_loop_component_start_time() - this->state_start_ms_ > START_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Output speaker did not start within %u ms", static_cast<unsigned>(START_TIMEOUT_MS));
        this->state_ = speaker::STATE_STOPPING;
        this->state_start_ms_ = App.get_loop_component_start_time();
        this->output_speaker_->stop();
      }
      break;
    case speaker::STATE_RUNNING:
      if (this->output_speaker_->is_stopped()) {
        this->state_ = speaker::STATE_STOPPED;
        this->disable_loop();
      }
      break;
    case speaker::STATE_STOPPING:
      if (this->output_speaker_->is_stopped()) {
        this->state_ = speaker::STATE_STOPPED;
        this->disable_loop();
      }
      break;
    case speaker::STATE_STOPPED:
      this->disable_loop();
      break;
  }
}

}  // namespace equalizer_speaker
}  // namespace esphome

#endif  // USE_ESP32
