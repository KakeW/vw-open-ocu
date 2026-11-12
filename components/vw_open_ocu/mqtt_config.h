#pragma once

namespace esphome::vw_open_ocu {

// The ESP-MQTT 5.5 configuration check rejects an explicit transport
// together with a URI. The validated mqtts:// URI selects TLS on its own.
template<typename Config, typename BundleAttach>
Config make_mqtt_config(const char *uri, const char *username, const char *password, const char *client_id,
                        const char *ca_pem, BundleAttach bundle_attach) {
  Config config{};
  config.broker.address.uri = uri;
  // Zero certificate_len tells ESP-MQTT to use its NUL-terminated PEM path.
  config.broker.verification.certificate = ca_pem;
  config.broker.verification.crt_bundle_attach = ca_pem == nullptr ? bundle_attach : nullptr;
  config.credentials.username = username;
  config.credentials.authentication.password = password;
  config.credentials.client_id = client_id;
  config.network.timeout_ms = 2000;
  config.task.priority = 4;
  return config;
}

}  // namespace esphome::vw_open_ocu
