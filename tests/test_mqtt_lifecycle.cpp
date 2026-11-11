#include <cassert>
#include "mqtt_lifecycle.h"

using namespace esphome::vw_open_ocu;

static void initial_connection_waits_for_wifi_and_time() {
  MqttLifecycle worker;
  MqttInputs input{};
  assert(worker.poll(input).state == MqttState::WAITING_WIFI);
  input.wifi_ready = true;
  for (input.now_ms = 10; input.now_ms < 20000; input.now_ms += 100) {
    auto decision = worker.poll(input);
    assert(decision.state == MqttState::WAITING_TIME && decision.action == MqttAction::WAIT);
  }
  input.clock_ready = true;
  assert(worker.poll(input).action == MqttAction::START);
  worker.started(input.now_ms, true);
  assert(worker.poll(input).state == MqttState::CONNECTING);
  input.connected = true;
  input.connection_epoch++;
  auto first = worker.poll(input);
  assert(first.action == MqttAction::HEARTBEAT && first.discard_backlog);
  worker.heartbeat(input.now_ms, false);
  assert(worker.poll(input).action == MqttAction::WAIT);
  input.now_ms += 1000;
  assert(worker.poll(input).action == MqttAction::HEARTBEAT);
  worker.heartbeat(input.now_ms, true);
  assert(worker.poll(input).action == MqttAction::FRAMES);
  input.now_ms += 5000;
  assert(worker.poll(input).action == MqttAction::HEARTBEAT);
}

static void reconnect_requires_a_new_heartbeat() {
  MqttLifecycle worker;
  MqttInputs input{0, true, true, false, false, 0, 0};
  assert(worker.poll(input).action == MqttAction::START);
  worker.started(0, true);
  input.connected = true;
  input.connection_epoch++;
  assert(worker.poll(input).action == MqttAction::HEARTBEAT);
  worker.heartbeat(0, true);
  input.connected = false;
  input.connection_epoch++;
  auto disconnected = worker.poll(input);
  assert(disconnected.state == MqttState::CONNECTING && disconnected.discard_backlog);
  input.connected = true;
  input.connection_epoch++;
  assert(worker.poll(input).action == MqttAction::HEARTBEAT);
  worker.heartbeat(0, true);

  // The station recovered before the next worker poll. The loss still forces a fresh client connection.
  input.wifi_generation++;
  assert(worker.poll(input).action == MqttAction::STOP);
  worker.stopped(0, false);
  assert(worker.poll(input).action == MqttAction::WAIT);
  input.now_ms = 5000;
  assert(worker.poll(input).action == MqttAction::STOP);
  worker.stopped(input.now_ms, true);
  input.connected = false;
  assert(worker.poll(input).action == MqttAction::START);
  worker.started(input.now_ms, true);

  input.clock_ready = false;
  assert(worker.poll(input).action == MqttAction::STOP);
  worker.stopped(0, true);
  assert(worker.poll(input).state == MqttState::WAITING_TIME);
  input.clock_ready = true;
  assert(worker.poll(input).action == MqttAction::START);
  worker.started(0, true);
  input.paused = true;
  assert(worker.poll(input).action == MqttAction::STOP);
  worker.stopped(0, true);
  assert(worker.poll(input).state == MqttState::PAUSED);
}

static void start_error_backs_off() {
  MqttLifecycle worker;
  MqttInputs input{0, true, true, false, false, 0, 0};
  assert(worker.poll(input).action == MqttAction::START);
  worker.started(0, false);
  input.now_ms = 4999;
  assert(worker.poll(input).action == MqttAction::WAIT);
  input.now_ms = 5000;
  assert(worker.poll(input).action == MqttAction::START);
}

int main() {
  initial_connection_waits_for_wifi_and_time();
  reconnect_requires_a_new_heartbeat();
  start_error_backs_off();
}
