#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP_IDF

#include "esphome/core/automation.h"
#include "sendspin_source.h"

namespace esphome::sendspin_source {

// All four fire on the main loop: the stream callbacks come from the hub's client loop, the signal callbacks
// from SendspinSource::loop().

class StreamingStartedTrigger : public Trigger<> {
 public:
  explicit StreamingStartedTrigger(SendspinSource *parent) {
    parent->add_on_streaming_started_callback([this]() { this->trigger(); });
  }
};

class StreamingStoppedTrigger : public Trigger<> {
 public:
  explicit StreamingStoppedTrigger(SendspinSource *parent) {
    parent->add_on_streaming_stopped_callback([this]() { this->trigger(); });
  }
};

class SignalPresentTrigger : public Trigger<> {
 public:
  explicit SignalPresentTrigger(SendspinSource *parent) {
    parent->add_on_signal_present_callback([this]() { this->trigger(); });
  }
};

class SignalAbsentTrigger : public Trigger<> {
 public:
  explicit SignalAbsentTrigger(SendspinSource *parent) {
    parent->add_on_signal_absent_callback([this]() { this->trigger(); });
  }
};

}  // namespace esphome::sendspin_source

#endif  // USE_ESP_IDF
