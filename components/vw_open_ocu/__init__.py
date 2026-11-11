"""ESPHome configuration for the passive ESP32-C3 CAN gateway."""

from urllib.parse import urlsplit
import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import esp32, socket
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_USERNAME
from esphome.core import CORE

DEPENDENCIES = ["esp32", "wifi", "ota", "time"]
AUTO_LOAD = ["socket"]
CONFLICTS_WITH = ["canbus"]

Gateway = cg.esphome_ns.namespace("vw_open_ocu").class_("Gateway", cg.Component)


def _broker_uri(value):
    value = cv.string_strict(value)
    try:
        uri = urlsplit(value)
        if (
            uri.scheme != "mqtts"
            or not uri.hostname
            or uri.username is not None
            or uri.path not in ("", "/")
            or uri.query
            or uri.fragment
            or (uri.port is not None and not 1 <= uri.port <= 65535)
            or any(character.isspace() for character in value)
        ):
            raise ValueError
    except ValueError as exc:
        raise cv.Invalid("Use mqtts://host[:port] with credentials in separate fields") from exc
    return value


def _gateway_id(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,31}", value):
        raise cv.Invalid("Gateway ID must match the Home Assistant MQTT gateway ID")
    return value


def _validate(config):
    if CORE.target_framework != "esp-idf" or esp32.get_esp32_variant() != "ESP32C3":
        raise cv.Invalid("VW Open OCU requires an ESP32-C3 with the ESP-IDF framework")
    if not cv.Version(5, 5, 0) <= esp32.idf_version() < cv.Version(6, 0, 0):
        raise cv.Invalid("VW Open OCU uses the ESP-IDF 5.5 TWAI driver")
    if config["rx_pin"] == config["unused_tx_pin"]:
        raise cv.Invalid("RX and the unconnected placeholder pin must be different")
    for entry in config["allowlist"]:
        if not entry["extended"] and entry["can_id"] > 0x7FF:
            raise cv.Invalid("Standard CAN identifiers are at most 0x7FF")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Gateway),
            cv.Required("gateway_id"): _gateway_id,
            cv.Required("broker_uri"): _broker_uri,
            cv.Required(CONF_USERNAME): cv.sensitive(cv.string_strict),
            cv.Required(CONF_PASSWORD): cv.sensitive(cv.string_strict),
            cv.Optional("ca_certificate"): cv.string_strict,
            cv.Optional("rx_pin", default="GPIO1"): pins.internal_gpio_input_pin_number,
            cv.Optional("unused_tx_pin", default="GPIO0"): pins.internal_gpio_output_pin_number,
            cv.Optional("capture_mode", default=False): cv.boolean,
            cv.Optional("allowlist", default=[]): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("can_id"): cv.All(cv.hex_int, cv.Range(min=0, max=0x1FFFFFFF)),
                        cv.Optional("extended", default=False): cv.boolean,
                    }
                )
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
    socket.consume_sockets(1, "vw_open_ocu"),
)


async def to_code(config):
    esp32.include_builtin_idf_component("driver")
    esp32.include_builtin_idf_component("mqtt")
    esp32.include_builtin_idf_component("esp-tls")
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    cg.add_define("USE_OTA_STATE_LISTENER")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_gateway_id(config["gateway_id"]))
    cg.add(var.set_broker(config["broker_uri"], config[CONF_USERNAME], config[CONF_PASSWORD]))
    if "ca_certificate" in config:
        cg.add(var.set_ca_certificate(config["ca_certificate"]))
    cg.add(var.set_pins(config["rx_pin"], config["unused_tx_pin"]))
    cg.add(var.set_capture_mode(config["capture_mode"]))
    for entry in config["allowlist"]:
        cg.add(var.add_filter(entry["can_id"], entry["extended"]))
