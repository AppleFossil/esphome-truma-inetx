import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_TYPE

from . import OPTIONAL_KEY_SCHEMA, register_listener, truma_ns, validate_key_or_type

DEPENDENCIES = ["truma_inetx"]

CONF_ON_VALUES = "on_values"

TrumaBinarySensor = truma_ns.class_("TrumaBinarySensor", binary_sensor.BinarySensor)

STATUS_TYPES = {
    "connected": 1,
    "paired": 2,
    "pairing_mode": 3,
}

CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(TrumaBinarySensor)
    .extend(OPTIONAL_KEY_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_TYPE): cv.one_of(*STATUS_TYPES, lower=True),
            # Values that mean "on" (default: anything but 0). For the
            # tri-state Active parameters use [1] (2 means idle).
            cv.Optional(CONF_ON_VALUES): cv.ensure_list(cv.int_),
        }
    ),
    validate_key_or_type(CONF_TYPE),
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await register_listener(var, config)
    if CONF_TYPE in config:
        cg.add(var.set_status_type(STATUS_TYPES[config[CONF_TYPE]]))
    for v in config.get(CONF_ON_VALUES, []):
        cg.add(var.add_on_value(v))
