#include <cassert>
#include <cstddef>
#include <cstring>
#include "mqtt_config.h"

using esphome::vw_open_ocu::make_mqtt_config;

// Model the ESP-MQTT fields written by the production configuration builder.
struct TestConfig {
  struct {
    struct {
      const char *uri;
      int transport;
    } address;
    struct {
      const char *certificate;
      std::size_t certificate_len;
      void (*crt_bundle_attach)();
      bool skip_cert_common_name_check;
    } verification;
  } broker;
  struct {
    const char *username;
    const char *client_id;
    struct {
      const char *password;
    } authentication;
  } credentials;
  struct { int timeout_ms; } network;
  struct { int priority; } task;
};

static void attach_bundle() {}

int main() {
  const char uri[] = "mqtts://192.0.2.1:8883";
  const char ca[] = "synthetic CA buffer";
  const auto explicit_ca = make_mqtt_config<TestConfig>(uri, "user", "example", "vw-open-ocu-bench", ca, attach_bundle);
  assert(explicit_ca.broker.address.uri == uri);
  // ESP-IDF 5.5.5 rejects uri && transport before creating a socket.
  assert(explicit_ca.broker.address.transport == 0);
  assert(explicit_ca.broker.verification.certificate == ca);
  assert(explicit_ca.broker.verification.certificate_len == 0);
  assert(explicit_ca.broker.verification.crt_bundle_attach == nullptr);
  assert(!explicit_ca.broker.verification.skip_cert_common_name_check);
  assert(std::strcmp(explicit_ca.credentials.username, "user") == 0);
  assert(std::strcmp(explicit_ca.credentials.authentication.password, "example") == 0);
  assert(std::strcmp(explicit_ca.credentials.client_id, "vw-open-ocu-bench") == 0);
  assert(explicit_ca.task.priority == 4);

  const auto bundle = make_mqtt_config<TestConfig>(uri, "user", "example", "vw-open-ocu-bench", nullptr, attach_bundle);
  assert(bundle.broker.address.transport == 0);
  assert(bundle.broker.verification.certificate == nullptr);
  assert(bundle.broker.verification.crt_bundle_attach == attach_bundle);
  assert(!bundle.broker.verification.skip_cert_common_name_check);
}
