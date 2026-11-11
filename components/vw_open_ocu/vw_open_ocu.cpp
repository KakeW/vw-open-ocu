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
#include "esp_wifi.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"

#include "esphome/core/log.h"

namespace esphome::vw_open_ocu {

static constexpr const char *TAG = "vw_open_ocu";
static constexpr unsigned QUEUE_DEPTH = 256;
static constexpr unsigned BATCH_MAX = 48;
static constexpr unsigned HEADER_SIZE = 12;
static constexpr unsigned FRAME_MAX_SIZE = 29;
static constexpr int64_t HEARTBEAT_US = 5000000;
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
}

void Gateway::dump_config() {
  ESP_LOGCONFIG(TAG, "Receive-only CAN gateway %s, GPIO%d RX, capture %s", this->gateway_id_.c_str(),
                this->rx_pin_, this->capture_mode_ ? "all frames" : "allowlist");
}

void Gateway::fail_setup_(const char *operation, esp_err_t error) {
  ESP_LOGE(TAG, "Could not %s: %s", operation, esp_err_to_name(error));
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

bool Gateway::heartbeat_(esp_mqtt_client_handle_t client, uint64_t session, unsigned epoch,
                         const uint64_t *last_published) {
  twai_status_info_t info{};
  if (twai_get_status_info(&info) != ESP_OK)
    return false;
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
  return length > 0 && static_cast<size_t>(length) < sizeof(payload) && !this->paused_.load() &&
         epoch == this->epoch_.load() &&
         esp_mqtt_client_publish(client, this->status_topic_.c_str(), payload, length, 0, 0) >= 0;
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
  auto client = esp_mqtt_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(TAG, "Could not create MQTT transport; OTA remains available");
    this->set_paused_(true);
    vTaskDelete(nullptr);
    return;
  }
  const auto result = esp_mqtt_client_register_event(
      client, MQTT_EVENT_ANY,
      [](void *context, esp_event_base_t, int32_t event, void *) {
        auto *gateway = static_cast<Gateway *>(context);
        if (event == MQTT_EVENT_CONNECTED) {
          gateway->epoch_.fetch_add(1);
          gateway->mqtt_ready_.store(true);
        } else if (event == MQTT_EVENT_DISCONNECTED) {
          gateway->mqtt_ready_.store(false);
          gateway->epoch_.fetch_add(1);
        }
      },
      this);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Could not register MQTT events: %s", esp_err_to_name(result));
    esp_mqtt_client_destroy(client);
    this->set_paused_(true);
    vTaskDelete(nullptr);
    return;
  }

  uint8_t batch[HEADER_SIZE + BATCH_MAX * FRAME_MAX_SIZE] = {'O', 'C', 'U', 1};
  uint64_t session = 0;
  bool session_created = false;
  bool started = false;
  unsigned wifi_generation = this->wifi_generation_.load();
  unsigned epoch = this->epoch_.load();
  int64_t next_heartbeat = 0;
  uint64_t last_published = 0;
  bool published_frames = false;
  for (;;) {
    const unsigned current_wifi = this->wifi_generation_.load();
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    const bool wifi_ready = netif != nullptr && esp_netif_is_netif_up(netif) &&
                            esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
    if (this->paused_.load() || !wifi_ready || current_wifi != wifi_generation) {
      if (started) {
        esp_mqtt_client_stop(client);
        this->mqtt_ready_.store(false);
        started = false;
      }
      wifi_generation = current_wifi;
      this->discard_queued_();
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }
    if (!started) {
      // Wait for ESPHome's time source before verifying TLS certificates.
      if (std::time(nullptr) < 1704067200 || esp_mqtt_client_start(client) != ESP_OK) {
        this->discard_queued_();
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      started = true;
      if (!session_created) {
        session = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
        put_be(batch + 4, session, 8);
        session_created = true;
      }
    }
    const bool mqtt_ready = this->mqtt_ready_.load();
    const unsigned current_epoch = this->epoch_.load();
    if (!mqtt_ready || current_epoch != epoch) {
      this->discard_queued_();
      epoch = current_epoch;
      next_heartbeat = 0;
      if (!mqtt_ready) {
        vTaskDelay(pdMS_TO_TICKS(40));
        continue;
      }
    }
    const int64_t now = esp_timer_get_time();
    if (now >= next_heartbeat) {
      if (!this->heartbeat_(client, session, epoch, published_frames ? &last_published : nullptr)) {
        this->discard_queued_();
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      next_heartbeat = now + HEARTBEAT_US;
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
    if (this->paused_.load() || epoch != this->epoch_.load() ||
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
