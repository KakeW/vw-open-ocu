#include "vw_open_ocu.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::vw_open_ocu {

static constexpr const char *TAG = "vw_open_ocu";
static constexpr unsigned QUEUE_DEPTH = 256;
static constexpr unsigned BATCH_MAX = 48;
static constexpr unsigned HEADER_SIZE = 12;
static constexpr unsigned FRAME_MAX_SIZE = 29;
// Lower bound for an initialized wall clock. TLS still validates certificate dates.
static constexpr std::time_t CLOCK_READY_EPOCH = 1704067200;
static constexpr uint32_t EXTENDED_FLAG = 1UL << 31;
static constexpr uint32_t REMOTE_FLAG = 1UL << 30;

static uint8_t *put_be(uint8_t *dest, uint64_t value, unsigned width) {
  for (unsigned i = width; i > 0; --i)
    *dest++ = static_cast<uint8_t>(value >> ((i - 1) * 8));
  return dest;
}

void Gateway::add_filter(uint32_t id, bool extended) {
  this->allowlist_.push_back(id | (extended ? EXTENDED_FLAG : 0));
}

void Gateway::setup() {
  this->frame_topic_ = "vw_open_ocu/" + this->gateway_id_ + "/v1/frames";
  this->status_topic_ = "vw_open_ocu/" + this->gateway_id_ + "/v1/status";
  this->client_id_ = "vw-open-ocu-" + this->gateway_id_;
  this->frames_ = xQueueCreate(QUEUE_DEPTH, sizeof(Frame));
  if (this->frames_ == nullptr) {
    this->fail_setup_("allocate frame queue", ESP_ERR_NO_MEM);
    return;
  }

  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(this->unused_tx_pin_, this->rx_pin_, TWAI_MODE_LISTEN_ONLY);
  general.tx_queue_len = 0;
  general.rx_queue_len = 128;
  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t result = twai_driver_install(&general, &timing, &filter);
  if (result != ESP_OK) {
    this->fail_setup_("install listen-only TWAI", result);
    vQueueDelete(this->frames_);
    return;
  }

  // The legacy driver requires a TX GPIO. Remove its matrix output before
  // starting the controller. The transceiver TXD must also be isolated physically.
  esp_rom_gpio_connect_out_signal(this->unused_tx_pin_, SIG_GPIO_OUT_IDX, false, false);
  result = gpio_set_direction(this->unused_tx_pin_, GPIO_MODE_INPUT);
  if (result != ESP_OK || !this->start_receiver_()) {
    this->fail_setup_("prepare passive TWAI pins and receiver", result == ESP_OK ? ESP_FAIL : result);
    twai_driver_uninstall();
    vQueueDelete(this->frames_);
    return;
  }

  if (xTaskCreate([](void *context) { static_cast<Gateway *>(context)->receive_(); }, "ocu-can-rx", 3072,
                  this, 8, nullptr) != pdPASS) {
    this->fail_setup_("start CAN receiver task", ESP_ERR_NO_MEM);
    twai_stop();
    twai_driver_uninstall();
    vQueueDelete(this->frames_);
    return;
  }

  result = esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
      [](void *context, esp_event_base_t, int32_t, void *) {
        auto *gateway = static_cast<Gateway *>(context);
        gateway->wifi_generation_.fetch_add(1);
        gateway->epoch_.fetch_add(1);
      },
      this);
  if (result != ESP_OK) {
    this->set_paused_(true);
    this->fail_setup_("register Wi-Fi disconnect handler", result);
    return;
  }

  if (xTaskCreate([](void *context) { static_cast<Gateway *>(context)->publish_(); }, "ocu-mqtt", 6144,
                  this, 3, nullptr) != pdPASS) {
    this->set_paused_(true);
    this->fail_setup_("start MQTT worker task", ESP_ERR_NO_MEM);
  }
  ota::get_global_ota_callback()->add_global_state_listener(this);
  this->set_interval(1000, [this]() { this->report_diagnostics_(); });
}

void Gateway::dump_config() {
  ESP_LOGCONFIG(TAG, "Receive-only CAN gateway %s, GPIO%d RX, capture %s", this->gateway_id_.c_str(),
                this->rx_pin_, this->capture_mode_ ? "all frames" : "allowlist");
}

void Gateway::fail_setup_(const char *operation, esp_err_t error) {
  ESP_LOGE(TAG, "Could not %s: %s", operation, esp_err_to_name(error));
  this->mqtt_state_.store(MqttState::UNAVAILABLE);
  this->record_error_({ErrorKind::SETUP, error});
  this->report_diagnostics_();
  this->mark_failed();
}

bool Gateway::start_receiver_() {
  const auto result = twai_start();
  if (result != ESP_OK)
    ESP_LOGE(TAG, "Could not start listen-only TWAI: %s", esp_err_to_name(result));
  return result == ESP_OK;
}

void Gateway::set_paused_(bool paused) {
  if (this->paused_.exchange(paused) != paused)
    this->epoch_.fetch_add(1);
}

void Gateway::on_shutdown() { this->set_paused_(true); }

void Gateway::on_ota_global_state(ota::OTAState state, float, uint8_t, ota::OTAComponent *) {
  if (this->is_failed())
    return;
  if (state == ota::OTA_STARTED)
    this->set_paused_(true);
  else if (state == ota::OTA_ABORT || state == ota::OTA_ERROR)
    this->set_paused_(false);
  // A completed update stays paused until reboot.
}

void Gateway::receive_() {
  uint64_t sequence = 0;
  bool running = true;
  unsigned processed = 0;
  for (;;) {
    if (this->paused_.load()) {
      if (running) {
        if (const auto result = twai_stop(); result != ESP_OK)
          ESP_LOGW(TAG, "Could not stop listen-only TWAI: %s", esp_err_to_name(result));
        running = false;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (!running) {
      running = this->start_receiver_();
      if (!running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
    }

    // Give the ESPHome loop time even if the controller queue never empties.
    if (++processed == 32) {
      processed = 0;
      vTaskDelay(1);
    }
    twai_message_t message;
    if (twai_receive(&message, pdMS_TO_TICKS(20)) != ESP_OK || this->paused_.load())
      continue;
    const auto timestamp = static_cast<uint64_t>(esp_timer_get_time());
    if (message.data_length_code > 8) {
      this->unsupported_dlc_.fetch_add(1);
      continue;
    }
    const uint32_t identifier = message.identifier | (message.extd ? EXTENDED_FLAG : 0);
    if (!this->capture_mode_ && std::find(this->allowlist_.begin(), this->allowlist_.end(), identifier) ==
                                    this->allowlist_.end()) {
      this->filtered_.fetch_add(1);
      continue;
    }
    Frame frame{sequence++, timestamp, identifier | (message.rtr ? REMOTE_FLAG : 0), message.data_length_code, {}};
    if (!message.rtr)
      std::memcpy(frame.data, message.data, frame.dlc);
    if (xQueueSend(this->frames_, &frame, 0) != pdTRUE) {
      this->dropped_queue_.fetch_add(1);
      continue;
    }
    unsigned depth = uxQueueMessagesWaiting(this->frames_);
    if (depth > this->queue_peak_.load())
      this->queue_peak_.store(depth);
  }
}

uint8_t *Gateway::encode_frame_(uint8_t *dest, const Frame &frame) {
  dest = put_be(dest, frame.sequence, 8);
  dest = put_be(dest, frame.timestamp_us, 8);
  dest = put_be(dest, frame.flags_id, 4);
  *dest++ = frame.dlc;
  if (!(frame.flags_id & REMOTE_FLAG)) {
    std::memcpy(dest, frame.data, frame.dlc);
    dest += frame.dlc;
  }
  return dest;
}

void Gateway::discard_queued_() {
  const unsigned queued = uxQueueMessagesWaiting(this->frames_);
  Frame discarded;
  for (unsigned i = 0; i < queued; ++i) {
    if (xQueueReceive(this->frames_, &discarded, 0) == pdTRUE)
      this->dropped_transport_.fetch_add(1);
  }
}

bool Gateway::LogGate::allows(uint32_t now, unsigned next, bool repeat) {
  const uint32_t elapsed = now - this->at;
  if (this->initialized && (elapsed < 5000 || (next == this->value && (!repeat || elapsed < 30000))))
    return false;
  this->at = now;
  this->value = next;
  this->initialized = true;
  return true;
}

const char *Gateway::wifi_label_(WifiState state) {
  switch (state) {
    case WifiState::NO_STATION: return "Station interface unavailable";
    case WifiState::INTERFACE_DOWN: return "Station interface down";
    case WifiState::NO_IPV4: return "Waiting for station IPv4 address";
    case WifiState::READY: return "Station IPv4 ready";
  }
  return "Unknown";
}

const char *Gateway::mqtt_label_(MqttState state) {
  switch (state) {
    case MqttState::INITIALIZING: return "Initializing";
    case MqttState::WAITING_WIFI: return "Waiting for Wi-Fi IPv4";
    case MqttState::WAITING_TIME: return "Waiting for valid system time";
    case MqttState::STARTING: return "Starting MQTT client";
    case MqttState::CONNECTING: return "Connecting or reconnecting";
    case MqttState::CONNECTED: return "Connected";
    case MqttState::RETRYING: return "Retrying MQTT client";
    case MqttState::PAUSED: return "Paused for OTA or shutdown";
    case MqttState::UNAVAILABLE: return "Gateway setup failed";
  }
  return "Unknown";
}

void Gateway::format_error_(const MqttError &error, char *text, size_t size) {
  const char *label = "Unknown MQTT error";
  switch (error.kind) {
    case ErrorKind::NONE:
      std::snprintf(text, size, "None");
      return;
    case ErrorKind::TLS:
    case ErrorKind::TRANSPORT:
      std::snprintf(text, size, "%s (esp=0x%x stack=%d verify=0x%x errno=%d)",
                    error.kind == ErrorKind::TLS ? "TLS error" : "TCP transport error",
                    static_cast<unsigned>(error.code), error.tls_stack, error.verify_flags, error.socket_errno);
      return;
    case ErrorKind::SETUP: label = "Gateway setup failed"; break;
    case ErrorKind::INIT: label = "MQTT client initialization failed"; break;
    case ErrorKind::EVENT_REGISTRATION: label = "MQTT event registration failed"; break;
    case ErrorKind::START: label = "MQTT client start failed"; break;
    case ErrorKind::STOP: label = "MQTT client stop failed"; break;
    case ErrorKind::AUTH: label = "MQTT authentication or authorization refused"; break;
    case ErrorKind::REFUSED: label = "MQTT broker refused connection"; break;
    case ErrorKind::UNKNOWN_MQTT: break;
    case ErrorKind::CONTROLLER: label = "Heartbeat TWAI status read failed"; break;
    case ErrorKind::HEARTBEAT_ENCODING: label = "Heartbeat encoding failed"; break;
    case ErrorKind::HEARTBEAT_PUBLISH: label = "Heartbeat local publish failed"; break;
  }
  std::snprintf(text, size, "%s (code=%d)", label, error.code);
}

void Gateway::record_error_(MqttError error) {
  portENTER_CRITICAL(&this->error_lock_);
  error.serial = this->last_error_.serial + 1;
  this->last_error_ = error;
  portEXIT_CRITICAL(&this->error_lock_);
}

void Gateway::report_diagnostics_() {
  const auto wifi = this->wifi_state_.load();
  const auto state = this->mqtt_state_.load();
  const bool clock = this->clock_ready_.load();
  const bool connected = state == MqttState::CONNECTED && this->mqtt_ready_.load() && !this->paused_.load();
  const unsigned starts = this->starts_.load();
  const unsigned attempts = this->attempts_.load();
  const unsigned connects = this->connects_.load();
  const unsigned disconnects = this->disconnects_.load();
  const unsigned heartbeats = this->heartbeat_count_.load();
  const unsigned failures = this->heartbeat_failures_.load();
  portENTER_CRITICAL(&this->error_lock_);
  const MqttError error = this->last_error_;
  portEXIT_CRITICAL(&this->error_lock_);
  char error_text[128];
  format_error_(error, error_text, sizeof(error_text));

  const auto publish = [](auto *entity, auto value) {
    if (entity != nullptr && (!entity->has_state() || entity->state != value))
      entity->publish_state(value);
  };
  publish(this->wifi_sensor_, wifi == WifiState::READY);
  publish(this->clock_sensor_, clock);
  publish(this->mqtt_sensor_, connected);
  publish(this->state_sensor_, mqtt_label_(state));
  publish(this->error_sensor_, static_cast<const char *>(error_text));
  publish(this->attempt_sensor_, static_cast<float>(attempts));
  publish(this->heartbeat_sensor_, static_cast<float>(heartbeats));
  publish(this->heartbeat_failure_sensor_, static_cast<float>(failures));

  const uint32_t now = millis();
  if (this->wifi_log_.allows(now, static_cast<unsigned>(wifi), state == MqttState::WAITING_WIFI))
    ESP_LOGI(TAG, "Wi-Fi: %s", wifi_label_(wifi));
  if (this->clock_log_.allows(now, clock, state == MqttState::WAITING_TIME))
    ESP_LOGI(TAG, "Clock: %s", clock ? "valid for TLS startup" : "waiting for valid time (check SNTP or other time source)");
  if (this->mqtt_log_.allows(now, static_cast<unsigned>(state)))
    ESP_LOGI(TAG, "MQTT: %s", mqtt_label_(state));
  if (starts != 0 && this->event_log_.allows(now, starts + attempts + connects + disconnects))
    ESP_LOGI(TAG, "MQTT events: client starts=%u attempts=%u connected=%u disconnected=%u", starts, attempts,
             connects, disconnects);
  if (error.serial != 0 && this->error_log_.allows(now, error.serial))
    ESP_LOGW(TAG, "%s", error_text);
  if (failures != 0 && this->heartbeat_log_.allows(now, failures))
    ESP_LOGW(TAG, "Heartbeat local results: sent=%u failed=%u", heartbeats, failures);
}

void Gateway::mqtt_event_(int32_t event, const esp_mqtt_event_t *data) {
  switch (event) {
    case MQTT_EVENT_BEFORE_CONNECT:
      this->attempts_.fetch_add(1);
      break;
    case MQTT_EVENT_CONNECTED:
      this->epoch_.fetch_add(1);
      this->mqtt_ready_.store(true);
      this->connects_.fetch_add(1);
      break;
    case MQTT_EVENT_DISCONNECTED:
      this->mqtt_ready_.store(false);
      this->epoch_.fetch_add(1);
      this->disconnects_.fetch_add(1);
      break;
    case MQTT_EVENT_ERROR: {
      const auto *error = data == nullptr ? nullptr : data->error_handle;
      if (error == nullptr) {
        this->record_error_({ErrorKind::UNKNOWN_MQTT});
      } else if (error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        const auto code = error->connect_return_code;
        const bool auth = code == MQTT_CONNECTION_REFUSE_BAD_USERNAME || code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED;
        this->record_error_({auth ? ErrorKind::AUTH : ErrorKind::REFUSED, code});
      } else if (error->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        const auto code = error->esp_tls_last_esp_err;
        const bool tls = error->esp_tls_cert_verify_flags != 0 || error->esp_tls_stack_err != 0 ||
                         (code >= ESP_ERR_MBEDTLS_CERT_PARTLY_OK && code <= ESP_ERR_MBEDTLS_SSL_READ_FAILED) ||
                         code == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT;
        this->record_error_({tls ? ErrorKind::TLS : ErrorKind::TRANSPORT, error->esp_tls_last_esp_err,
                             error->esp_tls_stack_err, static_cast<unsigned>(error->esp_tls_cert_verify_flags),
                             error->esp_transport_sock_errno});
      } else {
        this->record_error_({ErrorKind::UNKNOWN_MQTT, error->error_type});
      }
      break;
    }
    default: break;
  }
}

bool Gateway::heartbeat_(esp_mqtt_client_handle_t client, uint64_t session, unsigned epoch,
                         const uint64_t *last_published) {
  twai_status_info_t info{};
  if (const auto result = twai_get_status_info(&info); result != ESP_OK) {
    this->record_error_({ErrorKind::CONTROLLER, result});
    return false;
  }
  char payload[512];
  char published[21] = "null";
  if (last_published != nullptr)
    std::snprintf(published, sizeof(published), "%" PRIu64, *last_published);
  const int length = std::snprintf(
      payload, sizeof(payload),
      "{\"version\":1,\"session\":\"%016" PRIx64 "\",\"capture_mode\":%s,"
      "\"connection_epoch\":%u,\"last_published_sequence\":%s,"
      "\"queue_depth\":%u,\"queue_capacity\":%u,\"queue_peak\":%u,"
      "\"queue_dropped\":%u,\"transport_dropped\":%u,\"filtered\":%u,"
      "\"unsupported_dlc\":%u,\"driver_missed\":%" PRIu32 ",\"driver_overrun\":%" PRIu32 ","
      "\"bus_errors\":%" PRIu32 ",\"rx_errors\":%" PRIu32 ",\"controller_state\":%d}",
      session, this->capture_mode_ ? "true" : "false", epoch, published,
      static_cast<unsigned>(uxQueueMessagesWaiting(this->frames_)), QUEUE_DEPTH, this->queue_peak_.load(),
      this->dropped_queue_.load(), this->dropped_transport_.load(), this->filtered_.load(),
      this->unsupported_dlc_.load(), info.rx_missed_count, info.rx_overrun_count, info.bus_error_count,
      info.rx_error_counter, static_cast<int>(info.state));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload)) {
    this->record_error_({ErrorKind::HEARTBEAT_ENCODING, length});
    return false;
  }
  if (this->paused_.load() || !this->mqtt_ready_.load() || epoch != this->epoch_.load())
    return false;
  const int result = esp_mqtt_client_publish(client, this->status_topic_.c_str(), payload, length, 0, 0);
  if (result < 0)
    this->record_error_({ErrorKind::HEARTBEAT_PUBLISH, result});
  return result >= 0;
}

void Gateway::publish_() {
  esp_mqtt_client_config_t config{};
  config.broker.address.uri = this->uri_.c_str();
  config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
  config.broker.verification.certificate = this->ca_.empty() ? nullptr : this->ca_.c_str();
  config.broker.verification.crt_bundle_attach = this->ca_.empty() ? esp_crt_bundle_attach : nullptr;
  config.credentials.username = this->username_.c_str();
  config.credentials.authentication.password = this->password_.c_str();
  config.credentials.client_id = this->client_id_.c_str();
  config.network.timeout_ms = 2000;
  config.task.priority = 4;
  esp_mqtt_client_handle_t client = nullptr;
  const auto start_client = [this, &config, &client]() {
    if (client == nullptr) {
      client = esp_mqtt_client_init(&config);
      if (client == nullptr) {
        this->record_error_({ErrorKind::INIT});
        return false;
      }
      const auto result = esp_mqtt_client_register_event(
          client, MQTT_EVENT_ANY,
          [](void *context, esp_event_base_t, int32_t event, void *data) {
            static_cast<Gateway *>(context)->mqtt_event_(event, static_cast<const esp_mqtt_event_t *>(data));
          },
          this);
      if (result != ESP_OK) {
        this->record_error_({ErrorKind::EVENT_REGISTRATION, result});
        esp_mqtt_client_destroy(client);
        client = nullptr;
        return false;
      }
    }
    const auto result = esp_mqtt_client_start(client);
    if (result != ESP_OK)
      this->record_error_({ErrorKind::START, result});
    return result == ESP_OK;
  };

  MqttLifecycle lifecycle;
  uint8_t batch[HEADER_SIZE + BATCH_MAX * FRAME_MAX_SIZE] = {'O', 'C', 'U', 1};
  uint64_t session = 0;
  bool session_created = false;
  uint64_t last_published = 0;
  bool published_frames = false;
  for (;;) {
    const unsigned wifi_generation = this->wifi_generation_.load();
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    WifiState wifi = WifiState::NO_STATION;
    if (netif != nullptr) {
      wifi = !esp_netif_is_netif_up(netif) ? WifiState::INTERFACE_DOWN
             : esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0 ? WifiState::NO_IPV4
                                                                            : WifiState::READY;
    }
    const bool clock = std::time(nullptr) >= CLOCK_READY_EPOCH;
    this->wifi_state_.store(wifi);
    this->clock_ready_.store(clock);

    const uint64_t now = esp_timer_get_time() / 1000;
    const bool ready = this->mqtt_ready_.load();
    const unsigned epoch = this->epoch_.load();
    const auto decision = lifecycle.poll(
        {now, wifi == WifiState::READY, clock, this->paused_.load(), ready, wifi_generation, epoch});
    this->mqtt_state_.store(decision.state);
    if (decision.discard_backlog)
      this->discard_queued_();

    switch (decision.action) {
      case MqttAction::WAIT:
        vTaskDelay(pdMS_TO_TICKS(40));
        continue;
      case MqttAction::START: {
        if (this->paused_.load() || wifi_generation != this->wifi_generation_.load())
          continue;
        const bool success = start_client();
        lifecycle.started(esp_timer_get_time() / 1000, success);
        if (success) {
          this->starts_.fetch_add(1);
          if (!session_created) {
            session = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
            put_be(batch + 4, session, 8);
            session_created = true;
          }
        }
        continue;
      }
      case MqttAction::STOP: {
        const auto result = esp_mqtt_client_stop(client);
        this->mqtt_ready_.store(false);
        bool stopped = result == ESP_OK;
        if (!stopped) {
          this->record_error_({ErrorKind::STOP, result});
          // From this worker, ESP_FAIL means ESP-MQTT was already stopped.
          if (result == ESP_FAIL && esp_mqtt_client_destroy(client) == ESP_OK) {
            client = nullptr;
            stopped = true;
          }
        }
        lifecycle.stopped(esp_timer_get_time() / 1000, stopped);
        continue;
      }
      case MqttAction::HEARTBEAT: {
        const bool success = this->heartbeat_(client, session, epoch, published_frames ? &last_published : nullptr);
        if (success)
          this->heartbeat_count_.fetch_add(1);
        else if (!this->paused_.load() && epoch == this->epoch_.load())
          this->heartbeat_failures_.fetch_add(1);
        lifecycle.heartbeat(esp_timer_get_time() / 1000, success);
        continue;
      }
      case MqttAction::FRAMES: break;
    }

    Frame frame;
    if (xQueueReceive(this->frames_, &frame, pdMS_TO_TICKS(20)) != pdTRUE)
      continue;
    uint8_t *end = encode_frame_(batch + HEADER_SIZE, frame);
    uint64_t batch_last = frame.sequence;
    unsigned count = 1;
    if (uxQueueMessagesWaiting(this->frames_) < BATCH_MAX - 1)
      vTaskDelay(pdMS_TO_TICKS(10));
    while (count < BATCH_MAX && xQueueReceive(this->frames_, &frame, 0) == pdTRUE) {
      end = encode_frame_(end, frame);
      batch_last = frame.sequence;
      ++count;
    }
    if (this->paused_.load() || !this->mqtt_ready_.load() || epoch != this->epoch_.load() ||
        esp_mqtt_client_publish(client, this->frame_topic_.c_str(), reinterpret_cast<const char *>(batch),
                                end - batch, 0, 0) < 0) {
      this->dropped_transport_.fetch_add(count);
    } else {
      last_published = batch_last;
      published_frames = true;
    }
    vTaskDelay(1);
  }
}

}  // namespace esphome::vw_open_ocu
