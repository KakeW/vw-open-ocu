#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"

#include "esphome/components/ota/ota_backend.h"
#include "esphome/core/component.h"

namespace esphome::vw_open_ocu {

class Gateway final : public Component, public ota::OTAGlobalStateListener {
 public:
  void set_gateway_id(const std::string &id) { this->gateway_id_ = id; }
  void set_broker(const std::string &uri, const std::string &username, const std::string &password) {
    this->uri_ = uri;
    this->username_ = username;
    this->password_ = password;
  }
  void set_ca_certificate(const std::string &ca) { this->ca_ = ca; }
  void set_pins(int rx, int unused_tx) {
    this->rx_pin_ = static_cast<gpio_num_t>(rx);
    this->unused_tx_pin_ = static_cast<gpio_num_t>(unused_tx);
  }
  void set_capture_mode(bool enabled) { this->capture_mode_ = enabled; }
  void add_filter(uint32_t id, bool extended);
  void setup() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *component) override;

 private:
  struct Frame {
    uint64_t sequence;
    uint64_t timestamp_us;
    uint32_t flags_id;
    uint8_t dlc;
    uint8_t data[8];
  };

  static uint8_t *encode_frame_(uint8_t *dest, const Frame &frame);
  void receive_();
  void publish_();
  void discard_queued_();
  bool heartbeat_(esp_mqtt_client_handle_t client, uint64_t session, unsigned epoch, const uint64_t *last_published);
  bool start_receiver_();
  void set_paused_(bool paused);
  void fail_setup_(const char *operation, esp_err_t error);

  std::string gateway_id_;
  std::string uri_, username_, password_, ca_;
  std::string frame_topic_, status_topic_, client_id_;
  gpio_num_t rx_pin_{GPIO_NUM_1}, unused_tx_pin_{GPIO_NUM_0};
  bool capture_mode_{false};
  std::vector<uint32_t> allowlist_;

  QueueHandle_t frames_{nullptr};
  std::atomic<bool> paused_{false};
  std::atomic<bool> mqtt_ready_{false};
  std::atomic<unsigned> epoch_{0};
  std::atomic<unsigned> wifi_generation_{0};
  std::atomic<unsigned> dropped_queue_{0}, dropped_transport_{0}, filtered_{0}, unsupported_dlc_{0}, queue_peak_{0};
};

}  // namespace esphome::vw_open_ocu
