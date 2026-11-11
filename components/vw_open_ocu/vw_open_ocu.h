#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "mqtt_lifecycle.h"

#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ota/ota_backend.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
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
  void set_wifi_ready(binary_sensor::BinarySensor *entity) { this->wifi_sensor_ = entity; }
  void set_clock_ready(binary_sensor::BinarySensor *entity) { this->clock_sensor_ = entity; }
  void set_mqtt_connected(binary_sensor::BinarySensor *entity) { this->mqtt_sensor_ = entity; }
  void set_mqtt_state(text_sensor::TextSensor *entity) { this->state_sensor_ = entity; }
  void set_last_mqtt_error(text_sensor::TextSensor *entity) { this->error_sensor_ = entity; }
  void set_mqtt_attempts(sensor::Sensor *entity) { this->attempt_sensor_ = entity; }
  void set_heartbeat_count(sensor::Sensor *entity) { this->heartbeat_sensor_ = entity; }
  void set_heartbeat_failures(sensor::Sensor *entity) { this->heartbeat_failure_sensor_ = entity; }
  void add_filter(uint32_t id, bool extended);
  void setup() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *component) override;

 private:
  enum class WifiState : uint8_t { NO_STATION, INTERFACE_DOWN, NO_IPV4, READY };
  enum class ErrorKind : uint8_t {
    NONE, SETUP, INIT, EVENT_REGISTRATION, START, STOP, TRANSPORT, TLS, AUTH, REFUSED,
    UNKNOWN_MQTT, CONTROLLER, HEARTBEAT_ENCODING, HEARTBEAT_PUBLISH,
  };
  struct MqttError {
    ErrorKind kind{ErrorKind::NONE};
    int code{0};
    int tls_stack{0};
    unsigned verify_flags{0};
    int socket_errno{0};
    unsigned serial{0};
  };
  struct LogGate {
    uint32_t at{0};
    unsigned value{0};
    bool initialized{false};
    bool allows(uint32_t now, unsigned next, bool repeat = false);
  };
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
  void mqtt_event_(int32_t event, const esp_mqtt_event_t *data);
  void record_error_(MqttError error);
  void report_diagnostics_();
  static const char *wifi_label_(WifiState state);
  static const char *mqtt_label_(MqttState state);
  static void format_error_(const MqttError &error, char *text, size_t size);
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
  std::atomic<WifiState> wifi_state_{WifiState::NO_STATION};
  std::atomic<MqttState> mqtt_state_{MqttState::INITIALIZING};
  std::atomic<bool> clock_ready_{false};
  std::atomic<unsigned> starts_{0}, attempts_{0}, connects_{0}, disconnects_{0};
  std::atomic<unsigned> heartbeat_count_{0}, heartbeat_failures_{0};
  portMUX_TYPE error_lock_ = portMUX_INITIALIZER_UNLOCKED;
  MqttError last_error_{};
  LogGate wifi_log_, clock_log_, mqtt_log_, event_log_, error_log_, heartbeat_log_;

  binary_sensor::BinarySensor *wifi_sensor_{nullptr}, *clock_sensor_{nullptr}, *mqtt_sensor_{nullptr};
  text_sensor::TextSensor *state_sensor_{nullptr}, *error_sensor_{nullptr};
  sensor::Sensor *attempt_sensor_{nullptr}, *heartbeat_sensor_{nullptr}, *heartbeat_failure_sensor_{nullptr};
  std::atomic<unsigned> dropped_queue_{0}, dropped_transport_{0}, filtered_{0}, unsupported_dlc_{0}, queue_peak_{0};
};

}  // namespace esphome::vw_open_ocu
