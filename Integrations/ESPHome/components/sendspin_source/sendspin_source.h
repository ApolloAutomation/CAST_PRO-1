#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP_IDF

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/sendspin/sendspin_hub.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <sendspin/client.h>
#include <sendspin/config.h>
#include <sendspin/source_role.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef SENDSPIN_ENABLE_SOURCE
#error "sendspin_source: CONFIG_SENDSPIN_ENABLE_SOURCE is off; the FINAL-priority sdkconfig override did not run"
#endif

namespace esphome::sendspin_source {

/// @brief Adds sendspin-cpp's SourceRole (source@v1) to the sendspin hub and feeds it from a MicrophoneSource.
///
/// Threads:
///  - setup()/loop(), the SourceRoleListener callbacks and set_signal() run on the main loop.
///  - on_audio_() runs on the microphone's task and is the single producer calling SourceRole::write_audio().
///  Shared state between the two is limited to the atomics below.
class SendspinSource : public sendspin_::SendspinChild, public sendspin::SourceRoleListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_microphone_source(microphone::MicrophoneSource *mic_source) { this->mic_source_ = mic_source; }
  void set_codec(sendspin::SendspinCodecFormat codec) { this->codec_ = codec; }
  void set_chunk_duration_ms(uint32_t chunk_duration_ms) { this->chunk_duration_ms_ = chunk_duration_ms; }
  void set_capture_buffer_ms(uint32_t capture_buffer_ms) { this->capture_buffer_ms_ = capture_buffer_ms; }
  void set_line_sense(bool enabled, float threshold_db, float hysteresis_db, uint32_t attack_ms, uint32_t release_ms) {
    this->line_sense_ = enabled;
    this->threshold_db_ = threshold_db;
    this->hysteresis_db_ = hysteresis_db;
    this->attack_ms_ = attack_ms;
    this->release_ms_ = release_ms;
  }

  void add_on_streaming_started_callback(std::function<void()> &&callback) {
    this->streaming_started_callbacks_.add(std::move(callback));
  }
  void add_on_streaming_stopped_callback(std::function<void()> &&callback) {
    this->streaming_stopped_callbacks_.add(std::move(callback));
  }
  void add_on_signal_present_callback(std::function<void()> &&callback) {
    this->signal_present_callbacks_.add(std::move(callback));
  }
  void add_on_signal_absent_callback(std::function<void()> &&callback) {
    this->signal_absent_callbacks_.add(std::move(callback));
  }

  /// Whether the input stream to the server is open, as last reported on the main loop.
  bool is_streaming() const { return this->streaming_.load(std::memory_order_relaxed); }
  /// Line-sense state; always false when line_sense is off.
  bool is_signal_present() const { return this->signal_present_; }

  // sendspin::SourceRoleListener (main loop)
  void on_streaming_started() override;
  void on_streaming_stopped() override;

 protected:
  // Microphone task
  void on_audio_(const std::vector<uint8_t> &data);
  void accumulate_level_(const std::vector<uint8_t> &data);
  int64_t frames_to_us_(uint64_t frames) const {
    return static_cast<int64_t>(frames * 1000000ULL / this->sample_rate_);
  }

  // Main loop
  void update_line_sense_();
  void set_signal_(bool present);

  // Reads per anchor window (~100 ms at the i2s microphone's 16 ms reads; longer than the 80 ms beat between
  // 16 ms reads and 10 ms DMA descriptors, so every window holds one read that returned without DMA delay).
  static constexpr uint8_t ANCHOR_WINDOW_READS = 8;
  // A candidate this far above the anchor is either frame loss (one 10 ms DMA descriptor plus jitter) or a late
  // read; only a whole window of such candidates tells the two apart, so the anchor never moves on one of them.
  static constexpr int64_t REANCHOR_US = 12000;
  // Window re-evaluations below this are scheduling noise, not a drift or loss worth following.
  static constexpr int64_t ANCHOR_INCREASE_DEADBAND_US = 1000;

  microphone::MicrophoneSource *mic_source_{nullptr};
  sendspin::SourceRole *role_{nullptr};

  sendspin::SendspinCodecFormat codec_{sendspin::SendspinCodecFormat::PCM};
  uint32_t chunk_duration_ms_{sendspin::SourceRoleConfig::DEFAULT_CHUNK_MS};
  uint32_t capture_buffer_ms_{sendspin::SourceRoleConfig::DEFAULT_CAPTURE_BUFFER_MS};
  uint32_t sample_rate_{sendspin::SourceRoleConfig::DEFAULT_SAMPLE_RATE};
  uint8_t bits_per_sample_{16};
  uint8_t channels_{2};
  size_t bytes_per_frame_{4};

  // Capture timestamps: owned by the microphone task
  uint64_t frames_total_{0};
  int64_t anchor_us_{0};
  bool anchor_valid_{false};
  int64_t window_min_us_{0};
  uint8_t window_reads_{0};
  bool loss_pending_{false};  // a read sat REANCHOR_US above the anchor; the current window decides

  // Diagnostics, written on the microphone task and read on the main loop
  std::atomic<uint32_t> dropped_writes_{0};
  std::atomic<uint32_t> empty_reads_{0};
  std::atomic<uint32_t> reanchors_{0};
  std::atomic<uint32_t> anchor_increases_{0};

  // Line sense
  bool line_sense_{false};
  float threshold_db_{-60.0f};
  float hysteresis_db_{6.0f};
  uint32_t attack_ms_{100};
  uint32_t release_ms_{2000};
  std::atomic<uint32_t> block_mean_square_{0};  // mean of the squared 16-bit samples of the last block
  std::atomic<uint32_t> block_sequence_{0};
  uint32_t last_block_sequence_{0};
  bool signal_present_{false};
  bool candidate_timing_{false};
  uint32_t candidate_since_ms_{0};

  std::atomic<bool> streaming_{false};

  CallbackManager<void()> streaming_started_callbacks_{};
  CallbackManager<void()> streaming_stopped_callbacks_{};
  CallbackManager<void()> signal_present_callbacks_{};
  CallbackManager<void()> signal_absent_callbacks_{};
};

}  // namespace esphome::sendspin_source

#endif  // USE_ESP_IDF
