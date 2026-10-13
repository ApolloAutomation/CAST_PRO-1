#include "sendspin_source.h"

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_timer.h>

#include <cinttypes>
#include <cmath>
#include <memory>

namespace esphome::sendspin_source {

static const char *const TAG = "sendspin_source";

// The hub keeps its sendspin::SendspinClient protected with no accessor. An explicit template instantiation is
// exempt from access checks, so instantiating HubClientAccess<&SendspinHub::client_> defines the (inline) friend
// hub_client_member() for this translation unit only. A rename or retype upstream fails the build instead of
// misbehaving.
using HubClientMember = std::unique_ptr<sendspin::SendspinClient> sendspin_::SendspinHub::*;
inline HubClientMember hub_client_member();

template<HubClientMember M> struct HubClientAccess {
  friend HubClientMember hub_client_member() { return M; }
};
template struct HubClientAccess<&sendspin_::SendspinHub::client_>;

static sendspin::SendspinClient *hub_client(sendspin_::SendspinHub *hub) {
  return (hub->*hub_client_member()).get();
}

static const char *codec_name(sendspin::SendspinCodecFormat codec) {
  return codec == sendspin::SendspinCodecFormat::OPUS ? "opus" : "pcm";
}

// ============================================================================
// Main loop
// ============================================================================

void SendspinSource::setup() {
  sendspin::SendspinClient *client = hub_client(this->parent_);
  if (client == nullptr) {
    this->mark_failed(LOG_STR("The sendspin hub has no client; its setup() must run before this component"));
    return;
  }
  // The role must exist before the hub's first loop() starts the client: a role added later has no task and
  // ignores every server start.
  if (this->parent_->is_client_running()) {
    this->mark_failed(LOG_STR("The sendspin client is already running; the source role must be added before it starts"));
    return;
  }
  // add_source() would replace (destroy) a role the hub already owns while the hub keeps its pointer.
  if (client->source() != nullptr) {
    this->mark_failed(
        LOG_STR("The sendspin hub already owns a source role; remove the sendspin_source external component"));
    return;
  }

  const audio::AudioStreamInfo info = this->mic_source_->get_audio_stream_info();
  this->bits_per_sample_ = info.get_bits_per_sample();
  this->channels_ = info.get_channels();
  this->sample_rate_ = info.get_sample_rate();
  this->bytes_per_frame_ = info.frames_to_bytes(1);
  if ((this->bits_per_sample_ != 16 && this->bits_per_sample_ != 24) || this->channels_ < 1 || this->channels_ > 2 ||
      this->sample_rate_ == 0) {
    this->mark_failed(LOG_STR("The microphone source must deliver 16- or 24-bit audio with 1 or 2 channels"));
    return;
  }

  sendspin::SourceRoleConfig config;
  config.sample_rate = this->sample_rate_;
  config.chunk_duration_ms = this->chunk_duration_ms_;
  config.capture_buffer_ms = this->capture_buffer_ms_;
  config.codec = this->codec_;
  config.channels = this->channels_;
  config.bit_depth = this->bits_per_sample_;
  config.line_sense = this->line_sense_;

  this->role_ = &client->add_source(config);
  this->role_->set_listener(this);
  if (this->line_sense_) {
    // Stored in the role now and carried by the first client/state the client builds in start(). The callbacks
    // fire too, so on_signal_absent publishes the same initial state the server is told.
    this->role_->set_signal(sendspin::SourceSignal::ABSENT);
    this->signal_absent_callbacks_.call();
  } else {
    this->disable_loop();
  }

  // Always-on capture: the library's stream gate decides what is sent, and continuous RX keeps the shared
  // full-duplex clocks stable. The callback runs on the microphone task, the single producer.
  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_audio_(data); });
  this->mic_source_->start();
}

void SendspinSource::loop() { this->update_line_sense_(); }

void SendspinSource::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sendspin Source:\n"
                "  Format: %s, %" PRIu32 " Hz, %u channel(s), %u-bit\n"
                "  Chunk: %" PRIu32 " ms\n"
                "  Capture buffer: %" PRIu32 " ms\n"
                "  Line sense: %s",
                codec_name(this->codec_), this->sample_rate_, this->channels_, this->bits_per_sample_,
                this->chunk_duration_ms_, this->capture_buffer_ms_, YESNO(this->line_sense_));
  if (this->line_sense_) {
    ESP_LOGCONFIG(TAG,
                  "    Threshold: %.1f dBFS, hysteresis %.1f dB\n"
                  "    Attack: %" PRIu32 " ms, release %" PRIu32 " ms",
                  this->threshold_db_, this->hysteresis_db_, this->attack_ms_, this->release_ms_);
  }
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup failed; the source role is not available");
  }
}

// No on_shutdown(): the microphone task keeps running until reboot, and the library tolerates write_audio() after
// the client stops (the stream gate closes and the capture ring outlives it). MicrophoneSource::stop() would reset
// the samples buffer on the main thread while the microphone task may still be copying it.

void SendspinSource::on_streaming_started() {
  this->streaming_.store(true, std::memory_order_relaxed);
  this->dropped_writes_.store(0, std::memory_order_relaxed);
  this->empty_reads_.store(0, std::memory_order_relaxed);
  this->reanchors_.store(0, std::memory_order_relaxed);
  this->anchor_increases_.store(0, std::memory_order_relaxed);
  ESP_LOGI(TAG, "Streaming started");
  this->streaming_started_callbacks_.call();
}

void SendspinSource::on_streaming_stopped() {
  this->streaming_.store(false, std::memory_order_relaxed);
  ESP_LOGI(TAG, "Streaming stopped");
  ESP_LOGD(TAG, "Stream stats: %" PRIu32 " dropped writes, %" PRIu32 " empty reads, %" PRIu32 " re-anchors, %" PRIu32
                " anchor increases",
           this->dropped_writes_.load(std::memory_order_relaxed), this->empty_reads_.load(std::memory_order_relaxed),
           this->reanchors_.load(std::memory_order_relaxed), this->anchor_increases_.load(std::memory_order_relaxed));
  this->streaming_stopped_callbacks_.call();
}

void SendspinSource::update_line_sense_() {
  const uint32_t sequence = this->block_sequence_.load(std::memory_order_acquire);
  if (sequence == this->last_block_sequence_) {
    return;
  }
  this->last_block_sequence_ = sequence;

  const uint32_t mean_square = this->block_mean_square_.load(std::memory_order_relaxed);
  const float level_db =
      mean_square == 0 ? -200.0f : 10.0f * log10f(static_cast<float>(mean_square) / (32768.0f * 32768.0f));

  // PRESENT after `attack` above the threshold; ABSENT after `release` below threshold - hysteresis.
  const bool crossing = this->signal_present_ ? (level_db < this->threshold_db_ - this->hysteresis_db_)
                                              : (level_db >= this->threshold_db_);
  if (!crossing) {
    this->candidate_timing_ = false;
    return;
  }
  const uint32_t now = millis();
  if (!this->candidate_timing_) {
    this->candidate_timing_ = true;
    this->candidate_since_ms_ = now;
  }
  const uint32_t hold_ms = this->signal_present_ ? this->release_ms_ : this->attack_ms_;
  if (now - this->candidate_since_ms_ < hold_ms) {
    return;
  }
  this->candidate_timing_ = false;
  this->set_signal_(!this->signal_present_);
}

void SendspinSource::set_signal_(bool present) {
  this->signal_present_ = present;
  ESP_LOGD(TAG, "Line signal %s", present ? "present" : "absent");
  // Each call publishes a client/state, so only transitions reach here.
  this->role_->set_signal(present ? sendspin::SourceSignal::PRESENT : sendspin::SourceSignal::ABSENT);
  if (present) {
    this->signal_present_callbacks_.call();
  } else {
    this->signal_absent_callbacks_.call();
  }
}

// ============================================================================
// Microphone task
// ============================================================================

void SendspinSource::on_audio_(const std::vector<uint8_t> &data) {
  const int64_t now = esp_timer_get_time();  // platform time, the domain of SendspinClient::get_client_time()
  if (data.empty()) {
    // Read timeout: whatever the driver captured meanwhile is gone, so frame counts no longer map to time.
    this->anchor_valid_ = false;
    this->window_reads_ = 0;
    this->loss_pending_ = false;
    this->empty_reads_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t frames = data.size() / this->bytes_per_frame_;
  if (frames == 0) {
    return;
  }
  this->frames_total_ += frames;

  // Capture time of frame 0 if no frame was ever lost. A read only returns late (DMA granularity, scheduling),
  // so a candidate only overestimates: the lowest recent candidate is the best estimate of the true anchor.
  // The anchor therefore drops immediately but only ever rises by a window minimum: a single late read looks
  // the same as frame loss, and trusting it would hand the server a lead it fills with silence.
  const int64_t candidate = now - this->frames_to_us_(this->frames_total_);
  if (!this->anchor_valid_) {
    // Provisional (as late as this read); the minimum of the following reads pulls it down.
    this->anchor_us_ = candidate;
    this->anchor_valid_ = true;
    this->window_reads_ = 0;
  } else if (candidate < this->anchor_us_) {
    this->anchor_us_ = candidate;
  } else if (candidate - this->anchor_us_ > REANCHOR_US && !this->loss_pending_) {
    // Frame loss (RX queue overflow) or a stalled read: keep this anchor and let a fresh window decide.
    this->loss_pending_ = true;
    this->window_reads_ = 0;
  }
  // Sliding window: if every read of the last window sat above the anchor, frames were lost or a rate error
  // moved the true anchor up; follow it instead of lagging forever behind a stale minimum.
  if (this->window_reads_ == 0 || candidate < this->window_min_us_) {
    this->window_min_us_ = candidate;
  }
  if (++this->window_reads_ >= ANCHOR_WINDOW_READS) {
    if (this->window_min_us_ - this->anchor_us_ > ANCHOR_INCREASE_DEADBAND_US) {
      this->anchor_us_ = this->window_min_us_;
      if (this->loss_pending_) {
        this->reanchors_.fetch_add(1, std::memory_order_relaxed);
      } else {
        this->anchor_increases_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    this->loss_pending_ = false;
    this->window_reads_ = 0;
  }

  int64_t first_sample_us = this->anchor_us_ + this->frames_to_us_(this->frames_total_ - frames);
  if (first_sample_us <= 0) {
    first_sample_us = 1;  // 0 would tell the library "this write ends now"
  }

  if (this->line_sense_) {
    this->accumulate_level_(data);
  }
  if (!this->role_->write_audio(data.data(), data.size(), first_sample_us) &&
      this->streaming_.load(std::memory_order_relaxed)) {
    this->dropped_writes_.fetch_add(1, std::memory_order_relaxed);
  }
}

void SendspinSource::accumulate_level_(const std::vector<uint8_t> &data) {
  // Mean square of the top 16 bits of every sample in the block (both channels together).
  const size_t bytes_per_sample = this->bits_per_sample_ / 8;
  const size_t msb_offset = bytes_per_sample - 2;
  const size_t samples = data.size() / bytes_per_sample;
  if (samples == 0) {
    return;
  }
  int64_t sum_squares = 0;
  const uint8_t *ptr = data.data() + msb_offset;
  for (size_t i = 0; i < samples; i++, ptr += bytes_per_sample) {
    const int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(ptr[0] | (ptr[1] << 8)));
    sum_squares += static_cast<int64_t>(sample) * sample;
  }
  this->block_mean_square_.store(static_cast<uint32_t>(sum_squares / static_cast<int64_t>(samples)),
                                 std::memory_order_relaxed);
  this->block_sequence_.fetch_add(1, std::memory_order_release);
}

}  // namespace esphome::sendspin_source

#endif  // USE_ESP_IDF
