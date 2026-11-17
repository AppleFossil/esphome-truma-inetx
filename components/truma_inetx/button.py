import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_TYPE, CONF_VALUE

from . import (
    CONF_ADDRESS,
    CONF_PARAM,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    HUB_SCHEMA,
    truma_ns,
)

DEPENDENCIES = ["truma_inetx"]

TrumaButton = truma_ns.class_("TrumaButton", button.Button)

TYPES = {
    "pair": 0,
    "unpair": 1,
    "reconnect": 2,
    "dump": 3,
    "measure": 4,
    "write": 5,
}


def validate_write(config):
    if config[CONF_TYPE] == "write":
        if CONF_TOPIC not in config or CONF_PARAM not in config:
            raise cv.Invalid("type 'write' needs 'topic' and 'param'")
    elif CONF_TOPIC in config or CONF_PARAM in config:
        raise cv.Invalid("'topic'/'param' only apply to type 'write'")
    return config


CONFIG_SCHEMA = cv.All(
    button.button_schema(TrumaButton)
    .extend(HUB_SCHEMA)
    .extend(
        {
            cv.Required(CONF_TYPE): cv.one_of(*TYPES, lower=True),
            cv.Optional(CONF_TOPIC): cv.string_strict,
            cv.Optional(CONF_PARAM): cv.string_strict,
            cv.Optional(CONF_VALUE, default=1): cv.int_,
            cv.Optional(CONF_ADDRESS, default=0): cv.hex_uint16_t,
        }
    ),
    validate_write,
)


async def to_code(config):
    var = await button.new_button(config)
    hub = await cg.get_variable(config[CONF_TRUMA_INETX_ID])
    cg.add(var.set_parent(hub))
    cg.add(var.set_type(TYPES[config[CONF_TYPE]]))
    if config[CONF_TYPE] == "write":
        cg.add(
            var.set_write(
                config[CONF_TOPIC],
                config[CONF_PARAM],
                config[CONF_VALUE],
                config[CONF_ADDRESS],
            )
        )
