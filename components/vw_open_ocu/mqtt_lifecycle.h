#pragma once

#include <cstdint>

namespace esphome::vw_open_ocu {

enum class MqttState : uint8_t {
  INITIALIZING,
  WAITING_WIFI,
  WAITING_TIME,
  STARTING,
  CONNECTING,
  CONNECTED,
  RETRYING,
  PAUSED,
  UNAVAILABLE,
};

enum class MqttAction : uint8_t { WAIT, START, STOP, HEARTBEAT, FRAMES };

struct MqttInputs {
  uint64_t now_ms;
  bool wifi_ready;
  bool clock_ready;
  bool paused;
  bool connected;
  unsigned wifi_generation;
  unsigned connection_epoch;
};

struct MqttDecision {
  MqttState state;
  MqttAction action;
  bool discard_backlog;
};

// Owned by the MQTT worker. The SDK owns transport reconnection while started.
class MqttLifecycle {
 public:
  MqttDecision poll(const MqttInputs &input) {
    const bool wifi_changed = input.wifi_generation != this->wifi_generation_;
    const bool epoch_changed = input.connection_epoch != this->epoch_;
    this->wifi_generation_ = input.wifi_generation;
    this->epoch_ = input.connection_epoch;
    if (wifi_changed || epoch_changed || !input.connected) {
      this->heartbeat_at_ = 0;
      this->heartbeat_sent_ = false;
    }

    const bool blocked = input.paused || !input.wifi_ready || !input.clock_ready || wifi_changed;
    this->stop_pending_ |= blocked && this->running_;
    if (blocked || this->stop_pending_) {
      const auto state = input.paused         ? MqttState::PAUSED
                         : !input.wifi_ready  ? MqttState::WAITING_WIFI
                         : !input.clock_ready ? MqttState::WAITING_TIME
                                              : MqttState::RETRYING;
      const auto action = this->stop_pending_ && input.now_ms >= this->stop_at_ ? MqttAction::STOP
                                                                               : MqttAction::WAIT;
      return {state, action, true};
    }
    if (!this->running_) {
      if (input.now_ms < this->start_at_)
        return {MqttState::RETRYING, MqttAction::WAIT, true};
      return {MqttState::STARTING, MqttAction::START, true};
    }
    if (!input.connected)
      return {MqttState::CONNECTING, MqttAction::WAIT, true};
    if (input.now_ms >= this->heartbeat_at_)
      return {MqttState::CONNECTED, MqttAction::HEARTBEAT, epoch_changed};
    if (!this->heartbeat_sent_)
      return {MqttState::CONNECTED, MqttAction::WAIT, true};
    return {MqttState::CONNECTED, MqttAction::FRAMES, epoch_changed};
  }

  void started(uint64_t now_ms, bool success) {
    this->running_ = success;
    this->start_at_ = success ? 0 : now_ms + 5000;
  }

  void stopped(uint64_t now_ms, bool success) {
    this->running_ = !success;
    this->stop_pending_ = !success;
    this->stop_at_ = success ? 0 : now_ms + 5000;
    this->heartbeat_at_ = 0;
    this->heartbeat_sent_ = false;
    this->start_at_ = success ? 0 : now_ms + 5000;
  }

  void heartbeat(uint64_t now_ms, bool success) {
    this->heartbeat_sent_ = success;
    this->heartbeat_at_ = now_ms + (success ? 5000 : 1000);
  }

 private:
  bool running_{false};
  bool heartbeat_sent_{false};
  bool stop_pending_{false};
  unsigned wifi_generation_{0}, epoch_{0};
  uint64_t start_at_{0}, stop_at_{0}, heartbeat_at_{0};
};

}  // namespace esphome::vw_open_ocu
