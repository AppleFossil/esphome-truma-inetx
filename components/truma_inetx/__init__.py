"""Truma iNet X panel over direct BLE (ESP32 as the BLE central).

Replaces the Home Assistant integration + Bluetooth proxy combination with a
dedicated ESP32 that holds the link to the panel itself.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble, esp32_ble_tracker
from esphome.components import time as time_
from esphome.const import CONF_ID, CONF_TIME_ID

CODEOWNERS = []
DEPENDENCIES = ["esp32", "esp32_ble_tracker"]
AUTO_LOAD = ["esp32_ble_client"]
MULTI_CONF = False

CONF_TRUMA_INETX_ID = "truma_inetx_id"
CONF_BLE_CLIENT_ID = "ble_client_id"
CONF_USER_NAME = "user_name"
CONF_PAIRING_TIMEOUT = "pairing_timeout"
CONF_LOG_PARAMETERS = "log_parameters"
CONF_PRIVACY = "privacy"
CONF_LANGUAGE = "language"
CONF_SUPERVISION_TIMEOUT = "supervision_timeout"
CONF_TOPIC = "topic"
CONF_PARAM = "param"
CONF_ADDRESS = "address"

truma_ns = cg.esphome_ns.namespace("truma_inetx")
ble_client_base_ns = cg.esphome_ns.namespace("esp32_ble_client")
BLEClientBase = ble_client_base_ns.class_("BLEClientBase", cg.Component)

TrumaInetX = truma_ns.class_("TrumaInetX", cg.Component)
TrumaBleClient = truma_ns.class_("TrumaBleClient", BLEClientBase)

_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TrumaInetX),
            cv.GenerateID(CONF_BLE_CLIENT_ID): cv.declare_id(TrumaBleClient),
            cv.Optional(CONF_USER_NAME, default="ESPHome"): cv.All(
                cv.string_strict, cv.Length(min=1, max=32)
            ),
            cv.Optional(
                CONF_PAIRING_TIMEOUT, default="120s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LOG_PARAMETERS, default=False): cv.boolean,
            # Connect from a resolvable private address, like a phone.
            cv.Optional(CONF_PRIVACY, default=False): cv.boolean,
            # Language of the texts the ESP produces (status, last disconnect, timers).
            cv.Optional(CONF_LANGUAGE, default="en"): cv.one_of("en", "de", lower=True),
            # How long the BLE link survives without hearing the panel. Longer
            # rides out WiFi/BLE coexistence gaps (max 32 s by the BLE spec).
            cv.Optional(CONF_SUPERVISION_TIMEOUT, default="20s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=2), max=cv.TimePeriod(seconds=32)),
            ),
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)

# ESPHome counts BLE connection slots (esp32_ble in current releases,
# esp32_ble_tracker in older ones).
_consume = getattr(esp32_ble, "consume_connection_slots", None) or getattr(
    esp32_ble_tracker, "consume_connection_slots", None
)
CONFIG_SCHEMA = cv.All(_SCHEMA, _consume(1, "truma_inetx")) if _consume else _SCHEMA


# Shared by the entity platforms -------------------------------------------
HUB_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TRUMA_INETX_ID): cv.use_id(TrumaInetX),
    }
)

KEY_SCHEMA = HUB_SCHEMA.extend(
    {
        cv.Required(CONF_TOPIC): cv.string_strict,
        cv.Required(CONF_PARAM): cv.string_strict,
        # 0 = whichever bus device publishes it (or the panel for RoomClimate)
        cv.Optional(CONF_ADDRESS, default=0): cv.hex_uint16_t,
    }
)

OPTIONAL_KEY_SCHEMA = HUB_SCHEMA.extend(
    {
        cv.Optional(CONF_TOPIC): cv.string_strict,
        cv.Optional(CONF_PARAM): cv.string_strict,
        cv.Optional(CONF_ADDRESS, default=0): cv.hex_uint16_t,
    }
)


def validate_key_or_type(type_key):
    def validator(config):
        has_type = type_key in config
        has_key = CONF_TOPIC in config or CONF_PARAM in config
        if has_type and has_key:
            raise cv.Invalid(f"Use either '{type_key}' or 'topic'/'param', not both")
        if not has_type and not (CONF_TOPIC in config and CONF_PARAM in config):
            raise cv.Invalid(f"Set '{type_key}', or both 'topic' and 'param'")
        return config

    return validator


async def register_listener(var, config):
    hub = await cg.get_variable(config[CONF_TRUMA_INETX_ID])
    cg.add(var.set_parent(hub))
    if CONF_TOPIC in config:
        cg.add(
            var.set_key(config[CONF_TOPIC], config[CONF_PARAM], config[CONF_ADDRESS])
        )
    cg.add(hub.register_listener(var))
    return hub


async def to_code(config):
    cg.add_define("USE_ESP32_BLE_UUID")
    if hasattr(esp32_ble, "register_bt_logger") and hasattr(esp32_ble, "BTLoggers"):
        esp32_ble.register_bt_logger(esp32_ble.BTLoggers.GATT, esp32_ble.BTLoggers.SMP)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    client = cg.new_Pvariable(config[CONF_BLE_CLIENT_ID])
    await cg.register_component(client, {})
    await esp32_ble_tracker.register_client(client, config)
    cg.add(client.set_auto_connect(True))
    cg.add(client.set_hub(var))
    cg.add(var.set_client(client))

    cg.add(var.set_user_name(config[CONF_USER_NAME]))
    cg.add(var.set_pairing_timeout(config[CONF_PAIRING_TIMEOUT]))
    cg.add(var.set_log_parameters(config[CONF_LOG_PARAMETERS]))
    cg.add(var.set_privacy(config[CONF_PRIVACY]))
    cg.add(var.set_german(config[CONF_LANGUAGE] == "de"))
    cg.add(var.set_supervision_timeout(config[CONF_SUPERVISION_TIMEOUT]))
    if CONF_TIME_ID in config:
        rtc = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(rtc))
